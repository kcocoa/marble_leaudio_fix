# LE Audio (marble) — TODO 与问题记录

状态更新：2026-10-05 17:05（bootloop 事故恢复后）

---

## ⚠️ 本轮事故记录：VINTF fragment 导致无法开机

### 现象
- 部署 `vendor/etc/vintf/manifest/android.hardware.bluetooth@1.1.xml` fragment 后重启 →
  system_server 崩溃循环 → RescueParty 触发（rescue_boot_count=1）→ 无法进入系统
- 通过 KernelSU 安全模式（开机画面后连按音量减 3 次，按-松）救回，adb root 删除 fragment

### 根因（实测确凿，logcat 抓到 libvintf 原始报错）
libvintf **两条规则**都会让整个 device manifest 解析失败（fatal，所有依赖 manifest 的
system 进程拿不到 manifest → system_server 崩溃）：

1. **跨文件 FqInstance 冲突**：fragment 与主 manifest (manifest_ukee.xml) 对同一
   接口+实例声明（无论版本异同）：
   ```
   Cannot add manifest fragment ...: HAL "android.hardware.bluetooth" has a conflict:
   Conflicting FqInstance: @1.0::IBluetoothHci/default (from manifest_ukee.xml)
   vs. @1.1::IBluetoothHci/default (from fragment)
   ```
2. **同一 `<hal>` 块内重复 major**：`<fqname>@1.0::...` 与 `<fqname>@1.1::...` 并存：
   ```
   Illformed file: Duplicated major version: 1.0 vs 1.1
   ```

### 关键测试方法（已验证有效，无需重启！）
```bash
# bind-mount 临时替换 vendor 上的 VINTF 文件（umount 即恢复，零风险）
adb shell mount -o bind /data/local/tmp/test.xml '/vendor/etc/vintf/manifest/xxx.xml'
adb shell logcat -c
adb shell /system/bin/vintf dm        # 正常应输出 ~557 行；0 行 = 解析失败
adb logcat -d | grep "E vintf"        # ← libvintf 的真实报错原因在这里！
adb shell umount '...'
```
对照组结论：改 gnss 内容=正常；任何形式重复声明 bluetooth = 解析失败。

---

## ✅ 已验证的正确 VINTF 方案（TEST P 通过，待部署）

**Pixel 式声明**：在主 manifest_ukee.xml 的完整副本中，把 bluetooth 块改为
`<version>1.1</version>` + `<interface>` 形式（**不要用 fragment！不要用双 fqname！**）：
```xml
<hal format="hidl">
    <name>android.hardware.bluetooth</name>
    <transport>hwbinder</transport>
    <version>1.1</version>
    <interface>
        <name>IBluetoothHci</name>
        <instance>default</instance>
    </interface>
</hal>
```
- `vintf dm` 完整解析（557 行），规范化输出 `@1.1::IBluetoothHci/default` ✓
- 与 Pixel 6 (gs101) 等 1.1 设备的官方 manifest 模式一致
- HIDL 语义：1.1 声明向下覆盖 1.0 的 getTransport 查询
- 已保存：`leaudio_marble_fix_v2/vendor/etc/vintf/manifest_ukee.xml`（待用户批准后部署）

### 部署步骤（下次继续时）
1. `adb push leaudio_marble_fix_v2/vendor/etc/vintf/manifest_ukee.xml → 模块 vendor/etc/vintf/`
2. `rm /data/adb/modules/leaudio_marble_fix/disable`（恢复 shim）
3. 重启（提醒：libvintf 缓存 + overlay 新文件都需要重启）
4. 验证顺序：`vintf dm` 正常 → `lshal` 出现 1.1 → 开 BT →
   logcat 抓 `BluetoothHciHook: onTransact: initialize_1_1 (code 6)` →
   连耳机放音乐 → `sendIsoData: forwarded N bytes` → ISO credits > 0

### 残留风险
- getTransport 对 1.0/1.1 的双向兼容已在解析层验证，但未在真机 hwservicemanager 全链路验证
  （Pixel 先例强烈支持可行）。最坏情况：1.0 查询失败 → BT 起不来但**系统正常**，可秒回退。
- manifest_ukee.xml 全文件替换会遮蔽该文件的 OTA 更新 → 每次 OTA 后需 diff 同步。

---

## ✅ v3 shim 已完成（本会话成果，全部实测通过）

### 修复的 3 个 ABI bug
1. Shadow vtable 前缀错位：vptr 必须指向 `_ZTV+3`（拷贝 vptr[-3..+43] 完整 Itanium 前缀）
2. `sp<T>` 命名空间：真实是 `android::sp`（非 `android::hardware::sp`）
3. `sp<T>` 非平凡返回 ABI：真实 sp 带析构 → 8 字节也走 x8 sret（空析构函数复刻）

### 其他修复
- shim SONAME 继承原名 + DT_NEEDED 转发 `libbluetooth_qti_real.so`（SONAME 已改）
  → FM/ANT/SAR/btconfigstore 的 DataHandler 依赖恢复正常
- `sendIsoData` 非 oneway → handler 必须写 Status::ok() 到 reply
- `initialize_1_1` 完整实现（enforceInterface(1.1) → readNullableStrongBinder →
  new BpHwBluetoothHciCallbacks → impl->initialize(vptr[13]) → Status::ok()）

