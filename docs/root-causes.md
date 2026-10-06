# 已确诊根因证据链

每个根因都附**可复核的证据**（反汇编地址 / tombstone 栈 / 日志原文 / md5）。

> **固件版本前提**：第 1–12 节都是在**原厂蓝牙固件 00570**（控制器只有 3 个 ISO 缓冲）上排查得出的。
> 第 13 节起换成上游固件 **00680**（22 个 ISO 缓冲）。
> HAL / VINTF / SELinux / 音频 HAL 相关的根因（第 1–7、11 节）与固件无关，修复仍在用；
> 涉及控制器缓冲和吞吐的数字与结论（第 9、12 节）**不代表当前状况**。

---

## 1. 高通 HAL 静默丢弃 ISO RX 包 → 无声 + `remove_iso_data_path` 崩溃

**现象**：耳机一连上就蓝牙崩溃，`AOSP system/stack/btm/btm_iso_impl.h:439
remove_iso_data_path: No such iso connection: 0xffff`；同时耳机无声。

**机制**：耳机连接后立即每 ~10ms 回传 ISO 包（H4 type 5）。这个包在三处被拦：

### 1a. UART 掩码漏掉 type 5

`libbluetooth_qti_real.so` `UartController::OnDataReady` @ `0x503d4`：

```asm
mov  w9, #0x501e
movk w9, #0x12, lsl #16        ; w9 = 0x0012501e
```

逐 bit 展开 `0x0012501e`：含 Type 1(CMD)/2(ACL)/3(SCO)/4(EVT)，**独缺 Type 5 (ISO) 的 bit 5 (0x20)**。

第一字节 `0x05` 命中掩码失败 → 第二字节（handle）再失败 → HAL 判定 UART 严重失步：

```
vendor.qti.bluetooth@1.0-uart_controller: OnDataReady: Invalid packet type rcvd 0x5, invalid_bytes_counter_ = 0
vendor.qti.bluetooth@1.0-uart_controller: OnDataReady: Invalid packet type rcvd 0x6, invalid_bytes_counter_ = 1
vendor.qti.bluetooth@1.0-uart_controller: OnDataReady: Out Of Synchronization
vendor.qti.bluetooth@1.0-uart_controller: SsrCleanup: SSR triggered due to 10 sending special buffer
```

**HAL 主动触发 SSR 拉低芯片**，然后才是 `Hardware Error 0x0F` / `Killing daemon as SSR is completed!`。
所以早期观察到的"固件崩溃"实际是**用户态 HAL 主动行为**，不是固件缺陷。

**修复**：`0x503d8` 改 `mov w9, #0x503e`（掩码 → `0x0012503e`，置 bit 5）。

### 1b. Packetizer 头大小表 Type 5 = 0

`HciPacketizer::OnDataReady` 头长度查找表 @ `0x2f560`：Type 5 配置为 `0`（应为 `4`）。
跳转表 @ `0x2dd52`：Type 5 偏移 `34`（应为 `30`，即 ACL 的 16-bit 载荷长度解析）。

**修复**：`0x2f560` 改 `4`；`0x2dd52` 偏移 `34`→`30`。

### 1c. 分发 lambda 只处理 type 2 / type 4

`libbluetooth_qti_real.so` `BluetoothHci::initialize(...)::$_1::operator()`
（objdump 误标为 `sp<IBluetoothHciCallbacks>::operator=`）：

```
0x3ba8c  prologue: x20=this(ctx), w21=packet type, x19=const hidl_vec*
0x3bb38  cmp w21, #2   → ACL
0x3bb44  cmp w21, #4   → EVT
0x3bdec  错误日志块（9 条指令 / 0x24 字节，无其他分支跳入，唯一入口 0x3bb48）
```

type 5 落 `0x3bdec` 静默丢弃。

**修复**：在 `0x3bdec` 覆写 ISO RX 蹦桌 v2（见根因 2）。

### 结果链

耳机等不到 ISO RX → ~470ms 后 ACL 断开 → LE Audio 清理时 CIS handle 仍 `0xffff` → assert 崩。

---

