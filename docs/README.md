# LE Audio on marble — 文档索引

设备：Redmi Note 12 Turbo (`marble`)，LineageOS 23.2 / Android 16，KernelSU，序列号 `$SERIAL`（本地环境变量注入，不入库）
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
└── vendor/
    ├── lib64/hw/                        # ⚠ 手机侧二进制，不入库
    │   ├── android.hardware.bluetooth@1.0-impl-qti.so   # shim（SONAME 继承原名）
    │   ├── libbluetooth_qti_real.so                      # 原厂真实 HAL（已打 4 处二进制补丁）
    │   └── audio.bluetooth.default.so                    # SessionType 补丁 + 录音元数据禁用补丁
    ├── etc/
    │   ├── vintf/manifest_ukee.xml                       # @1.1 声明（设备提取，不入库）
    │   ├── le_audio_codec_capabilities.xml               # 本机手写（AOSP schema）
    │   ├── bluetooth/le_audio/audio_set_configurations.json   # ⚠ 栈不读（apex 内置），死文件（设备提取，不入库）
    │   └── audio/                                        # sku_ukee/sku_taro（设备提取，不入库）
```

`ro.boot.product.vendor.sku=ukee` → 活动主 manifest 是 `manifest_ukee.xml`。

> 注：手机侧二进制、固件与设备提取的 vendor 配置只存在于本机工作区与设备侧，
> 仓库不跟踪（见 `.gitignore`）。

设备侧备份：`/data/local/tmp/shim_v314_backup.so`（v3.14）、
`/data/local/tmp/audio.bluetooth.default.so.bak_20261006`（音频 HAL 原厂）、
`/data/local/tmp/system.prop.bak_20261006`。

## 关键 md5

> 专有二进制（固件 / 厂商 `.so` / 设备提取 XML）已全部移出仓库，此处只列库内文本文件。
> 二进制指纹按需在设备侧 `md5sum` 现场核对，不落入历史。

| 文件 | md5 |
|---|---|
| `leaudio_marble_fix_v2/system.prop`（ncpsynth=0, strip4=0） | `25a73fe10faca1b64b307c06b440084d` |

## git 提交索引

仓库身份 **`kcocoa <kcocoa410@hotmail.com>`**（`git config user.*` 已设）。

> ⚠️ **2026-10-06 历史已用 `git filter-repo` 全量重写**：设备序列号、耳机 MAC、
> 本机绝对路径/用户名已从所有提交中剔除，手机侧二进制与日志衍生文件已从历史移除，
> 因此**旧 commit 哈希全部失效**，下表不再按哈希索引，改用里程碑描述。
> 逐条提交的完成方署名见每条 commit message 末尾的 `Tool:` trailer：
> `Step 5 Preview & Gemini 3.8 Flash`（v4 之前）｜ `Claude Opus 5.5`（v4.x）｜
> `Step 5 Preview`（脱敏重写及其后）。

| 里程碑 | 内容 |
|---|---|
| 初始化 | 工作仓库 + BluetoothHciHook v3.10（QTI HAL HIDL 1.1 shim） |
| 模块 v2 | leaudio_marble_fix_v2 KernelSU 模块配置（system.prop / 音频策略 / VINTF） |
| 工具 | 部署迭代脚本 + 事件触发式监测器 + ISO RX 蹦桌补丁生成器 |
| v3.11–v3.14 | framed-SDU 剥离、hexdump 溢出修复、RX ISO 通路补齐、部署 SELinux 规则 |
| docs 重组 | 整理为 docs/ 五篇现行文档 |
| v4.x | ISO credit 代理 + CIG max-latency 截断、录音元数据静音、官方 00680 固件 bind-mount、终局结论 |

## 下一步

1. **项目目标全部达成**：双耳 LE Audio 稳定推流、48kHz LC3 音乐、零丢包、零破音、人耳验收通过
2. 模块已持久化（`post-fs-data.sh` 自动 bind-mount 官方 00680 固件），重启自愈
3. 如需测试设备重启后的持久化：须先提醒用户并取得确认（用户输入锁屏密码）
4. 仓库状态：历史已脱敏重写（filter-repo），手机侧内容不入库；后续提交沿用本身份与 `Tool:` 署名
