#!/usr/bin/env python3
"""LE Audio 监测数据分析：崩溃时间线 + 每 10s 速率 + 事件关联"""
import os, re, sys
from collections import Counter, defaultdict

d = sys.argv[1] if len(sys.argv) > 1 else "."
logf = os.path.join(d, "logcat.txt")
TS = re.compile(r'^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d\d\d)')

def tsec(m):
    return int(m.group(3)) * 3600 + int(m.group(4)) * 60 + int(m.group(5)) + int(m.group(6)) / 1000.0

def hhmmss(t):
    return "%02d:%02d:%02d" % (int(t) // 3600 % 24, int(t) // 60 % 60, int(t) % 60)

iso = []        # (t, pktno, handle)
drops = []      # (t, line)
ncps = []       # (t, nhandle, credits)
halinit = []    # (t,)
haldied = []    # (t, line)
aborts = []     # (t, line)
hwerr = []      # (t,)
twelve = []     # (t, line)
ciglines = []   # (t, line)
underrun = []   # (t,)

if os.path.exists(logf):
    for line in open(logf, errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        t = tsec(m)
        if "sendIsoData: forwarded" in line:
            mm = re.search(r'pkt #(\d+) h0=0x([0-9a-fA-F]+) h1=0x([0-9a-fA-F]+)', line)
            if mm:
                h = int(mm.group(2), 16) | ((int(mm.group(3), 16) & 0x0F) << 8)
                iso.append((t, int(mm.group(1)), h))
        elif "dropping ISO" in line:
            drops.append((t, line.strip()))
        elif "NCP synth" in line:
            mm = re.search(r'(\d+) handle\(s\), (\d+) credit', line)
            if mm:
                ncps.append((t, int(mm.group(1)), int(mm.group(2))))
        elif "BluetoothHciHook v" in line and "init" in line:
            halinit.append((t,))
        elif "serviceDied" in line or "HAL died" in line:
            haldied.append((t, line.strip()[-110:]))
        elif "Abort message" in line or "FATAL EXCEPTION" in line:
            aborts.append((t, line.strip()[-110:]))
        elif ("Hardware Error" in line or "hardware_error" in line
              or "Actual last RX pkt len: 4" in line or "04 10 01 0F" in line
              or "SSR is completed" in line or "Last RX packet before SSR" in line):
            hwerr.append((t,))
        elif "twelve" in line.lower() or "PlaybackState" in line:
            twelve.append((t, line.strip()[-100:]))
        elif "Current state:" in line or "cig state" in line:
            ciglines.append((t, line.strip()[-100:]))
        elif "underrun" in line.lower() or "Credits underflow" in line:
            underrun.append((t,))

out = []
P = out.append
P("=" * 100)
P("LE AUDIO 监测分析  目录: %s" % d)
P("=" * 100)

# ---------- 1) 崩溃/重启时间线 ----------
P("\n【1】HAL 重启 / 栈崩溃时间线")
restarts = sorted(halinit + [(t, None) for t in [x[0] for x in haldied]])
allcrash = sorted([(t, "HAL-init(重启)", "") for t in [x[0] for x in halinit]] +
                  [(t, "HAL-died", l) for t, l in haldied] +
                  [(t, "abort", l) for t, l in aborts] +
                  [(t, "HW-ERR", "") for t in hwerr])
if not allcrash:
    P("  （无）")
prev = None
for t, kind, l in allcrash:
    gap = "  (+%.1fs)" % (t - prev) if prev is not None else ""
    P("  %s  %-12s %s%s" % (hhmmss(t), kind, l[:80], gap))
    prev = t
P("  合计: HAL重启=%d  HAL死亡=%d  栈abort=%d  硬件错误=%d" %
  (len(halinit), len(haldied), len(aborts), len(hwerr)))

# ---------- 2) 每 10s 速率 ----------
P("\n【2】每 10s 速率（ISO 发送 / 丢帧 / NCP合成）")
if iso or drops:
    t0 = min([x[0] for x in iso] + [x[0] for x in drops] + [x[0] for x in ncps] + [9e9])
    t1 = max([x[0] for x in iso] + [x[0] for x in drops] + [x[0] for x in ncps] + [0])
    bins = defaultdict(lambda: {"iso": 0, "drop": 0, "ncp": 0, "nc": 0, "first": None, "last": None})
    for t, n, h in iso:
        b = int((t - t0) // 10) * 10
        bins[b]["iso"] += 1
        if bins[b]["first"] is None:
            bins[b]["first"] = n
        bins[b]["last"] = n
    for t, l in drops:
        b = int((t - t0) // 10) * 10
        bins[b]["drop"] += 1
    for t, nh, nc in ncps:
        b = int((t - t0) // 10) * 10
        bins[b]["ncp"] += 1
        bins[b]["nc"] += nc
    P("  时间窗      ISO日志条数  ISO速率*  丢帧/s  NCP合成/s  合成credits/s")
    for b in sorted(bins):
        v = bins[b]
        # ISO 日志每 64 包一条 → pkt# 增量 × 64 / 10s
        if v["first"] is not None and v["last"] is not None:
            rate = (v["last"] - v["first"]) * 64 / 10.0
            if rate < 0:
                rate = v["iso"] * 64 / 10.0
        else:
            rate = 0
        P("  %s     %6d      %7.1f/s  %5d/s   %5d/s     %6d/s" %
          (hhmmss(t0 + b), v["iso"], rate, v["drop"] / 10, v["ncp"] / 10, v["nc"] / 10))

# ---------- 3) 逐事件关联 ----------
P("\n【3】事件关联（每次崩溃前后 10s 的 ISO 速率）")
crashes = sorted(set([x[0] for x in halinit] + [x[0] for x in haldied] + [x[0] for x in aborts]))
for ct in crashes:
    before = [n for t, n, h in iso if ct - 10 <= t < ct]
    after = [n for t, n, h in iso if ct <= t < ct + 10]
    def rate(v):
        return (v[-1] - v[0]) * 64 / 10.0 if len(v) >= 2 else 0
    P("  %s  崩溃前 %.0f/s (%d条)  崩溃后 %.0f/s (%d条)" %
      (hhmmss(ct), rate(before), len(before), rate(after), len(after)))

# ---------- 4) 快照列表 ----------
P("\n【4】快照文件（事件时刻全量状态）")
sd = os.path.join(d, "snapshots")
if os.path.isdir(sd):
    for f in sorted(os.listdir(sd)):
        P("  %s" % f)
    P("  共 %d 份" % len(os.listdir(sd)))

# ---------- 5) 结论 ----------
P("\n【5】关键结论")
n_restart = len(halinit)
if n_restart:
    span = (max([x[0] for x in halinit]) - min([x[0] for x in halinit])) if len(halinit) > 1 else 0
    P("  HAL 重启 %d 次，跨度 %.0fs → 平均每 %.1fs 重启一次" %
      (n_restart, span, span / max(1, n_restart - 1)))
if iso:
    r = (iso[-1][1] - iso[0][1]) * 64 / max(1e-6, iso[-1][0] - iso[0][0])
    P("  全程平均 ISO 发送速率: %.1f SDU/s (需求 200/s)" % r)
if drops:
    P("  丢帧日志总数: %d" % len(drops))
if ncps:
    P("  NCP 合成总次数: %d, 总 credits: %d" % (len(ncps), sum(x[2] for x in ncps)))
if not iso and not drops:
    P("  ⚠ 全程无 ISO 流量（音乐未播放或链路未建立）")

report = "\n".join(out)
print(report)
open(os.path.join(d, "summary.txt"), "w").write(report + "\n")
print("\n[+] 报告已写入 %s/summary.txt" % d)