## 2. 蹦桌 v1 漏 `mov x8, sp` → `scoDataReceived` 写野地址 SIGSEGV

**tombstone**：

```
#00 BpHwBluetoothHciCallbacks::scoDataReceived(const hidl_vec&)+24   ← write fault @ 0x64540
#01 libbluetooth_qti_real.so 0x3be08  (蹦桌的 blr x9)
    <- DataHandler::OnPacketReady <- UartController::OnPacketReady <- HciPacketizer::OnDataReady
```

**根因**：HIDL 生成的代理方法（iface 库 `0x1a5c0`–`0x1a64c`）形如：

```asm
movi v0.2d, #0
add  x0, x0, #8
add  x1, x9, #0x30
str  xzr, [x8, #0x20]        ; ← 需要 x8 = Return<void> 的 sret 槽
stp  q0, q0, [x8]
b    _hidl_scoDataReceived@plt
```

原厂 ACL 路径调用前有 `mov x8, sp`（`0x3bd28: 910003e8`），v1 蹦桌漏了它，
x8 是野值 → `str xzr,[x8+0x20]` 写 `0x64540` 崩溃。

**v2 蹦桌（9 条指令，恰好填满 0x24 字节）**：

```asm
cmp  w21, #0x5          ; 仅处理 HCI ISO 包
b.ne 0x3be10            ; 其他类型：静默丢弃（同原厂行为）
ldr  x0, [x20, #0x10]   ; 回调包装器
ldr  x21, [x0]          ; vtable
ldr  x9, [x21, #0x80]   ; scoDataReceived（厂商分发从不调用此槽）
mov  x1, x19            ; const hidl_vec&
mov  x8, sp             ; Return<void> sret ← 关键修复
blr  x9
b    0x3be10
```

字节：`bf160071 01010054 800a40f9 150040f9 a94240f9 e10313aa e8030091 20013fd6 01000014`

**vtable 偏移实证**（解析 `/tmp/iface10.so` rela.dyn，vtable @`0x22040`，vptr=`0x22050`）：
`0x70`=hciEventReceived、`0x78`=aclDataReceived、**`0x80`=scoDataReceived**、`0x88`=isoDataReceived(@1.1)。

后续由 shim `HookedBinderTransact` 把 code 4 交易改写为 `@1.1::IBluetoothHciCallbacks` + code 5。

---

## 3. VINTF 只声明 @1.0 → `bt_hci_1_1_ == nullptr` → 音频根本没发出去

**现象**：`system/gd/hal/hci_backend_hidl.cc:169 sendIsoData: ISO is not supported in HAL v1.0`

**AOSP 源码**（`hci_backend_hidl.cc`）：

```cpp
HidlHci::HidlHci(Handler*) {
  hci_1_1_ = IBluetoothHci_1_1::getService();
  if (hci_1_1_) hci_ = hci_1_1_;
  else            hci_ = IBluetoothHci_1_0::getService();
}
void SendIsoData(...) {
  if (hci_1_1_ == nullptr) { log::error("ISO is not supported in HAL v1.0"); return; }
}
```

`lshal list -i` 证实 @1.1 已在 HAL 进程注册（可达），所以卡点不是注册而是 VINTF 声明。

**`hwservicemanager` 日志**：

```
getTransport: Cannot find entry android.hardware.bluetooth@1.1::IBluetoothHci/default
             in either framework or device VINTF manifest.
servicemanager: Caller(pid=17718,uid=1002,sid=u:r:bluetooth:s0)
             Could not find android.hardware.bluetooth.IBluetoothHci/default in the VINTF manifest.
```

**libvintf 语义**（`HalManifest.cpp:149 forEachInstanceOfVersion`）：

```cpp
if (manifestInstance.version().minorAtLeast(expectVersion)) return func(manifestInstance);
```

查询只按 **major version + interface + instance** 匹配，minor 用 `minorAtLeast`
（要求 major 相等且 minor ≥）。故 device manifest 声明 `@1.1` 即可同时满足 @1.0 与 @1.1 查询。

