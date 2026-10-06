# 死路与判断失误复盘

**这一章的目的是浪费时间前先读一遍。** 每条都标了投入成本和"早该什么时候止损"。

> **固件版本前提**：第 4、7.2、7.3 节涉及蓝牙栈 / 控制器的测量，都是在**原厂蓝牙固件
> 00570**（控制器只有 3 个 ISO 缓冲）上得出的。当前使用上游固件 **00680**（22 个 ISO 缓冲），
> 这些数字和结论**不代表当前状况**。

---

## 1. 硬件 Offload（ADSP 编码）—— 最大的时间黑洞

**投入**：约 4-5 小时逆向 + 多轮重启验证。
**止损时机**：第一次看到 `btaudio_offload_if.so` 符号表里只有 `AUDIO_A2DP_STATE_*` 时就该停。

### 过程

表层阻塞逐个被解开（每解开一个都像是"快通了"，这是陷阱）：

| 阻塞 | 解法 | 解开后现象 |
|---|---|---|
| `/vendor/etc/le_audio_codec_capabilities.xml` 原厂不存在 → HAL 报 `supports 0 codecs` | 按 AOSP schema 补文件 | HAL 报 `supports 19 codecs` |
| `audio.bluetooth.default.so` 的 `init_session_type` 把 `0x20000000` 硬编码到 Session 4（软件） | 二进制补丁 `0x13620/0x135e8/0x135cc` 改 Session 6/7/9 | `is not ready` 消失 |
| `AudioPolicyManager::getHwOffloadFormatsSupportedForBluetoothMedia` 硬编码只查 `primary` 模块 | 在 `primary` 里补 `BLE Headset Out` + `encodedFormats="AUDIO_FORMAT_LC3"` | `mCodecConfigOffloading size for le -> 1 (LC3)` |
| `audio_policy_configuration.xml` 的 bluetooth 模块手写残缺（无 BLE 输入、无 A2DP） | 补全 mixPort/devicePort/routes | 4 模块齐全 |
| `checkOutputsForDevice(): No output available for device 20000000` | 同上 | — |

**最终路由成功切入** `ble_headset(20000000)`，`dumpsys media.audio_flinger` 显示
`Patch 52 ... first device type 20000000`，歌曲自动切歌（Track 3 → Track 4），无掉线无 SSR。

> 后续：这条路最终放弃（改用软件编码，见 `root-causes.md`）；上表补的
> `le_audio_codec_capabilities.xml` 也随之从模块中删除 —— 关闭 offload 后它不再被读取。
看起来完全成功了 —— **然后耳机一点声音都没有。**

### 真正的死穴

```
PAL: Device: open: Enter. device id 21 (PAL_DEVICE_OUT_BLUETOOTH_BLE)
btaudio_offload: audio_stream_start: state = AUDIO_A2DP_STATE_STOPPED
btaudio_offload: audio_get_codec_config: state = AUDIO_A2DP_STATE_STOPPED
PAL: Bluetooth: startPlayback: 1746: invalid encoder config
PAL: StreamPCM: start: 451: Rx device start failed with status -22
```

- `/vendor/lib64/btaudio_offload_if.so`：符号表只有 `AUDIO_A2DP_STATE_*`
  （`STANDBY`/`SUSPENDED`/`STARTED`/`STOPPED`）—— **纯粹的经典蓝牙 A2DP Offload 胶水层，
  没有任何 LE Audio 实现**
- `/vendor/lib64/libar-pal.so` `BtA2dp::startPlayback` @ `0xc5828`：处理
  `PAL_DEVICE_OUT_BLUETOOTH_BLE`（Device 21）时**仍复用 `BtA2dp` 类**，
  调用 `audio_stream_start(6)`。当前连的是 LE Audio 单播而非 A2DP → `audio_get_codec_config`
  返回空（`cbz x0, 0xc5c44`）→ `invalid encoder config` → 关流丢数据

**结论**：Redmi Note 12 Turbo（SM7475）vendor blob 从未完成 LE Audio 的 ADSP 硬件编码。
开启硬件 offload 只会把音频送进未完工的高通专有库被静音丢弃。

**教训**：
1. "解开一个表层阻塞"和"接近成功"是两件事。每次解开阻塞后都应该问：**这个改动如果没有
   产生预期效果，它单独证明了什么？** 本例中 XML 补到 19 codec 只是让 HAL 承认有能力列表，
   不代表底层实现了编码。
2. **应该在动手前先 grep 闭源 blob 的符号表**。`nm -D btaudio_offload_if.so | grep -i leaudio`
   一条命令就能省 4 小时。

---

## 2. 用 `mount -o bind` 覆盖 `/vendor` 文件做热验证

**做法**：为了不重启就验证，直接 `mount -o bind` 覆盖 `/vendor/etc/...` 和 `/vendor/lib64/...`。

