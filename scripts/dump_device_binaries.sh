#!/usr/bin/env bash
# dump_device_binaries.sh — 从用户自己的设备导出原厂 HAL 与 vendor 配置到模块树。
#
# 公开仓库不含任何外来源二进制；本脚本从你自己的设备只读导出。产物全部在 .gitignore 排除区内。
#
# 用法:
#   SERIAL=<adb 序列号> ./scripts/dump_device_binaries.sh [--from-block] [--module-dir <路径>]
#                                                          [--with-build-deps] [--all]
#
# 两种导出方式:
#   默认          从 /vendor 读文件（su -c cp -> /data/local/tmp -> adb pull）。
#                 本模块启用时 /vendor 上是模块的文件，所以此时拒绝运行：先禁用模块并重启。
#   --from-block  只读整个 vendor 块设备（约 2 GB，传到电脑临时目录），用 debugfs 提取文件，
#                 不受模块状态影响。需要电脑上有 debugfs（e2fsprogs），且 vendor 为 ext4。
#
# 默认导出（部署必需，落到 module/ 下，之后由 patch_hal_binaries.sh / patch_vendor_configs.py 修改）:
#   vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so  -> libbluetooth_qti_real.so（原厂 QTI HAL）
#   vendor/lib64/hw/audio.bluetooth.default.so                  原厂音频 HAL
#   vendor/etc/vintf/manifest_ukee.xml                          活动 SKU 的 VINTF manifest
#   vendor/etc/audio/sku_ukee/audio_policy_configuration.xml    活动 SKU 的音频策略
# --with-build-deps: 追加 /system/lib64/{libhidlbase,libutils,libc++}.so 到 build/lib/（编译 shim 用）
# --all: 追加原厂固件/NV、HAL 服务二进制到 dump/（仅取证用）
set -euo pipefail

SERIAL="${SERIAL:?export SERIAL=<adb 序列号>}"
MODDIR=""
FROM_BLOCK=0
WITH_BUILD_DEPS=0
ALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --from-block) FROM_BLOCK=1; shift ;;
    --module-dir) MODDIR="$2"; shift 2 ;;
    --with-build-deps) WITH_BUILD_DEPS=1; shift ;;
    --all) ALL=1; shift ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODDIR="${MODDIR:-$HERE/module}"
ADB="adb -s $SERIAL"
MOD=/data/adb/modules/leaudio_marble_fix

command -v adb >/dev/null || { echo "[x] adb 不在 PATH"; exit 1; }
[ "$($ADB get-state 2>/dev/null || true)" = "device" ] || { echo "[x] 设备 $SERIAL 不在线"; exit 1; }

MODULE_ACTIVE=0
$ADB shell "su -c 'test -d $MOD && test ! -e $MOD/disable'" && MODULE_ACTIVE=1
if [ "$MODULE_ACTIVE" = "1" ] && [ "$FROM_BLOCK" = "0" ]; then
  echo "[x] 本模块处于启用状态，/vendor 上是模块的文件而不是原厂件。"
  echo "    先禁用模块并重启，或改用 --from-block。"
  exit 1
fi

# 设备路径 -> 本地路径（相对仓库根目录；module/ 前缀会替换为 --module-dir）
CORE=(
  "vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so|module/vendor/lib64/hw/libbluetooth_qti_real.so"
  "vendor/lib64/hw/audio.bluetooth.default.so|module/vendor/lib64/hw/audio.bluetooth.default.so"
  "vendor/etc/vintf/manifest_ukee.xml|module/vendor/etc/vintf/manifest_ukee.xml"
  "vendor/etc/audio/sku_ukee/audio_policy_configuration.xml|module/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml"
)
EXTRA=(
  "vendor/bt_firmware/image/hpbtfw21.tlv|dump/hpbtfw21.factory.tlv"
  "vendor/bt_firmware/image/hpnv21.bin|dump/hpnv21.bin"
  "vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti|dump/android.hardware.bluetooth@1.0-service-qti"
)
BUILDDEPS=(
  "system/lib64/libhidlbase.so|build/lib/libhidlbase.so"
  "system/lib64/libutils.so|build/lib/libutils.so"
  "system/lib64/libc++.so|build/lib/libc++.so"
)

