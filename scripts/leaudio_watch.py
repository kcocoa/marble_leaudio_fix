#!/usr/bin/env python3
"""leaudio_watch.py — 听歌时在后台监测 LE Audio 链路，帮你判断 artifact 出在手机侧还是空口 / 耳机侧。

做法：定期把设备上滚动的 btsnoop 拉下来，只处理新增的记录：
  - ISO 发送：序号是否连续（缺帧）、发送间隔（抖动 / 跳帧）、控制器回报的完成数（积压）
  - 把发出去的 SDU 用 liblc3 逐帧解码：坏帧（会触发丢帧补偿）、削波
  - HCI 事件：断连、CIS 建立失败、连接参数更新、硬件错误
  - 蓝牙进程和 HAL 进程有没有重启；logcat 里的关键报错
你一听到 artifact 就按回车打标记，脚本会回头看那一刻前 10 秒，告诉你主机侧有没有异常：
  有异常   -> 问题在手机这边（缺帧 / 间隔变大 / 坏帧 / 断连 / 进程重启）
  全部干净 -> 更可能是空口丢包或耳机侧。本脚本看不到空口：控制器把包发出去之后耳机有没有收到，
              主机这边的 HCI 完成数不区分「发出」和「超时丢弃」。

产出 monitor_runs/watch_MMDD_HHMMSS/：
  watch.log   屏幕上的全部输出
  stats.csv   每个采样周期、每条 CIS 一行
  mark_N/     每次打标记时保存的 btsnoop（当前和 .last），方便事后细看

用法:
  SERIAL=<adb 序列号> ./scripts/leaudio_watch.py [--interval 10] [--duration 0] [--no-lc3]
  Ctrl-C 结束并打印总结。--duration 0 表示一直跑。

依赖: adb、设备已 root（su）、btsnoop 为 full 模式（persist.bluetooth.btsnooplogmode=full）；
解码需要 liblc3（Arch: pacman -S liblc3）。时间统一用设备时钟（btsnoop 记录和 `date` 都来自设备）。
"""
import argparse
import collections
import ctypes
import ctypes.util
import datetime
import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time

EPOCH_US = 0x00dcddb30f2f8000          # btsnoop 时间戳是从公元 1 年起的微秒
SNOOP = "/data/misc/bluetooth/logs/btsnoop_hci.log"
LOG_RE = re.compile(r"dropping ISO|Missing samples|CIS creation failed|failed to create CIS|stopping the stream|"
                    r"octets per frame mismatch|Fatal signal|FATAL EXCEPTION|underrun|underflow")
REASONS = {0x08: "连接超时", 0x13: "对端主动断开", 0x16: "本机主动断开", 0x3e: "建立失败", 0x3d: "MIC 失败"}
GAP_MS = 15.0            # 超过它算跳帧（异常）
JITTER_MS = 12.5         # 超过它算抖动偏大（只统计，不算异常）
PAUSE_MS = 200.0         # 超过它认为是暂停 / 重新开始，不算间隔异常
BUFFERS = 22             # 控制器 ISO 缓冲数（固件 00680）

SERIAL = ""
u16 = lambda b: b[0] | (b[1] << 8)


def adb(args, timeout=30):
    return subprocess.run(["adb", "-s", SERIAL] + args, capture_output=True, timeout=timeout).stdout


def pull(path):
    return adb(["exec-out", "su -c 'cat %s'" % path])


def sh(cmd):
    return adb(["shell", cmd]).decode(errors="replace").strip()


def device_now():
    try:
        return float(sh("date +%s.%N").split()[0])
    except (ValueError, IndexError):
        return time.time()


