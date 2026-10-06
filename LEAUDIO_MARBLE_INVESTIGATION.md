# Redmi Note 12 Turbo (marble) — LE Audio 支持性排查记录

- 日期：2026-10-05（设备时间）
- 设备：Redmi Note 12 Turbo / marble，LineageOS 23.2 (Android 16, SDK 36)，KernelSU 3.3.0
- 蓝牙：SoC `hastings`(BT 5.3) / 平台 taro；设备名被改为 `HUAWEI Mate X7`
- 被测耳机：`ROSELINK` = `XX:XX:XX:XX:XX:XX`（LE Audio 侧，DevType=LE）＋ `Furina Endless Solo of Solitude-LEA` = `XX:XX:XX:XX:XX:XX`（同套 TWS，左右耳），主控 Bluetrum 中科蓝讯，音频描述符 `bluetrum earbuds`
- 经典侧旧配对：`XX:XX:XX:XX:XX:XX`（DevType=DS，A2DP/AVDTP 1.03，仅 SBC+AAC SEP）

---

## 1. 最终结论（TL;DR）

| 问题 | 结论 |
|---|---|
| 手机硬件/固件支持 LE Audio 吗？ | **支持**。控制器 LE 特性实测 CIS Central/Peripheral、Isochronous Broadcaster、Synchronized Receiver 全为 True；厂商 AIDL BT 音频 HAL 能开 `LE_AUDIO_SOFTWARE_ENCODING_DATAPATH` |
| 这台手机 + 当前 ROM 能实际用 LE Audio 播放吗？ | **不能**。能把链路跑起来（`CIS CONNECTED` + `ASE STREAMING` + LC3 48k/2ch），但**耳机始终没有声音** |
| 卡在哪一层？ | **闭源层**：QTI 的蓝牙音频会话/编解码实现（`libbluetooth_audio_session_qti_2_1.so` / `vendor.qti.hardware.bluetooth_audio@2.1-impl.so`，无任何 `LeAudio*.cpp`）+ BT 固件。配置层能做的都做完了 |
| 内核需要动吗？ | **不需要**（ISO/CIS 由 BT 固件 + HCI 处理，音频走 userspace HAL） |
| KernelSU 是必要层面吗？ | **不是**，仅作临时投递手段（`resetprop` + `mount --bind`），重启即失效 |

---

## 2. 三层证据

### 2.1 控制器固件（支持 ✅）

HCI `LE Read Local Supported Features` 实测（btsnoop 解析）：

```
Supported LE Features: 0x0000008eff01f9ff
  Connected Isochronous Stream - Central:     True
  Connected Isochronous Stream - Peripheral:  True
  Isochronous Broadcaster:                    True
  Synchronized Receiver:                      True
  Isochronous Channels (Host_support):        False   ← 仅表示 host 未显式声明，未证明是障碍
```

其他：`hci_version/lmp_version = V_5_3`，`ISO Manager: Available credits: 3, Controller buffer size: 155`。

### 2.2 厂商侧属性与配置（"宣称支持"但未产品化 ⚠️）

厂家预置（`/system_ext/etc/build.prop`，来源 `device_xiaomi_sm8450-common/properties/system_ext.prop`）：

```
ro.bluetooth.leaudio_offload.supported=true
persist.bluetooth.leaudio_offload.disabled=false
persist.bluetooth.leaudio.bypass_allow_list=true
bluetooth.leaudio.dual_bidirection_swb.supported=true
persist.bluetooth.leaudio.allow.multiple.context=false
persist.bluetooth.leaudio.notify.idle.during.call=true
```

但**缺**（全源码 grep `bap.unicast` / `bap.broadcast` = 0 处，AOSP 默认 false）：

```
bluetooth.profile.bap.unicast.client.enabled
bluetooth.profile.vcp.controller.enabled
bluetooth.profile.csip.set_coordinator.enabled
bluetooth.profile.bap.broadcast.assist.enabled
bluetooth.profile.hap.client.enabled
```

