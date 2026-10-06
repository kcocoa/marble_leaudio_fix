#!/usr/bin/env bash
# get_leaudio_config.sh — 查看当前连接的 LE Audio 耳机音频配置、码率与流状态
set -euo pipefail

SERIAL="${SERIAL:?export SERIAL=<adb 序列号>}"

adb -s "$SERIAL" shell 'dumpsys bluetooth_manager' | awk '
/LeAudio Manager:/ { found=1 }
found && /Current scenario:/ { scenario=$0; sub(/.*Current scenario: /, "", scenario) }
found && /Active config:/ { if (!active) { active=$0; sub(/.*Active config: /, "", active); gsub(/"/, "", active) } }
found && /SW Encoding codec config:/ { sw=1; next }
sw && /sample rate:/ {
    sr=$0; sub(/.*sample rate: /, "", sr); chan=sr; itv=sr;
    sub(/, .*/, "", sr);
    sub(/.*chan: /, "", chan); sub(/, .*/, "", chan);
    sub(/.*data_interval_us: /, "", itv); sub(/,.*/, "", itv);
    sw=0
}
/^[ \t]*== Active Groups:/ { in_active=1 }
/^[ \t]*== Inactive Groups:/ { in_active=0 }
found && in_active && /Num of devices:/ { dev=$0; sub(/.*Num of devices:[ \t]*/, "", dev) }
found && /id \| active/ { show_ase=1; next }
show_ase && /^[ \t]*[0-9]+/ {
    if ($2 == "true") {
        ase_info = ase_info "\n    - ASE " $1 " [" $3 "]: cis=" $4 " handle=" $6 " sdu=" $7 "B latency=" $8 "ms rtn=" $9 " (" $10 ")"
        last_sdu = $7
        n_stream++
    }
}
found && /CIS Connection handle:/ {
    h=$0; sub(/.*CIS Connection handle:[ \t]*/, "", h);
    getline; getline;
    uc=$0; sub(/.*Used Credits:[ \t]*/, "", uc);
    credits = credits " [hdl " h ": used_cr=" uc "]"
}
found && /----- ::le_audio/ { exit }
END {
    print "====== LE Audio 当前配置与状态 ======"
    print "场景 (Scenario):   " scenario
    print "当前配置 (Config): " active
    print "连接设备 (Devices):" dev
    print "软件编码器 (Host): " sr " Hz | " chan " 声道 | 帧长 " (itv/1000) " ms"
    if (last_sdu > 0 && itv > 0) {
        br = (last_sdu * 8) / (itv / 1000)
        print "单帧载荷 (SDU):    " last_sdu " 字节"
        if (n_stream > 0)
            print "音频码率 (Bitrate):" br " kbps/耳（" n_stream " 路，合计 " (br * n_stream) " kbps）"
        else
            print "音频码率 (Bitrate):" br " kbps/耳"
    }
    if (ase_info != "") {
        print "活跃流通道 (ASEs): " ase_info
    }
    if (credits != "") {
        print "Credit 状态:       " credits
    }
    print "====================================="
}'