**修复**：`manifest_ukee.xml` bluetooth 块改 `<version>1.1</version>` + `<interface>`。
**生效验证**：`lshal` 双注册 `DM,FC Y @1.0` + `DM,FC Y @1.1` 同 pid；
`sendIsoData: ISO is not supported in HAL v1.0` = **0 次**。

---

## 4. shim hexdump 缓冲区溢出 → `__fortify_fatal` abort（播放必崩）

**tombstone（HAL pid 10933，`logcat -b crash` 计 7 次）**：

```
__fortify_fatal <- snprintf <- shim+0x7284
  <- android.hardware.bluetooth@1.0.so _hidl_scoDataReceived
  <- libbluetooth_qti_real.so BluetoothHci::initialize()::$_1  (0x3be08)
  <- DataHandler::OnPacketReady <- UartController::OnPacketReady
  <- HciPacketizer::OnDataReady <- UartController::OnDataReady
栈侧连锁：hci_backend_hidl.cc:83 serviceDied: The Bluetooth HAL died.
```

**根因**（`BluetoothHciHook.cpp` v3.13，ISO RX 日志）：

```cpp
char h[96]; int o2 = 0;
for (size_t q = 0; q + 16 <= sz && q < 64; q += 8)
  o2 += snprintf(h + o2, sizeof(h) - o2, "%02x%02x%02x%02x%02x%02x%02x%02x ", db[q]...db[q+7]);
```

q = 0,8,…,48 共 **7 轮** × 17 字符 = **119 > 96**。

`snprintf` 返回"本应写入长度"而非实际写入数：第 6 轮 `o2=85`、`size=11`、只写 10+NUL、
**返回 17** → `o2=102`；第 7 轮 `h+102` 越界、`sizeof(h)-102` **下溢成 `(size_t)-6`** → FORTIFY → abort。

反汇编铁证（`/tmp/shim_v313.so`）：
`7238 mov w27,#0x60`(96) / `7270 sub x1,x27,x9`(size=96-o2) / `7274 add x0,x26,x9`(dst=h+o2) /
`7284 bl snprintf@plt` / `729c add w23,w0,w23`(o2 += 返回值)

**附带重要结论**：这个崩溃栈本身证明 **ISO RX 全链路已通** ——
type-5 包经蹦桌到达 `_hidl_scoDataReceived`，且 shim 走进了 `code==5` 转发分支
（崩在 `sIsoRxForwarded` 计数处）。

**修复**：新增 `hexCat()`：

```cpp
static inline void hexCat(char* buf, size_t cap, int* off, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
// 实现：if (o + 1 >= cap) { *off = (int)cap - 1; return; }   ← 保证 size = cap - o >= 2 永为正
//       vsnprintf 后 if (n < 0) return;
//       if ((size_t)n >= cap - o) { *off = (int)cap - 1; return; }  ← clamp
//       *off += n;
```

`h[96]`→`h[160]`；全部 **9 处** hexdump 统一改 `hexCat`；所有缓冲区 `= {}` 初始化。
反汇编确认 shim 内仅剩 hexCat 内一处 `vsnprintf`（`0x6d70`）。

---

## 5. `adb push` 进模块目录 → SELinux label 错 → HAL 无法加载

**现象**：蓝牙完全起不来，栈侧 abort。

**tombstone（栈侧 pid 11028, tid gd_stack_thread）**：

```
Abort message: 'system/gd/hal/hci_backend_hidl.cc:110 operator():
                Unable to get a Bluetooth service after 500ms,
                start the HAL before starting Bluetooth'
栈：bluetooth::hal::HidlHci::HidlHci(Handler*)::'lambda'(unsigned int)
    <- bluetooth::os::Alarm::on_fire <- Reactor::Run <- Thread::run
（崩溃栈里完全没有 shim，与 v314 无关）
```

HAL 进程活着（pid 1460, ELAPSED 02:41）但 `lshal list -i` 为空。

**dmesg 铁证**：

```
avc: denied { map } for comm="android.hardwar"
  path="/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so"
  dev="dm-4" ino=2087
  scontext=u:r:hal_bluetooth_default:s0
  tcontext=u:object_r:adb_data_file:s0     ← 错在这里
  tclass=file permissive=0
```

