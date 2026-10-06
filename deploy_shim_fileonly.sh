#!/bin/bash
# Deploy the shim by FILE REPLACEMENT ONLY (no mount -o bind).
# Rationale: manual bind mounts leave stale inodes stacked on /vendor and have
# previously caused "couldn't map segment: Permission denied" (execmem denial).
# KernelSU magic mount picks up the module dir on reboot; a reboot is required.
set -e
SER=${SER:-$SERIAL}
MOD=/data/adb/modules/leaudio_marble_fix
HW=$MOD/vendor/lib64/hw
SRC=${1:-/tmp/shim_v314.so}

adb -s $SER push "$SRC" /data/local/tmp/shim_new.so
adb -s $SER shell su -c "cp /data/local/tmp/shim_new.so $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chmod 644 $HW/android.hardware.bluetooth@1.0-impl-qti.so"
echo "--- md5 (must match) ---"
adb -s $SER shell su -c "md5sum /data/local/tmp/shim_new.so $HW/android.hardware.bluetooth@1.0-impl-qti.so"
echo "NOTE: reboot required for KernelSU magic mount to expose the new file."