**后果**：
- `SELinux: avc: denied { execmem }` + `couldn't map segment: Permission denied` → HAL 起不来
- 多次挂载叠加，旧 inode 残留，`umount` 后视图仍不对
- 绕过了模块，状态无法随模块启用/禁用统一管理

**正确做法**：只改模块目录内的文件。已存在的文件原地覆盖（同 inode）即时可见，
配合 `stop/start vendor.bluetooth-1-0-qti` 重启 HAL，**不需要重启手机**（先停 HAL 再覆盖，见 7.5）。
bind mount 只用于 VINTF XML 的零写入预验证（见 operations.md 第 4 节），且必须 umount + md5 核对。

---

## 3. 拿错了"原厂文件"做对比

**背景**：要让系统使用 HIDL 1.1 蓝牙 HAL，需要修改 VINTF manifest（`manifest_ukee.xml`）里的
蓝牙声明。改错会导致 bootloop（见 root-causes 第 6 节）。排查时需要拿改动后的文件和原厂文件对比。

**问题**：仓库里当作"原厂文件"保存的那份 `manifest_ukee.xml`（12136B）其实**不是从设备上
导出的**，而是调试过程中手写的，内容已经是改过的 `<version>1.1</version>` + `<interface>` 写法。
真正的原厂文件（12037B，md5 `e6798c05eb67713485cbf79567d70f69`）写的是
`<fqname>@1.0::IBluetoothHci/default</fqname>`。

**后果**：拿它做对比得出的所有差异和推断都是错的，bootloop 原因因此排查了四五轮。

**教训**：对比用的原厂文件必须从设备导出并记录 md5，不能凭记忆手写。
现在由 `scripts/dump_device_binaries.sh` 从设备导出。

---

## 4. 伪造 NCP 给协议栈"加额度"（固件 00570 时期）

**NCP 是什么**：HCI 的 *Number Of Completed Packets* 事件（event code `0x13`）。
控制器每发完一个数据包、空出一个缓冲，就用 NCP 告诉主机"又有 N 个缓冲可用了"。
主机侧协议栈按 NCP 记账（credit）：手里有 credit 才发包，没有就丢包。
credit 总数 = 控制器报告的缓冲数（`LE Read Buffer Size v2`，opcode `0x2060`）。

**当时的误判**：协议栈只有 3 个 credit，播放时大量 `dropping ISO packet, iso credits: 0`。
误以为是"控制器缓冲很多，但 HAL 把 NCP 吞了，导致 credit 饥饿"。

**试过的**：在 shim 里伪造 NCP，让协议栈以为控制器空出了更多缓冲：

| 方案 | 结果 |
|---|---|
| 按倍数放大 NCP 里的完成数（`isocred.mult`） | 协议栈 credit 计数下溢 → 大量超发 → 控制器 Hardware Error → 蓝牙崩溃 |
| 每发一包回一个 NCP（`ncpsynth=1`，`isocred.window=0`） | 协议栈不再丢包，但耳机周期性掉线 |
| 保持固定数量 credit 在途（`ncpsynth=1`，`isocred.window=12`） | 起流约 2 秒后控制器挂死 → SSR |
| 在 binder 层钩 `transact` 放大 NCP | 钩子生效，但芯片处理节奏不变 |

**真相**（btsnoop 抓包确认）：
- 固件 00570 的控制器**确实只有 3 个 ISO 缓冲**，3 个 credit 是真实上限，不是 bug
- 控制器的真实 NCP 会 1:1 传到协议栈，HAL 并没有吞掉
- 伪造 NCP 等于让协议栈往只有 3 个缓冲的控制器里硬塞包，必然挂死（root-causes 第 9 节）

**现状**：`ncpsynth=0`，不再伪造 NCP；shim 直发 ISO（v4.4 起连 credit 代理也一并删除）。
缓冲不足的问题最终靠换固件 00680（22 个缓冲）解决，不是靠主机侧算法。

**教训**：先测出下游的真实容量，再设计上游算法。

---

## 5. 主机 clang23 libc++ ABI 不兼容

**现象**：shim 编译链接都过，一加载就 SIGSEGV @ `ConcurrentMap::get`。

**根因**：Arch LLVM-23 头文件带 `abi:nn230101` tag + `std::map` 布局差异，
与设备平台 libc++ 不兼容。

**速查**：符号带 `B9nqn230101` 后缀 = 新 libc++ = 布局不兼容设备。
**正解**：用 NDK 30（clang 21.0.0 r574158c），与设备平台同代。
设备上部署版 `.comment` 可证实原构建即 NDK clang 21 + LLD 21。

---

## 6. 其他小坑（各几分钟到半小时）