**根因**：`adb push` 把文件 stamp 成 `adb_data_file`，挂载到 `/vendor`
后 label 不变（跟随源文件），`hal_bluetooth_default` 无权 `{ map }` adb_data_file。

**对照**：同目录 `audio.bluetooth.default.so` 与 `libbluetooth_qti_real.so` 都是 `vendor_file`
（当时用 `su -c cp` 部署），所以 v313 时代正常。

**修复**：`adb push` 到 `/data/local/tmp` → `su -c cp` 进模块目录 →
`chcon u:object_r:vendor_file:s0`。
**`restorecon` 不行** —— `/data/adb` 在 `file_contexts` 里本就是 `adb_data_file`，
restorecon 会把错 label 原样设回去。

---

## 6. VINTF manifest 四种错误写法 → 整份 manifest 解析失败 → bootloop

**现象**：卡厂商 logo，设备完全离线（`adb devices` / `fastboot devices` 均空）。

**根因**：错误写法让 libvintf 解析**整份** device manifest 失败 → VINTF 检查全拒 → bootloop。

**零写入预验证法**（临时 bind mount，事后 umount + md5 核对）：

```bash
adb shell 'cp /vendor/etc/vintf/manifest_ukee.xml /data/local/tmp/same.xml; chmod 644 /data/local/tmp/same.xml'
adb shell 'mount --bind /data/local/tmp/same.xml /vendor/etc/vintf/manifest_ukee.xml'
adb shell 'vintf dm | wc -l'      # 557 = 正常；0 = 解析失败
adb shell 'umount /vendor/etc/vintf/manifest_ukee.xml; md5sum /vendor/etc/vintf/manifest_ukee.xml'
```

**决定性对照**：bind mount 内容**相同**的文件 → `vintf dm` 557 行、与无挂载时逐行相同
（证明 bind mount 机制有效、`shell_data_file` label 不是问题）；
bind mount **改动过**的文件 → `vintf dm` **0 行**、`Device Manifest? DOES NOT EXIST`。

| 写法 | `vintf dm` | 说明 |
|---|---|---|
| 两个独立 `<hal>` 块（各一个 fqname） | 0 行 | `HalManifest::shouldAdd`（`HalManifest.cpp:41`）+ `HalGroup.h:165 addInternal`：同 package 多块若 major version 集合重叠则后者被**静默丢弃** |
| 同一 `<hal>` 块内两个 `<fqname>` | 0 行 | 同上 |
| `<version-range><min>1.0</min><max>1.1</max>` | 0 行 | `ManifestHal::isValid()` 拒绝同 major 多 minor |
| `<fqname>@1.1::IBluetoothHci/default</fqname>` | 557 行 ✅ | 可用 |
| `<version>1.1</version>` + `<interface>` | 557 行 ✅ | **采用**（assemble_vintf 标准格式） |

**原厂 `manifest_ukee.xml` bluetooth 块真容**（12037B, md5 `e6798c05eb67713485cbf79567d70f69`）：

```xml
<hal format="hidl">
  <name>android.hardware.bluetooth</name>
  <transport>hwbinder</transport>
  <fqname>@1.0::IBluetoothHci/default</fqname>
</hal>
```

**救援**：KernelSU 安全模式（音量减连按 3 次）→ safe mode 自动创建模块 `disable` 文件 →
`adb root` 拿 uid=0 → 删掉误加的 fragment → 推正确格式。

**教训**：排查初期用作对比的"原厂文件"是手写的（已是 `<version>1.1</version>`+`<interface>` 写法），
导致对 bootloop 根因的判断跑偏（dead-ends.md 第 3 节）。**对比基线必须从设备导出。**

---

## 7. 硬件 Offload 是物理死路（高通 SM7475 BSP 未实现 LE Audio ADSP 编码）

投入时间最多的一条死路，完整记录见 [`dead-ends.md`](dead-ends.md)。

**证据链**：路由成功切入 `ble_headset(20000000)`、AudioFlinger 持续写入（Total writes > 5000），
但耳机完全静音。动态追踪：