而且音频策略配置 `/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml` 里**完全没有 BLE 设备端口**（`AUDIO_DEVICE_OUT_BLE_HEADSET` 等 0 处）。设备实际加载的 sku 由 `ro.boot.product.vendor.sku=ukee` 决定（`dumpsys media.audio_policy` 里 `Config source` 可确认）。

### 2.3 复现出来的真实行为

改为临时配置后（见第 3 节）：

- 蓝牙栈：`LeAudio Manager` 从 `Not initialized` → 运行；`LE Audio: Connected: 2`；PACS 读到 LC3 能力（Coding format 0x06，8–48kHz，7.5/10ms，octet ≤155）
- 音频框架：输出设备出现 `Port ID: 28 "BT BLE Headset Out" {AUDIO_DEVICE_OUT_BLE_HEADSET, @:XX:XX:XX:XX:XX:XX}`
- 播放时：
  ```
  Group: Current state: STREAMING (0x04),  cig state: CREATED (0x02)
  Current Codec ID: 6   ← LC3
  Active config: "Two-OneChan-SnkAse-Lc3_48_4_High_Reliability"
  ASE 1: active=true sink cis=0/handle 9 sdu=120 latency=100 rtn=3
         state=STREAMING  cis_state=CONNECTED  data_path_state=CONFIGURED
  ```
- **但耳机无任何声音**（用户实测确认）。

---

## 3. 实验过程（三件套）

> 所有改动均为临时：非持久属性 + 内存 bind mount，不写开机脚本；重启即完全恢复。

### 3.1 改动清单

| # | 改动 | 位置（正式应在哪做） | 作用 |
|---|---|---|---|
| 1 | `bluetooth.profile.bap.unicast.client.enabled=true`（+ vcp.controller / csip.set_coordinator / bap.broadcast.assist / hap.client） | 设备树 `properties/product.prop` → `/product/etc/build.prop` | 启动 LE Audio 单播客户端（否则 `LeAudioService` 等 5 个服务根本不会起） |
| 2 | `persist.bluetooth.leaudio_offload.disabled=true` | 设备树 `properties/system_ext.prop` | 厂商 HAL 拒绝 `LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH`，必须走 CPU 软件编码（仍是 LC3） |
| 3 | 音频策略加 **BLE 输出端口**（独立 `bluetooth` 模块），**不声明 BLE 输入端口** | 设备树 `audio/audio_policy_configuration.xml` → `/vendor/etc/audio/sku_*/…` | 让 AudioPolicyManager 能 `createDevice`；且避免栈选 LIVE 双向配置 |

实验期投递方式（KernelSU）：

```bash
R=/data/adb/ksu/bin/resetprop
$R -n bluetooth.profile.bap.unicast.client.enabled true
# … 其余 4 个 profile 开关 …
$R -n persist.bluetooth.leaudio_offload.disabled true

# 打补丁的音频策略文件（SELinux 标签必须先改成 vendor_configs_file，否则 audioserver 读不了）
cp /data/local/tmp/apc3.xml /data/adb/leaudio/audio_policy_configuration.xml
chcon u:object_r:vendor_configs_file:s0 /data/adb/leaudio/audio_policy_configuration.xml
mount -o bind /data/adb/leaudio/audio_policy_configuration.xml \
              /vendor/etc/audio/sku_ukee/audio_policy_configuration.xml

killall com.android.bluetooth   # 让 AdapterService 重读属性
killall audioserver             # 让 AudioPolicyManager 重读配置
```

> 注意：属性必须在蓝牙服务启动前生效 —— 热验证时靠“杀进程重绑”；正式做法是写进 build.prop（开机即生效）。

### 3.2 走过的弯路（供以后避坑）

