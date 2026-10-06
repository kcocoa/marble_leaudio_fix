#!/usr/bin/env bash
# build_shim.sh — 编译 BluetoothHciHook.cpp 成可替换原厂 SONAME 的 shim .so。
#
# 依赖:
#   - Android NDK 30（clang 21，与设备平台同代；换版本踩 ABI tag 坑，见 docs/architecture.md §5）
#   - build/lib/{libhidlbase,libutils,libc++}.so:
#       SERIAL=<序列号> ./scripts/dump_device_binaries.sh --with-build-deps
#   - build/inc/hidl/ConcurrentMap.h: 取自 AOSP system/libhidl/transport/include/hidl/
#   - module/vendor/lib64/hw/libbluetooth_qti_real.so（已打补丁，见 patch_hal_binaries.sh）
#   build/inc/__config_site 缺失时自动从 NDK 生成。
#
# 用法:
#   ./scripts/build_shim.sh [--out <path>] [--ndk <dir>]
#
# 产物默认直接写入模块: module/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so
# 已安装模块时可热更新: ./deploy_shim_fileonly.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$HERE/build"
HW="$HERE/module/vendor/lib64/hw"
OUT="$HW/android.hardware.bluetooth@1.0-impl-qti.so"
NDK="${NDK:-/opt/android-sdk/ndk/30.0.16248370}"

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --ndk) NDK="$2"; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

T="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -x "$T/clang++" ] || { echo "[x] NDK 工具链不可用: $NDK"; exit 1; }

# __config_site: NDK 默认 ABI 命名空间 __ndk1，设备 libc++ 只有 std::__1（-D 覆盖无效）
if [ ! -f "$B/inc/__config_site" ]; then
  mkdir -p "$B/inc"
  sed 's/_LIBCPP_ABI_NAMESPACE __ndk1/_LIBCPP_ABI_NAMESPACE __1/' \
    "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/c++/v1/__config_site" \
    > "$B/inc/__config_site"
fi

missing=0
for f in "$B/inc/hidl/ConcurrentMap.h" "$B/lib/libhidlbase.so" "$B/lib/libutils.so" \
         "$B/lib/libc++.so" "$HW/libbluetooth_qti_real.so"; do
  [ -e "$f" ] || { echo "[x] 缺 $f" >&2; missing=1; }
done
[ "$missing" = "0" ] || { echo "[x] 见脚本头部的依赖说明" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
echo "[+] 编译 shim -> $OUT"
"$T/clang++" --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 \
  -I"$B/inc" \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  "$HERE/bluetooth_hci_shim/BluetoothHciHook.cpp" -o "$OUT" \
  -L"$B/lib" -L"$HW" -lhidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
"$T/llvm-strip" --strip-all "$OUT"

"$T/llvm-readelf" -d "$OUT" | grep NEEDED || true
echo "[✓] 完成"