```
PAL: Device: open: Enter. device id 21 (PAL_DEVICE_OUT_BLUETOOTH_BLE)
btaudio_offload: audio_stream_start: state = AUDIO_A2DP_STATE_STOPPED
btaudio_offload: audio_get_codec_config: state = AUDIO_A2DP_STATE_STOPPED
PAL: Bluetooth: startPlayback: 1746: invalid encoder config
PAL: StreamPCM: start: 451: Rx device start failed with status -22
```

反汇编：`/vendor/lib64/btaudio_offload_if.so` 只含 `AUDIO_A2DP_STATE_*` 符号；
`/vendor/lib64/libar-pal.so`（`BtA2dp::startPlayback` @ `0xc5828`）处理 BLE 设备仍复用 `BtA2dp`，
A2DP 为 STOPPED → `audio_get_codec_config` 返回空（`cbz x0, 0xc5c44`）→ `invalid encoder config`
→ 强制关流丢数据。

---

## 8. HCI ISO 包头的 4 字节不能剥（曾用开关 `iso.strip4`，已删除）

HCI ISO 数据包里 handle/长度之后的 `[2B 序号][2B 长度]` 是 HCI 规范强制的
`Packet_Sequence_Number` + `ISO_SDU_Length`（Core Spec Vol 4 Part E §5.4.5，PB=0b00/0b10 时必须存在），
不计入 `Max_SDU`。早期曾误认为它是多余的 framed-SDU 头、是"超过 Max_SDU 导致芯片崩溃"的原因，
在 shim 里加了 `iso.strip4` 剥掉它。

剥掉会产生畸形包（PB=3 续片还会被误剥掉 4 字节音频）；实测 `strip4=0` 时包结构正确，
当时的崩溃依旧 —— 崩溃真因见第 9 节。**这 4 字节永远不能剥，该开关及代码已在 v4.5 删除。**

---

## 9. 合成 NCP（曾用开关 `ncpsynth=1`）让控制器缓冲溢出 → 控制器挂死 → SSR（固件 00570）

**证据（btsnoop + A/B 实测）**：

- `LE Read Buffer Size v2` 返回 `ACL 251×16, ISO_Data_Packet_Length=155, Total_Num_ISO_Data_Packets=3`。
  **控制器只有 3 个 ISO 缓冲**（155 是包长，曾被误读成缓冲个数）。栈侧 credit=3 是对的，不是 bug。
- 控制器的真实 NCP（0x13）**会 1:1 回到栈**（btsnoop：TX≈NCP≈150/s），"QTI HAL 吞 NCP" 不成立。
- shim 合成 NCP 一次发 33 个 credit → 栈向 3 缓冲的控制器超发 → 首包后 ~90ms 控制器不再响应
  → `READ_CLOCK(0x1407)` 2s 超时 → `UART err 0x51` → SSR → HAL 自杀 → 栈 abort。

| 测试 | 结果 |
|---|---|
| `strip4=1 ncpsynth=1`（旧默认） | 每次起流 ~2s 崩（共 4 次） |
| `strip4=0 ncpsynth=1` | 照崩（2 次） |
| `strip4=0 ncpsynth=0` | **不崩**，STREAMING 持续 10+ 分钟，ISO RX 200/s |

**修复**：当时的做法是把 `ncpsynth`、`strip4` 置 0；这两个开关及对应代码后来在 v4.5 删除了。

### 9.1 剩余问题：吞吐上限 150/s → 约 25% 丢帧（卡顿）

需求 200 SDU/s（2 CIS × 10ms），实际 ~150/s，栈 `dropping ISO packet, iso credits: 0` ~50/s。

- CIS 参数（`LE CIS Established`）：ISO_Interval 10ms，NSE=3，BN=1，FT=1，Transport_Latency 7.17ms，双向
- TX→NCP 占用时间 13.6–16.9ms（随起流相位变化），NCP 在 CIS anchor 后约 8ms 到达主机
- 栈每 10ms 一次性发 2 包、无 credit 即丢弃。占用 >10ms ⇒ 下一 tick 只剩 1 个 credit ⇒ 2,1,2,1… = **150/s**，
  与占用时间具体值无关；要 200/s 需要每包在 10ms 内回 NCP，实际不可达