# --from-block: 只读导出 vendor 块设备到电脑临时目录
IMG=""
if [ "$FROM_BLOCK" = "1" ]; then
  command -v debugfs >/dev/null || { echo "[x] 电脑上没有 debugfs（安装 e2fsprogs）"; exit 1; }
  slot="$($ADB shell getprop ro.boot.slot_suffix | tr -d '\r')"
  dev="/dev/block/mapper/vendor$slot"
  size="$($ADB shell "su -c 'blockdev --getsize64 $dev'" | tr -d '\r')"
  [ -n "$size" ] || { echo "[x] 找不到 $dev"; exit 1; }
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  IMG="$TMP/vendor.img"
  echo "[*] 只读导出 $dev（$((size / 1048576)) MiB）-> $IMG"
  $ADB exec-out "su -c 'cat $dev'" > "$IMG"
  [ "$(stat -c%s "$IMG")" = "$size" ] || { echo "[x] 镜像大小不符，导出不完整"; exit 1; }
  debugfs -c -R stats "$IMG" >/dev/null 2>&1 || { echo "[x] vendor 不是 ext4，改用默认方式"; exit 1; }
fi

report() {
  echo "[✓] $1 -> $2  ($(stat -c%s "$2")B, md5 $(md5sum "$2" | cut -d' ' -f1))"
}

from_image() {
  local src="$1" dst="$2" out
  out="$(debugfs -c -R "dump /${src#vendor/} $dst" "$IMG" 2>&1)"
  if [ ! -s "$dst" ] || echo "$out" | grep -q "not found"; then
    rm -f "$dst"; echo "[!] 镜像中不存在: $src（跳过）"; return 1
  fi
  report "$src" "$dst"
}

from_adb() {
  local src="/$1" dst="$2"
  local stage="/data/local/tmp/leaudio_dump.tmp"
  $ADB shell "su -c 'test -e $src'" || { echo "[!] 设备上不存在: $src（跳过）"; return 1; }
  $ADB shell "su -c 'cp $src $stage && chmod 644 $stage'"
  $ADB pull "$stage" "$dst" >/dev/null 2>&1
  $ADB shell "su -c 'rm -f $stage'"
  report "$src" "$dst"
}

get() {
  local src="$1" dst="$2"
  case "$dst" in module/*) dst="$MODDIR/${dst#module/}" ;; *) dst="$HERE/$dst" ;; esac
  mkdir -p "$(dirname "$dst")"
  case "$src" in
    vendor/bt_firmware/image/hpbtfw21.tlv)
      # 固件不在 vendor 镜像里；模块启用时它被模块 bind mount 成上游版本
      if [ "$MODULE_ACTIVE" = "1" ]; then echo "[!] 模块启用中，$src 不是原厂件（跳过）"; return 1; fi
      from_adb "$src" "$dst" ;;
    vendor/bt_firmware/*) from_adb "$src" "$dst" ;;
    vendor/*) if [ -n "$IMG" ]; then from_image "$src" "$dst"; else from_adb "$src" "$dst"; fi ;;
    *) from_adb "$src" "$dst" ;;
  esac
}

LIST=("${CORE[@]}")
[ "$WITH_BUILD_DEPS" = "1" ] && LIST+=("${BUILDDEPS[@]}")
[ "$ALL" = "1" ] && LIST+=("${EXTRA[@]}")

echo "[+] 模块目录: $MODDIR"
for pair in "${LIST[@]}"; do
  get "${pair%%|*}" "${pair##*|}" || true
done

cat <<EOF

[=] 导出完成。下一步修改导出的原厂文件：
      ./patch/patch_hal_binaries.sh
      python3 patch/patch_vendor_configs.py
EOF
