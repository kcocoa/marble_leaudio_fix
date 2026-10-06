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

## 当前状态（2026-10-06 16:10）

**终局定性：双耳 150/s 硬上限（固件 ISO 流水线每包占 2×10ms，根因 12），25% 丢帧不可避免；
单耳（1 CIS，100/s）实测无丢帧、听感正常。不再追求双耳满速，主机侧已无剩余手段。**

时间线：12:55 版“全部打通”是错的（每次起流 ~2s 崩，根因 9）→ 15:30 `ncpsynth/strip4=0` 后不崩
但 150/s → v4.0 credit 代理没提升（根因 12）→ 重启实验去掉耳机麦克风陪跑（根因 11）+
v4.1 CIG 延迟截断（否则 MEDIA 配置选 40ms/BN4 只剩 38/s）→ 逐项排除调度假设，NCP 需求
19.9ms/包铁证定性。

| 环节 | 状态 | 证据 |
|---|---|---|
| HIDL @1.1 代理 | ✅ | `lshal` 双注册 `DM,FC Y @1.0` + `DM,FC Y @1.1`，同 pid |
| ISO 数据发送 | ✅ | `sendIsoData: ISO is not supported in HAL v1.0` = **0 次** |
| ISO RX 接收 | ✅ | 蹦桌 v2 + shim code4→5 改写；`remove_iso_data_path: No such iso connection` = **0 次** |
| 软件数据通路 | ✅ | `session_type=LE_AUDIO_SOFTWARE_{ENCODING,DECODING}_DATAPATH` 正常 SetUp/TearDown |
| shim 稳定性 | ✅ | v4.1（内含 v3.14 hexdump 修复），无 `__fortify_fatal` |
| 控制器稳定性 | ✅ | `ncpsynth=0 strip4=0`：STREAMING 无 SSR（crash buffer 仅重启/换库时的预期 serviceDied） |
| 栈侧丢包 | ✅ | v4.0 credit 代理：栈恒满 credit，0 丢包，多余的 ~50/s 在 shim 队列丢（延迟有界） |
| 场景 | ✅ | MEDIA sink-only（audio HAL 录音元数据补丁，根因 11；不再耳机麦克风陪跑） |
| CIG 调度 | ✅ | 10ms / BN=1 / FT=1（shim 截 maxlat=10，防 40ms/BN4 的 38/s 灾难） |
| 吞吐 | ⚠️ | 双耳固定 150/s（需 200/s）：固件流水线每包 ~19.9ms（根因 12）；单耳 100/s 满速 |
| **实际出声** | ✅ | 人耳实测：单耳正常，双耳连续但可闻丢帧 |

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
    │   └── audio.bluetooth.default.so                    # SessionType 补丁 + 录音元数据禁用补丁
    ├── etc/
    │   ├── vintf/manifest_ukee.xml                       # @1.1 声明（唯一活动 SKU manifest）
    │   ├── le_audio_codec_capabilities.xml
    │   ├── bluetooth/le_audio/audio_set_configurations.json   # ⚠ 栈不读（apex 内置），死文件
    │   └── audio/
    │       ├── sku_ukee/{audio_policy_configuration.xml,resourcemanager_ukee_mtp.xml}
    │       └── sku_taro/audio_policy_configuration.xml
```

**12 个文件**（`bluetooth/le_audio/*.json` 是早期遗留，栈从不读它）。`ro.boot.product.vendor.sku=ukee`
→ 活动主 manifest 是 `manifest_ukee.xml`。

设备侧备份：`/data/local/tmp/shim_v314_backup.so`（v3.14）、
`/data/local/tmp/audio.bluetooth.default.so.bak_20261006`（音频 HAL 原厂）、
`/data/local/tmp/system.prop.bak_20261006`。

## 关键 md5

| 文件 | md5 |
|---|---|
| shim v4.1 | `45bb1e5abd713ce2125819f216bde24f` |
| `audio.bluetooth.default.so`（补丁后） | `9769dbee5e5a16d8c2486527f43fc6a1` |
| `audio.bluetooth.default.so` 原厂（4 补丁版） | `1b96c8421c91338f2bc3841ae5cd4f45` |
| `system.prop`（ncpsynth=0, strip4=0） | `25a73fe10faca1b64b307c06b440084d` |
| shim v3.14（设备备份） | `ea155cdbda8d4af27dd9625054565fb3` |
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

1. ~~已验：单耳正常、双耳丢帧~~（root-causes 12）——双耳满速只能寄望新 BT 固件
   （`/vendor/bt_firmware` 不可写，换包是唯一途径）；如要尝试，先看日志里
   `hastings` 固件版本（`hpbtfw20/21.tlv`）有没有官方更新
2. 双耳听感如果需要微调：丢包集中在 shim，可试 `isoproxy=0`（退回栈侧丢包）对比；
   或降低音乐码率让 PLC 更好掩盖（当前 48_4/120B）
3. 若要恢复耳机麦克风录音场景：回滚 `audio.bluetooth.default.so`（备份在
   `/data/local/tmp/audio.bluetooth.default.so.bak_20261006`）+ 重启，但音乐会回到 LIVE 双向
4. 若无声/崩溃：按 `root-causes.md` 第 10 节「无声排查决策树」走
5. 待办：仓库改动未提交（shim v4.1、docs、deploy 脚本、system.prop）