- 耳机 PAC：`Max Codec Frames Per SDU = 1` ⇒ 不能靠多帧打包降包率
- 当时场景是 `LIVE`（双向，耳机麦克风也在流），因音频策略在连接时探测 BLE 输入留下录音元数据（后由第 11 节解决）；
  改成单向 MEDIA 可缩短 CIG 事件、提前 NCP，但不改变上面的离散 2,1,2,1 结构

**后续（v4.0/v4.1 实测）**：credit 代理已实现并部署（在飞 ≤3、真实 NCP 回补、每 5s 统计行）。
栈侧丢包归零（栈恒满 credit），但控制器仍只完成 150/s，多余的 ~50/s 改在 shim 队列里丢 ——
天花板不在主机调度，见第 12 节。

---

## 10. 无声 / 卡顿排查决策树

现象：耳机连上、ASE 显示 STREAMING，但没声音。

```
1. sendIsoData: ISO is not supported in HAL v1.0 ?
   └─ 有 → bt_hci_1_1_ 为空 → 查 lshal 是否双注册 / manifest 是否声明 @1.1
        （根因 3）

2. avc: denied { map } ... adb_data_file ?
   └─ 有 → SELinux label 错 → chcon vendor_file（根因 5）

3. Invalid packet type rcvd 0x5 / SsrCleanup / Hardware Error 0x0F ?
   └─ 有 → UART 掩码未放行 type 5 → 查 0x503d8 补丁是否在（根因 1a）

4. __fortify_fatal <- snprintf ?
   └─ 有 → shim hexdump 溢出 → 用当前源码重新构建 shim（根因 4）

5. remove_iso_data_path: No such iso connection: 0xffff ?
   └─ 有 → ISO RX 蹦桌未生效 → 查 0x3bdec 补丁 + vtable 0x80 槽（根因 1c / 2）

6. ASE 全 IDLE / handle 65535 + StartStream: current state: IDLE ?
   └─ 协商没成功 → 查 PACS 能力 / codec 配置

7. invalid encoder config / btaudio_offload STOPPED ?
   └─ 走到了 offload 路径 → system.prop 没关 offload（根因 7）

8. on_hci_timeout READ_CLOCK(0x1407) + UART err 0x51 + SSR，且起流后 ~2s 发生 ?
   └─ 根因 9（当时是合成 NCP 造成的；该代码已删）。先确认固件与 ring 补丁：
      `0x2060` 应返回 22 个缓冲，日志里应有 `ring-patch: ISO(type5) table entry`

9. 不崩但卡顿 / 丢包 → 先看协议栈有没有 `dropping ISO`（`iso credits: 0`）：
   └─ 有 → credit 被打光：确认固件是 00680（`0x2060` 应返回 22 个缓冲，根因 12、13）
   └─ 无但仍有异常 → 抓 btsnoop 分析

10. 以上全无 + 链路全通 + AudioFlinger 在写 → 真正需要人耳验证
```

---

## 11. 录音元数据残留 → 听音乐也走 LIVE 双向场景

**现象**：听音乐时 `Current scenario: LIVE`、`Recording metadata context type mask: 0x0040`、
每个耳机的 source ASE 也在 STREAMING —— 耳机麦克风全程陪跑，白白拖长 CIG 事件。

**机制（日志证据链）**：
1. 耳机一连上，APM 探测 BLE 输入设备：`APM::HwModule: createDevice: adding dynamic device
   AUDIO_DEVICE_IN_BLE_HEADSET` → `adev_open_input_stream: device=0xa0000000` →
   `in_update_sink_metadata_v7: state=STANDBY, 1 track(s)`。
2. AOSP `stream_apis.cc` 的 `in_update_sink_metadata_v7` 开头就是
   `if (sink_metadata == nullptr || sink_metadata->track_count == 0) return;`
   —— **"0 tracks"（录音结束）的上报被 HAL 静默丢弃**，栈的
   `local_decoding_context_types_` 永远停在 LIVE。