| 尝试 | 现象 | 原因 |
|---|---|---|
| 直接把 BLE 端口加进 **primary** 模块 | 媒体能切过去，但 `AHAL: AudioStream: onWriteError: write error -22 usecase(deep-buffer-playback)` 循环 | QTI 的 primary HAL 不处理 BLE 设备，音频流开在了错误的 HAL 模块 |
| bind mount 后未改 SELinux 标签 | `avc: denied { read } … tcontext=u:object_r:adb_data_file:s0`，audioserver 退回 `AudioPolicyConfig::setDefault`，输出设备只剩 1 个 | /data 下的文件标签是 `adb_data_file`，audioserver 读不了 → 配置加载失败 |
| 声明了 **BLE 输入端口**（麦克风） | `APM::HwModule: adding dynamic device AUDIO_DEVICE_IN_BLE_HEADSET`，随后栈选 `LIVE` **双向** 配置 `VND_Two-OneChan-SnkAse-Lc3_48_2-Two-OneChan-SrcAse-Lc3_48_2`，而耳机 Source PAC 只支持 1 声道 → `RECONFIGURATION_NEEDED` 后 ASE 永远 IDLE，抓包无 ASCS 写入、无 `LE Create CIS` | 双向配置与耳机能力不匹配，配置协商不了 |
| 去掉输入端口后（v3） | `CIS CONNECTED` + `ASE STREAMING` + LC3 48_4 High_Reliability，但**无声音** | 链路层全通；缺口在数据通路（闭源 HAL/固件） |

### 3.3 关键日志片段

```text
# 厂商 HAL 拒绝硬件 offload（所以必须关 offload）
BTAudioClientAIDL: FetchAudioProvider: SessionType=LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH
                   Not supported by BluetoothAudioHal
BTAudioClientLeAudioStub: GetSink: BluetoothAudio HAL for Le Audio is invalid?!
audio_source_hal_client.cc:521 AcquireUnicast: Could not acquire Unicast Source on LE Audio HAL endpoint
client.cc:1839 GroupSetActive: could not acquire audio source interface

# 关掉 offload 后，软件通路可用
BTAudioProviderLeAudioSW: startSession - size of audio buffer 2560 byte(s)
BTAudioHalDeviceProxyAIDL: SetUp: session_type=LE_AUDIO_SOFTWARE_ENCODING_DATAPATH, cookie=0x400
BTAudioHalStream: adev_open_output_stream: state=STANDBY, sample_rate=48000, channels=0x3, format=1

# 无声音时的现象（HAL 流没被栈启动）
BTAudioHalStream: out_write: state=DISABLED failed to resume     ← 反复出现
RouterInfoMediaManager: onTransferFailure(), route: …:LE_AUDIO_1
```

---

## 4. 开源 / 闭源边界

### 开源（ROM/设备树可改，二进制里能读到源码路径）

| 组件 | 位置 ← 源码 |
|---|---|
| Bluetooth APEX（协议栈、`LeAudioService`、`bta/le_audio/*`、属性开关） | `/apex/com.android.bt` ← `packages/modules/Bluetooth` |
| BT 音频 HAL 模块（AudioFlinger↔BT HAL 桥） | `/vendor/lib64/hw/audio.bluetooth.default.so` ← `packages/modules/Bluetooth/system/audio_bluetooth_hw/{audio_bluetooth_hw,stream_apis,device_port_proxy,utils}.cc` |
| AIDL BT audio HAL 实现（含 `BTAudioProviderLeAudioSW`） | `/vendor/lib64/android.hardware.bluetooth.audio-impl.so` ← `hardware/interfaces/bluetooth/audio/aidl/…V5-ndk` |
| 音频策略配置 | `/vendor/etc/audio/sku_*/audio_policy_configuration.xml` ← 设备树 `audio/audio_policy_configuration.xml` + `common.mk:67/77` |
| 属性文件 | 设备树 `properties/{product,system_ext}.prop` |
| QTI primary 音频 HAL | `/vendor/lib64/hw/audio.primary.taro.so` ← `hardware/qcom-caf/sm8450/audio/primary-hal`（CodeAurora，公开） |
| 内核 | — （本场景无需改动） |

### 闭源（厂商 blob，无源码）