### 实测状态（16:28-16:33，重启前）
- 服务稳定无 crash，FM/SAR/ConfigStore 正常注册
- `android.hardware.bluetooth@1.1::IBluetoothHci/default` 已注册进 hwservicemanager
- BT 正常开启（Fluoride 因 VINTF 缺失退回 1.0 —— 即上面待部署方案要解决的最后一环）

### 文件
- 源码: `bluetooth_hci_shim/BluetoothHciHook.cpp`（v3）
- 构建产物: `leaudio_marble_fix_v2/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so`
- 真实库: `leaudio_marble_fix_v2/vendor/lib64/hw/libbluetooth_qti_real.so`（SONAME 已改）
- 设备上模块当前 disabled（内容为 v3 shim，无 vintf 文件，boot 安全已验证）

---

## 待用户决定
1. 是否部署 Pixel 式 manifest_ukee.xml 并重启验证完整 ISO 数据通路
2. 回退选项：删模块 vintf 目录=纯 shim 状态；删模块目录=完全 stock

## 备忘
- KernelSU 安全模式进入：开机画面后连按音量减 3+ 次（按-松）；安全模式下 su 二进制不可用，
  但 LineageOS userdebug 可用 `adb root`
- 设备 SKU：`ro.boot.product.vendor.sku=ukee` → 活动主 manifest 是 `manifest_ukee.xml`
  （`hardware.sku=marble` 不用于 vendor manifest 选择）

---

## ✅ 2026-10-05 17:31 第二轮部署验证 —— 1.1 链路全通！

### 成果（实测）
- VINTF: Pixel 式 `<version>1.1</version>` manifest 生效，getTransport 通过，bootloop 修复
- hwservicemanager: 1.0 + 1.1 双注册（同一 pid）
- **Fluoride 走 initialize_1_1 (code 6) → stock initialize status=0 → BT state: ON，零崩溃**
- HCI 数据流正常（UART /dev/ttyHS0、patch 下载、54 条 vendor 日志）

### 本轮修复的 3 个新 bug（shim v3.1）
1. **initialize 调用 sret 错位**（致命 SIGSEGV）：`Return<void>`(40B) 必须走 x8 sret，
   x0=this, x1=&sp。手写 fn-ptr 时把 sret 放进了 x0 → callee 把 sretBuf 当 this、
   impl 当 callbacks → 全链路雪崩。修复：`typedef HidlReturnVoid40 (*)(void*, void*)`
   结构体返回，clang 自动 x8 sret。（tombstone_42 反汇编逐寄存器证实）
2. **主机 clang23 libc++ ABI 不兼容**（SIGSEGV @ ConcurrentMap::get）：
   Arch LLVM-23 头文件带 `abi:nn230101` tag + std::map 布局差异。
   修复：改用 NDK r30 (clang 21.0.0, r574158c) 工具链 —— 与设备平台同代。
   `NDK=/opt/android-sdk/ndk/30.0.16248370 .../bin/clang++ -target aarch64-linux-android24`
   （部署版 .comment 证实原构建即 NDK clang 21 + LLD 21）
3. **Return<void> 33 字节陷阱**：stock initialize 成功路径只写 sret 前 33 字节
   （`stp q0,q0` + `strb #0x20`），尾部 7 字节 padding 是栈垃圾。
   "全 40 字节全零=OK"检查误报错误 → 客户端拿到空 reply → BT OFF。
   修复：只检查 bytes[0..3]（return_status 枚举，0=OK）。

### 免重启热更新方法论（已验证）
KernelSU magic-mount 是 inode bind：`cat 新so > 模块内文件` 后 /vendor 视图即时同步，
`su -c setprop ctl.restart vendor.bluetooth-1-0-qti` 重启 HAL 服务即加载新 shim，
无需重启手机。BnHw 回调包装对象死亡后 hwservicemanager 会自动重新注册 1.0+1.1。

### 关键 ABI 知识补充
- `BnHwBluetoothHci::onTransact` 返回 `status_t`(w0)，**不是** sret（prologue 直接
  拿 x8 当临时寄存器 `ldr x8,[x5,#0x20]` 取回调）—— 我们的 `int32_t` hook 签名本来就对
- stock `BluetoothHci::initialize` prologue：x19←x8(sret), x20←x1(cb), x21←x0(this)
- ABI tag 症状速查：符号带 `B9nqn230101` 后缀 = 新 libc++ = 布局不兼容设备

### 最终一步（待用户配合）
连上 LE Audio 耳机播放音乐，观察：
- `sendIsoData: forwarded N bytes`（code 7 路径）
- `dumpsys bluetooth_manager` 中 ISO credits
- LC3 音频输出

---

## 🟡 2026-10-05 第三轮：LE Audio 端到端打通，但音频严重卡顿（调查进行中）

### ✅ 已打通的部分（实测确认）
- **LE Audio 端到端已通**：耳机连接成功，Fluoride 走 1.1（initialize_1_1 code 6），
  ISO 数据经 sendIsoData (code 7) 真实到达芯片，耳机**有声音**
- 编解码协商：`Two-OneChan-SnkAse-Lc3_48_4_High_Reliability`（48kHz/10ms/120B SDU，双耳 = handle 0x5/0x6）
- HCI 抓包（btsnooz）：ISO 包 host→controller 正常，NCP 事件正常返回

