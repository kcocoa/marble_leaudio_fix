# 操作流程与事故复盘

**每次操作设备前读这一章。**

---

## 0. 构建与安装

完整步骤见根目录 `README.md`。各脚本的产物均被 `.gitignore` 排除，细节见脚本头注释与
`docs/architecture.md` §2.2/§3。

- 正式安装用 `scripts/build_zip.sh` 打的 zip（KernelSU 管理器 →「模块」→「从本地安装」），**之后必须重启**。
- `scripts/deploy_module.sh` 只是调试时的快捷方式：直接推文件到 `/data/adb/modules/`，KernelSU 不会替它跑 `customize.sh`，所以脚本自己在设备上 source 一遍，标签和 zip 安装一致。
- 只改了 shim 时，用下面第 1 节的热更新，不用重启。
- `dump_device_binaries.sh` 默认从 `/vendor` 读，本模块启用时拒绝运行（此时 `/vendor` 上是模块的文件）。
  这时用 `--from-block`：只读导出整个 vendor 块设备（约 2 GB）到电脑，用 `debugfs` 提取原厂文件。

### adb 序列号

所有脚本通过 `SERIAL` 环境变量指定设备。手机开启「开发者选项 → USB 调试」并连上电脑，
在手机上允许本机调试后：

```bash
adb devices
# List of devices attached
# 1a2b3c4d	device        ← 第一列就是序列号；unauthorized 表示手机上还没允许调试

export SERIAL=1a2b3c4d
```

---

## 1. 热更新 shim

```bash
./scripts/build_shim.sh                          # 产物直接写入 module/vendor/lib64/hw/
SERIAL=<adb 序列号> ./scripts/deploy_shim_fileonly.sh
```

脚本做的事（**顺序不能变**）：

```bash
adb -s $SER push "$SRC" /data/local/tmp/shim_new.so          # 1. push 到 tmp（label 无关）
adb -s $SER shell su -c "stop vendor.bluetooth-1-0-qti"        # 2. 先停 HAL：运行中的 HAL 已 mmap
                                                               #    该 .so，原地覆盖会 SIGSEGV（dead-ends 7.5）
adb -s $SER shell su -c "cp /data/local/tmp/shim_new.so $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chmod 644 $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chcon u:object_r:vendor_file:s0 $HW/..."   # 3. 必须 chcon
adb -s $SER shell su -c "ls -Z $HW/"                                # 4. 核对 label
adb -s $SER shell su -c "start vendor.bluetooth-1-0-qti"      # 5. 重启 HAL（栈自行 abort 后重启）
```

### 三条铁律

1. **绝不用 `adb push` 直推 `/data/adb/modules/` 下任何文件** —— label 会变成 `adb_data_file`，
   挂载到 `/vendor` 后 label 不变，HAL 无权 `map`（见 root-causes.md 根因 5）
2. **`restorecon` 无效** —— `/data/adb` 在 `file_contexts` 里本就是 `adb_data_file`，
   restorecon 会把错 label 设回去。必须 `chcon u:object_r:vendor_file:s0`
3. **改 label 不需要重启**（`/vendor` 视图的 label 跟随模块内源文件），但 **HAL 必须先停再复制**，
   脚本已内置；音视频类 HAL（如 `audio.bluetooth.default.so`）同理，否则 crash loop。

### 热更新 vs 重启

| 改动类型 | 是否需要重启设备 |
|---|---|
| 修改模块内**已存在**的文件（so / XML） | 否，`stop/start vendor.bluetooth-1-0-qti` |
| 模块内**新增**文件 | **是**（新文件只在开机时挂载） |
| `ro.` 属性 | **是** |
| `persist.` 属性 | 否 |

---

## 2. HAL 与栈侧服务名（别搞错）

| 想做的事 | 正确命令 |
|---|---|
| 重启蓝牙 HAL | `adb shell 'stop vendor.bluetooth-1-0-qti; start vendor.bluetooth-1-0-qti'` |
| 重启蓝牙栈 | `adb shell cmd bluetooth_manager enable`（先 disable 如需） |
| 重启音频 HAL | `adb shell setprop ctl.restart vendor.audio-hal` |

**`setprop ctl.restart android.hardware.bluetooth@1.0-service-qti` 会失败**
（"Failed to set property ... See dmesg"）—— 那是 rc 文件名，不是 init 服务名。

**`setprop ctl.restart audioserver` / `ctl.restart android.hardware.audio.service` 都别用** ——
前者让 AudioFlinger 只加载 primary 模块，后者直接失败并导致 policy 模块丢失。

