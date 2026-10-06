#!/usr/bin/env bash
# leaudio_iterate.sh — LE Audio shim 热更新迭代脚本
#
# 用法:
#   ./leaudio_iterate.sh deploy <shim.so>     # 部署 .so + 重启 HAL + 健康监测(崩溃自动回退 LKG)
#   ./leaudio_iterate.sh restart               # 只重启 HAL 服务 + 健康监测
#   ./leaudio_iterate.sh rollback              # 回退到上次部署前的备份 (LKG)
#   ./leaudio_iterate.sh watch [秒]            # 实时观测 BluetoothHciHook 日志 + 到期摘要
#   ./leaudio_iterate.sh musictest [mults] [秒/档] # 自动播放+mult 扫描+丢帧/速率对比
#   ./leaudio_iterate.sh status                # 只读状态总览
#
# 环境要求: 设备 $SERIAL 在线且已解锁; 模块 leaudio_marble_fix enabled (自动检查)
set -u

SERIAL="${SERIAL:?export SERIAL=<你的设备序列号>}"
ADB="adb -s $SERIAL"
MODULE_ID="leaudio_marble_fix"
MODULE_SO="/data/adb/modules/$MODULE_ID/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so"
LKG_DEV="/data/local/tmp/leaudio_lkg.so"       # 设备侧: 部署前的备份 (rollback 源)
SVC="android.hardware.bluetooth@1.0-service-qti"
LOGTAG="BluetoothHciHook"

# ---------------- helpers ----------------
c_red=$'\033[31m'; c_grn=$'\033[32m'; c_ylw=$'\033[33m'; c_off=$'\033[0m'
info(){ echo "${c_grn}[+]${c_off} $*"; }
warn(){ echo "${c_ylw}[!]${c_off} $*"; }
fail(){ echo "${c_red}[-]${c_off} $*" >&2; }

check_device() {
    local st
    st=$($ADB get-state 2>/dev/null) || { fail "设备 $SERIAL 离线"; exit 1; }
    [ "$st" = "device" ] || { fail "设备状态异常: $st (需先解锁)"; exit 1; }
}

check_module() {
    local enabled
    enabled=$($ADB shell "su -c '[ -d /data/adb/modules/$MODULE_ID ] && ls /data/adb/modules/$MODULE_ID/module.prop'" 2>/dev/null | tr -d '\r')
    [ -n "$enabled" ] || { fail "模块 $MODULE_ID 不存在"; exit 1; }
    if $ADB shell "su -c 'test -e /data/adb/modules/$MODULE_ID/disable'" 2>/dev/null; then
        warn "模块 $MODULE_ID 当前 disabled!"
    fi
}

md5_dev() { $ADB shell "su -c 'md5sum $1 2>/dev/null'" | awk '{print $1}' | tr -d '\r'; }
md5_host(){ md5sum "$1" 2>/dev/null | awk '{print $1}'; }
pid_svc() { $ADB shell "pidof $SVC" 2>/dev/null | tr -d '\r'; }

# ---- 持续采样 (非 one-shot): 每次调用即时探测 ----
is_streaming(){ $ADB shell "dumpsys bluetooth_manager" 2>/dev/null | grep -a 'Current state' | head -1 | grep -q STREAMING; }
# 路由探测: ble=输出线程含 AUDIO_DEVICE_OUT_BLE_HEADSET, spk=仅扬声器, none=无
route_state(){
    # 判据 (实测确认): dumpsys audio 的 STREAM_MUSIC 段 "Devices:" 行显示策略当前输出设备
    #   ble_headset = 路由到 LE Audio 耳机; speaker = 外放
    # 无活跃播放时返回 idle (避免把空闲配置当"正在用耳机")
    local started devs
    started=$(active_players)
    [ "${started:-0}" -eq 0 ] 2>/dev/null && { echo idle; return; }
    devs=$($ADB shell "dumpsys audio" 2>/dev/null | grep -a -A9 -- '- STREAM_MUSIC' | grep -a 'Devices:' | head -1)
    if echo "$devs" | grep -aq 'ble_headset'; then echo ble; return; fi
    if echo "$devs" | grep -aq 'speaker'; then echo spk; return; fi
    echo none
}
# 活跃播放器数 (state:started)
active_players(){ $ADB shell "dumpsys audio" 2>/dev/null | grep -a "AudioPlaybackConfiguration" | grep -ac "state:started"; }
get_credits(){ $ADB shell "dumpsys bluetooth_manager" 2>/dev/null | grep -a 'Available credits' | head -1 | grep -o '[0-9]*'; }
# 恢复播放: 重启蓝牙后音乐会暂停; 每 3s 重试 dispatch play 直到 STREAMING
resume_play() {
    local tries="${1:-10}" i
    for i in $(seq 1 $tries); do
        if is_streaming; then return 0; fi
        $ADB shell "input keyevent 126" >/dev/null 2>&1
        sleep 3
    done
    is_streaming
}

