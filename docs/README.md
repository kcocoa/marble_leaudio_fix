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

## 当前状态（2026-10-06 17:05）

**🎉 完全打通：双耳 LE Audio 满速推流、零丢包、人耳验证无瑕疵！**

突破关键：从官方 `linux-firmware` 提取高通最新开源 `hpbtfw21.tlv`（版本 `2.1.0-00680`，
天然带高通官方 RSA 签名，芯片 ROM 顺利验签加载）。
**芯片报告的 ISO 缓冲区从原厂旧固件（00570）的 3 个暴增到新固件的 22 个！**
吞吐天花板从 150/s 直接跃升至 430+/s，双耳 200/s 需求全额满足，丢包彻底归零，经用户人耳实测音质完全正常。

| 环节 | 状态 | 证据 |
|---|---|---|
| HIDL @1.1 代理 | ✅ | `lshal` 双注册 `DM,FC Y @1.0` + `DM,FC Y @1.1`，同 pid |
| ISO 数据发送 | ✅ | `sendIsoData: ISO is not supported in HAL v1.0` = **0 次** |
| ISO RX 接收 | ✅ | 蹦桌 v2 + shim code4→5 改写；`remove_iso_data_path: No such iso connection` = **0 次** |
| 软件数据通路 | ✅ | `session_type=LE_AUDIO_SOFTWARE_{ENCODING,DECODING}_DATAPATH` 正常 SetUp/TearDown |
| shim 稳定性 | ✅ | v4.1（内含 v3.14 hexdump 修复），无 `__fortify_fatal` |
| 控制器稳定性 | ✅ | `ncpsynth=0 strip4=0`：STREAMING 无 SSR |
| 芯片 ISO 缓冲 | ✅ | **22 个**（原厂 3 个）；固件升级至官方 `2.1.0-00680` |
| 栈侧 / shim 丢包 | ✅ | **0 丢包**：实测 `enq=8008 sent=8007 drop=0 ncp=7996 acked=8006 inflight=11/22` |
| 场景 | ✅ | MEDIA sink-only（audio HAL 录音元数据补丁，根因 11；单向音乐无麦克风干扰） |
| CIG 调度 | ✅ | 10ms / BN=1 / FT=1（shim 截 maxlat=10，防 40ms/BN4 调度灾难） |
| 吞吐速率 | ✅ | **双耳精准恒定 200.0 包/秒**（LC3 48_4 High Reliability，96 kbps/耳） |
| **实际出声** | ✅ | **人耳实测验证通过（用户反馈：YES, IT WORKS!）** |

模块当前 **enabled**（版本 `v4.2-upstream-fw`）。

## 模块目录清单

```
/data/adb/modules/leaudio_marble_fix/
├── module.prop                          # v4.2-upstream-fw
├── system.prop                          # 软件模式 + shim 开关
├── post-fs-data.sh                      # 开机自动 bind-mount 官方 00680 固件到 /vendor/bt_firmware
├── service.sh                           # 兜底确保固件 mount 正常
├── firmware/
│   └── hpbtfw21.tlv                     # 高通官方 00680 固件（22 ISO buffers，带高通官方签名）
└── vendor/
    ├── lib64/hw/
    │   ├── android.hardware.bluetooth@1.0-impl-qti.so   # shim（SONAME 继承原名）
    │   ├── libbluetooth_qti_real.so                      # 原厂真实 HAL（已打 4 处二进制补丁）
    │   └── audio.bluetooth.default.so                    # SessionType 补丁 + 录音元数据禁用补丁
    ├── etc/
    │   ├── vintf/manifest_ukee.xml                       # @1.1 声明（唯一活动 SKU manifest）
    │   ├── le_audio_codec_capabilities.xml
    │   ├── bluetooth/le_audio/audio_set_configurations.json   # ⚠ 栈不读（apex 内置），死文件
    │   └── audio/
    │       ├── sku_ukee/{audio_policy_configuration.xml,resourcemanager_ukee_mtp.xml}
    │       └── sku_taro/audio_policy_configuration.xml
```

`ro.boot.product.vendor.sku=ukee` → 活动主 manifest 是 `manifest_ukee.xml`。

设备侧备份：`/data/local/tmp/shim_v314_backup.so`（v3.14）、
`/data/local/tmp/audio.bluetooth.default.so.bak_20261006`（音频 HAL 原厂）、
`/data/local/tmp/system.prop.bak_20261006`。

## 关键 md5

| 文件 | md5 |
|---|---|
| `firmware/hpbtfw21.tlv`（00680 固件） | `9a6b0cb34a82a7015141dd19b2d779da`（163332B） |
| 原厂 `hpbtfw21.tlv`（00570 固件） | `fac75b203eb19dec5ae026a0e72c7be0`（146068B） |
| shim v4.1 | `45bb1e5abd713ce2125819f216bde24f` |
| `audio.bluetooth.default.so`（补丁后） | `9769dbee5e5a16d8c2486527f43fc6a1` |
| `audio.bluetooth.default.so` 原厂 | `1b96c8421c91338f2bc3841ae5cd4f45` |
| `system.prop`（ncpsynth=0, strip4=0） | `25a73fe10faca1b64b307c06b440084d` |
| `manifest_ukee.xml` | `d600c9c552af2ccae0d0eb3461f40ac1`（12136B） |
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

1. **项目目标全部达成**：双耳 LE Audio 稳定推流、48kHz LC3 音乐、零丢包、零破音、人耳验收通过
2. 模块已持久化（`post-fs-data.sh` 自动 bind-mount 官方 00680 固件），重启自愈
3. 如需测试设备重启后的持久化：须先提醒用户并取得确认（用户输入锁屏密码）
4. 仓库变更提交：新固件、post-fs-data/service 脚本、module.prop 与全部文档更新