### 🔬 卡顿根因（数字闭环，实测确凿）
```
芯片上报: ISO buffers = 3, buffer size = 155（LE Read Buffer Size V2 响应）
NCP 节奏: 每 ~78ms 一批 3 个 NCP（每批含 handle 0x5/0x6 各 1-2 个，完成数=1）
持续吞吐 = 3 credits ÷ 78ms ≈ 38 SDU/s/handle 对
需求 = 100 SDU/s（10ms 帧）→ ~62% 帧 Fluoride 侧丢弃 → 非常卡
```
- Fluoride 丢包日志铁证：`btm_iso_impl.h:562 send_iso_data: dropping ISO packet, iso credits: 0`
- 已排除的嫌疑（对照实验）：
  - ❌ UART 波特率：实测 patch 后成功切换 **3.2 Mbps**（"GetBaudRate: 3200000 bps"），带宽余量 10 倍
  - ❌ SIBS/IBS 片间休眠：`persist.vendor.service.bdroid.sibs=false` 后速率不变（~36/s）
- 待排除：CIS 无线事件真实节奏 vs NCP 报告节奏（无法从 HCI 侧区分，只能靠倍增实验反推）

### 🔧 v3.2 NCP 信用倍增方案（当前卡点）
**思路**：钩住事件回调，把 ISO handle 的 NCP"完成数"×N，让栈维持更深的在途管道，实测芯片真实吞咽能力。
- 实现：`HookedHciEventReceived` 解析 evt 0x13，对 ISO handle（由 sendIsoData 钩子记录）倍增计数
- 运行时系数：`persist.vendor.leaudio.isocred.mult`（默认 6，1=禁用）
- **卡点：钩子从未被触发**（无 evt 日志），事件走的路径绕过了我们钩的槽位

### 🔍 事件路径调查记录（关键发现）
1. wrapper 的 vtable 解码（运行时实测）：**hciEventReceived @ vptr[14]（offset 0x70）**，
   [13]=initializationComplete、[15]=aclDataReceived、[16]=scoDataReceived；
   vptr = _ZTV+0x18（虚继承 IBase 的 vbase offset 占 1 槽）
2. impl initialize 只跑了一次（我们的 code 6），mCb = 我们的 wrapper；impl 对象前 256B 无 cbObj
   （cb 存进了 DataHandler 成员 [dh+0x10]）
3. **vendor 库 (libbluetooth_qti_real) 开启 CFI：578 处 `__cfi_slowpath` 调用！**
   而 1.0 iface 库和 libhidlbase 无 CFI —— 这解释了 impl/BnHw 侧钩子为何安全
4. 事件投递链（静态反汇编定位）：
   ```
   DataHandler::OnPacketReady (0x4798c)
     → std::function invoke: [__func_obj] vptr[6] @ +0x30（此调用无 CFI 检查！）
       → lambda operator() @ 0x3ba8c（__func vtable 0x91e20 slot[6]，文件内 vtable 槽位已解出）
         → x20 = [capture+0x10]（cb sp）；x21 = [x20]（wrapper vptr）
         → __cfi_slowpath(type, x21)   ← CFI 检查 wrapper vptr！
         → [x21+0x70] = hciEventReceived / [x21+0x78] = aclDataReceived（按包类型分发）
   ```
5. **未解矛盾**：若事件真的经我们的 shadow vptr，CFI 应 abort（服务没死）或钩子应触发（没触发）。
   可能解释（待验证）：
   a. x20 是 RefBase 偏移后的指针（cbObj+0x20），[x20]=第二个 vptr（未挂钩的次级 vtable）→ 事件走次级 thunk
   b. CFI 慢路径在此 ROM 上是 no-op（不 abort），但有其它原因钩子不触发
   c. logd 因 Fluoride 366 行/s 的丢包告警刷屏丢掉了我们的日志（已部分排除：tag 过滤后仍无）
   d. Fluoride 在 code-6 之后又调了 1.0 code-1（stock 自建未挂钩 wrapper 覆盖 mCb）——
      需 onTransact code 日志验证（源码已加，未编译部署）

### 📋 下一步计划（按优先级）
1. **编译部署 v3.2e**（源码已加 onTransact code 日志），不播音乐、无刷屏环境下重启 HAL 服务，
   记录所有 transaction code —— 验证假设 d：是否发生第二次 code-1 初始化
2. **dump wrapper 对象完整 216 字节**（shim 内加日志），枚举所有嵌入 vptr：
   - 找出次级 vtable（cbObj+0x20 等），解码其槽位，确认事件实际走的表
   - 若确认走次级 vptr：钩次级 vptr 的对应槽（同样的 shadow 手法）
3. **CFI 实测**：确认 __cfi_slowpath 对非法 vptr 是否真的 abort
   （低风险试错：临时钩次级 vptr 看服务是否崩溃）
4. **备选钩点（若 wrapper 侧全被 CFI 卡死）**：std::function __func 对象
   - __func 是 vendor 在堆上 new 的，vptr 可换（heap 可写）
   - OnPacketReady→operator() 的间接调用**无 CFI 检查**（0x47c94 附近确认无 slowpath）
   - 钩 operator() (slot[6])：直接拦 (HciPacketType, vec)，按类型过滤 NCP 倍增
   - 需要：定位 DataHandler 里的 __func 对象地址（initialize 后从 impl dump 的堆指针链下钻）
