# 技术文档索引

模块的用途、适用范围与安装见仓库根目录 [`README.md`](../README.md)。

| 文档 | 内容 |
|---|---|
| [`architecture.md`](architecture.md) | 软件栈分层、各类修改手段、二进制补丁表、shim 构建与运行时开关 |
| [`root-causes.md`](root-causes.md) | 各问题的根因与证据（反汇编 / tombstone / 日志），附无声排查决策树 |
| [`dead-ends.md`](dead-ends.md) | 试过但走不通的方案，避免重复踩坑 |
| [`operations.md`](operations.md) | 部署、热更新、抓日志、健康检查、bootloop 救援 |

## 固件版本说明

排查的大部分过程在**原厂蓝牙固件 00570**（控制器 3 个 ISO 缓冲）上进行；
最终方案换用**上游固件 00680**（22 个 ISO 缓冲）。
文档中涉及控制器缓冲、吞吐、丢包的数字，除特别注明外均为 00570 下的测量。

## 模块文件

```
module/
├── module.prop
├── system.prop                       LE Audio profile 开关、关闭 offload、shim 开关
├── customize.sh                      安装时设置文件的 SELinux 标签和属主
├── post-mount.sh                     开机时把 apex/ 下的文件用 hybrid_mount 的 vfs 规则重定向到 /apex
├── apex/com.android.bt/              （均不入库，从设备导出）
│   ├── lib64/libbluetooth_jni.so       蓝牙栈，6 处补丁（奇数帧长）
│   └── etc/bluetooth/le_audio/audio_set_scenarios.json   调试入口，要改预设就改这个文件；与原厂只差一行（删掉单耳 155 B 预设）
└── vendor/
    ├── bt_firmware/image/hpbtfw21.tlv  上游固件 00680（脚本下载，不入库）
    ├── lib64/hw/                     （均不入库）
    │   ├── android.hardware.bluetooth@1.0-impl-qti.so   shim，沿用原厂 SONAME
    │   ├── libbluetooth_qti_real.so                      原厂 impl-qti.so 改 SONAME，4 处补丁
    │   └── audio.bluetooth.default.so                    原厂音频 HAL，2 处补丁
    └── etc/
        ├── vintf/manifest_ukee.xml                       声明 IBluetoothHci @1.1（基于设备导出件修改，不入库）
        └── audio/sku_ukee/audio_policy_configuration.xml 补全 bluetooth 模块（基于设备导出件修改，不入库）
```

`ro.boot.product.vendor.sku=ukee` → 活动 manifest 与音频策略都取 `sku_ukee` 下的文件。
