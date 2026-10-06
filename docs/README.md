# LE Audio on marble — 文档索引

设备：Redmi Note 12 Turbo (`marble`)，LineageOS 23.2 / Android 16，KernelSU，序列号 `$SERIAL`
耳机：ROSELINK / `XX:XX:XX:XX:XX:XX`（LE Audio Unicast，CSIP 双耳组，LC3，Bluetrum 主控）

## 文档地图

| 文档 | 内容 | 什么时候读 |
|---|---|---|
| [`architecture.md`](architecture.md) | 三层架构、五种修改手段、构建配方、shim 开关 | 动手前先读这个 |
| [`root-causes.md`](root-causes.md) | 9 个已确诊根因的完整证据链（反汇编/tombstone/日志） | 遇到现象想查是否已知 |
| [`dead-ends.md`](dead-ends.md) | 5 条死路 + 我的判断失误复盘 | **避免重复浪费时间** |
| [`operations.md`](operations.md) | 部署/重启/救援流程、两次事故复盘、只读检查命令 | 每次操作设备前读这个 |

历史流水账（未删除，仅供追溯）：[`../TODO.md`](../TODO.md)、
[`../LEAUDIO_MARBLE_INVESTIGATION.md`](../LEAUDIO_MARBLE_INVESTIGATION.md)、[`../README.md`](../README.md)。

## 根本任务

让 LE Audio 单播音频在 marble 上真正发出连续声音。

硬约束（`.AGENTS.md`）：系统分区零写入，
一切持久化修改只能通过 `/data/adb/modules/leaudio_marble_fix/` 的 KernelSU overlay 生效；
重启需用户明确确认并在其解锁后才继续；白名单模块（`zygisk_vector` / `hma_oss_zygisk` / `zygisksu`）只读。

## 当前状态（2026-10-06 12:55）

**代码层面全部打通，待真人播放验证。**

| 环节 | 状态 | 证据 |
|---|---|---|
| HIDL @1.1 代理 | ✅ | `lshal` 双注册 `DM,FC Y @1.0` + `DM,FC Y @1.1`，同 pid |
| ISO 数据发送 | ✅ | `sendIsoData: ISO is not supported in HAL v1.0` = **0 次** |
| ISO RX 接收 | ✅ | 蹦桌 v2 + shim code4→5 改写；`remove_iso_data_path: No such iso connection` = **0 次** |
| 软件数据通路 | ✅ | `session_type=LE_AUDIO_SOFTWARE_{ENCODING,DECODING}_DATAPATH` 正常 SetUp/TearDown |
| shim 稳定性 | ✅ | v3.14 hexdump 溢出已修，无 `__fortify_fatal` |
| **实际出声** | ⏳ | 未验证 —— 需要人戴耳机播放 |

模块当前 **enabled**；HAL / 栈侧 pid 随每次重启变化，查 `ps -A -o PID,ELAPSED,NAME | grep -i bluetooth`。

## 模块目录清单

```
/data/adb/modules/leaudio_marble_fix/
├── module.prop
├── system.prop                          # 软件模式 + shim 开关
└── vendor/
    ├── lib64/hw/
    │   ├── android.hardware.bluetooth@1.0-impl-qti.so   # shim（SONAME 继承原名）
    │   ├── libbluetooth_qti_real.so                      # 原厂真实 HAL（已打 4 处二进制补丁）
    │   └── audio.bluetooth.default.so                    # SessionType 补丁
    ├── etc/
    │   ├── vintf/manifest_ukee.xml                       # @1.1 声明（唯一活动 SKU manifest）
    │   ├── le_audio_codec_capabilities.xml
    │   └── audio/sku_ukee/audio_policy_configuration.xml
```

**11 个文件**。`ro.boot.product.vendor.sku=ukee` → 活动主 manifest 是 `manifest_ukee.xml`。

## 关键 md5

| 文件 | md5 |
|---|---|
| shim v3.14 | `ea155cdbda8d4af27dd9625054565fb3` |
| `manifest_ukee.xml` | `d600c9c552af2ccae0d0eb3461f40ac1`（12136B） |
| `manifest_ukee.xml` 原厂 | `e6798c05eb67713485cbf79567d70f69`（12037B） |
| `libbluetooth_qti_real.so`（4 补丁） | `ce7fed1c…` |

## git 提交索引

仓库 `./`，身份 `pi-agent <pi.dev>`。

| commit | 内容 |
|---|---|
| `1b1c695` / `cd96586` | 硬件 Offload 逆向与死路结论归档 |
| `7433698` | VINTF @1.1 标准格式（`<version>1.1</version>`+`<interface>`） |
| `1cc6b00` | libvintf 五种写法实测结论 |
| `94125d6` | v3.14 修 shim hexdump 溢出 |
| `53df581` | VINTF 改合并式单 `<hal>` 块（已被 `7433698` 取代） |
| `8157d3c` | deploy 脚本禁用 `adb push` 直推、改用 `chcon` |
| `aebec53` | 新增 `deploy_shim_fileonly.sh`，弃用 bind mount 版 |

## 下一步

1. 用户戴 ROSELINK 耳机播放 → 确认有连续声音
2. 若无声：按 `root-causes.md` 第 10 节「无声排查决策树」走
3. 若稳定：清理诊断日志、固化发布版、打模块 zip
