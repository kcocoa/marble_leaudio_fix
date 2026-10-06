#!/usr/bin env bash
# build_shim.sh — 编译 BluetoothHciHook.cpp 成可替换原厂 SONAME 的 shim .so。
#
# 依赖:
#   - Android NDK 30（clang 21，与设备平台同代；换版本踩 ABI tag 坑，见 docs §5）
#   - 前置基件放在 /tmp/leaudio_build/（inc/ lib/），缺失时给出重建指引。
#     其中 libhidlbase/libutils/libc++ 用 scripts/dump_device_binaries.sh --with-build-deps 拉。
#
# 用法:
#   ./scripts/build_shim.sh [--out <path>] [--ndk <dir>]
#
# 产物默认 /tmp/leaudio_build/shim_v41.so，随后:
#   ./deploy_shim_fileonly.sh /tmp/leaudio_build/shim_v41.so
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="${B:-/tmp/leaudio_build}"
OUT=""
NDK="${NDK:-/opt/android-sdk/ndk/30.0.16248370}"

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --ndk) NDK="$2"; shift 2 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

OUT="${OUT:-$B/shim_v41.so}"
T="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

[ -x "$T/clang++" ] || { echo "[x] NDK 工具链不可用: $NDK"; exit 1; }
[ -f "$HERE/bluetooth_hci_shim/BluetoothHciHook.cpp" ] || { echo "[x] 源码缺失"; exit 1; }

# --- 基件自检（缺失则打印重建表；/tmp 重启会丢） -----------------------------
missing=0
for f in inc/__config_site inc/bidl/ConcurrentMap.h lib/libhidlbase.so lib/libutils.so lib/libc++.so lib/libbluetooth_qti_real.so; do
  [ -e "$B/$f" ] || { echo "[!] 缺 $B/$f"; missing=1; }
done
if [ "$missing" = "1" ]; then
  cat >&2 <<EOF
[x] 构建前置缺失。重建:
  1) inc/__config_site:
       cp $NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/c++/v1/__config_site $B/inc/
       sed -i 's/_LIBCPP_ABI_NAMESPACE __ndk1/__1/' $B/inc/__config_site
  2) inc/bidl/ConcurrentMap.h: 取自 AOSP system/libhidl/transport/include/hidl/
  3) lib/{libhidlbase,libutils,libc++}.so:
       SERIAL=<序列号> ./scripts/dump_device_binaries.sh --with-build-deps
  4) lib/libbluetooth_qti_real.so: 打补丁后的原厂 HAL（见 leaudio_marble_fix_v2/vendor/lib64/hw/）
EOF
  exit 1
fi

mkdir -p "$B"
echo "[+] 编译 shim -> $OUT"
# shellcheck disable=SC2086
"$T/clang++" --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 \
  -I"$B/inc" \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  "$HERE/bluetooth_hci_shim/BluetoothHciHook.cpp" -o "$OUT" \
  -L"$B/lib" -lbidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
"$T/llvm-strip" --strip-all "$OUT"

echo "[=] 自检: NEEDED 与未定义符号应与上一版一致"
"$T/llvm-readelf" -d "$OUT" | grep NEEDED || true
md5sum "$OUT"
echo "[✓] 完成。部署: ./deploy_shim_fileonly.sh $OUT"