# ---------------- 部署 ----------------
do_deploy() {
    local so="$1"
    [ -f "$so" ] || { fail "文件不存在: $so"; exit 1; }
    check_device; check_module

    local h1 h2 h3
    h1=$(md5_host "$so")
    info "目标: $so (md5 $h1)"

    # 1. 备份当前部署版到设备侧 LKG
    if $ADB shell "su -c 'cat $MODULE_SO > $LKG_DEV && md5sum $LKG_DEV'" >/dev/null 2>&1; then
        info "已备份当前部署版 → $LKG_DEV ($(md5_dev $LKG_DEV | cut -c1-8)…)"
    else
        warn "备份失败, 将无法自动回退!"
    fi

    # 2. push + cat 热更新 (magic-mount 即时生效)
    $ADB push "$so" /data/local/tmp/leaudio_new.so >/dev/null || { fail "push 失败"; exit 1; }
    $ADB shell "su -c 'cat /data/local/tmp/leaudio_new.so > $MODULE_SO'" || { fail "写入模块失败"; exit 1; }
    h2=$(md5_dev "$MODULE_SO"); h3=$(md5_dev "/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so")
    if [ "$h1" != "$h2" ] || [ "$h1" != "$h3" ]; then
        fail "md5 校验失败 host=$h1 module=$h2 vendor=$h3"; exit 1
    fi
    info "部署完成 (模块 + /vendor 视图 md5 一致)"

    # 3. 清日志 + 重启 HAL
    $ADB shell "logcat -c"
    $ADB shell "su -c 'setprop ctl.restart vendor.bluetooth-1-0-qti'"
    info "HAL 服务已重启, 健康监测中…"

    health_check || { do_rollback_now; exit 2; }

    # 4. 关键日志
    echo "---- 关键日志 ----"
    $ADB shell "logcat -d -s $LOGTAG" | grep -E "init \(|transact hook installed|dh=|stock initialize|onTransact code=6|SECOND INITIALIZE|hciEventReceived hooks" | head -10
    info "部署成功 ✅"
}

# 健康监测: 重启后 ~15s 内 pid 必须出现且稳定; 崩溃循环 = pid 变化或消失
health_check() {
    local first="" cur i
    sleep 4
    for i in $(seq 1 8); do
        cur=$(pid_svc)
        if [ -z "$first" ] && [ -n "$cur" ]; then first="$cur"; fi
        if [ -n "$first" ] && [ -n "$cur" ] && [ "$cur" != "$first" ]; then
            fail "检测到崩溃循环: pid $first → $cur"
            return 1
        fi
        if [ -n "$first" ] && [ -z "$cur" ]; then
            fail "服务 pid $first 已消失"
            return 1
        fi
        sleep 1.5
    done
    if [ -z "$first" ]; then fail "服务未在 16s 内启动"; return 1; fi
    info "服务稳定: pid=$first"
    local crashed
    crashed=$($ADB shell "logcat -d -b crash 2>/dev/null | grep -ac 'Fatal signal.*bluetooth@1.0-s'" | tr -d '\r')
    [ "${crashed:-0}" = "0" ] || warn "crash buffer 中有 $crashed 条 HAL fatal 记录 (可能是本轮的)"
    return 0
}