| 坑 | 现象 | 正解 |
|---|---|---|
| `setprop ctl.restart android.hardware.bluetooth@1.0-service-qti` | "Failed to set property ... See dmesg" | init 服务名是 `vendor.bluetooth-1-0-qti`，用 `stop/start vendor.bluetooth-1-0-qti` |
| `setprop ctl.restart audioserver` | AudioFlinger 只加载 primary 模块，usb/r_submix/bluetooth 全丢 | 验证 policy 改动必须重启设备 |
| `setprop ctl.restart android.hardware.audio.service` | 失败 + policy 模块丢失 | 音频 HAL init 服务名是 `vendor.audio-hal`（rc 里带 `onrestart restart audioserver`） |
| `input keyevent 85` 恢复播放 | 是切换键，会误暂停 | `input keyevent 126` 或 `cmd media_session dispatch play` |
| 只声明 BLE 输出、不声明 BLE 输入端口 | `createDevice: could not find HW module for device type 'AUDIO_DEVICE_IN_BLE_HEADSET'` → `APM failed to make available LE Audio device` → 路由永不切到耳机 | 必须声明 `BLE Headset In`。它会让栈选 LIVE 双向场景，由 audio HAL 补丁解决（root-causes 第 11 节） |
| BLE 端口加进 `primary` 模块 | `onWriteError: write error -22 usecase(deep-buffer-playback)` 循环 | QTI primary HAL 不处理 BLE 设备 |
| 误判崩溃归因 | v313 崩在 shim `snprintf`，先怀疑了蹦桌指令、SELinux、芯片固件，绕了一大圈 | tombstone 栈顶函数名就是答案，先读栈顶再猜 |
| `persist.vendor.service.bdroid.sibs=false` | 设为 false 实验，速率不变 | 无效果，不要改 |

---

## 7. v4.x 系列（固件 00570）：换掉“哪种主机调度”都不能胜过固件流水线

**投入**：v4.0 credit 代理 + v4.1 CIG 参数截断，约 2 小时、5 次热部署、1 次重启。
**止损时机**：第二次看到“改了调度参数但吞吐纹丝不动地钉在 150.0/s”时就该测 hold 了
（后来一测就一锤定音：`send call avg=128us`、`hold avg=19880us`）。

### 7.1 vendor 的 `audio_set_configurations.json` 是死文件

模块里改的 `/vendor/etc/bluetooth/le_audio/audio_set_configurations.json`（293KB）栈根本不读：
`libbluetooth_jni.so` 的字符串证据 —— 配置全部来自 `/apex/com.android.bt/etc/bluetooth/le_audio/`，
而 `/apex` 在红线分区清单里。改 apex 才能影响场景→配置选择。

### 7.2 credit 代理不能提高吞吐

代理把栈侧丢包归零、每包补发时刻紧跟 NCP（距下一 anchor 最多 8ms 余量），
但 NCP 需求恒 ~19.9ms（2×10ms）：瓶颈在固件 SDU 流水线，不在主机（root-causes 12）。
代理仍保留：它把丢包收敛到 shim（延迟有界），且栈侧 credit 零抖动。

### 7.3 MEDIA 场景 + 3 缓冲 = 40ms/BN=4 灾难

去掉录音元数据后栈选 `48_4_High_Reliability`（maxlat 100ms）→ 控制器选 ISO_Interval=40ms、
BN=4、FT=2 → 双耳每间隔需 8 缓冲 → **38/s**（比 LIVE 还糟）。v4.1 截 maxlat=10ms 恢复
10ms/BN=1。教训：**场景与 qos 目标一起决定控制器调度；换场景后必须重测**。

00680（22 缓冲）上重新验证过，结论相同：不加截断仍是 `ISO_Interval=40ms`、传输延迟 84.57ms，
流建立瞬间丢 6–8 包；截到 10ms 后为 10ms / 7.21ms、零丢包。这条不是 00570 专有现象。

### 7.4 改函数入口不能碰 paciasp

把 `UpdateSinkMetadata` 的 `paciasp` 改成 `ret` → PLT 间接调用没有 BTI 落点 →
音频 HAL `SIGILL (ILL_ILLOPC)` crash loop ~15 次。正解：保留 paciasp，从第二条指令起写
`autiasp; ret`。

### 7.5 在跑的 HAL 面前 `cp` 覆盖它的 .so

`cp` 原地重写同一 inode，运行中的 HAL 已 mmap 该文件，代码页被换 → 老 HAL SIGSEGV
（tombstone 栈顶在 impl-qti.so）。deploy 脚本已改为先 stop 后 cp 再 start。

## 总结：判断误区

1. **把"能解释现象的机制"当成"根因"**。UART `0x51` / `Hardware Error 0x0F` 被当成固件缺陷查了很久，
   实际是用户态 HAL 主动 SSR。
2. **在错的基线上做增量推断**（manifest 事件）。
3. **不先测下游能力上限就设计上游算法**（credit 倍增）。
4. **偏好能立即验证的临时手段，而不是走模块**（bind mount）。
5. **忽略最便宜的检查**：grep 符号表 / 读 tombstone 栈顶 / 从设备提取基线。