| 组件 | 位置 ← 源码路径证据 |
|---|---|
| **QTI 蓝牙音频会话/编解码实现** | `/vendor/lib64/hw/vendor.qti.hardware.bluetooth_audio@2.1-impl.so`、`/vendor/lib64/libbluetooth_audio_session_qti*.so` ← **`vendor/qcom/proprietary/bluetooth/bluetooth_audio/2.1/default/…`**；该 blob 仅含 `A2dpSoftwareAudioProvider.cpp`、`BluetoothAudioProvider.cpp`、`BluetoothAudioProvidersFactory.cpp`、`HearingAidAudioProvider.cpp`，**无任何 `LeAudio*.cpp`** |
| QTI BT HAL 服务 | `/vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti`、`…@1.0-impl-qti.so` |
| **BT 芯片固件/NVM** | `/vendor/bt_firmware/image/hpbtfw{20,21}.tlv`、`hpnv20.*` |
| 厂商 LE Audio 配置数据 | `/vendor/etc/aidl/le_audio/*.json`、`*.bfcs`（AOSP 默认版在 `/apex/com.android.bt/etc/bluetooth/le_audio/`） |
| 厂商预置属性 | `/system_ext/etc/build.prop` 中的 `ro.bluetooth.leaudio_offload.supported` 等 |

**判定**：配置类改动全部落在开源层 → 可以自己改、可以验证；真正缺的"音频数据通路"落在闭源层 → 只能等厂商更新 blob，或移植其它已做通 LE Audio 的同平台机型 vendor 部件（ABI/HIDL 版本需匹配，属 ROM 移植工作）。

---

## 5. 源码树对照（evolution-x-source）

`device_xiaomi_sm8450-common/`：

- `audio/audio_policy_configuration.xml`
  - 第 436 行确实 include 了 AOSP 的 `/vendor/etc/bluetooth_audio_policy_configuration.xml`（`common.mk:77` 负责安装），AOSP 该文件内含 `AUDIO_DEVICE_OUT_BLE_HEADSET` 等端口 —— **这是通往 LE Audio 的正确方向**
  - 但同文件 `primary` 模块里已有 `BT A2DP Out`/`BT SCO*` 端口（273–307 行）→ 与 AOSP include 里的同名端口**重复声明**，有冲突风险
  - 且 AOSP 该文件同时声明 BLE **输入**端口 → 会触发上文 3.2 的 LIVE 双向问题
- `properties/product.prop`：只有经典 profile 开关，**没有** LE Audio profile 开关
- `properties/system_ext.prop`：`ro.bluetooth.leaudio_offload.supported=true` / `persist.bluetooth.leaudio_offload.disabled=false`（厂商默认，实测不可用）

> 手机实际运行的 ROM 是 LineageOS 23.2 nightly，其 `/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml` 与上述 evolution 源码树的文件**不一致**（前者含 FM 端口、无 Dolby spatializer mixPort、无 include），说明两者并非同一份树/版本。

### 若要正式修（未执行，仅供参考）

```diff
# properties/product.prop
+bluetooth.profile.bap.unicast.client.enabled=true
+bluetooth.profile.vcp.controller.enabled=true
+bluetooth.profile.csip.set_coordinator.enabled=true
+bluetooth.profile.bap.broadcast.assist.enabled=true
+bluetooth.profile.hap.client.enabled=true

# properties/system_ext.prop
-persist.bluetooth.leaudio_offload.disabled=false
+persist.bluetooth.leaudio_offload.disabled=true

# audio/audio_policy_configuration.xml
-        <xi:include href="/vendor/etc/bluetooth_audio_policy_configuration.xml"/>
+        <!-- 只加 BLE 输出端口（独立 bluetooth 模块）：避免与 primary 的 BT A2DP/SCO 端口重名，
+             且不声明 BLE 输入端口（否则触发 LIVE 双向配置导致 ASE 无法配置） -->
+        <module name="bluetooth" halVersion="2.0"> …（见附录 A）… </module>
```

结论：这些只能把"链路"做通，**仍不能保证出声**，因为瓶颈在闭源 HAL。

---

## 6. 复现 / 回退

### 复现要点

