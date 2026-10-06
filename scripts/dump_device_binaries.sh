#!/usr/bin/env bash
# dump_device_binaries.sh — 从用户自己的设备 dump 厂商二进制与 vendor 配置到模块树。
#
# 公开仓库不含任何外来源二进制；落在自己设备上的 /vendor 文件由本脚本拉取。
# 全部操作只读设备；落点全部在 .gitignore 排除区内。
#
# 用法:
#   SERIAL=<adb 序列号> ./scripts/dump_device_binaries.sh [--module-dir <路径>]
#                                                          [--with-build-deps] [--all]
#
#   默认拉取（部署必需）:
#     vendor/lib64/hw/libbluetooth_qti_real.so        原厂高通蓝牙 HAL（补丁基体）
#     vendor/lib64/hw/audio.bluetooth.default.so      音频 HAL（补丁基体）
#     vendor/etc/vintf/manifest_ukee.xml              活动 SKU VINTF（@1.1 声明）
#     vendor/etc/audio/sku_ukee/*                     音频策略基线
#     vendor/etc/audio/sku_taro/*                     备用 SKU 基线
#     vendor/etc/le_audio_codec_capabilities.xml      若原厂已存在则拉取（不存在则用手写件）
#   --with-build-deps: 追加 /system/lib64/{libhidlbase,libutils,libc++}.so（编译 shim 的 ABI 基件）
#   --all: 追加 bt_firmware 原厂固件/NV、apex le_audio json、service-qti 二进制（仅取证用）
#
# 需要设备已解锁、adb 可用、su 可用（KernelSU/Magisk）。SELinux 语境:
#   /vendor 对 shell 不可直读，一律 su -c cp -> /data/local/tmp -> adb pull。
set -euo pipefail

SERIAL="${SERIAL:?export SERIAL=<你的 adb 序列号>}"
MODDIR=""
WITH_BUILD_DEPS=0
ALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --module-dir) MODDIR="$2"; shift 2 ;;
    --with-build-deps) WITH_BUILD_DEPS=1; shift ;;
    --all) ALL=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODDIR="${MODDIR:-$HERE/leaudio_marble_fix_v2}"
ADB="adb -s $SERIAL"

command -v adb >/dev/null || { echo "[x] adb 不在 PATH"; exit 1; }
ST="$($ADB get-state 2>/dev/null || true)"
[ "$ST" = "device" ] || { echo "[x] 设备 $SERIAL 不可用 (state=$ST)，需先连接并解锁"; exit 1; }
echo "[+] 设备 $SERIAL 在线"

# 源设备路径 -> 目标相对路径
CORE=(
  "vendor/lib64/hw/libbluetooth_qti_real.so|vendor/lib64/hw/libbluetooth_qti_real.so"
  "vendor/lib64/hw/audio.bluetooth.default.so|vendor/lib64/hw/audio.bluetooth.default.so"
  "vendor/etc/vintf/manifest_ukee.xml|vendor/etc/vintf/manifest_ukee.xml"
  "vendor/etc/audio/sku_ukee|vendor/etc/audio/sku_ukee"
  "vendor/etc/audio/sku_taro|vendor/etc/audio/sku_taro"
  "vendor/etc/le_audio_codec_capabilities.xml|vendor/etc/le_audio_codec_capabilities.xml"
)
EXTRA=(
  "vendor/bt_firmware/image/hpbtfw21.tlv|firmware_dump/hpbtfw21.factory.tlv"
  "vendor/bt_firmware/image/hpnv21.bin|firmware_dump/hpnv21.bin"
  "vendor/etc/bluetooth/le_audio/audio_set_configurations.json|vendor/etc/bluetooth/le_audio/audio_set_configurations.json"
  "vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti|bluetooth_hci_shim/android.hardware.bluetooth@1.0-service-qti"
  "vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so|bluetooth_hci_shim/android.hardware.bluetooth@1.0-impl-qti.so"
)
BUILDDEPS=(
  "system/lib64/libhidlbase.so|build/deps/libhidlbase.so"
  "system/lib64/libutils.so|build/deps/libutils.so"
  "system/lib64/libc++.so|build/deps/libc++.so"
)

pull() {
  local src="$1" dst="$2"
  local stage="/data/local/tmp/leaudio_dump_$(echo -n "$src" | md5sum | cut -c1-8)"
  if $ADB shell "su -c 'ls $src >/dev/null 2>&1'"; then :; else
    echo "[!] 设备上不存在: $src（跳过）"; return 1
  fi
  $ADB shell "su -c 'cp -a $src $stage && chmod 644 $stage'" >/dev/null
  mkdir -p "$MODDIR/$(dirname "$dst")"
  $ADB pull "$stage" "$MODDIR/$dst" >/dev/null
  $ADB shell "su -c 'rm -f $stage'" >/dev/null 2>&1 || true
  local md5ver
  md5ver=$(md5sum "$MODDIR/$dst" | cut -d' ' -f1)
  echo "[✓] $src -> $dst  ($(stat -c%s "$MODDIR/$dst" 2>/dev/null || echo 0)B, md5 $md5ver)"
}

LIST=("${CORE[@]}")
[ "$WITH_BUILD_DEPS" = "1" ] && LIST+=("${BUILDDEPS[@]}")
[ "$ALL" = "1" ] && LIST+=("${EXTRA[@]}")

echo "[+] 模块目标目录: $MODDIR"
for pair in "${LIST[@]}"; do
  pull "${pair%%|*}" "${pair##*|}" || true
done

cat <<EOF

[=] dump 完成。注意：
  1. 以上产物全部被 .gitignore 排除，不会意外入库。
  2. 两个 HAL 还需打补丁才能用：
       python3 patch_qti_iso_rx.py <clean libbluetooth_qti_real.so> -o <out.so>
       （audio.bluetooth.default.so 的补丁偏移见 docs/architecture.md §2.2，暂无生成器）
  3. 固件请改用上游下载（ISO 缓冲 22 个，原厂仅 3 个）：
       ./scripts/fetch_upstream_firmware.sh
EOF