5. **保底方案（若信用倍增彻底不可行）**：
   - 评估 ROM flag `leaudio_set_codec_config_preference` / codec 配置切换到更低的码率帧长组合
     （但 LC3 帧长只有 7.5/10ms 两种，38/s 的芯片节奏两种都喂不饱 —— 大概率无解，仅作记录）
   - 结论可能是：此芯片固件 ISO 消费节奏 ~26-52ms/SDU，与 LC3 10ms 帧根本不匹配，
     LE Audio 在 marble 上限于此固件缺陷（除非信用倍增证明芯片实际节奏是 10ms 而仅报告慢）

### ⚠️ 待清理项
- `persist.vendor.service.bdroid.sibs` 被我设为 false（实验用，无效果）→ 应恢复默认（未设/true）
- 源码中 onTransact code 日志（v3.2e）已编辑未构建；`impl dump` / evt 日志为临时诊断，验证后应移除
- `/tmp/shim_v32*.so` 各调试构建产物；部署版本 = v3.2d（shim_v32d.so, 47128B）
- 模块内当前文件：v3.2d shim + Pixel 式 manifest + audio 配置；模块 enabled

### 🟡 2026-10-05 第五轮（20:19-21:02）：合成 NCP 实验 —— 瓶颈定性完成

### 本轮完整证据链（实测铁证）
1. **QCI 无标准 NCP**：evt code 直方图（3072 parcel）—— 0x0e:565/0x14:78/0x3e:26，
   **无 0x13**。QCI 在 DataHandler 层拦截 NCP 用于自己的流控，从不转发 Fluoride
2. **CC opcode 0x1407 与 sendIsoData 1:1**（1157≈发送数）—— QCI UART 协议
   无 ISO 包类型，每个 ISO 数据包包装成 vendor 命令 0x1407 发芯片，每包回一条
   CC（携带递增计数器 +319/条）→ **芯片真实处理节奏 ≈ 27ms/包 → 固有吞吐 ≈ 36 SDU/s**
3. **Fluoride credits 来源**（AOSP 源码 btm_iso_impl.h）：初始 3（LE Read Buffer
   Size V2）+ 仅 NCP 0x13 补充 → 无 NCP 下 Fluoride 在 36/s 低速下靠 3 credits
   反复偿还 + 某种残余机制勉强运行（dropping 刷屏）
4. **v3.3i（合成 NCP × mult=6）**：used_credits 下溢 → ISO 洪泛 →
   **固件 Hardware Error 0x0f** → 全栈崩溃级联（已回退修复）
5. **v3.3j（合成 NCP count=1 FIFO）**：dropping 0、credits 满、
   发送率放开 —— 但 **耳机整机周期性掉线重启**（双耳同断 reason -2，
   30-60s 周期，TWS 同步重启行为），听感无任何改善（用户实测）

### 结论（瓶颈定性）
**卡顿根因在闭源链路末端：hastings 固件 ISO vendor 通道 ~36 SDU/s 的处理节奏，
与 LC3 10ms 帧的 100 SDU/s 需求物理不匹配。**软件层（shim/HAL/credit 机制）
已全部打通并验证到极限：credits 机制修好（合成 NCP）→ 发送端全速 →
空中/耳机侧崩溃。这不是主机软件能解决的：QCI 把 ISO 包裹成 vendor 命令的
设计本身就意味着固件内部有串行处理瓶颈，且耳机侧（Bluetrum 主控）
也无法承受超量 CIS 数据。

### 收尾状态
- 部署版：v3.3h（诊断直方图版，无合成 NCP，稳定基线 = 卡但不断）
- musictest/watch 已支持连续采样：流在线率、路由耳机%（STREAM_MUSIC 的
  Devices: ble_headset 判据，实测确认）、credits、丢帧、速率；
  恢复播放用 input keyevent 126（比 cmd media_session dispatch play 可靠，
  避开淘宝等后台 ERROR 会话抢占）
- 现象记录：淘 宝媒体会话在蓝牙断时报 ERROR「蓝牙音频已断开连接」并抢占
  active，自动恢复需定向 Twelve（keyevent 126 走 MediaSessionService 优先级）

### 剩余可试项（价值递减）
1. `leaudio_set_codec_config_preference` 切换 LC3 16_2/24_2（低码率：帧数不变
   但 SDU 减半，固件节奏若按字节计可能跟上）—— 机会渺茫
2. LC3 7.5ms 帧长（133 帧/s 更差，基本无意义）
3. 等厂商固件更新（hpbtfw20.tlv）或 ROM 移植同平台已通 LE Audio 机型的
   全套 vendor（高风险）
4. **接受现状**：LE Audio 在 marble 上的上限 = 本固件缺陷；详细记录留档

## 🟢 2026-10-05 20:15 第四轮：事件路径谜底 + v3.3c transact hook 上线

### 谜底（本轮实锤，推翻上轮三条假设 a/c/d）
1. **假设 d 否定**：v3.2e onTransact code 日志实测 —— 重启 HAL 后 Fluoride 直接 code 6（无二次 code 1），
   mCb 从未被 stock wrapper 覆盖
