#!/system/bin/sh
MODDIR=${0%/*}
FW_SRC="$MODDIR/firmware/hpbtfw21.tlv"
FW_DST="/vendor/bt_firmware/image/hpbtfw21.tlv"

# Fallback: ensure bind mount is active
if [ -f "$FW_SRC" ] && [ -f "$FW_DST" ]; then
    if ! mount | grep -q "$FW_DST"; then
        chcon u:object_r:bt_firmware_file:s0 "$FW_SRC"
        chmod 644 "$FW_SRC"
        chown 1002:3002 "$FW_SRC"
        mount -o bind "$FW_SRC" "$FW_DST"
    fi
fi
