#!/usr/bin/env bash
# LE Audio 事件触发式全量快照监测器
#
#   ./scripts/leaudio_monitor.sh [秒数=900]
#
# 原理：
#   - 全量 logcat 流（所有 buffer）→ 文件，同时 tail -F 做事件触发
#   - 高频 pid 轮询（0.5s）兜底检测 HAL/栈重启
#   - 任何「变化时刻」立即抓一份全量 snapshot（完整系统状态，不是 diff）
#
# 产出 monitor_MMDD_HHMMSS/：
#   logcat.txt          全量日志流
#   snapshots/*.txt     事件时刻的全量快照
#   events.txt          事件时间线（触发原因）
#   summary.txt         分析报告
#
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
ADB="adb -s ${SERIAL:?export SERIAL=<你的设备序列号>}"
DUR="${1:-900}"
OUT="$ROOT/monitor_runs/monitor_$(date +%m%d_%H%M%S)"
mkdir -p "$OUT/snapshots"
echo "[+] 输出: $OUT  时长 ${DUR}s"

SNAP_N=0
LAST_SNAP=0

# ---------- 全量快照 ----------
take_snapshot() {
  local reason="$1"
  local force="${2:-0}"
  local now; now=$(date +%s)
  # 节流：最快 2s 一份；但崩溃类事件(force=1)永远不被节流
  [ "$force" = "0" ] && [ $((now - LAST_SNAP)) -lt 2 ] && return
  LAST_SNAP=$now
  SNAP_N=$((SNAP_N + 1))
  local ts; ts=$(date '+%H%M%S')
  local tag; tag=$(echo "$reason" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-44)
  local f="$OUT/snapshots/${ts}_$(printf '%03d' $SNAP_N)_${tag}.txt"
  echo "$(date '+%F %T')  SNAP#$SNAP_N  $reason" >> "$OUT/events.txt"

  {
    echo "########## SNAPSHOT #$SNAP_N   $(date '+%F %T.%3N')   reason: $reason"
    echo "### host: $(date -Is)   device uptime: $($ADB shell cat /proc/uptime 2>/dev/null)"
    echo
    echo "===== [1] 进程 ====="
    $ADB shell 'for p in android.hardware.bluetooth@1.0-service-qti com.android.bluetooth audioserver; do printf "%-52s " "$p"; pidof $p 2>/dev/null || echo "-"; done; echo "--- 进程年龄/线程数 ---"; for p in $(pidof android.hardware.bluetooth@1.0-service-qti com.android.bluetooth 2>/dev/null); do echo "pid=$p age=$(ps -o ETIME= -p $p 2>/dev/null) thr=$(ls /proc/$p/task 2>/dev/null | wc -l)"; done'
    echo
    echo "===== [2] 蓝牙状态 ====="
    $ADB shell "dumpsys bluetooth_manager 2>/dev/null" | grep -aE 'Current state|Available credits|Credits underflow|currentlyActiveGroupId|cig state|ISO|Iso|LeAudio|le_audio|LE_AUDIO|Connection|Codec|codec|interval|latency|rtn|PHY|phy' | head -70
    echo
    echo "===== [3] 音频路由/音轨 ====="
    $ADB shell "dumpsys audio 2>/dev/null" | grep -aE 'state:started|state:paused|- STREAM_MUSIC|Devices:|AUDIO_DEVICE_OUT_BLE|AudioPlaybackConfiguration' | head -40
    echo
    echo "===== [4] 媒体会话 ====="
    $ADB shell "dumpsys media_session 2>/dev/null" | grep -aE 'Media button session|state=PlaybackState|metadata:' | head -12
    echo
    echo "===== [5] 耳机/LE 连接 ====="
    $ADB shell "dumpsys bluetooth_manager 2>/dev/null" | grep -aiE 'LeAudio|LE_AUDIO|BleAudio|device|Address|connected' | head -40
    echo
    echo "===== [6] 最新 tombstone ====="
    local tb; tb=$($ADB shell "su -c 'ls -t /data/tombstones/ 2>/dev/null | grep -v pb | head -1'" | tr -d '\r')
    echo "newest: $tb"
    [ -n "$tb" ] && $ADB shell "su -c 'strings /data/tombstones/$tb 2>/dev/null | head -80'" 2>/dev/null
    echo
    echo "===== [7] logcat 尾部 200 行 ====="
    tail -200 "$OUT/logcat.txt" 2>/dev/null
  } 2>&1 | tr -d '\000' > "$f"
  echo "    -> $(basename "$f")"
}

# ---------- 1) 全量 logcat ----------
$ADB logcat -b all -v threadtime > "$OUT/logcat.txt" 2>&1 &
LOGPID=$!
echo "[+] logcat 流 pid=$LOGPID"
sleep 2

# ---------- 2) logcat 事件触发（零轮询延迟） ----------
tail -F -n 0 "$OUT/logcat.txt" 2>/dev/null | while IFS= read -r line; do
  case "$line" in
    # ---- 崩溃类：强制快照，不节流 ----
    *serviceDied*|*"HAL died"*|*"Abort message"*|*SIGABRT*|*SIGSEGV*|*FATAL*|*"died"*"bluetooth"*|*"bluetooth"*"died"*)
      take_snapshot "CRASH:${line:0:90}" 1 ;;
    *"Hardware Error"*|*hardware_error*|*"Credits underflow"*|*underrun*)
      take_snapshot "CRASH:${line:0:90}" 1 ;;
    *"BluetoothHciHook v"*"init"*)
      take_snapshot "HAL-INIT:${line:0:90}" 1 ;;
    # ---- 状态变化类：节流 ----
    *"Current state:"*|*"cig state"*|*CIG_CREATE*|*CIG_TERMINATE*|*"ISO interval"*)
      take_snapshot "STATE:${line:0:90}" ;;
    *twelve*|*Twelve*|*PlaybackState*)
      take_snapshot "PLAYER:${line:0:90}" ;;
    *"Starting Bluetooth"*|*"Bluetooth process"*|*"bt_stack"*)
      take_snapshot "BT-START:${line:0:90}" ;;
  esac
done &
TRIGPID=$!

# ---------- 3) 高频 pid 轮询兜底 ----------
PREV=""
END=$((SECONDS + DUR))
while [ $SECONDS -lt $END ]; do
  cur=$($ADB shell 'printf "%s|%s|%s" "$(pidof android.hardware.bluetooth@1.0-service-qti)" "$(pidof com.android.bluetooth)" "$(pidof audioserver)"' 2>/dev/null | tr -d '\r')
  if [ "$cur" != "$PREV" ] && [ -n "$PREV" ]; then
    take_snapshot "PID-CHANGE: $PREV -> $cur" 1
  fi
  PREV="$cur"
  sleep 0.5
done

kill $TRIGPID $LOGPID 2>/dev/null
wait 2>/dev/null
$ADB shell "su -c 'ls -t /data/tombstones/ 2>/dev/null | grep -v pb | head -12'" > "$OUT/tombstones_recent.txt" 2>/dev/null

echo "[+] 采集完成：$SNAP_N 份快照"
python3 "$HERE/leaudio_monitor_analyze.py" "$OUT"