2. **假设 a 否定**：wrapper 216B dump + 主机侧 vtable 解码 —— BpCallbacks 的 4 个次级 vptr
   （+0x08/+0x10/+0x30/+0xC0）全是 BpHwRefBase/RefBase 生命周期方法，
   **没有任何 HCI 事件槽位**；hciEventReceived 只在主 vtable slot 14
3. **真相**：vendor lambda（0x3bb8c）经 `[capture+0x10]` 持有的 cb ≠ 我们的 wrapper ——
   stock initialize 后 vendor 内部重建了自己的 wrapper（同 binder），并在 
   `__cfi_slowpath(0x7b13…, vptr)` **显式 CFI 保护**下虚调 slot 14 ——
   这就是 wrapper vptr shadow 永不触发且不 crash 的唯一自洽解释

### v3.3c 方案：mRemote transact hook（CFI 安全路径，已部署）
- **原理**：Bp hciEventReceived 封送后 `mRemote->transact(code=2, data, reply, 0, onDone)`
   （0x19c94，vptr **slot 0**，调用方 = iface 库无 CFI）；ProcessState 每 handle 单例 BpHwBinder
   → vendor 重建的 wrapper 与我们的共享同一 mRemote（wrapper[3]）
- **实现**：code 6 初始化时钩 mRemote（BpHwBinder, libhidlbase 无 CFI）vtable slot 0 →
   transact(code==2) 时扫描 parcel data buffer 找自洽 NCP 结构（0x13,len=2+4n,3+4n≤size）→
   对 sendIsoData 记录的 ISO handle 倍增 count（persist.vendor.leaudio.isocred.mult, 默认 6）
- ** Parcel::data()/dataSize()** 直接链接 libhidlbase 符号
- 实测：`transact hook: evt parcel #1 (size=136)` —— 事件流实测经过 hook ✅

### 事故记录（v3.3 → v3.3b）
- v3.3 诊断 DumpVendorCallbackPath 裸解引用 dh+0x140 垃圾指针 → SIGSEGV → HAL 崩溃循环
  （fault addr 0x34cf000000018112）；已快速 cat 回 v3.2e 恢复
- v3.3b 改用 /proc/self/mem pread 探测（EIO 代替 SIGSEGV），但 fallback 堆指针判断写错
  （`>>44 != 0` 拒了 0xb4… 堆地址）→ "dh unreadable"；v3.3c 修正为 `>>48 == 0xb400`
- v3.3c 诊断实测：dh+0x10 = vendor 库地址（上轮 "cb 存 dh+0x10" 判断系误读）；
  dh+0x140/+0xb8 初始化时为 0（运行期才建）

### 🛠 新工具：leaudio_iterate.sh（本轮新增）
```bash
./leaudio_iterate.sh deploy <shim.so>  # 热更新+重启+健康监测, 崩溃循环自动回退 LKG
./leaudio_iterate.sh restart           # 只重启 HAL
./leaudio_iterate.sh rollback          # 回退上次部署前备份
./leaudio_iterate.sh watch [秒]        # 实时观测+摘要 (ISO帧/NCP倍增/evt parcel/丢帧/LE Audio状态)
./leaudio_iterate.sh status           # 只读状态总览
```
- 部署前自动备份当前版本到设备 /data/local/tmp/leaudio_lkg.so；健康监测 pid 变化即判崩溃循环

### 待办
- [ ] **音乐实测**（需用户配合）：连 LE Audio 耳机放音乐 →
      `./leaudio_iterate.sh watch 60` 观察 `NCP mult x6`、丢帧警告是否归零、卡顿是否消失；
      mult 可用 persist.vendor.leaudio.isocred.mult 调优 (1=禁用)
- [ ] 若 mult=6 丢帧仍高 → 试 12/24；若 chip 实际节奏跟不上（控制台压力大）→ 回调 mult
- [ ] 清理项：persist.vendor.service.bdroid.sibs=false 残留待恢复默认
- [ ] 验证稳定后：移除 dump 类诊断日志，把 v3.3c 固化为发布版，同步 staging 目录 + zip

### 📦 本轮产物
- `/tmp/shim_v33c.so`（部署中， 50848B）| v33b/v33c 未 strip 备份同目录
- 构建配方见上（v3.2e 记录）

## 📦 v3.2e 构建记录（2026-10-05 19:58，设备离线期间完成）

源码新增两项诊断（不动钩子逻辑）：
- `HandleInitialize11`：wrapper 构造后 dump 完整 216 字节（枚举嵌入 vptr，验证次级 vtable 假设 a）
- `HookedOnTransact`：code 1 在 code 6 之后出现时打 `SECOND INITIALIZE` 警告（直接验证假设 d）
+ 原有 onTransact code 全量日志（首次 24 条 + 每 256 条，code 7 热路径除外）

产物：
- `/tmp/shim_v32e.so`（48024B，stripped，与 v32d UND 符号集 diff 仅多 `_ZTINSt3__117bad_function_callE`，设备 libc++ 已导出 ✓）
- `/tmp/shim_v32e.unstripped.so`（调试用，含 symtab）
- DT_NEEDED / SONAME 与 v32d 逐项一致；HookedInterfaceChain sret 模式（x8）反汇编确认

