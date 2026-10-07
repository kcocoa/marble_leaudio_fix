# TODO

尚未决定或未验证的事项。已确认的结论见 [`docs/`](docs/README.md)。

## 待验证

1. **耳机麦克风 / 通话路径**：删除 `resourcemanager_ukee_mtp.xml` 覆盖后，播放音乐时 source ASE 保持
   IDLE（符合预期，耳机麦克风不再陪跑）；普通录音走手机自带麦克风（`BLE Headset In` 未被打开，
   符合 Android 行为）。耳机麦克风只在通话场景（`VOICE_COMMUNICATION`）启用，
   需要一次真实通话才能验证。

## 部署注意

2. 已从仓库删除 `module/vendor/etc/le_audio_codec_capabilities.xml`（原厂镜像里没有这个文件，
   实测在关闭 offload 的配置下也不被读取）。设备上的那份要**下次重启**才会从 `/vendor` 消失，
   在此之前它只是躺着不生效。

## 待修复：单耳播放有「风声 / 磁带衰减」杂音（奇数帧长 `/2` bug）

**根因（已确认）**：蓝牙栈 LE Audio 发送路径把 LC3 输出放进 `vector<int16_t>`，
元素数按 `(out_offset + out_size) / 2` 算，发送长度按 `size() * 2` 算。帧长为奇数时少发 1 字节：
155 B → 77 个元素 → 154 B。耳机收到截断的帧，LC3 解码失败，走丢帧补偿（PLC），听感即风声 / 音高飘忽。

- 触发条件：ROM 的 `audio_set_scenarios.json` Media 列表里，`VND_One-OneChan-SnkAse-Lc3_48_2_155octs_High_Reliability_2`
  排在标准 `One-OneChan 48_4`（120 B，偶数）之前，单耳先匹配到它。双耳先匹配 `Two-OneChan 48_4`，不受影响。
- 证据：btsnoop 里 4003 个 SDU 全是 154 B（配置 155 B），ISO 包间隔 10.000 ms、序号连续、无丢包；
  用 liblc3 按 155 B 解码坏帧率约 2%，按 154 B 约 45%。
- 代码位置：`system/bta/le_audio/client.cc`（`PrepareAndSendToSingleCis` / `PrepareAndSendToTwoCises`）
  与 `codec_interface.cc`（`Encode`）。不在控制器、固件、耳机或 shim 里。
- 目标二进制 `/apex/com.android.bt/lib64/libbluetooth_jni.so` 属于 LineageOS 签名的 APEX
  （`com.android.bt@361099999`，预装 `/system/apex/com.android.bt.capex`），**没有我们的签名密钥，
  不能重打包更新 APEX**，只能走 bind mount。

两条修法，**都未实施**：

3. **绕过（bypass，推荐先做，风险低）**：改 `audio_set_scenarios.json`，删掉 Media 里的两条
   `VND_*_155octs_High_Reliability_2`（单耳回落到标准 120 B 预设，96 kbps），
   或把 `VND_One-OneChan-SnkAse-Lc3_48_2_155octs_1` 的 `octets_per_codec_frame` 改成 154（偶数，仍在耳机 PAC 上限 155 内）。
   只改 JSON，不碰二进制。
4. **补丁（patch）**：给 `libbluetooth_jni.so` 打 2 条指令，写成 `patch_bt_jni_odd_octets.py`，
   风格同 `patch/patch_qti_iso_rx.py`（带原始字节校验、可重复执行）。偏移对应 APEX 361099999，换版本必须重新校准：
   - `0x510d2c`：`lsr w9, w8, #1` → `add w9, w8, #1`（缓冲区不再偏短）。
   - `0x44070c`：`and w3, w9, #0xfffffffe` → `ldrh w3, [sp, #0x6c]`（SDU 长度取 `octets_per_codec_frame`，精确 155）。
   - 待确认：共用尾部 `0x4406fc` 是否只被这两条发送路径调用，`[sp+0x6c]` 在所有入口都有效。

**两条路共同的前置问题（先验证）**：两者都要把改过的文件 bind mount 到 `/apex/com.android.bt/...`。
运行时补挂行不通：`com.android.bluetooth` 有自己的挂载命名空间，每次重启还会换 PID，
全局命名空间里后挂的文件它看不到；`nsenter -t <pid> -m` 挂进某个进程，进程一重启就丢了（已实测）。
模块已不再自己 bind mount（固件也交给元模块），`/apex` 的文件同样应交给元模块挂载，而不是在脚本里自己挂。
hybrid_mount 的 vfs 模式按文件由内核注入，不依赖进程的挂载命名空间，理论上能解决蓝牙进程重启丢挂载的问题
（**待验证**：vfs 规则能否指向 `/apex/com.android.bt/...`，以及蓝牙进程能否看到）。
建议先用无害的 JSON 验证，再决定要不要做二进制补丁。
补丁出错会让蓝牙进程反复崩溃，有触发 RescueParty 的风险。

## 可以考虑：重写 `audio.bluetooth.default.so` 的补丁方式

5. `audio.bluetooth.default.so` 由 AOSP 开源代码编译（`packages/modules/Bluetooth/system/audio_bluetooth_hw/`），
   不像 `libbluetooth_qti_real.so`（高通闭源，只能字节补丁）那样受限。
   目前对它只有 2 处字节补丁（`UpdateSinkMetadata` 空函数化，见 `patch/patch_hal_binaries.sh`）。
   可以评估改成从源码重新编译一份带修复的版本，而不是继续字节补丁。
   两者都不带自身签名，`/vendor` 靠 AVB / dm-verity 整体校验，模块 bind mount 不受影响。
