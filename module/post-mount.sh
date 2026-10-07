#!/system/bin/sh
# post-mount.sh — 把模块 apex/ 下的文件重定向到 /apex/...（依赖 hybrid_mount 的 vfs 规则）。
#
# /apex/com.android.bt 是 LineageOS 签名的 APEX，不能重打包，普通元模块也挂不上去。
# hybrid_mount 的 vfs 是内核里的路径重定向，不依赖进程的挂载命名空间，蓝牙进程重启也不丢。
# 规则只在运行时有效，每次开机重新加；模块禁用或删除后自然消失。
#
# 约定：module/apex/<路径> 对应 /apex/<路径>，放进去的文件都会被重定向。
# 目前有两个：
#   apex/com.android.bt/lib64/libbluetooth_jni.so   补丁版（修奇数帧长丢 1 字节）
#   apex/com.android.bt/etc/bluetooth/le_audio/audio_set_scenarios.json
#                                                    原厂原样，留作调试入口：以后改预设直接改这个文件
#
# 调试：改完模块里的文件后，`sh post-mount.sh` 重新加规则（同一路径会被替换），再重启蓝牙栈。

MODDIR="${0%/*}"
APEX_VER=361099999                       # 这些文件取自该版本的 com.android.bt，APEX 升级后不能再用
HM=/data/adb/metamodule/hybrid-mount

say() { log -t leaudio_marble_fix "$*"; }

if [ ! -d "/apex/com.android.bt@$APEX_VER" ]; then
  say "com.android.bt 不是 $APEX_VER，跳过 /apex 重定向"
  exit 0
fi
if [ ! -x "$HM" ]; then
  say "没有 hybrid-mount，跳过 /apex 重定向（奇数帧长修复不会生效）"
  exit 0
fi

cd "$MODDIR/apex" 2>/dev/null || exit 0
find . -type f | while read -r f; do
  f="${f#./}"
  if "$HM" vfs rule add "/apex/$f" "$MODDIR/apex/$f" >/dev/null 2>&1; then
    say "redirect /apex/$f"
  else
    say "FAILED /apex/$f"
  fi
done
exit 0