3. 栈自己的 workaround（`client.cc` VBC close timeout + `audio_hal_is_capable_to_send_empty_metadata_`）
   依赖 `leaudio_use_context_type_manager` flag 的一组时序，在本设备上没有触发（日志实测
   `local_decoding_context_types_` 数分钟仍是 0x0040）。

**修复**：模块内 `audio.bluetooth.default.so` 把
`BluetoothAudioPortAidl::UpdateSinkMetadata` 打成空函数（`0x11548` autiasp + `0x1154c` ret，
**入口 paciasp 不能动，见 architecture.md 2.2**）。此 HAL 只服务 BLE 输入的元数据转发，
禁掉后音乐走 MEDIA sink-only；通话场景由 inCallState/CONVERSATIONAL 驱动，不依赖这条路径。
代价：耳机麦克风录音（通话以外的录音 app）可能选不到录音场景。

**首次补丁翻车记录**：把入口 `paciasp` 改成 `ret` → 音频 HAL 每次开 BLE 输入都
`SIGILL (ILL_ILLOPC)`（paciasp 同时是 BTI 落点），crash loop ~15 次后热修复，未重启设备。

## 12. 固件 00570：3 缓冲 × 2 周期流水线 = 150/s 硬上限

**测量（shim 内打点，旧固件 00570，maxrtn=-1；该打点随 credit 代理在 v4.4 删除）**：

```
hold(send->NCP) avg=19880us max=24936us n=750, send call avg=128us
```

- **每包从下发到 NCP 恒 ~19.9ms ≈ 2×10ms 间隔**，与 CIG 事件长度无关。
- 3 缓冲 × (1/0.02s) = **150 包/s**。双耳需 200/s，缺口恒 25% 导致卡顿；单耳 100/s 满足。

## 13. 终局解决：升级高通官方 00680 固件（开放 22 ISO 缓冲）→ 双耳 200/s 满速零丢包

**突破依据**：
1. 逆向 `libbluetooth_qti_real.so` 的 `PatchDLManager::OpenPatchFile` 确认：固件加载在每次开蓝牙时从 `/vendor/bt_firmware/image/hpbtfw21.tlv` 读入 RAM 执行，**验签在芯片 ROM 里**，普通自制 patch 无法通过高通 RSA 验签。
2. 检索官方 `linux-firmware.git` 发现高通开源工程师（`jinwang.li@oss.qualcomm.com`）于 2026-09-29 提交了最新的 QCA2066/WCN6855 固件：
   - 手机出厂原版：`BTFW.HSP.2.1.0-00570-PATCHZ-1`（146,068 B）
   - 上游最新版本：`BTFW.HSP.2.1.0-00680-VER_PATCHZ-1`（163,332 B）
   跨越 110+ 个高通内部 changeset，天然附带高通官方 RSA 签名，芯片 ROM 顺利通过验签。

**实测结果（MEDIA 48_4 High Reliability）**：
- 芯片 `0x2060 (LE Read Buffer Size v2)` 真实返回：**22 个 ISO 缓冲区**（原厂为 3 个）！
- 双耳同时 STREAMING 40+ 秒持续统计（这行来自 v4.0 的 credit 代理，v4.4 已删除；
  `inflight=11/22` 说明当时的控制器缓冲数是 22）：
  ```
  isoproxy: enq=8008 sent=8007 drop=0 ncp=7996 acked=8006 inflight=11/22 maxq=1
  ```
- **速率精准恒定 200.0 包/秒，丢包恒 0**；在飞 11 个缓冲（富余 11 个，50% 冗余）；队列深度 1（无排队延迟）。
- **实测听感**：完全消除卡顿、爆音和毛刺，双耳音质完美。

**持久化落地**：
- 将固件存入模块目录 `$MODDIR/firmware/hpbtfw21.tlv`。
- 在 `$MODDIR/post-fs-data.sh` 中通过 `mount -o bind` 将其覆盖到 `/vendor/bt_firmware/image/hpbtfw21.tlv`（系统分区零写入）。
- 开机时在 Bluetooth HAL 启动前自动挂载生效，重启完全自愈。
