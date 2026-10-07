# TODO

尚未决定或未验证的事项。已确认的结论见 [`docs/`](docs/README.md)。

## 待验证

1. **耳机麦克风 / 通话路径**：删除 `resourcemanager_ukee_mtp.xml` 覆盖后，播放音乐时 source ASE 保持
   IDLE（符合预期，耳机麦克风不再陪跑）；普通录音走手机自带麦克风（`BLE Headset In` 未被打开，
   符合 Android 行为）。耳机麦克风只在通话场景（`VOICE_COMMUNICATION`）启用，
   需要一次真实通话才能验证。

## 待查：单耳放入第二只耳机时有时没声音

2. 155 B（124 kbps）已经是默认配置（耳机 PAC 声明的上限）。但给已有一个 CIS 的 CIG 动态追加第二个 CIS 有时会被控制器拒绝
   （0x1e，暂停再播放可恢复；更稳的备选是单耳也用 120 B，见 root-causes.md 第 15 节）。原因没查清（见 `docs/root-causes.md` 第 15 节）：**不是 RTN**（控制器给的 NSE 恒为 4），
   怀疑和被追加的 CIS 排在前面还是后面、新 ACL 连接的状态有关。
   要继续查的话做对照实验：固定先连 / 后连的耳机顺序，开 shim 日志抓 `CIG params`（先把每包日志关掉，不然会被刷掉），
   并在建流后立刻拉 btsnoop（滚动日志只保留约 10 分钟）；再试一次关掉 shim 的 10 ms 截断（`persist.vendor.leaudio.cig.maxlat=0`）。

3. 广播（BIS）路径也用同一个 `Encode`，奇数帧长时发送长度会比真实长度多 1 字节（原来是少 1 字节）。
   没有用到广播，没测。

## 可以考虑：重写 `audio.bluetooth.default.so` 的补丁方式

4. `audio.bluetooth.default.so` 由 AOSP 开源代码编译（`packages/modules/Bluetooth/system/audio_bluetooth_hw/`），
   不像 `libbluetooth_qti_real.so`（高通闭源，只能字节补丁）那样受限。
   目前对它只有 2 处字节补丁（`UpdateSinkMetadata` 空函数化，见 `patch/patch_hal_binaries.sh`）。
   可以评估改成从源码重新编译一份带修复的版本，而不是继续字节补丁。
   两者都不带自身签名，`/vendor` 靠 AVB / dm-verity 整体校验，模块 bind mount 不受影响。