1. 设 6 个属性（见 3.1）
2. 打补丁的音频策略 XML（附录 A）→ `chcon u:object_r:vendor_configs_file:s0` → bind mount 到 `/vendor/etc/audio/sku_<sku>/audio_policy_configuration.xml`
3. `killall com.android.bluetooth; killall audioserver`
4. 校验：`dumpsys media.audio_policy | grep -m1 "Config source"`、`Available output devices`（应 +1 出现 `BT BLE Headset Out`）、`dumpsys bluetooth_manager | grep -A3 "LE Audio:"`
5. 播放 → 观察 `dumpsys bluetooth_manager | grep -A20 "Active Groups"` 是否 `STREAMING` / `cig state: CREATED`

### 回退（已执行并验证）

```bash
umount /vendor/etc/audio/sku_ukee/audio_policy_configuration.xml
R=/data/adb/ksu/bin/resetprop
for p in bluetooth.profile.bap.unicast.client.enabled bluetooth.profile.vcp.controller.enabled \
         bluetooth.profile.csip.set_coordinator.enabled bluetooth.profile.bap.broadcast.assist.enabled \
         bluetooth.profile.hap.client.enabled persist.bluetooth.leaudio_offload.disabled; do $R -d $p; done
rm -rf /data/adb/leaudio /data/adb/post-fs-data.d
killall com.android.bluetooth; killall audioserver
```

回退后核对结果：

| 检查项 | 结果 |
|---|---|
| `/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml` md5 | `48584020a0fbe0fec4734a43e97a6046`（与原始副本一致 → 分区文件从未被修改，只做过内存挂载） |
| `Config source` | `/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml` |
| 可用输出设备 | 3（回到原始） |
| BLE 端口数 | 0 |
| Profile 服务 | 14 个（无 `LeAudioService`/`VolumeControlService`/`CsipSetCoordinator`/`HapClient`/`BassClient`） |
| `LE Audio` | `Connected: 0` |
| 6 个属性 | 全部为空 |
| 白名单模块 | 三个只读白名单模块均 enabled，本次未改动（模块名/版本记于本地笔记，不入库） |

---

## 7. 后续可行路径

1. **等厂商/ROM 更新**（推荐）：需要 QTI/小米更新蓝牙音频 blob（补 LE Audio 数据通路），或 LineageOS 上游把这块做通。
2. **移植 vendor 部件**：从已把 LE Audio 做通的同平台（sm8450/sm8550 系）机型移植 `vendor/etc/audio`、`vendor/etc/aidl/le_audio`、`vendor.qti.hardware.bluetooth_audio@2.1-impl`、`libbluetooth_audio_session_qti*` 等；需 ABI/HIDL 版本匹配并整机刷机验证，风险高。
3. **不建议**：继续用 KernelSU 改属性/挂载 —— 配置层已到顶，无法解决数据通路。

---

## 附录 A：v3 音频策略补丁（仅 BLE 输出端口）

以 `/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml` 为基线，在 `</module>`（primary 模块结束）之后、`usb` 模块之前插入：

```xml
        <module name="bluetooth" halVersion="2.0">
            <mixPorts>
                <mixPort name="ble output" role="source">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                             samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
                </mixPort>
            </mixPorts>
            <devicePorts>
                <devicePort tagName="BT BLE Headset Out" type="AUDIO_DEVICE_OUT_BLE_HEADSET" role="sink"
                            encodedFormats="AUDIO_FORMAT_LC3">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                             samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
                </devicePort>
                <devicePort tagName="BT BLE Speaker Out" type="AUDIO_DEVICE_OUT_BLE_SPEAKER" role="sink"
                            encodedFormats="AUDIO_FORMAT_LC3">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                             samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
                </devicePort>
                <devicePort tagName="BT BLE Broadcast Out" type="AUDIO_DEVICE_OUT_BLE_BROADCAST" role="sink"
                            encodedFormats="AUDIO_FORMAT_LC3">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                             samplingRates="48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>
                </devicePort>
            </devicePorts>
            <routes>
                <route type="mix" sink="BT BLE Headset Out" sources="ble output"/>
                <route type="mix" sink="BT BLE Speaker Out" sources="ble output"/>
                <route type="mix" sink="BT BLE Broadcast Out" sources="ble output"/>
            </routes>
        </module>
```

