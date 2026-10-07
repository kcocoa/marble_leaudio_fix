#!/usr/bin/env bash
# patch_hal_binaries.sh — 对 dump 来的原厂 HAL 套用二进制补丁，产出部署用 .so。
#
# 输入（原厂文件，由 scripts/dump_device_binaries.sh 导出）:
#   module/vendor/lib64/hw/libbluetooth_qti_real.so    原厂 android.hardware.bluetooth@1.0-impl-qti.so
#   module/vendor/lib64/hw/audio.bluetooth.default.so
#   module/apex/com.android.bt/lib64/libbluetooth_jni.so   蓝牙栈（patch_bt_jni_odd_octets.py）
# 就地修改，可重复执行（已打过的补丁会跳过）。
#
# 用法:
#   ./patch/patch_hal_binaries.sh [--module-dir <路径>] [--qti-only | --audio-only | --jni-only]
#
# 机器码均经本机已部署件反汇编校准（llvm-objdump）。
# 命中不足会中止——ROM/版本不同时不要盲改。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODDIR="$HERE/module"
DO_QTI=1
DO_AUDIO=1
DO_JNI=1

while [ $# -gt 0 ]; do
  case "$1" in
    --module-dir) MODDIR="$2"; shift 2 ;;
    --qti-only) DO_AUDIO=0; DO_JNI=0; shift ;;
    --audio-only) DO_QTI=0; DO_JNI=0; shift ;;
    --jni-only) DO_QTI=0; DO_AUDIO=0; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

V="$MODDIR/vendor/lib64/hw"
warn() { echo "[!] $*"; }
ok()   { echo "[✓] $*"; }

# ---------------------------------------------------------------- QTI HAL ---
if [ "$DO_QTI" = "1" ]; then
  SRC="$V/libbluetooth_qti_real.so"
  if [ ! -f "$SRC" ]; then
    warn "缺少 $SRC（先跑 scripts/dump_device_binaries.sh）"
  else
    echo "[*] QTI HAL 补丁（patch_qti_iso_rx.py：SONAME 改名 + 4 处 ISO 接收补丁）"
    python3 "$HERE/patch/patch_qti_iso_rx.py" "$SRC" "$SRC"
    ok "QTI HAL -> $SRC"
  fi
fi

# ------------------------------------------------------------- audio HAL ---
if [ "$DO_AUDIO" = "1" ]; then
  SRC="$V/audio.bluetooth.default.so"
  if [ ! -f "$SRC" ]; then
    warn "缺少 $SRC（先跑 scripts/dump_device_binaries.sh）"
  else
    echo "[*] audio.bluetooth.default.so 补丁（UpdateSinkMetadata 空函数化，2 处）"
    OUT="$SRC"
    python3 - "$OUT" <<'PY'
import sys

path = sys.argv[1]

# (offset, 期望原厂字节 or None, 目标字节, 说明)
# None = 不校验原值，按空函数化语义写入（仍校验入口 paciasp 未被动过）
GUARDS = [
    (0x11544, "d503233f", "UpdateSinkMetadata 入口 paciasp 必须原样（BTI 落点，改了必 SIGILL）"),
]
PATCHES = [
    # 注意：不要改 init_session_type（0x135cc/0x135e8/0x13620）的会话号——那是硬件
    # offload 尝试期间的补丁，软件编码方案下会让音频 HAL 找错会话（docs/dead-ends.md 第 1 节）
    # UpdateSinkMetadata() 整体空函数化：sub sp,sp,#0x90 -> autiasp ; stp x29,x30 -> ret
    (0x11548, None,       "d50323bf", "sub sp,sp,#0x90 -> autiasp（与入口 paciasp 配对）"),
    (0x1154c, None,       "d65f03c0", "stp x29,x30,[sp,#0x40] -> ret（函数体其余部分不可达）"),
]

blob = bytearray(open(path, "rb").read())


def word(off):
    # 文件小端存 → 反转成大端人读序，与表内 hex 常量同序
    return bytes(blob[off:off + 4][::-1]).hex()


def put(off, hexword):
    blob[off:off + 4] = bytes.fromhex(hexword)[::-1]


hit = 0
for off, want, why in GUARDS:
    cur = word(off)
    if cur != want:
        print(f"[x] 0x{off:x} 守卫失败（得 {cur}，期望 {want}）：{why}", file=sys.stderr)
        sys.exit(1)

for off, find, repl, why in PATCHES:
    cur = word(off)
    if cur == repl:
        print(f"  [=] 0x{off:x} 已是目标值（{why}）"); hit += 1
    elif find is None or cur == find:
        prev = cur
        put(off, repl)
        print(f"  [+] 0x{off:x} {prev} -> {repl}（{why}）"); hit += 1
    else:
        print(f"  [!] 0x{off:x} 字节不符（得 {cur}，期望 {find}），跳过：{why}")

if hit < len(PATCHES):
    print(f"[x] 仅命中 {hit}/{len(PATCHES)} 处——确认这是同机型原厂件后再跑", file=sys.stderr)
    sys.exit(1)
open(path, "wb").write(bytes(blob))
print(f"[✓] audio.bluetooth.default.so 补丁完成: {path}")
PY
    ok "audio HAL -> $OUT"
  fi
fi

# --------------------------------------------------- 蓝牙栈 libbluetooth_jni ---
if [ "$DO_JNI" = "1" ]; then
  SRC="$MODDIR/apex/com.android.bt/lib64/libbluetooth_jni.so"
  if [ ! -f "$SRC" ]; then
    warn "缺少 $SRC（先跑 scripts/dump_device_binaries.sh）"
  else
    echo "[*] libbluetooth_jni.so 补丁（奇数帧长丢 1 字节，6 处）"
    python3 "$HERE/patch/patch_bt_jni_odd_octets.py" "$SRC"
    ok "libbluetooth_jni.so -> $SRC"
  fi
fi