**重建配方（重要，含本轮踩坑）**：
```bash
NDK=/opt/android-sdk/ndk/30.0.16248370  # clang 21.0.0 r574158c，与设备平台同代
# 坑 1：NDK 的 __config_site 无条件 #define _LIBCPP_ABI_NAMESPACE __ndk1，
#       -D 覆盖无效 → 符号变 _ZNSt6__ndk1，设备 libc++ 只有 std::__1 会 dlopen 失败
# 坑 2：不能只用 -D_LIBCPP_ABI_NAMESPACE=__1（重定义被文件里的 #define 覆盖）
# 正解：拷贝 __config_site 到前置 -I 目录，把 __ndk1 改成 __1（/tmp/ndk_platform_config/）
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
  --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 \
  -I/tmp/ndk_platform_config -I/tmp/system_libhidl/transport/include \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  BluetoothHciHook.cpp -o /tmp/shim_v32e.so \
  -L/tmp -lhidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
# 再 llvm-strip --strip-all（v32d 即 stripped，symtab 占 ~20KB）
# 链接用的设备库：/tmp/{libhidlbase,libutils,libc++,libbluetooth_qti_real}.so
```

### 待设备重新连接后的验证流程（无需重启，热更新）
1. `adb push /tmp/shim_v32e.so /data/local/tmp/` 后 `su -c 'cat ... > 模块内 .so'`（magic-mount 即时生效）
2. `su -c setprop ctl.restart vendor.bluetooth-1-0-qti`（不播音乐、无刷屏环境）
3. `logcat -s BluetoothHciHook` 记录全部 transaction code：
   - 出现 `SECOND INITIALIZE` → 假设 d 成立 → 下一步钩 1.0 initialize 交接或让 mCb 指向我们的 wrapper
   - 无二次 code 1 → 排除 d，分析 wrapper dump 里的次级 vptr → 钩次级 vtable 槽位
4. 验证后回退：`cat` 回 v3.2d（/tmp/shim_v32d.so 备份在）或 workspace 目录里的版本

## 📚 本轮新增硬知识（写入用）
- vtable 虚继承：vptr 地址点 = _ZTV+0x18（1 个 vbase offset 槽时）；vptr[-3] = vbase offset
  （BpCallbacks 场景 RefBase 在 +0x20，由 vptr[-3]=0x20 调整 —— stock 代码即这么用）
- APS2 packed reloc 解码替代法：直接 dd 读 .data.rel.ro 文件偏移（此库 file offset == vaddr）
  拿 vtable 槽位函数地址，配合 dynsym 反查函数名
- std::function ABI（libc++）：__func vtable：[0]=D1 [1]=D0 [2..5]=clone/destroy 系 [6]=operator()；
  vendor 代码里 `ldr x9,[x?,#0x30]; blr x9` + 前置 `__cfi_slowpath` = 虚调用指纹
- **CFI 侦察法**：`llvm-objdump -d | grep -c "__cfi_slowpath@plt>"` —— 钩子目标库必须做此检查；
  iface/hidlbase 库无 CFI（钩子安全），vendor impl 库重度 CFI（578 处，vptr shadow 高危）

## 2026-10-06 00:35 状态（重启验证 offload + policy 修复）

### 已解决
- **控制器崩溃**：offload 路径打通（`le_audio_codec_capabilities.xml` 补齐后 HAL 报
  `supports 19 codecs`），主机不再发 ISO 包 → 无 UART 失败、无 SSR、无 HAL 死亡，
  音乐可持续播放不再 4 秒中断。
- **policy 配置**：`audio_policy_configuration.xml` 的 `bluetooth` 模块补全
  （`ble output` / **`ble input`** / A2DP / hearing aid + 8 个 devicePort + routes）。
  重启后 `dumpsys media.audio_policy` 确认 4 模块齐全、BLE 端口齐全。

### 仍未解决：音频路由不切到耳机
现象（重启后）：
- 耳机 LE Audio 侧**全部 Connected**（LeAudioStateMachine ×2、CsipSetCoordinator ×2、
  VolumeControl ×2、Battery ×2）
- 栈侧**已设活动设备**：`[API call] setActiveDevice: device=XX:XX:XX:XX:A7:C8`
  + `[To AudioManager]: handleBluetoothActiveDeviceChanged ... isLeOutput: true`
- **但**：`dumpsys media.audio_policy` 的 Available output devices **只有 3 个静态设备**
  （Earpiece / Speaker / Telephony Tx），**BLE 耳机不在其中**
- 且本轮 logcat **完全没有** APM/AudioDeviceInventory 的
  `failed to make available` / `No output available` 日志
  → 说明「LE 设备连接状态」根本没送达 AudioPolicy（不是 openOutput 失败）
- 媒体路由：`selected = ROUTE_ID_BUILTIN_SPEAKER`，`LE_AUDIO_1 | ROSELINK`
  存在于 `mTransferableRoutes`，但 `RouterInfoMediaManager: onTransferFailure()` 报切换失败

### 环境事实（本 ROM）
- 设备时钟比主机慢约 30 分钟；`/proc/uptime` 才是可靠的启动时间
- logcat 缓冲很小（~3 分钟 / 约 12k 行），排查必须**实时抓**
- `setprop ctl.restart audioserver` → AudioFlinger 只加载 primary；
  `ctl.restart android.hardware.audio.service` 失败（See dmesg）→ 只能重启设备