do_rollback_now() {
    warn "自动回退 LKG …"
    $ADB shell "su -c 'cat $LKG_DEV > $MODULE_SO'" || { fail "回退写入失败 — 需手动处理!"; exit 3; }
    $ADB shell "su -c 'setprop ctl.restart vendor.bluetooth-1-0-qti'"
    sleep 6
    local p
    p=$(pid_svc)
    if [ -n "$p" ]; then
        info "已回退并恢复 (pid=$p, md5 $(md5_dev "$MODULE_SO" | cut -c1-8)…)"
    else
        fail "回退后服务仍未起来 — 手动检查!"
        exit 3
    fi
}

do_rollback() {
    check_device; check_module
    do_rollback_now
    $ADB shell "logcat -c"; $ADB shell "su -c 'setprop ctl.restart vendor.bluetooth-1-0-qti'"
    health_check || { fail "回退后仍不稳定"; exit 2; }
    info "回退完成 ✅ (当前部署: $(md5_dev "$MODULE_SO" | cut -c1-8)…)"
}

# ---------------- 重启 ----------------
do_restart() {
    check_device; check_module
    $ADB shell "logcat -c"
    $ADB shell "su -c 'setprop ctl.restart vendor.bluetooth-1-0-qti'"
    info "HAL 服务已重启, 健康监测中…"
    health_check || { warn "服务不稳定 (当前部署版本本身有问题? 可用 rollback 回退)"; exit 2; }
    $ADB shell "logcat -d -s $LOGTAG" | grep -E "init \(|onTransact code=6|initialize_1_1|transact hook installed" | head -6
}

# ---------------- 实时观测 (连续采样) ----------------
do_watch() {
    check_device
    local dur="${1:-60}" tmp interval=5
    tmp=$(mktemp /tmp/leaudio_watch.XXXXXX)
    local samples=0 s_ok=0 r_ble=0 r_spk=0 r_none=0 r_idle=0 players=0 cred_sum=0 cred_n=0
    info "连续观测 ${dur}s (每 ${interval}s 采样: 流/路由/credits + 日志流)"
    # background log stream
    timeout --signal=INT --kill-after=3 "$dur" \
        $ADB shell "logcat -s $LOGTAG" 2>/dev/null | \
        grep --line-buffered -aE "sendIsoData|NCP mult x|CC1407|evt parcel|initialize_1_1|transact hook installed|SECOND INITIALIZE|dropping ISO" \
        | tee "$tmp" &
    local logpid=$!
    local end=$((SECONDS + dur))
    while [ $SECONDS -lt $end ]; do
        sleep $interval
        samples=$((samples+1))
        local st rt cr pl
        st=none; is_streaming && st=ok
        [ "$st" = ok ] && s_ok=$((s_ok+1))
        local rt; rt=$(route_state)
        case $rt in
            ble)  r_ble=$((r_ble+1));;
            spk)  r_spk=$((r_spk+1));;
            none) r_none=$((r_none+1));;
            idle) r_idle=$((r_idle+1));;
        esac
        pl=$(active_players); players=$((players+pl))
        cr=$(get_credits); [ -n "$cr" ] && { cred_sum=$((cred_sum+cr)); cred_n=$((cred_n+1)); }
        printf "\r  [%3ds/%3ds] 流:%s 路由:%-4s 播放器:%-2s credits:%s" \
            $SECONDS "$dur" "$st" "$rt" "$pl" "${cr:-?}"
    done
    echo
    kill $logpid 2>/dev/null; wait $logpid 2>/dev/null
    echo "======== ${dur}s 摘要 (采样 ${samples} 次, 每 ${interval}s) ========"
    echo "流在线率:   ${s_ok}/${samples} = $(( samples>0 ? s_ok*100/samples : 0 ))%"
    echo "路由→耳机:  ${r_ble}/${samples} = $(( samples>0 ? r_ble*100/samples : 0 ))%"
    echo "路由→扬声器: ${r_spk}/${samples}  无路由: ${r_none}/${samples}  空闲: ${r_idle}/${samples}"
    echo "平均活跃播放器: $(( samples>0 ? players/samples : 0 ))"
    [ $cred_n -gt 0 ] && echo "credits 均值: $((cred_sum/cred_n))" || echo "credits 均值: -"
    c_iso=$(grep -ac 'sendIsoData: forwarded' "$tmp" 2>/dev/null); c_iso=${c_iso:-0}
    c_ncp=$(grep -ac 'CC1407' "$tmp" 2>/dev/null); c_ncp=${c_ncp:-0}
    c_drop=$(grep -ac 'dropping ISO' "$tmp" 2>/dev/null); c_drop=${c_drop:-0}
    c_2nd=$(grep -ac 'SECOND INITIALIZE' "$tmp" 2>/dev/null); c_2nd=${c_2nd:-0}
    echo "日志统计: ISO转发采样 ${c_iso} 条(×64) | 合成NCP采样 ${c_ncp} 条(×256) | 丢帧 ${c_drop} 条 | 二次初始化 ${c_2nd}"
    firstN=$(grep -a 'sendIsoData: forwarded' "$tmp" 2>/dev/null | head -1 | grep -o 'pkt #[0-9]*' | grep -o '[0-9]*')
    lastN=$(grep -a 'sendIsoData: forwarded' "$tmp" 2>/dev/null | tail -1 | grep -o 'pkt #[0-9]*' | grep -o '[0-9]*')
    firstT=$(grep -a 'sendIsoData: forwarded' "$tmp" 2>/dev/null | head -1 | awk '{print $1, $2}')
    lastT=$(grep -a 'sendIsoData: forwarded' "$tmp" 2>/dev/null | tail -1 | awk '{print $1, $2}')
    if [ -n "$firstN" ] && [ -n "$lastN" ] && [ "$firstN" != "$lastN" ]; then
        t0=$(parse_ts $firstT); t1=$(parse_ts $lastT)
        if [ -n "$t0" ] && [ -n "$t1" ]; then
            rate=$(awk -v a=$lastN -v b=$firstN -v x=$t1 -v y=$t0 'BEGIN{printf "%.1f", (a-b)/(x-y)}')
            echo "ISO 实际速率: ${rate} pkt/s (需求 ~100/s = 10ms 帧×2 handle)"
        fi
    fi
    echo "---- LE Audio 状态 ----"
    $ADB shell "dumpsys bluetooth_manager" 2>/dev/null | grep -aE "Current state|cig state|data_path_state|Current Codec" | head -4
    $ADB shell "dumpsys bluetooth_manager" 2>/dev/null | grep -a -A3 "ISO Manager" | head -4
    rm -f "$tmp"
}

