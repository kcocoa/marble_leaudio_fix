#!/usr/bin/env bash
# deploy_module.sh — 【仅供调试】把 module/ 直接推到设备 /data/adb/modules/leaudio_marble_fix。
# 正式安装一律用 scripts/build_zip.sh 打出来的 zip。
# 装完需要重启设备才生效。
#
# 每个文件先复制成 <name>.new 再 mv 覆盖：
#   - 运行中的 HAL 已 mmap 的旧 .so 不会被原地改写（原地 cp 会让它 SIGSEGV）
#   - 不用 adb push 直接写模块目录（label 会变成 adb_data_file，HAL 无法加载）
# 之后在设备上 source module/customize.sh 设置 SELinux label 和属主——KernelSU 只有装 zip 时才会跑它，
# adb 部署要自己跑；这里提供 set_perm / set_perm_recursive / ui_print 的最小实现（同 KernelSU 安装环境）。
#
# 用法: SERIAL=<adb 序列号> ./scripts/deploy_module.sh
set -euo pipefail

SERIAL="${SERIAL:?export SERIAL=<adb 序列号>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
M="$HERE/module"
DST=/data/adb/modules/leaudio_marble_fix
STAGE=/data/local/tmp/leaudio_module
ADB="adb -s $SERIAL"

FILES=(
  module.prop
  system.prop
  customize.sh
  post-mount.sh
  apex/com.android.bt/lib64/libbluetooth_jni.so
  apex/com.android.bt/etc/bluetooth/le_audio/audio_set_scenarios.json
  vendor/bt_firmware/image/hpbtfw21.tlv
  vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so
  vendor/lib64/hw/libbluetooth_qti_real.so
  vendor/lib64/hw/audio.bluetooth.default.so
  vendor/etc/vintf/manifest_ukee.xml
  vendor/etc/audio/sku_ukee/audio_policy_configuration.xml
)

for f in "${FILES[@]}"; do
  [ -f "$M/$f" ] || { echo "[x] 缺 module/$f（构建步骤见 README.md）"; exit 1; }
done
grep -q '<name>IBluetoothHci</name>' "$M/vendor/etc/vintf/manifest_ukee.xml" &&
grep -q 'tagName="BLE Headset In"' "$M/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml" ||
  { echo "[x] vendor 配置还是原厂版本，先跑 python3 patch/patch_vendor_configs.py"; exit 1; }

[ "$($ADB get-state 2>/dev/null)" = "device" ] || { echo "[x] 设备 $SERIAL 不在线"; exit 1; }

# 设备端安装脚本
script="$(mktemp)"
trap 'rm -f "$script"' EXIT
{
  echo "set -e"
  # 上一次安装中断留下的临时文件
  echo "find '$DST' -name '*.new' -delete 2>/dev/null || true"
  for f in "${FILES[@]}"; do
    case "$f" in *.sh) mode=755 ;; *) mode=644 ;; esac
    echo "mkdir -p '$DST/$(dirname "$f")'"
    echo "cp '$STAGE/$f' '$DST/$f.new'"
    echo "chown 0:0 '$DST/$f.new'; chmod $mode '$DST/$f.new'"
    echo "mv -f '$DST/$f.new' '$DST/$f'"
  done
  # KernelSU 安装环境的最小复刻，然后跑 customize.sh
  cat <<'HELPERS'
ui_print() { echo "$1"; }
set_perm() {
  chown "$2:$3" "$1" || return 1
  chmod "$4" "$1" || return 1
  chcon "${5:-u:object_r:system_file:s0}" "$1" || return 1
}
set_perm_recursive() {
  find "$1" -type d | while read -r d; do set_perm "$d" "$2" "$3" "$4" "$6"; done
  find "$1" \( -type f -o -type l \) | while read -r f; do set_perm "$f" "$2" "$3" "$5" "$6"; done
}
HELPERS
  echo "MODPATH='$DST'"
  echo ". '$DST/customize.sh'"
  # 旧版本留下的启动脚本和固件目录（现在固件由元模块挂载）
  echo "rm -rf '$DST/post-fs-data.sh' '$DST/service.sh' '$DST/firmware'"
  echo "rm -rf '$STAGE'"
} > "$script"

echo "[*] 推送到 $STAGE"
$ADB shell "rm -rf $STAGE && mkdir -p $STAGE"
for f in "${FILES[@]}"; do
  $ADB shell "mkdir -p '$STAGE/$(dirname "$f")'"
  $ADB push "$M/$f" "$STAGE/$f" >/dev/null
done
$ADB push "$script" "$STAGE/install.sh" >/dev/null

echo "[*] 安装到 $DST"
$ADB shell "su -c 'sh $STAGE/install.sh'"
$ADB shell "su -c 'ls -lZR $DST'"
echo "[✓] 完成。重启设备后生效。"
