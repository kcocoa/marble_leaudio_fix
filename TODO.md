# TODO

尚未验证或未完成的事项。已确认的结论见 [`docs/`](docs/README.md)。

## 待验证

1. **shim 运行时开关是否还需要**（[`docs/architecture.md` §4](docs/architecture.md)）
   - `isoproxy`（credit 代理）：当年为 3 缓冲固件设计，00680 有 22 个缓冲，可能已经多余。
   - `cig.maxlat=10`：理由来自 00570（不截时控制器会选 40ms/BN=4）。
   - 验证方法：改 prop 后重启蓝牙 HAL（`stop` / `start vendor.bluetooth-1-0-qti`，**不用重启设备**），
     观察播放是否仍正常、`isoproxy` 的 `drop` 与 `inflight` 是否正常。
2. **`module/vendor/etc/le_audio_codec_capabilities.xml`**：原厂没有这个文件，是手写的；
   关掉 offload 后是否仍被读取，未验证。
3. **录音与通话路径**：删掉 `resourcemanager_ukee_mtp.xml` 覆盖后，重启无报错、音乐播放正常，
   但录音与通话未测。