**不要**加 `BT BLE Headset In`（`AUDIO_DEVICE_IN_BLE_HEADSET`）—— 见 3.2。

## 附录 B：常用排查命令

```bash
# LE Audio 是否启用 / 活动状态
adb shell dumpsys bluetooth_manager | grep -A3 "^  LE Audio:"
adb shell dumpsys bluetooth_manager | grep -A20 "LeAudio Manager"
adb shell dumpsys bluetooth_manager | grep -A20 "Active Groups"
adb shell dumpsys bluetooth_manager | grep -E "^Profile: "

# 音频策略
adb shell dumpsys media.audio_policy | grep -m1 "Config source"
adb shell dumpsys media.audio_policy | grep -A4 "Available output devices"

# 属性
adb shell getprop bluetooth.profile.bap.unicast.client.enabled
adb shell getprop persist.bluetooth.leaudio_offload.disabled

# 音频 HAL / 蓝牙栈日志
adb shell logcat -d --pid=$(adb shell pidof com.android.bluetooth) | grep -i -E "le_audio|stream|cis|ase|hal"
adb shell logcat -d --pid=$(adb shell pidof android.hardware.audio.service) | grep -i -E "BTAudioHal|LeAudio|session"

# HCI 抓包（只读解析）
adb shell su -c "cat /data/misc/bluetooth/logs/btsnooz_hci.log" > snoop.log
tshark -r snoop.log -Y 'bthci_cmd.opcode == 0x2003' -V | grep -i -A8 "LE Supported Features"
tshark -r snoop.log -T fields -e _ws.col.Info | grep -i "create cis"
```

---

## 2026-10-05 会话记录（v3 shim + VINTF 事故）

### 成果
1. **v3 shim 实测通过**：修复 shadow vtable Itanium 前缀错位、`android::sp` 命名空间、
   `sp<T>` x8-sret 返回 ABI 三个 bug；`android.hardware.bluetooth@1.1::IBluetoothHci/default`
   成功注册进 hwservicemanager（lshal 确认）；兄弟服务（FM/ANT/SAR/btconfigstore）经
   DT_NEEDED 转发 + SONAME 改名全部恢复正常。
2. **最后一环定位**：Fluoride `V1_1::getService()` 被 `hwservicemanager::getTransport`
   拦截（"Cannot find entry ... in VINTF manifest"）→ 退回 1.0。
3. **VINTF fragment 方案导致 bootloop**（RescueParty 触发），经安全模式救回；根因与
   正确方案详见 TODO.md（Pixel 式 `<version>1.1</version>` 声明已通过 bind-mount 解析验证）。
4. **零重启 VINTF 测试法**：`mount -o bind` 临时替换 + `/system/bin/vintf dm` +
   `logcat | grep "E vintf"` 可在不重启的情况下安全迭代 manifest。

### 关键 ABI 知识（AArch64 / LineageOS 23.2 / Android 15）
- BnHwBluetoothHci 主 vptr slot 11 = onTransact；impl vptr slot 1 = interfaceChain、
  13 = initialize；vbase offset 在 vptr[-3]（BnHw=0x88，impl=0x18）
- `sp<T>`=8 字节但带析构 → 返回走 x8 sret；`Status`=32B 全零=ok(sret)；
  `Return<void>`=40B 全零(sret)；`hidl_string/hidl_vec`=16B
- std::function 调用：`__func` 对象在 cb+0x20，`operator()` 在其 vptr[6]
- 真实 `readNullableStrongBinder` 符号使用 `android::sp`（utils/StrongPointer.h）

### 待续
- 部署 `leaudio_marble_fix_v2/vendor/etc/vintf/manifest_ukee.xml`（Pixel 式）→ 重启 →
  验证 initialize_1_1(code 6) → sendIsoData(code 7) → ISO credits → LC3 播放
