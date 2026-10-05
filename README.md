# LE Audio on marble — Redmi Note 12 Turbo

设备：Redmi Note 12 Turbo (`marble`)，LineageOS 23.2 Nightly / Android 16，KernelSU
耳机：ROSELINK（LE Audio Unicast，CSIP 双耳组，LC3）

目标：让 LE Audio 正常工作。原始症状是「链路建立、有声音但严重卡顿」，后续排查中
发现真正的阻塞是**控制器固件崩溃**（表现为「没声音 + 3-4 秒后蓝牙重启」）。

---

## 目录结构

```
bluetooth_hci_shim/BluetoothHciHook.cpp   核心：QTI HAL 的 HIDL 1.1 shim（v3.10）
bluetooth_hci_shim/BluetoothHciShim.cpp   早期尝试（已被 HciHook 取代）
leaudio_marble_fix_v2/                    KernelSU 模块（system.prop / 音频策略 / VINTF）
leaudio_iterate.sh                        部署 / 回滚 / 监测 / 音乐测试 / 状态
leaudio_monitor.sh                        事件触发式全量快照监测器
leaudio_monitor_analyze.py                监测数据分析器（崩溃时间线 + 速率 + 关联）
deploy_shim.sh                            整模块部署
TODO.md                                   累积的调查记录（含 bootloop 事故复盘）
LEAUDIO_MARBLE_INVESTIGATION.md           早期调查笔记
monitor_runs/                             历史采集（只保留 summary/events，原始 logcat 已忽略）
```

## 构建 shim

```bash
NDK=/opt/android-sdk/ndk/30.0.16248370
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
  --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 \
  -I/tmp/ndk_platform_config -I/tmp/system_libhidl/transport/include \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  bluetooth_hci_shim/BluetoothHciHook.cpp -o /tmp/shim.so \
  -L/tmp -lhidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all /tmp/shim.so
./leaudio_iterate.sh deploy /tmp/shim.so      # 热更新（含 HAL 重启），不重启设备
```

`libbluetooth_qti_real.so`（厂商真实 HAL，618KB）需放在模块的 `vendor/lib64/hw/` 下，
shim 通过 `dlopen` 加载它。该二进制不入库（可从设备 `/vendor/lib64/hw/` 提取）。

---

## 关键结论（2026-10-05 夜）

### 1. 「没声音 + 3-4 秒蓝牙重启」的完整因果链

```
ISO 数据开始流动
  → 控制器固件断言：HCI Hardware Error event (0x10) code 0x0F
  → SSR（子系统重启）: uart_controller: "Killing daemon as SSR is completed!"
  → HAL 进程死亡
  → 蓝牙栈 abort: hci_backend_hidl.cc:83 serviceDied: The Bluetooth HAL died
  → CIS 断开 → 音频通路崩塌 → 播放器暂停（不是播放器的问题）
```

证据：`vendor.qti.bluetooth@1.0-btstateinfo` 在 SSR 前打印的最后收包：
```
14:39:57:753-Last RX packet: 04 10 01 0F      ← 0x10=Hardware Error, 0x0F=错误码
E uart_controller: Killing daemon as SSR is completed!
```

**与速率无关**：credit 窗口设 0（最保守，仅 3 个 credit 在飞）时同样 3-4 秒崩。

### 2. credit 饥饿的数学解释（卡顿的旧症状）

- 栈侧 credit 池只有初始 **3**（`ISO Manager: Available credits: 3`），控制器实际有 **155** 个缓冲
- QTI HAL 把控制器发来的真 NCP（0x13）**全部吞掉**（传输层 2410 条，栈侧 0 条）
- 栈只能靠 HAL 自己的 `0x1407` 轮询（~10/s）补 credit
- 稳态吞吐 = 初始 credit × 发放频率 = **3 × 10/s = 30/s** ✓ 与实测 30–37/s 吻合
- 播放时丢帧稳定在 **200/s**（= 2 CIS × 100/s，正好是整个音频需求）

因此「诚实回声」式的 NCP 合成（发放数 = 发送数）**不可能**突破 30/s——池子恒定。
v3.9 起改为**窗口式**（保持 W 个 credit 在飞，`persist.vendor.leaudio.isocred.window`，默认 12，可热调）。

### 3. QTI ring buffer 拒收 ISO 包（type 5）

反汇编 `PacketBuff::AddBuffNode`（`libbluetooth_qti_real.so` @0x81bbc）的类型跳转表
（`.rodata` @0x2e4ac，表项 = 相对 0x81cb4 的字节偏移/4）：

