#!/bin/bash
# Deploy the shim by FILE REPLACEMENT ONLY (no mount -o bind).
#
# Two hard-won rules baked in here:
#  1. NEVER use `adb push` into /data/adb/modules/. adb push stamps the file
#     with the `adb_data_file` SELinux label, and KernelSU magic mount carries
#     that label into /vendor verbatim. hal_bluetooth_default is NOT allowed to
#     { map } an adb_data_file, so the HAL dies with:
#       avc: denied { map } ... path=".../android.hardware.bluetooth@1.0-impl-qti.so"
#       scontext=u:r:hal_bluetooth_default:s0 tcontext=u:object_r:adb_data_file:s0
#     and the stack then aborts in hci_backend_hidl.cc:110
#     ("Unable to get a Bluetooth service after 500ms").
#     Use `adb push` to /data/local/tmp (label there is fine) and then
#     `su -c cp` into the module dir so the file inherits vendor_file.
#  2. Do NOT use `restorecon`: /data/adb is adb_data_file by file_contexts, so
#     restorecon would put the wrong label straight back. Use `chcon`.
set -e
SER=${SER:-$SERIAL}
MOD=/data/adb/modules/leaudio_marble_fix
HW=$MOD/vendor/lib64/hw
SRC=${1:-/tmp/shim_v314.so}

adb -s $SER push "$SRC" /data/local/tmp/shim_new.so
adb -s $SER shell su -c "cp /data/local/tmp/shim_new.so $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chmod 644 $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chcon u:object_r:vendor_file:s0 $HW/android.hardware.bluetooth@1.0-impl-qti.so"
echo "--- label + md5 (label MUST be vendor_file, md5 must match) ---"
adb -s $SER shell su -c "ls -Z $HW/ ; md5sum /data/local/tmp/shim_new.so $HW/android.hardware.bluetooth@1.0-impl-qti.so"
echo "NOTE: no reboot needed for the label to take effect (magic mount follows the source),"
echo "      but the HAL must be restarted: stop/start vendor.bluetooth-1-0-qti"
