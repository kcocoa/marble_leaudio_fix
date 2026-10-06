#!/bin/bash
set -e

DEVICE_SERIAL="${SERIAL:?export SERIAL=<你的设备序列号>}"

echo "=== LE Audio Shim Deployment Tool for marble ==="

if ! command -v adb &>/dev/null; then
    echo "[-] adb command not found!"
    exit 1
fi

echo "[*] Waiting for device $DEVICE_SERIAL..."
adb -s "$DEVICE_SERIAL" wait-for-device

echo "[*] Staging files to /data/local/tmp/leaudio_update..."
adb -s "$DEVICE_SERIAL" shell "rm -rf /data/local/tmp/leaudio_update && mkdir -p /data/local/tmp/leaudio_update"
adb -s "$DEVICE_SERIAL" push ./leaudio_marble_fix_v2/. /data/local/tmp/leaudio_update/

echo "[*] Moving files to /data/adb/modules/leaudio_marble_fix via su..."
adb -s "$DEVICE_SERIAL" shell "su -c 'mkdir -p /data/adb/modules/leaudio_marble_fix && cp -a /data/local/tmp/leaudio_update/. /data/adb/modules/leaudio_marble_fix/ && rm -rf /data/local/tmp/leaudio_update && chown -R root:root /data/adb/modules/leaudio_marble_fix && chmod -R 755 /data/adb/modules/leaudio_marble_fix && chmod 644 /data/adb/modules/leaudio_marble_fix/module.prop /data/adb/modules/leaudio_marble_fix/system.prop /data/adb/modules/leaudio_marble_fix/vendor/lib64/hw/*.so /data/adb/modules/leaudio_marble_fix/vendor/etc/vintf/*.xml /data/adb/modules/leaudio_marble_fix/vendor/etc/audio/*/*.xml && chcon -R u:object_r:vendor_file:s0 /data/adb/modules/leaudio_marble_fix/vendor/ && chcon u:object_r:hal_bluetooth_default_exec:s0 /data/adb/modules/leaudio_marble_fix/vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti'"

echo "[+] Deployment completed successfully!"
