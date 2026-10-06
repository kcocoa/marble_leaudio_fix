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
│   │    = 我们的 shim（SONAME 继承原厂名，进程无感知）            │
│   │    └─ dlopen → /vendor/lib64/hw/libbluetooth_qti_real.so   │
│   │                = 原厂真实实现（618KB，已打 4 处二进制补丁）   │
│   └─ 兄弟服务 FM / ANT / SAR / btconfigstore 靠 DT_NEEDED 转发  │
└───────────────┬──────────────────────────────────────────────┘
                │ UART /dev/ttyHS0（3.2 Mbps）
                ▼
          BT 芯片 hastings (BT 5.3) + 固件 hpbtfw20/21.tlv
```

音频侧（另一条独立链路）：

```
AudioFlinger
  → /vendor/lib64/hw/audio.bluetooth.default.so   （AIDL，SessionType 已打补丁）
    → IBluetoothAudioProviderFactory
      → session_type = LE_AUDIO_SOFTWARE_{ENCODING,DECODING}_DATAPATH
        （HARDWARE_OFFLOAD_* 已确认是死路，见 dead-ends.md）
  → 栈侧 hci_backend_hidl.cc sendIsoData()
```

## 2. 五种修改手段

### 2.1 XML overlay（最安全，先试这个）

模块内文件，KernelSU magic mount 到 `/vendor`。

**行为差异（重要）**：
- 修改模块里**已存在**的文件 → 即时生效（magic mount 是 inode bind，`/vendor` 视图同步）
- **新增**文件（如全新的 `vendor/etc/` 顶层文件）→ **必须重启设备**才被挂载

**SELinux**：XML 需要 `vendor_configs_file`，so 需要 `vendor_file`（见 operations.md 第 3 节）。

### 2.2 二进制 patch（改原厂 `libbluetooth_qti_real.so`）

必须**静态改文件**，不能运行时 `mprotect` 改 `.text`：
`SELinux: avc: denied { execmem } for ... scontext=hal_bluetooth_default`，运行时改必崩。

生成脚本：`../patch_qti_iso_rx.py`（从纯净源生成，可重复执行）。

| 地址 | 原值 | 改后 | 作用 |
|---|---|---|---|
| `0x503d8` | `mov w9,#0x501e`（掩码 `0x0012501e`） | `mov w9,#0x503e`（掩码 `0x0012503e`） | UART 掩码放行 type 5 (ISO) |
| `0x2f560` | Type 5 头大小 `0` | `4` | 2B handle + 2B length |
| `0x2dd52` | Type 5 跳转偏移 `34` | `30` | 复用 ACL 的 16-bit 长度解析 |
| `0x3bdec` | 错误日志块（9 条指令，0x24 字节） | ISO RX 蹦桌 v2 | 把 type 5 转成 scoDataReceived 调用 |

`libbluetooth_qti_real.so` 特性：**file offset == vaddr**，反汇编/找符号直接用绝对地址。

### 2.3 shim（C++，NDK30）

`bluetooth_hci_shim/BluetoothHciHook.cpp`，编译成 `android.hardware.bluetooth@1.0-impl-qti.so`
（**SONAME 继承原厂名**，进程加载时无感知替换）。

钩子点全部选在 **libhidlbase / libutils / iface 库**（无 CFI），
绝不钩 `libbluetooth_qti_real.so`（578 处 `__cfi_slowpath`，vptr shadow 必崩）。

侦察法：`llvm-objdump -d <lib> | grep -c "__cfi_slowpath@plt>"`

### 2.4 属性（`system.prop`）

`ro.` 属性需重启；`persist.` 可热改。shim 运行时开关见第 4 节。

### 2.5 KernelSU 安全模式（救援用）

开机画面后连按音量减 3 次（按-松）。安全模式下所有模块被 disable、`su` 二进制不可用，
但 LineageOS userdebug 可 `adb root` 拿到 uid=0。这是 bootloop 的唯一救援手段。

## 3. 构建配方

```bash
NDK=/opt/android-sdk/ndk/30.0.16248370    # clang 21.0.0 r574158c，与设备平台同代
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
  --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 \
  -I/tmp/ndk_platform_config -I/tmp/system_libhidl/transport/include \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  bluetooth_hci_shim/BluetoothHciHook.cpp -o /tmp/shim_v314.so \
  -L/tmp -lhidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all /tmp/shim_v314.so
```

**三个必须的 `-I` / `-L` 前置准备**（否则 dlopen 失败）：

| 路径 | 作用 |
|---|---|
| `/tmp/ndk_platform_config/` | NDK 的 `__config_site` 无条件 `#define _LIBCPP_ABI_NAMESPACE __ndk1`，`-D` 覆盖无效。拷贝该文件到前置 `-I` 目录，把 `__ndk1` 改成 `__1`（设备 libc++ 只有 `std::__1`） |
| `/tmp/system_libhidl/transport/include/` | HIDL transport 头 |
| `/tmp/*.so` | 链接用的设备库：`libhidlbase.so` `libutils.so` `libc++.so` `libbluetooth_qti_real.so`（从设备 `/vendor/lib64` 提取） |

**ABI tag 症状速查**：符号带 `B9nqn230101` 后缀 = 新 libc++ = 布局与设备不兼容 → 必须换 NDK 版本。

## 4. shim 运行时开关（`persist.vendor.leaudio.*`）

| 属性 | 默认 | 作用 |
|---|---|---|
| `isocred.window` | 12 | 窗口式 credit（保持 W 个在飞）；0 = 诚实回声。**热调，无需重启** |
| `ncpsynth` | 1 | 合成 NCP 事件 |
| `iso.patchring` | 1 | ring buffer type-5 补丁 |
| `iso.strip4` | 1 | 剥掉 4 字节 framed 前缀 `[2B 序号][2B 长度]` |
| `isocred.mult` | 6 | 早期乘法式 NCP —— **已知会下溢，勿用**（见 dead-ends.md） |

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

## 7. 抓日志

`adb logcat -d` 和 harness 起的 `logcat > file &` 都会漏（缓冲小、进程被 kill）。**有效手段**：

```bash
# 设备侧起后台 daemon（harness 杀不掉）
adb -s $SERIAL shell 'setsid logcat -v threadtime -b main,system,crash -f /data/local/tmp/btlog.txt &'
# ... 操作 ...
adb -s $SERIAL pull /data/local/tmp/btlog.txt /tmp/btlog.txt
```

ROM 特性：logcat 缓冲很小（~3 分钟 / ~12k 行），必须**实时抓**。