- KernelSU overlay：改**已存在**的文件即时生效；**新增**文件需重启
- AIDL BT audio HAL：`/vendor/etc/vintf/manifest/bluetooth_audio.xml`
  （`android.hardware.bluetooth.audio` v5，`IBluetoothAudioProviderFactory/default`）
- `audio.bluetooth.default.so` 存在（policy 里 `halVersion="2.0"` 对应它）

### 下一步
1. 实时 logcat 抓「在输出切换器里选耳机」这一刻：看 AudioService 是否调用
   `setDeviceConnectionState(AUDIO_DEVICE_OUT_BLE_HEADSET, AVAILABLE)`
2. 若无 → 触发侧（LE 设备连接状态上报）问题：考虑重启蓝牙栈 / 关掉
   LE_AUDIO_BROADCAST profile / 检查 `mUnicastGroupIdDeactivatedForBroadcastTransition`
3. 若有 → 回到 `checkOutputsForDevice` 第二处失败，抓 HAL 的 `openOutputStream` 错误

## 2026-10-06 01:30 状态：硬件 Offload 路线已完整逆向并确认不可行

### 结论
1. **硬件 Offload 是死路**：高通 vendor 库 `btaudio_offload_if.so` 及 `libar-pal.so` 仅实现了 A2DP Offload，LE Audio 会话下 `audio_get_codec_config` 返回空配置 -> `invalid encoder config` -> PAL 拒绝启动音频流并丢弃数据，导致耳机彻底无声。
2. **软件编码（Host LC3）是唯一可行路线**：
   - 此前卡顿根因：credit 饥饿（3 credit @ 10Hz 轮询 = 30 SDU/s），已由 shim 的窗口 credit 机制 (`isocred.window`) 解决。
   - 此前死机根因：48_4 高码率（120B 帧 + 头 = 158B > Max_SDU 155B）导致切包为 155+3B，控制器无法处理 Unframed CIS 切包而崩溃。
   - 解决方案：切回软件模式，将码率设定为整包不超 155B（例如 48kHz 100B 或 32kHz 80B），彻底避免分包与死机。

### 待办
- [ ] 恢复 system.prop 为软件模式：`persist.bluetooth.leaudio_offload.disabled=true`，`ro.bluetooth.leaudio_offload.supported=false`
- [ ] 还原 audio_policy_configuration.xml 中 primary 模块的伪声明（避免与软件模式抢端口）
- [ ] 设定 LC3 偏好为 48_2 (100 octets) 或 32_2 (80 octets)
- [ ] 验证软件模式下流畅连续发声

## 2026-10-06 02:00 突破：查明软件模式 2 秒崩溃的真正元凶并完成 v3.12 修复

### 根因确诊（非控制器固件崩溃，而是高通用户态 HAL 主动触发 SSR）
分析动态日志（`01:49:46.871`）：
```text
vendor.qti.bluetooth@1.0-uart_controller: OnDataReady: Invalid packet type rcvd 0x5, invalid_bytes_counter_ = 0
vendor.qti.bluetooth@1.0-uart_controller: OnDataReady: Invalid packet type rcvd 0x6, invalid_bytes_counter_ = 1
vendor.qti.bluetooth@1.0-uart_controller: OnDataReady: Out Of Synchronization
vendor.qti.bluetooth@1.0-uart_controller: SsrCleanup: SSR triggered due to 10 sending special buffer
```
反汇编 `libbluetooth_qti_real.so` 中的 `UartController::OnDataReady`（`0x503d4`）：
- 原厂指令：`mov w9, #0x501e; movk w9, #0x12, lsl #16`，即校验掩码为 `0x0012501e`。
- 逐 bit 展开发现：掩码包含了 Type 1 (CMD)、2 (ACL)、3 (SCO)、4 (EVT)，**但唯独漏掉了 Type 5 (ISO) 的 bit 5（0x20）**！
- 当耳机或芯片通过 UART RX 上传 ISO 数据（麦克风或控制流）时，第一字节 `0x05` 命中掩码失败，第二字节（handle）再次失败，HAL 误以为 UART 严重失步，**主动调用 `SsrCleanup` 发送崩溃包强制拉低芯片并重启**！

### v3.12 修复（TX + RX 全双工打通）
1. `0x503d8` 指令补丁：`mov w9, #0x503e`（将 bit 5 设为 1，将 Type 5 声明为合法 H4 封包）。
2. `0x2f560` 头大小表补丁：Type 5 头大小由 `0` 改为 `4`（2B Handle + 2B Length）。
3. `0x2dd52` 跳转表补丁：Type 5 跳转偏移由 `34` 改为 `30`（复用 ACL 的 16-bit 载荷长度解析逻辑）。
4. 修复 W^X 保护：严格分离 `PROT_READ|PROT_WRITE` 与 `PROT_READ|PROT_EXEC`，消除 SELinux `execmem` 拦截。
5. 修复 `audio_set_configurations.json`：修正 155 octets 配置为 100 octets，防止帧切片。

## 2026-10-06 03:20 ISO RX 蹦床 v2：补上遗漏的 `mov x8, sp`

