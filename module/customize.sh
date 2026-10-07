# KernelSU installer: SELinux labels and ownership for the module files.
# The metamodule mounts module/vendor/ as-is, so the labels have to be right here.
# Later calls override earlier ones.

set_perm_recursive $MODPATH/vendor 0 0 0755 0644 u:object_r:vendor_file:s0
set_perm_recursive $MODPATH/vendor/etc 0 0 0755 0644 u:object_r:vendor_configs_file:s0

# /vendor/bt_firmware is a separate vfat mount (bluetooth:net_bt, bt_firmware_file).
set_perm_recursive $MODPATH/vendor/bt_firmware 1002 3002 0550 0644 u:object_r:bt_firmware_file:s0

# apex/ 下的文件由 post-mount.sh 通过 hybrid_mount 的 vfs 规则重定向到 /apex（原件在 APEX 里）。
set_perm_recursive $MODPATH/apex 0 0 0755 0644 u:object_r:system_file:s0
set_perm_recursive $MODPATH/apex/com.android.bt/lib64 0 0 0755 0644 u:object_r:system_lib_file:s0

ui_print "- NOTE: do NOT use overlayfs mode for your metamodule."
ui_print "- The Bluetooth firmware is mounted onto /vendor/bt_firmware (a vfat partition),"
ui_print "- which overlayfs cannot use, so mounting will fail (possibly for all modules)."
ui_print "- Use vfs or magic mount instead."
ui_print "- The /apex part (LC3 odd frame size fix) needs hybrid_mount (vfs)."
ui_print "- With another metamodule that part is skipped; everything else still works."
