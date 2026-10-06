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

### 2.2 二进制 patch（改原厂 `libbluetooth_qti_real.so`）

必须**静态改文件**，不能运行时 `mprotect` 改 `.text`：
`SELinux: avc: denied { execmem } for ... scontext=hal_bluetooth_default`，运行时改必崩。

补丁脚本：`scripts/patch_hal_binaries.sh`（QTI HAL 部分调用 `patch_qti_iso_rx.py`，从原厂文件生成，可重复执行）。

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

### 2.6 固件 overlay（`post-fs-data.sh` bind-mount）

`/vendor/bt_firmware` 是独立的 vfat 挂载点（`sde36`），嵌套在 `/vendor` 内。
模块 overlay 无法遮盖这个子挂载点，因此在模块的 `post-fs-data.sh` 中使用内存级 `mount -o bind`：

```bash
mount -o bind $MODDIR/firmware/hpbtfw21.tlv /vendor/bt_firmware/image/hpbtfw21.tlv
```

- 权限设为 `chmod 644`、`chown bluetooth:net_bt`、`chcon u:object_r:bt_firmware_file:s0`。
- 执行时机：`post-fs-data` 阶段，早于 `class hal` 的 `vendor.bluetooth-1-0-qti` 服务启动。
- 零写入系统分区，开机自启生效，模块禁用时完全不留痕迹。

## 3. 构建配方

`scripts/build_shim.sh` 完成以下工作，前置文件放在仓库内 `build/`（不入库）：

| 路径 | 作用 / 来源 |
|---|---|
| `build/inc/__config_site` | 脚本自动从 NDK 生成：把 `_LIBCPP_ABI_NAMESPACE __ndk1` 改成 `__1`（设备 libc++ 只有 `std::__1`；`-D` 覆盖无效） |
| `build/inc/hidl/ConcurrentMap.h` | 手动取自 AOSP `system/libhidl/transport/include/hidl/ConcurrentMap.h`（只依赖 `<mutex>` `<map>`） |
| `build/lib/{libhidlbase,libutils,libc++}.so` | `dump_device_binaries.sh --with-build-deps` 从设备 `/system/lib64/` 导出 |
| `module/vendor/lib64/hw/libbluetooth_qti_real.so` | 打过补丁的原厂 HAL |

```bash
NDK=/opt/android-sdk/ndk/30.0.16248370    # clang 21.0.0 r574158c，与设备平台同代
T=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
$T/clang++ --target=aarch64-linux-android24 -shared -fPIC -O2 -std=c++17 -Ibuild/inc \
  -Wl,-soname,android.hardware.bluetooth@1.0-impl-qti.so \
  bluetooth_hci_shim/BluetoothHciHook.cpp -o module/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so \
  -Lbuild/lib -Lmodule/vendor/lib64/hw -lhidlbase -lutils -lc++ -l:libbluetooth_qti_real.so -llog
```

构建后自检：`llvm-readelf -d` 的 NEEDED 应与旧版一致；`llvm-nm -D --undefined-only` 与旧版 diff，
新增符号必须在设备库里存在（`llvm-nm -D --defined-only build/lib/libhidlbase.so`）。

**ABI tag 症状速查**：符号带 `B9nqn230101` 后缀 = 新 libc++ = 布局与设备不兼容 → 必须换 NDK 版本。

## 4. shim 运行时开关（`persist.vendor.leaudio.*`）

标 *缓存* 的在首次使用后缓存（`static`），改完需 `stop/start vendor.bluetooth-1-0-qti`；
标 *实时* 的每次下发 `LE Set CIG Parameters` 时重读，`setprop` 后暂停→播放（CIG 重建）即可。

| 属性 | 默认 | 读取 | 作用 |
|---|---|---|---|
| `isoproxy` | 1 | 缓存 | v4.0 credit 代理：ISO 包排队，在飞 ≤ 控制器缓冲数（从 `LE Read Buffer Size v2`（`0x2060`）的返回值读取：00570 为 3，00680 为 22），用真实 NCP 回补栈 credit。0 = 直发 |
| `isoproxy.qmax` | 4 | 缓存 | 每 handle 队列上限，满了丢最旧（限制延迟） |
| `cig.maxlat` | 10 | 实时 | v4.1 把 `0x2062` 的 Max_Transport_Latency 截到 N ms（0=关）。00570 下不截时 MEDIA 配置会让控制器选 40ms/BN=4 → 只剩 38 包/s（00680 下未重新评估） |
| `cig.maxrtn` | -1（关） | 实时 | 截 RTN（每 CIS 的 rtn 字段）。实测缩短 CIG 事件对吞吐无帮助，只会降低空口可靠性，保持关闭 |
| `ncpsynth` | **0** | 缓存 | 合成 NCP。**必须为 0**：合成 credit 会让栈超出控制器缓冲数发包 → 控制器挂死（root-causes 第 9 节） |
| `iso.strip4` | **0** | 缓存 | 剥掉 HCI ISO 头里的 `Packet_Sequence_Number`+`ISO_SDU_Length`。**必须为 0**：那是规范强制字段（root-causes 第 8 节更正）。v4.0 起属性缺失时默认 0 |
| `isocred.window` | 12 | 实时 | 仅在 `ncpsynth=1` 时有意义 |
| `iso.patchring` | 1 | 缓存 | ring buffer type-5 补丁 |
| `isocred.mult` | 1 | 实时 | 早期乘法式 NCP —— **已知会下溢，勿用**；v4.0 起默认 1（关） |

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
