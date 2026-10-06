#!/usr/bin/env bash
# patch_hal_binaries.sh — 对 dump 来的原厂 HAL 套用二进制补丁，产出部署用 .so。
#
# 输入（未打补丁的原厂文件）:
#   leaudio_marble_fix_v2/vendor/lib64/hw/libbluetooth_qti_real.so
#   leaudio_marble_fix_v2/vendor/lib64/hw/audio.bluetooth.default.so
# 输出（就地覆盖，或 --out-dir 另存）:
#   同名 .so，已含本仓库验证过的补丁集。
#
# 用法:
#   ./scripts/patch_hal_binaries.sh [--out-dir <目录>] [--qti-only | --audio-only]
#
# 期望原厂 md5（不一致时仍尝试匹配 patch，命中数不足 4 处则中止）:
#   libbluetooth_qti_real.so   d...（未记录）→ 以 patch 命中为准
#   audio.bluetooth.default.so 1b926c8421c91338f2bc3841eccd4f45
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V="$HERE/leaudio_marble_fix_v2/vendor/lib64/hw"
OUTDIR="$V"
DO_QTI=1
DO_AUDIO=1

while [ $# -gt 0 ]; do
  case "$1" in
    --out-dir) OUTDIR="$2"; shift 2 ;;
    --qti-only) DO_AUDIO=0; shift ;;
    --audio-only) DO_QTI=0; shift ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUTDIR"
warn() { echo "[!] $*"; }
ok()   { echo "[✓] $*"; }

# ---------------------------------------------------------------- QTI HAL ---
if [ "$DO_QTI" = "1" ]; then
  SRC="$V/libbluetooth_qti_real.so"
  [ -f "$SRC" ] || { warn "缺少 $SRC（先跑 scripts/dump_device_binaries.sh）"; }
  if [ -f "$SRC" ]; then
    TMP="$OUTDIR/libbluetooth_qti_real.so"
    echo "[*] QTI HAL 补丁（generator: patch_qti_iso_rx.py）"
    if [ "$SRC" = "$TMP" ]; then
      cp "$SRC" "$SRC.orig.bak"
      python3 "$HERE/patch_qti_iso_rx.py" "$SRC.orig.bak" -o "$TMP"
    else
      python3 "$HERE/patch_qti_iso_rx.py" "$SRC" -o "$TMP"
    fi
    ok "QTI HAL -> $TMP"
  fi
fi

# ------------------------------------------------------------- audio HAL ---
if [ "$DO_AUDIO" = "1" ]; then
  SRC="$V/audio.bluetooth.default.so"
  [ -f "$SRC" ] || { warn "缺少 $SRC（先跑 scripts/dump_device_binaries.sh）"; }
  if [ -f "$SRC" ]; then
    echo "[*] audio.bluetooth.default.so 补丁（偏移表见 docs/architecture.md §2.2）"
    if [ "$SRC" = "$OUTDIR/audio.bluetooth.default.so" ]; then
      cp "$SRC" "$SRC.orig.bak"
      SRC="$SRC.orig.bak"
    fi
    OUT="$OUTDIR/audio.bluetooth.default.so"
    cp "$SRC" "$OUT"
    python3 - "$OUT" <<'PY'
import sys, struct

path = sys.argv[1]
EXPECT_MD5_ORIG = "1b96c8421c91338f2bc3841eccd4f45"

# (offset, find_bytes, replace_bytes, 说明)
PATCHES = [
    # BluetoothAudioPortAidl::init_session_type 会话号改写
    (0x13620, bytes.fromhex("08018052"), bytes.fromhex("c8018052"),
     "Session 4 (SOFTWARE_ENCODING) -> 6 (HWBW_ENCODING)"),      # mov w8,#4 -> #6
    (0x135e8, bytes.fromhex("a8018052"), bytes.fromhex("e7018052"),
     "Session 5 (SOFTWARE_DECODING) -> 7 (HWBW_DECODING)"),      # mov w8,#5 -> #7
    (0x135cc, bytes.fromhex("28028052"), bytes.fromhex("49028052"),
     "Session 8 -> 9"),                                          # mov w8,#8 -> #9
    # UpdateSinkMetadata 静音（空函数化）：入口保 paciasp 与 BTI 落点！
    (0x11548, bytes.fromhex("ff4300d1"), bytes.fromhex("df3f03d5"),
     "sub sp,sp,#0x90 -> autiasp（与入口 paciasp 配对）"),
    (0x1154c, bytes.fromhex("fd7bbfa9"), bytes.fromhex("c0035fd6"),
     "stp x29,x30,[sp,#0x40] -> ret（函数体其余部分永达）"),
]

blob = bytearray(open(path, "rb").read())
hit = 0
for off, find, repl, why in PATCHES:
    cur = bytes(blob[off:off + len(find)])
    if cur == repl:
        print(f"  [=] 0x{off:x} 已是目标值（{why}）"); hit += 1
    elif cur == find:
        blob[off:off + len(find)] = repl
        print(f"  [+] 0x{off:x} {why}"); hit += 1
    else:
        print(f"  [!] 0x{off:x} 字节不符（得 {cur.hex()}），可能版本不同，跳过：{why}")

if hit < len(PATCHES):
    print(f"[x] 仅命中 {hit}/{len(PATCHES)} 处——确认这是本机型原厂件后再跑", file=sys.stderr)
    sys.exit(1)
open(path, "wb").write(bytes(blob))
print(f"[✓] audio.bluetooth.default.so 补丁完成: {path}")
PY
    ok "audio HAL -> $OUT"
  fi
fi

cat <<EOF

[=] 补丁完成。部署前核对（设备侧）:
    md5sum $OUTDIR/*.so
  libbluetooth_qti_real.so 期望: ce7fed1ce47a96f4b94505908bd31cc4（4 补丁版）
  audio.bluetooth.default.so 期望: 9769dbee5e5a16d8c2486527f43fc6a1
EOF