---

## 3. SELinux label 对照表

| 文件类型 | 需要的 label |
|---|---|
| `/vendor/lib64/hw/*.so` | `u:object_r:vendor_file:s0` |
| `/vendor/etc/**/*.xml` / `*.json` | `u:object_r:vendor_configs_file:s0` |

检查：

```bash
adb -s $SERIAL shell 'ls -Z /data/adb/modules/leaudio_marble_fix/vendor/lib64/hw/'
adb -s $SERIAL shell 'ls -Z /vendor/lib64/hw/ | grep -i bluetooth'
```

**`/vendor` 视图的 label 必须与模块目录一致**（跟随源文件）。
不一致 = 部署方式用错了。

---

## 4. VINTF manifest 零写入预验证（唯一允许的 bind mount 场景）

改 manifest 有 bootloop 风险，**部署前必须本地验**：

```bash
adb -s $SERIAL shell 'md5sum /vendor/etc/vintf/manifest_ukee.xml'   # 挂载前记录
adb -s $SERIAL shell 'cp /vendor/etc/vintf/manifest_ukee.xml /data/local/tmp/same.xml; chmod 644 /data/local/tmp/same.xml'
adb -s $SERIAL shell 'mount --bind /data/local/tmp/same.xml /vendor/etc/vintf/manifest_ukee.xml'
adb -s $SERIAL shell 'vintf dm | wc -l'      # 557 = 正常；0 = 解析失败
adb -s $SERIAL shell 'umount /vendor/etc/vintf/manifest_ukee.xml'
adb -s $SERIAL shell 'md5sum /vendor/etc/vintf/manifest_ukee.xml'   # 必须与挂载前一致
```

**事后必须 umount + md5 核对还原。**

设备侧 `/system/bin/vintf` 只有 dump 子命令（legacy/dm/fm/dcm/fcm/ri），**无 checkcompat**，
所以只能靠 `vintf dm | wc -l` 判解析成败。

---

## 5. 抓日志

`adb logcat -d` 和 harness 起的 `logcat > file &` 都会漏（缓冲小、进程被 kill）。

**有效手段**：

```bash
adb -s $SERIAL shell 'setsid logcat -v threadtime -b main,system,crash -f /data/local/tmp/btlog.txt &'
# ... 操作 ...
adb -s $SERIAL pull /data/local/tmp/btlog.txt /tmp/btlog.txt
```

ROM 特性：logcat 缓冲 ~3 分钟 / ~12k 行，**必须实时抓**。

### 关键指标 grep

```bash
grep -c "sendIsoData: ISO is not supported in HAL v1.0" /tmp/btlog.txt   # 必须 0
grep -c "remove_iso_data_path: No such iso connection" /tmp/btlog.txt   # 必须 0
grep -c "__fortify_fatal" /tmp/btlog.txt                                # 必须 0
grep -c "Invalid packet type rcvd 0x5" /tmp/btlog.txt                   # 必须 0
grep -c "avc: denied" /tmp/btlog.txt                                    # 必须 0
adb -s $SERIAL shell logcat -b crash -d -v threadtime | tail -20
```

---

## 6. 健康状态检查

```bash
SER=$SERIAL
# 进程年龄（ELAPSED 小 = 刚崩过重启）
adb -s $SER shell 'ps -A -o PID,ELAPSED,NAME | grep -iE "bluetooth"'
# HIDL 双注册（必须 @1.0 + @1.1 都 DM,FC Y）
adb -s $SER shell lshal list -i | grep IBluetoothHci
# VINTF @1.1 声明
adb -s $SER shell 'vintf dm | grep -A3 android.hardware.bluetooth'
# SELinux
adb -s $SER shell 'ls -Z /vendor/lib64/hw/ | grep -i "impl-qti\|bluetooth"'
# 控制器 ISO 缓冲数（00680 固件应为 22）：从 btsnoop 里 LE Read Buffer Size v2 的返回读
# snoop 日志在 /data/misc/bluetooth/logs/btsnoop_hci.log（需 persist.bluetooth.btsnooplogmode=full）
adb -s $SER shell su -c 'cat /data/misc/bluetooth/logs/btsnoop_hci.log' > /tmp/bt.snoop
tshark -r /tmp/bt.snoop -Y "bthci_evt.total_num_iso_data_pkts > 0" \
  -T fields -e bthci_evt.total_num_iso_data_pkts -e bthci_evt.iso_data_pkt_len
# 丢包：协议栈自己的日志（v4.4 删掉 credit 代理后，没有 shim 侧的统计行了）
adb -s $SER logcat -d | grep "dropping ISO"
```