class Lc3:
    def __init__(self, dt_us, rate):
        path = ctypes.util.find_library("lc3") or "/usr/lib/liblc3.so.1"
        self.lib = ctypes.CDLL(path)
        L = self.lib
        L.lc3_decoder_size.restype = ctypes.c_uint
        L.lc3_decoder_size.argtypes = [ctypes.c_int, ctypes.c_int]
        L.lc3_setup_decoder.restype = ctypes.c_void_p
        L.lc3_setup_decoder.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        L.lc3_decode.restype = ctypes.c_int
        L.lc3_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_void_p, ctypes.c_int]
        self.dt_us, self.rate = dt_us, rate
        self.dec = {}
        self.pcm = (ctypes.c_int16 * (rate * dt_us // 1000000))()

    def decode(self, handle, data):
        """返回 (坏帧?, 削波?)。坏帧 = liblc3 走了丢帧补偿（帧长不对或码流损坏）。"""
        if handle not in self.dec:
            mem = ctypes.create_string_buffer(self.lib.lc3_decoder_size(self.dt_us, self.rate))
            self.dec[handle] = (mem, self.lib.lc3_setup_decoder(self.dt_us, self.rate, 0, mem))
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        r = self.lib.lc3_decode(self.dec[handle][1], buf, len(data), 0, self.pcm, 1)
        mx = max(max(self.pcm), -min(self.pcm))
        return r != 0, mx >= 32767


def records(data):
    off, n = 16, len(data)
    while off + 24 <= n:
        ol, il, fl, dr, t = struct.unpack_from(">IIIIq", data, off)
        off += 24
        yield t, (t - EPOCH_US) / 1e6, fl, data[off:off + il]
        off += il


class Watch:
    def __init__(self, lc3):
        self.lc3 = lc3
        self.last_raw = 0
        self.last_t = 0.0                              # 已处理的最新记录时间（设备时钟，秒）
        self.frag = {}                                 # handle -> [t, seq, sdu_len, bytearray]
        self.iso = collections.defaultdict(list)       # handle -> [(t, dt_ms, missing, bad, clip)]
        self.prev = {}                                 # handle -> (t, seq)
        self.sent_pkts = collections.Counter()
        self.ncp = collections.Counter()
        self.events = []                               # (t, kind, text)   kind: bad / info
        self.snoop_cur = b""

    # ---- 单条记录
    def feed(self, t, flags, p):
        if not p:
            return
        if p[0] == 5 and not flags & 1:
            self.feed_iso(t, p)
        elif p[0] == 4:
            self.feed_event(t, p)

    def feed_iso(self, t, p):
        h = u16(p[1:3])
        handle, pb, ts = h & 0xfff, (h >> 12) & 3, (h >> 14) & 1
        k = 4 if ts else 0
        body = p[5:]
        self.sent_pkts[handle] += 1
        if pb in (0, 2):
            self.frag[handle] = [t, u16(body[k:k + 2]), u16(body[k + 2:k + 4]) & 0x3fff, bytearray(body[k + 4:])]
        elif handle in self.frag:
            self.frag[handle][3] += body
        f = self.frag.get(handle)
        if f and len(f[3]) >= f[2]:
            del self.frag[handle]
            self.sdu(handle, f[0], f[1], bytes(f[3][:f[2]]))

    def sdu(self, handle, t, seq, data):
        dt_ms, missing = 0.0, 0
        if handle in self.prev:
            pt, ps = self.prev[handle]
            dt_ms = (t - pt) * 1000
            if dt_ms > PAUSE_MS:
                dt_ms = 0.0                            # 暂停后重新开始
            else:
                d = (seq - ps) & 0xffff
                if 1 < d < 1000:
                    missing = d - 1
        self.prev[handle] = (t, seq)
        bad = clip = False
        if self.lc3:
            bad, clip = self.lc3.decode(handle, data)
        self.iso[handle].append((t, dt_ms, missing, bad, clip))

    def feed_event(self, t, p):
        code = p[1]
        if code == 0x13:                               # Number Of Completed Packets
            n = p[3]
            for i in range(n):
                h = u16(p[4 + 2 * i:6 + 2 * i]) & 0xfff
                self.ncp[h] += u16(p[4 + 2 * n + 2 * i:6 + 2 * n + 2 * i])
        elif code == 0x05 and len(p) >= 7:             # Disconnection Complete
            r = p[6]
            self.events.append((t, "bad" if r not in (0x13, 0x16) else "info",
                                "断开 handle %d 原因 0x%02x %s" % (u16(p[4:6]) & 0xfff, r, REASONS.get(r, ""))))
        elif code == 0x10:
            self.events.append((t, "bad", "控制器 Hardware Error 0x%02x" % p[3]))
        elif code == 0x3e and len(p) > 8:
            sub = p[3]
            if sub == 0x19:                            # CIS Established
                st, h = p[4], u16(p[5:7])
                self.events.append((t, "bad" if st else "info",
                                    "CIS handle %d %s" % (h, "建立失败 0x%02x" % st if st else "建立")))
            elif sub == 0x03 and p[4] == 0:            # Connection Update Complete
                self.events.append((t, "info", "ACL handle %d 连接间隔 %.1f ms" % (u16(p[5:7]) & 0xfff, u16(p[7:9]) * 1.25)))

    # ---- 拉取 + 增量处理
    def poll(self):
        cur = pull(SNOOP)
        if len(cur) < 16 or cur[:8] != b"btsnoop\0":
            return False
        self.snoop_cur = cur
        recs = list(records(cur))
        if self.last_raw and recs and recs[0][0] > self.last_raw:      # 滚动了，补读 .last
            last = pull(SNOOP + ".last")
            if len(last) > 16 and last[:8] == b"btsnoop\0":
                self.process(records(last))
        self.process(recs)
        return True

    def process(self, recs):
        top = self.last_raw
        for raw, t, flags, p in recs:
            if raw <= self.last_raw:
                continue
            top = max(top, raw)
            self.last_t = max(self.last_t, t)
            self.feed(t, flags, p)
        self.last_raw = top

    # ---- 窗口统计 [t0, t1]
    def window(self, t0, t1):
        out = {}
        for h, rows in self.iso.items():
            r = [x for x in rows if t0 <= x[0] <= t1]
            if not r:
                continue
            out[h] = dict(n=len(r), missing=sum(x[2] for x in r),
                          gap=sum(1 for x in r if x[1] > GAP_MS), jit=sum(1 for x in r if x[1] > JITTER_MS),
                          maxdt=max(x[1] for x in r), bad=sum(1 for x in r if x[3]), clip=sum(1 for x in r if x[4]))
        return out

    def inflight(self, handles):
        return {h: self.sent_pkts[h] - self.ncp.get(h, 0) for h in handles}


def anomalies(stats, ev, infl):
    why = []
    for h, s in sorted(stats.items()):
        if s["missing"]:
            why.append("handle %d 缺 %d 帧" % (h, s["missing"]))
        if s["gap"]:
            why.append("handle %d 有 %d 次发送间隔 >%.0f ms（最大 %.1f ms）" % (h, s["gap"], GAP_MS, s["maxdt"]))
        if s["bad"]:
            why.append("handle %d 有 %d 帧 LC3 解码异常" % (h, s["bad"]))
        if s["clip"]:
            why.append("handle %d 有 %d 帧削波" % (h, s["clip"]))
    why += [e[2] for e in ev if e[1] == "bad"]
    for h, v in sorted(infl.items()):
        if v > BUFFERS - 2:
            why.append("handle %d 控制器积压 %d 个包（缓冲 %d）" % (h, v, BUFFERS))
    return why


def hhmmss(t):
    return datetime.datetime.fromtimestamp(t).strftime("%H:%M:%S")


def main():
    global SERIAL
    ap = argparse.ArgumentParser(description="LE Audio 链路监测（听歌时用）")
    ap.add_argument("--serial", default=os.environ.get("SERIAL", ""))
    ap.add_argument("--interval", type=float, default=10.0, help="采样周期（秒）")
    ap.add_argument("--duration", type=float, default=0, help="总时长（秒），0 = 直到 Ctrl-C")
    ap.add_argument("--window", type=float, default=10.0, help="打标记时回看多少秒")
    ap.add_argument("--no-lc3", action="store_true", help="不做 LC3 解码")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--frame-us", type=int, default=10000)
    a = ap.parse_args()
    SERIAL = a.serial
    if not SERIAL:
        sys.exit("[x] export SERIAL=<adb 序列号>")
    if adb(["get-state"]).strip() != b"device":
        sys.exit("[x] 设备 %s 不在线" % SERIAL)

    lc3 = None
    if not a.no_lc3:
        try:
            lc3 = Lc3(a.frame_us, a.rate)
        except OSError as e:
            print("[!] 没有 liblc3，跳过解码检查（%s）" % e)

    root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "monitor_runs")
    out = os.path.join(root, "watch_" + time.strftime("%m%d_%H%M%S"))
    os.makedirs(out)
    logf = open(os.path.join(out, "watch.log"), "w", buffering=1)
    csvf = open(os.path.join(out, "stats.csv"), "w", buffering=1)
    csvf.write("time,handle,sdu,missing,gap_gt15ms,jitter_gt12.5ms,max_interval_ms,lc3_bad,clip,inflight\n")

    def say(msg=""):
        print(msg, flush=True)
        logf.write(msg + "\n")

    say("[+] 输出: %s" % out)
    say("[+] 采样周期 %.0f s。听到 artifact 就按回车打标记（分析前 %.0f 秒）；Ctrl-C 结束。" % (a.interval, a.window))

    marks = queue.Queue()
    logq = queue.Queue()

    def reader():
        for _ in sys.stdin:
            marks.put(1)
    threading.Thread(target=reader, daemon=True).start()

    logcat_proc = subprocess.Popen(["adb", "-s", SERIAL, "logcat", "-b", "all", "-v", "epoch", "-T", "1"],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")

    def logcat():
        for line in logcat_proc.stdout:
            if LOG_RE.search(line) and "BluetoothHciHook: sendIsoData" not in line:
                try:
                    logq.put((float(line.split()[0]), line.strip()[:200]))
                except (ValueError, IndexError):
                    pass
    threading.Thread(target=logcat, daemon=True).start()

    def pids():
        return sh("pidof com.android.bluetooth; echo -; pidof android.hardware.bluetooth@1.0-service-qti").replace("\n", " ")

    w = Watch(lc3)
    last_pids = pids()
    say("[+] 进程 (com.android.bluetooth - HAL): %s" % last_pids)
    say("[+] 读取现有的 btsnoop ……")
    if not w.poll():
        sys.exit("[x] 读不到 btsnoop（需要 root，且 btsnoop 模式为 full）")
    # 只看从现在起的数据
    w.events.clear()
    for h in w.iso:
        w.iso[h].clear()
    iso_from = w.last_t
    ev_idx = 0
    say("[+] 开始。")

    t_end = time.time() + a.duration if a.duration else None
    n_mark = 0
    summary = collections.defaultdict(collections.Counter)
    mark_log = []
    try:
        while t_end is None or time.time() < t_end:
            try:
                marks.get(timeout=a.interval)
                marked = True
                while not marks.empty():
                    marks.get_nowait()
            except queue.Empty:
                marked = False
            if not w.poll():
                say("[!] %s 读 btsnoop 失败" % hhmmss(device_now()))
                continue
            now_dev = device_now()
            while not logq.empty():
                t, line = logq.get()
                w.events.append((t, "bad", "logcat: " + line))
            cur_pids = pids()
            if cur_pids != last_pids:
                w.events.append((now_dev, "bad", "进程变了: %s -> %s" % (last_pids, cur_pids)))
                last_pids = cur_pids

            # ---- 常规采样行：(iso_from, last_t]，事件按到达顺序只显示一次
            stats = w.window(iso_from + 1e-6, w.last_t)
            ev, ev_idx = w.events[ev_idx:], len(w.events)
            iso_from = w.last_t
            infl = w.inflight(stats)
            parts = []
            for h, s in sorted(stats.items()):
                parts.append("h%d: %d sdu 缺%d 跳%d 抖%d 最大%.1fms 坏%d 削%d" %
                             (h, s["n"], s["missing"], s["gap"], s["jit"], s["maxdt"], s["bad"], s["clip"]))
                csvf.write("%s,%d,%d,%d,%d,%d,%.1f,%d,%d,%d\n" % (hhmmss(w.last_t), h, s["n"], s["missing"], s["gap"],
                                                                  s["jit"], s["maxdt"], s["bad"], s["clip"], infl.get(h, 0)))
                for k in ("n", "missing", "gap", "jit", "bad", "clip"):
                    summary[h][k] += s[k]
                summary[h]["maxdt"] = max(summary[h]["maxdt"], s["maxdt"])
            why = anomalies(stats, ev, infl)
            say(("!! " if why else "   ") + "%s  %s" % (hhmmss(now_dev), " | ".join(parts) if parts else "（没有 ISO 数据：没在播放？）"))
            for x in why:
                say("     ! " + x)

            # ---- 标记：回看 window 秒
            if marked:
                n_mark += 1
                d = os.path.join(out, "mark_%d" % n_mark)
                os.makedirs(d)
                open(os.path.join(d, "btsnoop_hci.log"), "wb").write(w.snoop_cur)
                open(os.path.join(d, "btsnoop_hci.log.last"), "wb").write(pull(SNOOP + ".last"))
                t0 = now_dev - a.window
                mstats = w.window(t0, now_dev + 1.0)
                mev = [e for e in w.events if t0 <= e[0] <= now_dev + 1.0]
                minfl = w.inflight(mstats)
                mwhy = anomalies(mstats, mev, minfl)
                say("")
                say("=== 标记 #%d  %s（回看 %.0f 秒，已存 mark_%d/）" % (n_mark, hhmmss(now_dev), a.window, n_mark))
                for h, s in sorted(mstats.items()):
                    say("    h%d: %d sdu，缺帧 %d，间隔>%.0fms %d 次（最大 %.1f ms），LC3 异常 %d，削波 %d，积压 %d" %
                        (h, s["n"], s["missing"], GAP_MS, s["gap"], s["maxdt"], s["bad"], s["clip"], minfl.get(h, 0)))
                if not mstats:
                    say("    这段时间没有 ISO 数据")
                for e in mev:
                    say("    %s %s" % (hhmmss(e[0]), e[2]))
                if mwhy:
                    verdict = "主机侧有异常 -> 问题在手机这边：" + "；".join(mwhy)
                else:
                    verdict = "主机侧全部干净（序号连续、间隔正常、LC3 无异常、无断连、进程没重启）-> 更可能是空口丢包或耳机侧"
                say("    结论: " + verdict)
                say("")
                mark_log.append((n_mark, hhmmss(now_dev), bool(mwhy)))
    except KeyboardInterrupt:
        pass
    finally:
        logcat_proc.terminate()                # 不然 adb logcat 会留在后台

    say("")
    say("====== 总结 ======")
    for h, s in sorted(summary.items()):
        say("h%d: %d sdu，缺帧 %d，间隔>%.0fms %d 次，间隔>%.1fms %d 次，最大间隔 %.1f ms，LC3 异常 %d，削波 %d" %
            (h, s["n"], s["missing"], GAP_MS, s["gap"], JITTER_MS, s["jit"], s["maxdt"], s["bad"], s["clip"]))
    bad_ev = [e for e in w.events if e[1] == "bad"]
    say("异常事件 %d 条" % len(bad_ev))
    for e in bad_ev[-20:]:
        say("  %s %s" % (hhmmss(e[0]), e[2]))
    for n, tm, flagged in mark_log:
        say("标记 #%d %s: %s" % (n, tm, "主机侧有异常" if flagged else "主机侧干净"))
    say("日志: %s" % out)


if __name__ == "__main__":
    main()
