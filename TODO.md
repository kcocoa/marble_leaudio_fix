# TODO

尚未决定或未验证的事项。已确认的结论见 [`docs/`](docs/README.md)。

## 待验证

1. **耳机麦克风 / 通话路径**：删除 `resourcemanager_ukee_mtp.xml` 覆盖后，播放音乐时 source ASE 保持
   IDLE（符合预期，耳机麦克风不再陪跑）；普通录音走手机自带麦克风（`BLE Headset In` 未被打开，
   符合 Android 行为）。耳机麦克风只在通话场景（`VOICE_COMMUNICATION`）启用，
   需要一次真实通话才能验证。

## 待查：单耳播放中放入第二只耳机，蓝牙栈 abort

蓝牙进程 abort，断言在 `state_machine.cc` 的 `AddCisToStreamConfiguration`：
`octets per frame mismatch: 155!=120`（`/data/tombstones` 里 6 份 tombstone 的消息完全相同，
最早的在 10-07 00:58，早于补丁，所以不是补丁引起的）。
单耳在播放（走 155 B 预设）时把第二只耳机拿出充电盒，第二只被配成 120 B，和已有的 155 B 不一致，栈直接 abort，重启后恢复。
**原因还只是推测，没有验证**（没抓到崩溃前的日志）。可能的缓解：让单耳也走 120 B 预设（删掉那条 `VND_One-OneChan…155octs`），
这样前后配置一致；但会放弃单耳的 124 kbps。

## 可选：双耳 124 kbps

2. 已实测：把 `audio_set_scenarios.json` Media 列表里的 `VND_Two-OneChan-SnkAse-Lc3_48_2_155octs_High_Reliability_2`
   挪到最前，双耳每耳 155 B（124 kbps）可用。目前没有启用（默认仍是标准 120 B）。
   要启用就改模块里的 `apex/com.android.bt/etc/bluetooth/le_audio/audio_set_scenarios.json`（现在是原厂原样）。
   还没验证：长时间播放的稳定性、信号差时（QoS 是重传 24 次、延迟 100 ms）是否更容易断续。

3. 广播（BIS）路径也用同一个 `Encode`，奇数帧长时发送长度会比真实长度多 1 字节（原来是少 1 字节）。
   没有用到广播，没测。

## 可以考虑：重写 `audio.bluetooth.default.so` 的补丁方式

4. `audio.bluetooth.default.so` 由 AOSP 开源代码编译（`packages/modules/Bluetooth/system/audio_bluetooth_hw/`），
   不像 `libbluetooth_qti_real.so`（高通闭源，只能字节补丁）那样受限。
   目前对它只有 2 处字节补丁（`UpdateSinkMetadata` 空函数化，见 `patch/patch_hal_binaries.sh`）。
   可以评估改成从源码重新编译一份带修复的版本，而不是继续字节补丁。
   两者都不带自身签名，`/vendor` 靠 AVB / dm-verity 整体校验，模块 bind mount 不受影响。