| type | 处理分支 |
|---|---|
| 1 | CMD |
| 2 | ACL（16 位长度 @偏移2，+5） |
| 3 | SCO |
| 4 | EVT |
| **5 (ISO)** | **0x81ce8 = "Received packet with wrong packet type" 错误分支** |

→ ISO 数据根本到不了控制器。shim 在运行时（仅进程内存，不碰分区）把该表项
13 → 25（改用 ACL 的帧规则；ISO 与 ACL 的 HCI 帧格式相同）：
```
ring-patch: ISO(type5) table entry 13 -> 25 at 0x73d10324b0 (base 0x73d1004000)
```
补丁后 `wrong packet type` 报错归零。开关：`persist.vendor.leaudio.iso.patchring`。

### 4. ISO 帧格式与 CIS 协商不一致（当前最可疑的崩溃原因）

btsnoop 里 2061 个 ISO 包的原始结构：
```
05 | 06 20 | 7c 00 | 00 00 | 78 00 | <120 字节 LC3>
type  handle  len=124  seq=0   len=120
      (PB=2)
```
即主机发出的 SDU = **4 字节 framed 前缀 `[2B 序号][2B 长度]` + LC3 帧**，
而 `LE Set CIG Parameters`（opcode 0x2062，已从 btsnoop 解出）协商的是：

```
CIG_ID=1  SDU_Interval_C2P=10000µs  Framing=0(UNFRAMED)
Max_Transport_Latency_C2P=100ms  CIS_Count=2
每个 CIS: Max_SDU_C2P=0x0078=120, PHY_C2P=2M, RTN_C2P=3
```

→ **unframed 的 CIS 收到 framed 格式、且比 Max_SDU 多 4 字节的 SDU**。
v3.10 因此尝试在转发前剥掉这 4 字节前缀（`persist.vendor.leaudio.iso.strip4`，默认开），
但**当前会话的包结构不匹配该条件**（`forwarded 159 bytes (orig 159)`，未触发剥离），
说明 framing 随 codec 配置变化，需要重新抓包确认。**这是当前的待办。**

### 5. 硬件 offload 路径缺配置（Plan B）

```
E BTAudioCodecsProviderAidl: GetLeAudioCodecCapabilities: input le_audio_offload_setting content need to be non empty
I BTAudioProviderFactoryAIDL: SessionType=LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH supports 0 codecs
```
`/vendor/etc/aidl/le_audio/` 只有软件路径的配置（`aidl_default_audio_set_configurations.json` 等），
没有 offload 的 codec 配置；且模块 `system.prop` 主动关掉了 offload
（`ro.bluetooth.leaudio_offload.supported=false`）。若软件路径走不通，可通过模块 overlay
补一份 offload 配置再试。

---

## shim 开关（`persist.vendor.leaudio.*`）

| 属性 | 默认 | 作用 |
|---|---|---|
| `ncpsynth` | 1 | 合成 NCP（0=关，回到完全被动转发） |
| `isocred.window` | 12 | 窗口式 credit（保持 W 个在飞；0=诚实回声；**热调，无需重启**） |
| `iso.patchring` | 1 | ring buffer type-5 补丁（0=不打补丁） |
| `iso.strip4` | 1 | 剥掉 4 字节 framed 前缀（0=原样转发） |
| `isocred.mult` | 6 | 早期乘法式 NCP（**已知会下溢，勿用**） |

## 监测器

```bash
./leaudio_monitor.sh 900        # 采集 900 秒；事件触发式全量快照
```
- 全量 logcat 流 + `tail -F` 事件触发（崩溃类强制快照、状态类 2s 节流）+ 0.5s pid 轮询兜底
- 每份快照 7 段：进程年龄/线程、蓝牙状态、音频路由、媒体会话、耳机连接、最新 tombstone 全文、logcat 尾部 200 行
- 结束自动跑分析器：崩溃时间线（含间隔）、每 10s 速率、事件关联、结论

## 复现要点（踩过的坑）

- **VINTF fragment 会导致 system_server 崩溃循环**（RescueParty 无法开机）——详见 `TODO.md`。
  只允许改 `/data/adb/modules/<模块>/` 内的 overlay，绝不 rw 挂载系统分区。
- 部署 shim 会重启 HAL → 蓝牙栈重启 → 音乐暂停；恢复播放用 `cmd media_session dispatch play`
  （`input keyevent 85` 是切换键，会误暂停）。
