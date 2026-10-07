# 架构与修改手段

## 1. 三层架构

```
┌─ 用户态：AOSP GD 协议栈 ────────────────────────────────────┐
│ /apex/com.android.bt/lib64/libbluetooth_jni.so               │
│ 进程 com.android.bluetooth（栈侧），Fluoride C++17            │
│ · hci_backend_hidl.cc  ← ISO 数据发送/死亡通知都在这里         │
│ · btm_iso_impl.h        ← credit 池 / remove_iso_data_path    │
└───────────────┬──────────────────────────────────────────────┘
                │ HIDL hwbinder（@1.0 与 @1.1 同时注册）
┌───────────────▼──────────────────────────────────────────────┐
│ 厂商 HAL（vendor 侧）                                          │
│ init 服务名：vendor.bluetooth-1-0-qti                          │
│   对应 rc：/vendor/etc/init/android.hardware.bluetooth@1.0-service-qti.rc
│ 二进制：/vendor/bin/hw/android.hardware.bluetooth@1.0-service-qti │
│   ├─ /vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so │
│   │    = 本项目 shim（SONAME 继承原厂名，进程无感知）            │
│   │    └─ dlopen → /vendor/lib64/hw/libbluetooth_qti_real.so   │
│   │                = 原厂真实实现（618KB，已打 4 处二进制补丁）   │
│   └─ 兄弟服务 FM / ANT / SAR / btconfigstore 靠 DT_NEEDED 转发  │
└───────────────┬──────────────────────────────────────────────┘
                │ UART /dev/ttyHS0（3.2 Mbps）
                ▼
          BT 芯片 hastings (BT 5.3) + 固件 hpbtfw21.tlv（模块换为上游 00680）
```

音频侧（另一条独立链路）：

```
AudioFlinger
  → /vendor/lib64/hw/audio.bluetooth.default.so   （AIDL，UpdateSinkMetadata 已打补丁）
    → IBluetoothAudioProviderFactory
      → session_type = LE_AUDIO_SOFTWARE_{ENCODING,DECODING}_DATAPATH
        （HARDWARE_OFFLOAD_* 已确认是死路，见 dead-ends.md）
  → 栈侧 hci_backend_hidl.cc sendIsoData()
```

## 2. 修改手段

### 2.1 XML overlay（最安全，先试这个）

模块内 `vendor/` 下的文件在开机时由元模块挂载到 `/vendor`。

**行为差异（重要）**：
- 原地覆盖模块里**已存在**的文件（同一 inode）→ `/vendor` 视图即时可见
- **新增**文件（如全新的 `vendor/etc/` 顶层文件）→ **必须重启设备**才被挂载

**SELinux**：XML 需要 `vendor_configs_file`，so 需要 `vendor_file`（见 operations.md 第 3 节）。

### 2.2 二进制 patch

`libbluetooth_qti_real.so` 就是原厂 `android.hardware.bluetooth@1.0-impl-qti.so`：
`.dynstr` 里的 SONAME 改成 `libbluetooth_qti_real.so`（原地覆盖、补 NUL，文件大小不变），
原名让给 shim，shim 再以新名链接它。下表是 SONAME 之外的 4 处补丁。

必须**静态改文件**，不能运行时 `mprotect` 改 `.text`：
`SELinux: avc: denied { execmem } for ... scontext=hal_bluetooth_default`，运行时改必崩。

补丁脚本：`patch/patch_hal_binaries.sh`（QTI HAL 部分调用 `patch/patch_qti_iso_rx.py`，从原厂文件生成，可重复执行）。

| 地址 | 原值 | 改后 | 作用 |
|---|---|---|---|
| `0x503d8` | `mov w9,#0x501e`（掩码 `0x0012501e`） | `mov w9,#0x503e`（掩码 `0x0012503e`） | UART 掩码放行 type 5 (ISO) |
| `0x2f560` | Type 5 头大小 `0` | `4` | 2B handle + 2B length |
| `0x2dd52` | Type 5 跳转偏移 `34` | `30` | 复用 ACL 的 16-bit 长度解析 |
| `0x3bdec` | 错误日志块（9 条指令，0x24 字节） | ISO RX 蹦桌 v2 | 把 type 5 转成 scoDataReceived 调用 |

