#!/usr/bin/env bash
# build_zip.sh — 把 module/ 打成 KernelSU 模块安装包 build/<id>-<version>.zip。
# 安装方式：在 KernelSU 管理器里「模块 → 从本地安装」选这个 zip，然后重启。
# （customize.sh 只在这条路径上运行，用来设置 SELinux 标签。）
#
# 用法: ./scripts/build_zip.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
M="$HERE/module"

FILES=(
  module.prop
  system.prop
  customize.sh
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

ID="$(sed -n 's/^id=//p' "$M/module.prop")"
VER="$(sed -n 's/^version=//p' "$M/module.prop")"
OUT="$HERE/build/$ID-$VER.zip"
mkdir -p "$HERE/build"

python3 - "$M" "$OUT" <<'PY'
import os, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
if os.path.exists(out):
    os.remove(out)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            arc = os.path.relpath(path, src)
            info = zipfile.ZipInfo(arc, date_time=(2009, 1, 1, 0, 0, 0))  # 固定时间戳，产物可复现
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name.endswith(".sh") else 0o644) << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read())
PY

echo "[✓] $OUT"
unzip -l "$OUT" | tail -n +4 | head -n -2
