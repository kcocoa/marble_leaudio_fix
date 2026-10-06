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
#   默认拉取（部署必需，落到 module/ 下，之后由 patch_hal_binaries.sh /
#   patch_vendor_configs.py 修改）:
#     vendor/lib64/hw/libbluetooth_qti_real.so                    原厂高通蓝牙 HAL
#     vendor/lib64/hw/audio.bluetooth.default.so                  原厂音频 HAL
#     vendor/etc/vintf/manifest_ukee.xml                          活动 SKU 的 VINTF manifest
#     vendor/etc/audio/sku_ukee/audio_policy_configuration.xml    活动 SKU 的音频策略
#   --with-build-deps: 追加 /system/lib64/{libhidlbase,libutils,libc++}.so 到 build/lib/（编译 shim 用）
#   --all: 追加原厂固件/NV、vendor le_audio json、HAL 服务与原厂 impl 到 dump/（仅取证用）
#
# 本模块启用时 /vendor 显示的是模块里的文件，导出的不是原厂件；此时脚本拒绝运行，
# 请先在管理器中禁用本模块并重启。
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
MODDIR="${MODDIR:-$HERE/module}"
ADB="adb -s $SERIAL"

command -v adb >/dev/null || { echo "[x] adb 不在 PATH"; exit 1; }
ST="$($ADB get-state 2>/dev/null || true)"
[ "$ST" = "device" ] || { echo "[x] 设备 $SERIAL 不可用 (state=$ST)，需先连接并解锁"; exit 1; }
echo "[+] 设备 $SERIAL 在线"

if $ADB shell "su -c 'test -d /data/adb/modules/leaudio_marble_fix && test ! -e /data/adb/modules/leaudio_marble_fix/disable'"; then
  echo "[x] 本模块处于启用状态，/vendor 上是模块的文件而不是原厂件。请先禁用模块并重启。"
  exit 1
fi

# 源设备路径 -> 本地路径（相对仓库根目录；module/ 前缀会替换为 --module-dir）
CORE=(
  "vendor/lib64/hw/libbluetooth_qti_real.so|module/vendor/lib64/hw/libbluetooth_qti_real.so"
  "vendor/lib64/hw/audio.bluetooth.default.so|module/vendor/lib64/hw/audio.bluetooth.default.so"
  "vendor/etc/vintf/manifest_ukee.xml|module/vendor/etc/vintf/manifest_ukee.xml"
  "vendor/etc/audio/sku_ukee/audio_policy_configuration.xml|module/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml"
)
EXTRA=(
  "vendor/bt_firmware/image/hpbtfw21.tlv|dump/hpbtfw21.factory.tlv"
  "vendor/bt_firmware/image/hpnv21.bin|dump/hpnv21.bin"
  "vendor/etc/bluetooth/le_audio/audio_set_configurations.json|dump/audio_set_configurations.json"
  "vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti|dump/android.hardware.bluetooth@1.0-service-qti"
  "vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so|dump/android.hardware.bluetooth@1.0-impl-qti.so"
)
BUILDDEPS=(
  "system/lib64/libhidlbase.so|build/lib/libhidlbase.so"
  "system/lib64/libutils.so|build/lib/libutils.so"
  "system/lib64/libc++.so|build/lib/libc++.so"
)

pull() {
  local src="$1" dst="$2"
  case "$dst" in module/*) dst="$MODDIR/${dst#module/}" ;; *) dst="$HERE/$dst" ;; esac
  local stage="/data/local/tmp/leaudio_dump_$(echo -n "$src" | md5sum | cut -c1-8)"
  if $ADB shell "su -c 'ls $src >/dev/null 2>&1'"; then :; else
    echo "[!] 设备上不存在: $src（跳过）"; return 1
  fi
  $ADB shell "su -c 'cp -a $src $stage && chmod 644 $stage'" >/dev/null
  mkdir -p "$(dirname "$dst")"
  $ADB pull "$stage" "$dst" >/dev/null
  $ADB shell "su -c 'rm -f $stage'" >/dev/null 2>&1 || true
  echo "[✓] $src -> $dst  ($(stat -c%s "$dst")B, md5 $(md5sum "$dst" | cut -d' ' -f1))"
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
  2. 下一步修改导出的原厂文件：
       ./scripts/patch_hal_binaries.sh
       python3 scripts/patch_vendor_configs.py
  3. 固件用上游版本（ISO 缓冲 22 个，原厂仅 3 个）：
       ./scripts/fetch_upstream_firmware.sh
EOF