`libbluetooth_qti_real.so` 特性：**file offset == vaddr**，反汇编/找符号直接用绝对地址。

**`audio.bluetooth.default.so`（原厂 md5 `1b96c842…`）**：

只改这两处。硬件 offload 尝试期间曾改过 `init_session_type` 的 SessionType 映射
（见 dead-ends.md 第 1 节），软件编码方案**不需要**，也不应打。

| 地址 | 原值 | 改后 | 作用 |
|---|---|---|---|
| `0x11548` | `sub sp, sp, #0x90` | `autiasp` | 与入口 `paciasp` 配对 |
| `0x1154c` | `stp x29, x30, [sp,#0x40]` | `ret` | `BluetoothAudioPortAidl::UpdateSinkMetadata` 变空函数 → HAL 不再把录音元数据转给栈（root-causes 第 11 节） |

**入口 `0x11544` 的 `paciasp` 绝不能改**：它同时是 BTI 落点。函数经 PLT 被 `br` 间接调用，
把它换成 `ret` 会 `SIGILL (ILL_ILLOPC)`，音频 HAL 每次打开 BLE 输入都崩（实测）。
同理，任何改函数入口的补丁都要保留 `paciasp`/`bti c`。此库 text 段 file offset == vaddr。

### 2.3 shim（C++，NDK30）

`patch/bluetooth_hci_shim/BluetoothHciHook.cpp`，编译成 `android.hardware.bluetooth@1.0-impl-qti.so`
（**SONAME 继承原厂名**，进程加载时无感知替换）。

钩子点全部选在 **libhidlbase / libutils / iface 库**（无 CFI），
绝不钩 `libbluetooth_qti_real.so`（578 处 `__cfi_slowpath`，vptr shadow 必崩）。

侦察法：`llvm-objdump -d <lib> | grep -c "__cfi_slowpath@plt>"`

### 2.4 属性（`system.prop`）

`ro.` 属性需重启；`persist.` 可热改。shim 运行时开关见第 4 节。

### 2.5 KernelSU 安全模式（救援用）

开机画面后连按音量减 3 次（按-松）。安全模式下所有模块被 disable、`su` 二进制不可用，
但 LineageOS userdebug 可 `adb root` 拿到 uid=0。这是 bootloop 的唯一救援手段。

### 2.6 固件（`vendor/bt_firmware/image/hpbtfw21.tlv`，由元模块挂载）

固件和其他 vendor 文件一样，放在模块的 `vendor/` 下由元模块挂载，标签和属主由 `customize.sh` 设置
（`bt_firmware_file`，`bluetooth:net_bt`，目录 0550、文件 0644）。模块不自己 `mount`。

`/vendor/bt_firmware` 是独立的 vfat 挂载点（`sde36`，嵌套在 `/vendor` 内）。内核的 overlayfs 不接受 vfat 做下层
（dmesg：`overlayfs: filesystem on '/vendor/bt_firmware' not supported`），所以**元模块用 overlayfs 时这条挂载会失败**，
并且 hybrid_mount 会回滚**全部**模块的挂载。这是有意保留的行为：遇到的用户应改用 vfs 或 magic mount
（hybrid_mount：`default_mode = "vfs"` 或 `"magic"`）。

- 控制器每次开蓝牙时从该路径读固件，验签在芯片 ROM 里，所以必须是高通官方签名的上游版本。
- 零写入系统分区，模块禁用或移除后完全还原。

### 2.7 `/apex` 重定向（`apex/`，由 `post-mount.sh` 加 vfs 规则）

蓝牙栈（`libbluetooth_jni.so` 和 LE Audio 预设 JSON）在 LineageOS 签名的 APEX `com.android.bt@361099999` 里，
**不能重打包**（没有签名密钥），元模块的 `system/apex/...` 也挂不到 `/apex`（只会建 `/system/apex/...` 的虚拟路径）。

做法：模块的 `apex/<路径>` 对应 `/apex/<路径>`，`post-mount.sh` 对每个文件执行
`hybrid-mount vfs rule add /apex/<路径> $MODDIR/apex/<路径>`。要点：