### 崩溃现象
HAL 进程 SIGSEGV（write fault @ 0x64540），栈：
```
#00 BpHwBluetoothHciCallbacks::scoDataReceived(const hidl_vec&)+24
#01 libbluetooth_qti_real.so 0x3be08 (蹦床的 blr x9)
    <- DataHandler::OnPacketReady <- HciPacketizer::OnDataReady
```

### 根因
HIDL 生成的代理方法（`initializationComplete/hciEventReceived/aclDataReceived/
scoDataReceived` @ iface 库 0x1a5c0-0x1a64c）形如：
```asm
movi v0.2d, #0 ; add x0, x0, #8 ; add x1, x9, #0x30
str  xzr, [x8, #0x20]        ; <-- 需要 x8 = Return<void> 的 sret 槽
stp  q0, q0, [x8]
b    _hidl_xxx@plt
```
原厂 ACL 路径在调用前有 `mov x8, sp`（0x3bd28: `910003e8`），
v1 蹦床漏了它，x8 是野值 -> `str xzr,[x8+0x20]` 写 0x64540 崩溃。

### v2 蹦床（9 条指令，恰好填满 0x24 字节）
把 x20 直接装进 x0（省一个槽位）腾出空间给 `mov x8, sp`：
```asm
cmp  w21, #0x5          ; 仅处理 HCI ISO 包
b.ne 0x3be10            ; 其他类型：静默丢弃（同原厂行为）
ldr  x0, [x20, #0x10]   ; 回调包装器
ldr  x21, [x0]          ; vtable
ldr  x9, [x21, #0x80]   ; scoDataReceived（厂商分发从不调用此槽）
mov  x1, x19            ; const hidl_vec&
mov  x8, sp             ; Return<void> sret —— 关键修复
blr  x9
b    0x3be10
```

### vtable 偏移实证（tombstone 确认）
- EVT(0x70)=hciEventReceived、ACL(0x78)=aclDataReceived、
  **0x80=scoDataReceived**（v1 蹦床确实调到了 scoDataReceived，槽位判断正确）
- 后续由 shim 的 HookedBinderTransact 把 code 4 交易改写为
  `@1.1::IBluetoothHciCallbacks` + code 5 (isoDataReceived)

### 操作教训
不再手动 `mount -o bind` 覆盖 /vendor，只维护模块目录文件，
由 KernelSU 开机 magic mount（只读）统一生效。

## 2026-10-06 VINTF @1.1 声明：libvintf 实测结论（bootloop 根因）

**根因**：`<hal>` 块写法错误会让 libvintf 解析**整份** device manifest 失败
（`vintf dm` 输出 0 行 / `Device Manifest? DOES NOT EXIST`），VINTF 检查全拒 → bootloop。

设备侧零写入验证手段（AGENTS.md 允许的临时 bind mount，事后 umount + md5 核对）：
```bash
adb shell 'cp /vendor/etc/vintf/manifest_ukee.xml /data/local/tmp/same.xml; chmod 644 /data/local/tmp/same.xml'
adb shell 'mount --bind /data/local/tmp/same.xml /vendor/etc/vintf/manifest_ukee.xml'
adb shell 'vintf dm | wc -l'      # 557 = 正常；0 = 解析失败
adb shell 'umount /vendor/etc/vintf/manifest_ukee.xml; md5sum /vendor/etc/vintf/manifest_ukee.xml'
```
（label 差异不是问题：`shell_data_file` bind mount 到 `vendor_configs_file` 目标实测可读；
内容相同的文件 dm 输出与无挂载时逐行相同。）

| 写法 | `vintf dm` | 说明 |
|---|---|---|
| 两个独立 `<hal>` 块（各一个 fqname） | 0 行 | `HalManifest::shouldAdd` 按 major version 去重，同名 package 第二个块被拒并让整份 manifest 失败 |
| 同一 `<hal>` 块内两个 `<fqname>` | 0 行 | 同上 |
| `<version-range><min>1.0</min><max>1.1</max>` | 0 行 | `ManifestHal::isValid()` 拒绝同 major 多 minor 的 versions |
| `<fqname>@1.1::IBluetoothHci/default</fqname>` | 557 行 ✅ | 可用 |
| `<version>1.1</version>` + `<interface>` | 557 行 ✅ | **采用**（assemble_vintf 标准格式） |

**语义要点**（`system/libvintf` `HalManifest.cpp:149 forEachInstanceOfVersion`）：
```cpp
if (manifestInstance.version().minorAtLeast(expectVersion)) return func(manifestInstance);
```
查询只按 **major version + interface + instance** 匹配，minor 用 `minorAtLeast`。
故 device manifest 声明 `@1.1` 即可同时满足 `@1.0` 与 `@1.1` 的 VINTF 查询
（hwservicemanager `canGet`/`canAdd` 均走此路径）——这也解释了原厂只声明 @1.0 时
`@1.0` 能注册、而栈侧 `IBluetoothHci_1_1::getService()` 返回 null 的现象。

**当前采用文件**：`leaudio_marble_fix_v2/vendor/etc/vintf/manifest_ukee.xml`，md5 `d600c9c552af2ccae0d0eb3461f40ac1`，12136B，与原厂 diff 仅 bluetooth 块一行。
