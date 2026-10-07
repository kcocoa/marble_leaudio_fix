# TODO

尚未决定或未验证的事项。已确认的结论见 [`docs/`](docs/README.md)。

## 待验证

1. **耳机麦克风 / 通话路径**：删除 `resourcemanager_ukee_mtp.xml` 覆盖后，播放音乐时 source ASE 保持
   IDLE（符合预期，耳机麦克风不再陪跑）；普通录音走手机自带麦克风（`BLE Headset In` 未被打开，
   符合 Android 行为）。耳机麦克风只在通话场景（`VOICE_COMMUNICATION`）启用，
   需要一次真实通话才能验证。

## 可选：更高码率

2. 155 B（124 kbps，重传 24 次）在两只耳机**一起建流**时可用，但不能给已有一个 CIS 的 CIG 动态追加第二个 CIS
   （控制器返回 0x1e，见 `docs/root-causes.md` 第 15 节），所以默认没有启用。
   想试的话可以改 `VND_QoS_Config_R24_L100` 的重传次数（比如 5），看控制器能不能接受动态追加；还没试。

3. 广播（BIS）路径也用同一个 `Encode`，奇数帧长时发送长度会比真实长度多 1 字节（原来是少 1 字节）。
   没有用到广播，没测。

## 可以考虑：重写 `audio.bluetooth.default.so` 的补丁方式

4. `audio.bluetooth.default.so` 由 AOSP 开源代码编译（`packages/modules/Bluetooth/system/audio_bluetooth_hw/`），
   不像 `libbluetooth_qti_real.so`（高通闭源，只能字节补丁）那样受限。
   目前对它只有 2 处字节补丁（`UpdateSinkMetadata` 空函数化，见 `patch/patch_hal_binaries.sh`）。
   可以评估改成从源码重新编译一份带修复的版本，而不是继续字节补丁。
   两者都不带自身签名，`/vendor` 靠 AVB / dm-verity 整体校验，模块 bind mount 不受影响。
