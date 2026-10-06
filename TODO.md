# TODO

尚未决定或未验证的事项。已确认的结论见 [`docs/`](docs/README.md)。

## 待决定

1. **其余默认关闭的实验开关是否也删掉**：`ncpsynth`（必须为 0）、`isocred.mult`（会下溢，勿用）、
   `isocred.window`（只在 `ncpsynth=1` 时有意义）、`iso.strip4`（必须为 0）都是 00570 时期的实验开关，
   默认值下对应的代码路径永远不会走到 —— 与已删除的 `isoproxy` 同理，属于"不必要的实体"。
   删除它们不改变任何默认行为（纯死代码），但会让 shim 再小一截。

## 待验证

2. **耳机麦克风 / 通话路径**：删除 `resourcemanager_ukee_mtp.xml` 覆盖后，播放音乐时 source ASE 保持
   IDLE（符合预期，耳机麦克风不再陪跑）；普通录音走手机自带麦克风（`BLE Headset In` 未被打开，
   符合 Android 行为）。耳机麦克风只在通话场景（`VOICE_COMMUNICATION`）启用，
   需要一次真实通话才能验证。

## 部署注意

3. 已从仓库删除 `module/vendor/etc/le_audio_codec_capabilities.xml`（原厂镜像里没有这个文件，
   实测在关闭 offload 的配置下也不被读取）。设备上的那份要**下次重启**才会从 `/vendor` 消失，
   在此之前它只是躺着不生效。
