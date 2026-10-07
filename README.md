# LE Audio Enabler for Redmi Note 12 Turbo (marble)

让 Redmi Note 12 Turbo 在 LineageOS 上正常使用 **LE Audio（LC3）耳机** 的 KernelSU 模块。

原厂状态下本机无法使用 LE Audio。

## 实测效果

作者手上只有两副 LE Audio 耳机，用它们验证连接与播放；模块不针对具体型号做适配：

- Furina Endless Solo of Solitude-LEA（弱水时砂芙宁娜不休独舞耳机）
- ZZZ-ANGELS-OWS-LEA（水月雨妄想天使耳机）

以 **192 kbps LC3**（2-channel，每声道 96 kbps，10 ms 帧，120 B）播放正常。

## 适用范围

| 项目 | 要求 |
|---|---|
| 设备 | Redmi Note 12 Turbo（`marble`，SM7475，`ro.boot.product.vendor.sku=ukee`） |
| 系统 | LineageOS 23.2 / Android 16 |
| Root | KernelSU，并已安装一个挂载用的**元模块**（metamodule），且**不能用 overlayfs 模式**，见下 |
| 耳机 | LE Audio 单播（unicast）耳机 |

本模块要用 overlay 替换 `/vendor` 下的文件，而 KernelSU 本身不负责挂载模块文件，这一步由元模块完成。
没有元模块时，模块可以安装，但 vendor 文件不会生效。

蓝牙固件（`/vendor/bt_firmware`，独立的 vfat 分区）也由元模块挂载。内核 overlayfs 不接受 vfat 做下层，
所以元模块用 overlayfs 时挂载会失败（hybrid_mount 还会回滚所有模块）。请改用 vfs 或 magic mount，
例如 hybrid_mount：`default_mode = "vfs"`。

其他机型、ROM 或版本**不要直接套用**——二进制补丁按本机原厂文件的字节校准。

## 模块做了什么

vendor 文件（含蓝牙固件）都由元模块挂载，标签由 `customize.sh` 设置，**不写入任何系统分区**；禁用模块即完全还原。

- **开启 LE Audio 单播相关 profile**，走软件 LC3 编码（本机高通 BSP 不支持 LE Audio 硬件 offload）
- **修补高通蓝牙 HAL**：让它能收发 LE Audio 音频数据包（ISO），并以 HIDL 1.1 接口暴露给系统
- **shim 层**：控制 ISO 数据的发送节奏，避免蓝牙芯片缓冲溢出
- **补齐音频配置**：LE Audio codec 能力声明、音频路由
- **替换蓝牙固件**为高通上游 `2.1.0-00680` 版本（ISO 缓冲 3 → 22，消除双耳卡顿）

## 已知限制

- 耳机麦克风用于普通录音（非通话）时可能无法正确选择录音场景
- 部分配置改动需重启设备才生效

## 构建与安装

仓库不包含任何厂商二进制或固件，需从上游和**你自己的设备**获取。
已装有本模块时，导出要加 `--from-block`（直接读 vendor 分区，否则导出的是模块里的文件）。

```bash
export SERIAL=<adb 序列号>
./scripts/fetch_upstream_firmware.sh                  # 下载上游蓝牙固件
./scripts/dump_device_binaries.sh --with-build-deps   # 从设备导出原厂 HAL、配置和编译依赖
./patch/patch_hal_binaries.sh                       # 给两个 HAL 打补丁
python3 patch/patch_vendor_configs.py               # 修改 VINTF manifest 与音频策略
./scripts/build_shim.sh                               # 编译 shim（需 Android NDK 30）
./scripts/build_zip.sh                                # 打成 build/leaudio_marble_fix-<版本>.zip
```

把生成的 zip 传到手机，在 KernelSU 管理器里「模块 → 从本地安装」选它，然后重启。
安装时 `customize.sh` 会设置 SELinux 标签，所以**必须用 zip 安装**。

`scripts/deploy_module.sh` 只是调试用的快捷方式（KernelSU 不会替 adb 部署跑 `customize.sh`，该脚本会自己跑一遍），不作为安装方式。

详细步骤与注意事项见 [`docs/operations.md`](docs/operations.md)。

## 目录

```
module/                  模块本体（module.prop、system.prop、customize.sh、vendor overlay）
patch/                   二进制补丁脚本和 shim 源码（bluetooth_hci_shim/BluetoothHciHook.cpp）
scripts/                 获取固件、导出设备文件、打补丁、编译、打包；以及手动调试用的部署 / 监测脚本
docs/                    技术文档
```

## 文档

- [`docs/architecture.md`](docs/architecture.md)：架构、修改手段、构建细节
- [`docs/root-causes.md`](docs/root-causes.md)：各问题的根因与证据
- [`docs/dead-ends.md`](docs/dead-ends.md)：试过但走不通的方案
- [`docs/operations.md`](docs/operations.md)：部署、调试、救援
- [`TODO.md`](TODO.md)：待验证事项