- 播放器暂停**通常不是播放器的问题**，而是控制器崩溃导致音频通路崩塌。
- logcat 里 `sendIsoData` 每 64 个包才记一条，不能据「无日志」断定无流量。

## 当前状态 / 下一步

### 结论（2026-10-05 23:42）：软件路径在本固件上不可行

v3.11 已能正确剥离 4 字节 framed 头（`hcilen 155 -> 151`，SDU 变成 154 = LC3 帧 ≤ Max_SDU 155），
**但控制器仍然崩溃**。崩溃点在传输层：

```
23:42:04.338  第一个 ISO 包转发
23:42:04.427  栈给 READ_CLOCK(0x1407) 设 2s 超时     ← 控制器 ~90ms 后不再响应
23:42:06.429  on_hci_timeout: Timed out waiting for READ_CLOCK(0x1407) for 2000ms
23:42:06.430  CheckForUartFailureCode: UART driver returns err code = 0x51
23:42:06.430  uart_controller: Captured UART CTS: 1 / Crash kernel TS
23:42:08.519  Last RX packet: 04 10 01 0F   (Hardware Error 0x0F)
23:42:08.635  SSR is completed!
```

且 ring 补丁关闭（ISO 包被 ring buffer 丢弃）时**同样崩溃**（当时 4/4 关联）。
→ 只要 CIS 被建立（软件路径），控制器必死：**有数据 → 传输层崩；无数据 → 通路饥饿**。
→ QTI 的 UART/daemon 传输不支持 HCI ISO 包，**软件编码路径在本固件上不可行**。

### 下一步：改走硬件 offload（控制器侧编码，主机不发 ISO）

**根因已定位到具体文件**：反汇编 `libbluetooth_audio_session_aidl.so` 得到

```
BluetoothAudioCodecs::GetLeAudioOffloadCodecCapabilities()
  -> BluetoothLeAudioCodecsProvider::ParseFromLeAudioOffloadSettingFile()
       readLeAudioOffloadSetting("/vendor/etc/le_audio_codec_capabilities.xml")
  -> GetLeAudioCodecCapabilities(optional<LeAudioOffloadSetting>)
```

**`/vendor/etc/le_audio_codec_capabilities.xml` 在原厂镜像里根本不存在** —— 所以 setting 为空、
上报 0 codec。该文件已按 AOSP 参考 schema（`hardware/interfaces/bluetooth/audio/utils/
le_audio_codec_capabilities/le_audio_codec_capabilities.xml`）为本机写好，放在
`leaudio_marble_fix_v2/vendor/etc/le_audio_codec_capabilities.xml`（LC3 16/24/32/48kHz、
7.5/10ms、octets 30/40/60/80/120/155，含 MONO/STEREO 单/双 CIS 策略）。

HAL 是**每次查询都重新读文件**（`ParseFromLeAudioOffloadSettingFile` 在
`GetLeAudioOffloadCodecCapabilities` 内部调用），所以文件一旦可见即生效，无需重启 audioserver。
但 **KernelSU overlay 只挂载已存在的子目录，新增的 `vendor/etc/` 顶层文件需要重启**才会出现。

原始阻塞描述（供参考）：

```
E BTAudioCodecsProviderAidl: GetLeAudioCodecCapabilities:
    input le_audio_offload_setting content need to be non empty
I BTAudioProviderFactoryAIDL: SessionType=LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH supports 0 codecs
```

音频 HAL（`libbluetooth_audio_session_aidl.so`）读取：
- `/vendor/etc/aidl/le_audio/aidl_default_audio_set_configurations.json`（存在，191KB，但无 offload 会话条目）
- `/vendor/etc/aidl/le_audio/aidl_audio_set_configurations.json`（**设备覆盖，不存在**）

要做的：
1. 模块 `system.prop` 恢复 `ro.bluetooth.leaudio_offload.supported=true`、
   去掉 `persist.bluetooth.leaudio_offload.disabled=true`（`ro.` 属性需重启生效）
2. 通过模块 overlay 补一份带 `LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH` codec 能力的
   `aidl_audio_set_configurations.json`
3. 验证 offload 会话上报非 0 codec，再测播放

### 稳定性取舍

在 offload 打通之前，模块会**主动触发**软件路径 → 一放音乐就崩。若要暂时消除崩溃，
可在 `system.prop` 关闭 BAP profile（`bluetooth.profile.bap.unicast.client.enabled=false`），
代价是耳机完全不能出声（ROSELINK 是 LE-Audio-only，`A2DP=0` 即禁止）。