# ---------------- 状态 ----------------
do_status() {
    check_device
    echo "==== 设备 & 服务 ===="
    echo "蓝牙: $($ADB shell 'dumpsys bluetooth_manager' 2>/dev/null | grep -m1 'state:')"
    echo "HAL pid: $(pid_svc)"
    echo "部署 md5: $(md5_dev "$MODULE_SO")"
    echo "LKG md5:  $(md5_dev "$LKG_DEV" 2>/dev/null || echo '(无备份)')"
    echo
    echo "==== 白名单模块状态 ===="
    $ADB shell 'su -c "
for m in zygisk_vector hma_oss_zygisk zygisksu leaudio_marble_fix; do
  if [ -e /data/adb/modules/\$m/disable ]; then s=disabled; else s=enabled; fi
  v=\$(grep \"^version=\" /data/adb/modules/\$m/module.prop 2>/dev/null | head -1 | cut -d= -f2)
  printf \"%-20s %-12s %s\\n\" \"\$m\" \"\$s\" \"\$v\"
done"'
    echo
    echo "==== 最近关键日志 ===="
    $ADB shell "logcat -d -s $LOGTAG" | grep -E "init \(|transact hook installed|dh=|NCP mult|evt parcel|sendIsoData|SECOND" | tail -8
}

# ---------------- 自动音乐测试 (mult 扫描 + 连续采样) ----------------
# 用法: musictest [mult列表] [每档秒数]
# 每档连续采样(每5s): 流在线/路由(耳机vs扬声器)/credits; 结束统计丢帧、速率、合成NCP
do_musictest() {
    check_device
    local mults="${1:-1}"
    local secs="${2:-30}"
    info "自动音乐测试: mult ∈ {$mults}, 每档 ${secs}s (连续采样每 5s)"

    # 尝试自动恢复/开始播放 (重启蓝牙后音乐暂停: 每 3s 重试 dispatch play 直到恢复)
    if ! resume_play 10; then
        warn "自动播放未生效 —— 请手动开始播放音乐（路由到 LE Audio 耳机），流起来后按回车继续…"
        read -r
    fi
    if ! is_streaming; then fail "未进入 STREAMING 状态, 终止"; exit 1; fi
    info "STREAMING 确认, 开始扫描…"

    local m
    printf "%-6s %-7s %-7s %-8s %-8s %-10s %s\n" "mult" "流在线%" "路由耳%" "credits" "丢帧" "iso pkt/s" "备注"
    for m in ${mults//,/ }; do
        $ADB shell "su -c 'setprop persist.vendor.leaudio.isocred.mult $m'" >/dev/null
        sleep 2
        if ! is_streaming; then
            info "mult=$m: 流已断, 恢复播放 (3s 间隔重试)…"
            resume_play 6 || { warn "mult=$m: 无法恢复流, 跳过"; continue; }
        fi
        $ADB shell "logcat -c" 2>/dev/null
        # 连续采样
        local samples=0 s_ok=0 r_ble=0 r_spk=0 cred_sum=0 cred_n=0
        local end=$((SECONDS + secs))
        while [ $SECONDS -lt $end ]; do
            sleep 5
            samples=$((samples+1))
            is_streaming && s_ok=$((s_ok+1))
            rt=$(route_state)
            [ "$rt" = ble ] && r_ble=$((r_ble+1))
            [ "$rt" = spk ] && r_spk=$((r_spk+1))
            local cr; cr=$(get_credits); [ -n "$cr" ] && { cred_sum=$((cred_sum+cr)); cred_n=$((cred_n+1)); }
        done
        # 统计
        local log drops rate firstN lastN firstT lastT t0 t1 ncp_cnt
        log=$($ADB shell "logcat -d" 2>/dev/null)
        drops=$(echo "$log" | grep -ac 'dropping ISO')
        ncp_cnt=$(echo "$log" | grep -ac 'CC1407→NCP')
        rate="-"
        firstN=$(echo "$log" | grep -a 'sendIsoData: forwarded' | head -1 | grep -o 'pkt #[0-9]*' | grep -o '[0-9]*')
        lastN=$(echo "$log" | grep -a 'sendIsoData: forwarded' | tail -1 | grep -o 'pkt #[0-9]*' | grep -o '[0-9]*')
        firstT=$(echo "$log" | grep -a 'sendIsoData: forwarded' | head -1 | awk '{print $1, $2}')
        lastT=$(echo "$log" | grep -a 'sendIsoData: forwarded' | tail -1 | awk '{print $1, $2}')
        if [ -n "$firstN" ] && [ -n "$lastN" ] && [ "$firstN" != "$lastN" ]; then
            t0=$(parse_ts $firstT); t1=$(parse_ts $lastT)
            [ -n "$t0" ] && [ -n "$t1" ] && rate=$(awk -v a=$lastN -v b=$firstN -v x=$t1 -v y=$t0 'BEGIN{printf "%.1f", (a-b)/(x-y)}')
        fi
        local up="-" rb="-" ca="-" note=""
        if [ $samples -gt 0 ]; then
            up=$((s_ok*100/samples))
            rb=$((r_ble*100/samples))
            [ $((samples-s_ok)) -gt 0 ] && note="流断$((samples-s_ok))次采样"
        fi
        [ $cred_n -gt 0 ] && ca=$((cred_sum/cred_n))
        [ "$r_spk" -gt 0 ] && note="$note 有${r_spk}次采样路由到扬声器!"
        printf "%-6s %-7s %-7s %-8s %-8s %-10s %s\n" "$m" "${up}%" "${rb}%" "$ca" "$drops" "${rate}" "$note"
    done
    $ADB shell "su -c 'setprop persist.vendor.leaudio.isocred.mult 1'" >/dev/null
    info "已恢复 mult=1 (合成 NCP 路径不使用 mult)"
}

case "${1:-}" in
    deploy)  do_deploy "${2:?用法: deploy <shim.so>}";;
    restart) do_restart;;
    rollback) do_rollback;;
    watch)   do_watch "${2:-60}";;
    musictest) do_musictest "${2:-1,6,12,24}" "${3:-30}";;
    status)  do_status;;
    *) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1;;
esac