- hybrid_mount 的 vfs 是**内核里的路径重定向**，对所有进程生效，不依赖挂载命名空间，蓝牙进程重启不丢
  （bind mount 做不到：`com.android.bluetooth` 每次重启换 PID 和挂载命名空间）。
- 规则**只在运行时有效**，每次开机重新加；模块禁用或删除后下次开机自然消失。
- 规则按 inode 生效，`/apex/com.android.bt` 和 `/apex/com.android.bt@361099999` 两个路径都会看到替换后的内容。
  所以要导出原厂件，不能读 `/apex`，`dump_device_binaries.sh` 改从解压 APEX 镜像里取。
- APEX 版本不是 `361099999` 时脚本直接跳过（补丁偏移按该版本校准，拿旧文件替换新版本会崩）。
- 没有 hybrid-mount 时脚本只打日志后退出，不影响其他部分。
- 调试：改模块里的文件后 `sh post-mount.sh`（同一路径的规则会被替换），再 `cmd bluetooth_manager disable/enable`。

`audio_set_scenarios.json` 目前是**原厂原样**，只是留作调试入口：以后要换预设（例如把 `VND_Two-OneChan…155octs`
挪到 Media 列表最前，双耳就用 124 kbps，实测可用），直接改模块里这个文件。

`libbluetooth_jni.so` 的补丁见 `patch/patch_bt_jni_odd_octets.py`，根因和证据见 root-causes.md 第 14 节。

## 3. 构建配方

`scripts/build_shim.sh` 完成以下工作，前置文件放在仓库内 `build/`（不入库）：

| 路径 | 作用 / 来源 |
|---|---|
| `build/inc/__config_site` | 脚本自动从 NDK 生成：把 `_LIBCPP_ABI_NAMESPACE __ndk1` 改成 `__1`（设备 libc++ 只有 `std::__1`；`-D` 覆盖无效） |
| `build/inc/hidl/ConcurrentMap.h` | 取自 AOSP `system/libhidl/transport/include/hidl/ConcurrentMap.h`（只依赖 `<mutex>` `<map>`）；缺失时脚本自动从 `android.googlesource.com` 下载 |
| `build/lib/{libhidlbase,libutils,libc++}.so` | `dump_device_binaries.sh --with-build-deps` 从设备 `/system/lib64/` 导出 |
| `module/vendor/lib64/hw/libbluetooth_qti_real.so` | 打过补丁的原厂 HAL |

```bash
NDK=/opt/android-sdk/ndk/30.0.16248370    # clang 21.0.0 r574158c，与设备平台同代
T=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
$T/clang++ --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 -Ibuild/inc \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  patch/bluetooth_hci_shim/BluetoothHciHook.cpp -o module/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so \
  -Lbuild/lib -Lmodule/vendor/lib64/hw -lhidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
```

构建后自检：`llvm-readelf -d` 的 NEEDED 应与旧版一致；`llvm-nm -D --undefined-only` 与旧版 diff，
新增符号必须在设备库里存在（`llvm-nm -D --defined-only build/lib/libhidlbase.so`）。

**ABI tag 症状速查**：符号带 `B9nqn230101` 后缀 = 新 libc++ = 布局与设备不兼容 → 必须换 NDK 版本。

## 4. shim 运行时开关（`persist.vendor.leaudio.*`）

只剩两个。标 *缓存* 的在首次加载后缓存（`static`），改完需 `stop/start vendor.bluetooth-1-0-qti`；
标 *实时* 的每次下发 `LE Set CIG Parameters` 时重读，`setprop` 后暂停→播放（CIG 重建）即可。

| 属性 | 默认 | 读取 | 作用 |
|---|---|---|---|
| `iso.patchring` | 1 | 缓存 | ring buffer type-5 补丁：QTI HAL 的 `PacketBuff::AddBuffNode` 分发表把 type 5 当成错误路径，ISO 包全被丢弃。必须为 1 |
| `cig.maxlat` | 10 | 实时 | 把 `0x2062` 的 Max_Transport_Latency 截到 N ms（0=关）。实测必要，见下 |