---

### 听歌时监测 artifact（`scripts/leaudio_watch.py`）

怀疑耳机里偶尔有杂音时，一边听歌一边跑：

```bash
SERIAL=<adb 序列号> ./scripts/leaudio_watch.py          # 每 10 秒一行；Ctrl-C 结束并打印总结
```

听到杂音就**按回车打标记**，脚本回看那一刻前 10 秒，并把当时的 btsnoop 存到 `monitor_runs/watch_*/mark_N/`。
它从滚动的 btsnoop 里增量检查：ISO 序号是否连续、发送间隔（>15 ms 记为异常，>12.5 ms 记为抖动）、
把发出去的 SDU 用 liblc3 解码（坏帧 = 触发丢帧补偿）、断连 / CIS 建立失败 / 硬件错误、
蓝牙进程和 HAL 进程有没有重启、logcat 里的关键报错。

怎么读结论：

- 标记时主机侧有异常：问题在手机这边（缺帧、发送间隔变大、坏帧、断连、进程重启）。
- 标记时主机侧全部干净：更可能是空口丢包或耳机侧。**脚本看不到空口**：控制器的完成数（NCP）不区分"发出"和"超时丢弃"，
  要测空口得让 shim 去读控制器的 `LE Read ISO Link Quality`（0x2075，有 TX_UnACKed / TX_Flushed / Retransmitted 计数），目前没做。
- 发送间隔偶尔超过 15 ms 不一定有声音问题：耳机有呈现延迟，能吸收一部分主机侧抖动。要靠标记对照耳朵。

btsnoop 是滚动日志（每 65536 条记录一个文件，保留两个，约 10 分钟），脚本每个周期都拉一次并处理新增部分，
不用担心滚动；但时间间隔别设得太大（建议不超过 60 秒）。

## 7. 事故复盘

### 事故一：VINTF fragment → bootloop

**触发**：部署 `vendor/etc/vintf/manifest/android.hardware.bluetooth@1.1.xml` fragment 后重启。

**现象**：system_server 崩溃循环 → RescueParty 触发 → 卡厂商 logo，
`adb devices` / `fastboot devices` 均空。

**根因**：libvintf 两条规则任一命中都会让**整份** device manifest 解析失败
（fatal，所有依赖 manifest 的 system 进程拿不到 manifest → system_server 崩）：

1. 跨文件 FqInstance 冲突：
   ```
   Cannot add manifest fragment ...: HAL "android.hardware.bluetooth" has a conflict:
   Conflicting FqInstance: @1.0::IBluetoothHci/default (from manifest_ukee.xml)
   vs. @1.1::IBluetoothHci/default (from fragment)
   ```
2. 同一 `<hal>` 块内重复 major：
   ```
   Illformed file: Duplicated major version: 1.0 vs 1.1
   ```

**救援**：

1. KernelSU 安全模式（开机画面后连按音量减 3 次，按-松）→ 系统正常启动
2. safe mode 下 `su: inaccessible or not found` → 用 `adb root` 拿 uid=0
3. 发现 `/data/adb/modules/leaudio_marble_fix/disable` 已被 safe mode **自动创建**
   （mtime 1973-04-30 06:32, size 0）—— 这就是救援入口
4. recovery 里救不了：`/data` 是 f2fs + `fileencryption=ice` + metadata encryption，
   `mount /data` 返回 `Invalid argument`，`/data/adb` 不存在。**必须进安全模式**
5. 备份原文件 → 删掉 fragment → 推正确格式

**预防**：改 manifest 前必做第 4 节的 bind mount 预验证。

### 事故二：`adb push` 直推模块目录 → HAL 无法加载

见 root-causes.md 根因 5。修复 = `chcon` + `stop/start vendor.bluetooth-1-0-qti`，
**不需要重启**。

### 事故三：音频 HAL 补丁碰了 paciasp → crash loop

把 `audio.bluetooth.default.so` 的 `UpdateSinkMetadata` 入口 `paciasp` 改成 `ret` →
PLT 间接调用没有 BTI 落点 → 每次开 BLE 输入都 `SIGILL (ILL_ILLOPC)`，音频 HAL crash loop
~15 次（约 1 分钟）。**不重启就热修**：`cat 新文件 > 模块内文件`（同 inode，
`/vendor` 视图即时可见）+ init 自动重启的音频 HAL 加载新代码，~30s 内自愈。
教训：改函数入口必须保留 `paciasp`/`bti c`（见 architecture.md 2.2、dead-ends.md 7.4）。