#!/usr/bin/env bash
# fetch_upstream_firmware.sh — 从上游 linux-firmware 取高通官方蓝牙固件。
#
# 为什么需要脚本而不是入库：
#   固件二进制虽属上游开源件，但按仓库卫生一律不跟踪；用户自行下载。
#   本仓库开发验证的唯一版本：BTFW.HSP.2.1.0-00680-VER_PATCHZ-1
#   （163,332 B，md5 9a6b0cb34a82a7015141dd19b2d779da）——芯片 ISO 缓冲 3 → 22。
#
# 用法:
#   ./scripts/fetch_upstream_firmware.sh [--dest <module/firmware 目录>]
#                                        [--expect-md5 <md5>] [--force]
#
# 源（按序回退，任一下载到期望 md5 即停）：
#   1. gitlab.com/kernel-firmware/linux-firmware   -/raw/main/qca/hpbtfw21.tlv
#   2. git.kernel.org cgit                        tree/qca/hpbtfw21.tlv?plain=1
#   3. kernel.googlesource.com                    +/refs/heads/main/qca/hpbtfw21.tlv?format=TEXT
set -euo pipefail

EXPECT_MD5="9a6b0cb34a82a7015141dd19b2d779da"
EXPECT_SIZE=163332
FORCE=0
DEST=""

while [ $# -gt 0 ]; do
  case "$1" in
    --dest) DEST="$2"; shift 2 ;;
    --expect-md5) EXPECT_MD5="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${DEST:-$HERE/module/firmware}"

SOURCES=(
  "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/qca/hpbtfw21.tlv"
  "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/qca/hpbtfw21.tlv?plain=1"
  "https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/qca/hpbtfw21.tlv?format=TEXT"
)

mkdir -p "$DEST"
OUT="$DEST/hpbtfw21.tlv"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

verify() {
  local f="$1" sz md5
  sz=$(stat -c%s "$f" 2>/dev/null || echo 0)
  md5=$(md5sum "$f" | cut -d' ' -f1)
  [ "$sz" = "$EXPECT_SIZE" ] && [ "$md5" = "$EXPECT_MD5" ]
}

if [ -f "$OUT" ] && [ "$FORCE" = "0" ] && verify "$OUT"; then
  echo "[=] 已存在且校验通过: $OUT"
  echo "    md5=$EXPECT_MD5 size=$EXPECT_SIZE"
  exit 0
fi

echo "[+] 目标: $OUT  (期望 md5=$EXPECT_MD5 size=$EXPECT_SIZE)"

for i in "${!SOURCES[@]}"; do
  url="${SOURCES[$i]}"
  echo "[*] 源 $((i+1))/${#SOURCES[@]}: $url"
  if curl -fsSL --max-time 120 -o "$TMP/hpbtfw21.tlv" "$url"; then
    case "$url" in *format=TEXT) base64 -d < "$TMP/hpbtfw21.tlv" > "$TMP/decoded" && mv "$TMP/decoded" "$TMP/hpbtfw21.tlv" ;; esac
    if verify "$TMP/hpbtfw21.tlv"; then
      cp "$TMP/hpbtfw21.tlv" "$OUT"
      chmod 644 "$OUT"
      echo "[✓] 下载并校验通过: $OUT"
      md5sum "$OUT"
      exit 0
    fi
    echo "[!] 校验不符（上游可能已更新版本）"
    if [ "$FORCE" = "1" ]; then
      cp "$TMP/hpbtfw21.tlv" "$OUT"; chmod 644 "$OUT"
      echo "[!] 已按 --force 写入未校验版本，请自行确认兼容性"
      exit 0
    fi
  else
    echo "[!] 下载失败，尝试下一个源"
  fi
done

cat >&2 <<EOF
[x] 所有在线源均失败/不符。

手动方式（任选其一）：
  A. 浏览器打开任一 SOURCES 地址，保存为 hpbtfw21.tlv 后:
       cp ~/Downloads/hpbtfw21.tlv $OUT
  B. 内核官方快照:
       curl -O https://cdn.kernel.org/linux/firmware/linux-firmware-YYYYMMDD.tar.xz
       tar -xf linux-firmware-*.tar.xz qca/hpbtfw21.tlv
       cp qca/hpbtfw21.tlv $OUT
  C. 本机原厂固件（fallback，ISO 缓冲仅 3 个，性能受限）:
       见 scripts/dump_device_binaries.sh
EOF
exit 1