`cig.maxlat=10` 的实测依据（00680 固件 + 已打补丁的 HAL，数据取自 btsnoop 里的 `LE CIS Established` 事件）：

- **不截**：控制器选 `ISO_Interval=40ms`、`Transport Latency=84570µs`，in-flight 打满 22/22，
  流建立瞬间丢 6–8 包（`btm_iso_impl.h:562 … iso credits: 0`）；原因是长 ISO 间隔下首包到第一个 NCP
  之间 credit 恒为 0。
- **截到 10ms**：`10ms` / `7210µs`、in-flight 8/22、零丢包。

### 已删除的实验开关

00570 时期加过一批开关，实测都无用（默认值下对应代码路径永不走到），v4.4 / v4.5 已连同代码全部删除：

| 曾用属性 | 结论 |
|---|---|
| `isoproxy`、`isoproxy.qmax` | credit 代理，为 00570 的 3 个缓冲设计；00680 有 22 个，协议栈自身记账够用（删除前后 CIS 参数完全相同、零丢包） |
| `ncpsynth` | 合成 NCP 会让栈超出控制器缓冲数发包 → 控制器挂死 → SSR（`root-causes.md` 第 9 节） |
| `isocred.mult` | 放大 NCP 完成数 → 栈的 credit 计数下溢 → 大量超发 → 控制器 Hardware Error（`dead-ends.md`） |
| `isocred.window` | 只在 `ncpsynth=1` 时有意义 |
| `iso.strip4` | 那 4 字节是 HCI 规范强制的 `Packet_Sequence_Number`+`ISO_SDU_Length`，剥掉会产生畸形包（`root-causes.md` 第 8 节） |
| `cig.maxrtn` | 截 RTN 对吞吐无帮助，只会降低空口可靠性 |

删掉后控制器缓冲数改用 btsnoop 读（见 `operations.md`）。

## 5. AArch64 / HIDL ABI 硬知识

踩过坑换来的，改 shim 前必读：

- **`sp<T>` = 8 字节但带析构 → 返回走 x8 sret**。手写函数指针时必须用结构体返回让 clang 自动生成 sret，把 sret 塞进 x0 会全链路雪崩
- **`Return<void>` = 40B，`Status` = 32B**。HIDL 代理方法 prologue 有 `str xzr,[x8,#0x20]; stp q0,q0,[x8]`，**需要调用方先 `mov x8, sp`**
- **`BnHwBluetoothHci::onTransact` 返回 `status_t`(w0)，不是 sret**（prologue 直接拿 x8 当临时寄存器）
- **stock `initialize` 成功路径只写 sret 前 33 字节**（`stp q0,q0` + `strb #0x20`），尾部 7 字节是栈垃圾。"全 40 字节全零 = OK"检查会误报
- **vtable 虚继承**：vptr 地址点 = `_ZTV+0x18`（1 个 vbase offset 槽时）；`vptr[-3]` = vbase offset（BpCallbacks 场景 RefBase 在 +0x20）
- **`BpHwBluetoothHciCallbacks` vtable 槽位**（实证）：`0x70`=hciEventReceived、`0x78`=aclDataReceived、`0x80`=scoDataReceived、`0x88`=isoDataReceived(@1.1)
- **`std::function` ABI (libc++)**：`__func` 对象 vtable `[0]`=D1 `[1]`=D0 `[2..5]`=clone/destroy `[6]`=`operator()`
- **APS2 packed reloc 解码替代法**：直接 `dd` 读 `.data.rel.ro` 文件偏移（此库 file offset == vaddr）拿 vtable 槽位函数地址

## 6. VINTF manifest 选择

设备 SKU 由 `ro.boot.product.vendor.sku=ukee` 决定 → 活动主 manifest 是
`/vendor/etc/vintf/manifest_ukee.xml`。

`/vendor/etc/vintf/manifest.xml` **不存在**；per-SKU manifest 有 4 个
（ukee/cape/diwali/taro，内容完全相同且都含 IBluetoothHci）；
`/vendor/etc/vintf/manifest/` 39 个 fragment 里无 IBluetoothHci；
`/odm/etc/vintf/manifest_marble.xml` 只声明 secure_element + esepowermanager。

抓日志方法见 operations.md 第 5 节。
