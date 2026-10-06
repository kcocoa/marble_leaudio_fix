# 操作流程与事故复盘

**每次操作设备前读这一章。**

---

## 0. 公开仓库用户：先备齐外来源二进制

本仓库不含任何厂商二进制/固件/设备提取配置。工作副本缺少下列产物时，按序执行：

```bash
./scripts/fetch_upstream_firmware.sh                          # 上游固件（必需）
SERIAL=<你的 adb 序列号> ./scripts/dump_device_binaries.sh     # 设备 dump HAL + vendor 配置
./scripts/patch_hal_binaries.sh                              # 打字节补丁
./scripts/build_shim.sh                                      # 编译 shim
```

以上产物均被 `.gitignore` 排除；详见脚本头注释与 `docs/architecture.md` §2.2/§3。

---

## 1. 部署 shim（唯一正确姿势）

```bash
cd .
SER=$SERIAL ./deploy_shim_fileonly.sh /tmp/leaudio_build/shim_v41.so
```

脚本做的事（**顺序不能变**）：

```bash
adb -s $SER push "$SRC" /data/local/tmp/shim_new.so          # 1. push 到 tmp（label 无关）
adb -s $SER shell su -c "stop vendor.bluetooth-1-0-qti"        # 2. 先停 HAL：运行中的 HAL 已 mmap
                                                               #    该 .so，原地覆盖会 SIGSEGV（dead-ends 7.5）
adb -s $SER shell su -c "cp /data/local/tmp/shim_new.so $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chmod 644 $HW/android.hardware.bluetooth@1.0-impl-qti.so"
adb -s $SER shell su -c "chcon u:object_r:vendor_file:s0 $HW/..."   # 3. 必须 chcon
adb -s $SER shell su -c "ls -Z $HW/ ; md5sum ..."                    # 4. 核对 label + md5
adb -s $SER shell su -c "start vendor.bluetooth-1-0-qti"      # 5. 重启 HAL（栈自行 abort 后重启）
```

### 三条铁律

1. **绝不用 `adb push` 直推 `/data/adb/modules/` 下任何文件** —— label 会变成 `adb_data_file`，
   magic mount 原样带到 `/vendor`，HAL 无权 `map`（见 root-causes.md 根因 5）
2. **`restorecon` 无效** —— `/data/adb` 在 `file_contexts` 里本就是 `adb_data_file`，
   restorecon 会把错 label 设回去。必须 `chcon u:object_r:vendor_file:s0`
3. **改 label 不需要重启**（magic mount 跟随源文件），但 **HAL 必须先停再复制**，
   脚本已内置；音视频类 HAL（如 `audio.bluetooth.default.so`）同理，否则 crash loop。

### 热更新 vs 重启

| 改动类型 | 是否需要重启设备 |
|---|---|
| 修改模块内**已存在**的文件（so / XML） | 否，`stop/start vendor.bluetooth-1-0-qti` |
| 模块内**新增**文件 | **是**（magic mount 不挂新文件） |
| `ro.` 属性 | **是** |
| `persist.` 属性 | 否 |

---

## 2. HAL 与栈侧服务名（别搞错）

| 想做的事 | 正确命令 |
|---|---|
| 重启蓝牙 HAL | `adb shell 'stop vendor.bluetooth-1-0-qti; start vendor.bluetooth-1-0-qti'` |
| 重启蓝牙栈 | `adb shell cmd bluetooth_manager enable`（先 disable 如需） |
| 重启音频 HAL | `adb shell setprop ctl.restart vendor.audio-hal` |

**`setprop ctl.restart android.hardware.bluetooth@1.0-service-qti` 会失败**
（"Failed to set property ... See dmesg"）—— 那是 rc 文件名，不是 init 服务名。

**`setprop ctl.restart audioserver` / `ctl.restart android.hardware.audio.service` 都别用** ——
前者让 AudioFlinger 只加载 primary 模块，后者直接失败并导致 policy 模块丢失。

---

## 3. SELinux label 对照表

| 文件类型 | 需要的 label |
|---|---|
| `/vendor/lib64/hw/*.so` | `u:object_r:vendor_file:s0` |
| `/vendor/etc/**/*.xml` / `*.json` | `u:object_r:vendor_configs_file:s0` |

检查：

```bash
adb -s $SERIAL shell 'ls -Z /data/adb/modules/leaudio_marble_fix/vendor/lib64/hw/'
adb -s $SERIAL shell 'ls -Z /vendor/lib64/hw/ | grep -i bluetooth'
```

**`/vendor` 视图的 label 必须与模块目录一致**（magic mount 跟随源文件）。
不一致 = 部署方式用错了。

---

## 4. VINTF manifest 零写入预验证（唯一允许的 bind mount 场景）

改 manifest 有 bootloop 风险，**部署前必须本地验**：

```bash
adb -s $SERIAL shell 'cp /vendor/etc/vintf/manifest_ukee.xml /data/local/tmp/same.xml; chmod 644 /data/local/tmp/same.xml'
adb -s $SERIAL shell 'mount --bind /data/local/tmp/same.xml /vendor/etc/vintf/manifest_ukee.xml'
adb -s $SERIAL shell 'vintf dm | wc -l'      # 557 = 正常；0 = 解析失败
adb -s $SERIAL shell 'umount /vendor/etc/vintf/manifest_ukee.xml'
adb -s $SERIAL shell 'md5sum /vendor/etc/vintf/manifest_ukee.xml'   # 必须回到 e6798c05...
```

**事后必须 umount + md5 核对还原。**

设备侧 `/system/bin/vintf` 只有 dump 子命令（legacy/dm/fm/dcm/fcm/ri），**无 checkcompat**，
所以只能靠 `vintf dm | wc -l` 判解析成败。

---

## 5. 抓日志

`adb logcat -d` 和 harness 起的 `logcat > file &` 都会漏（缓冲小、进程被 kill）。

**有效手段**：

```bash
adb -s $SERIAL shell 'setsid logcat -v threadtime -b main,system,crash -f /data/local/tmp/btlog.txt &'
# ... 操作 ...
adb -s $SERIAL pull /data/local/tmp/btlog.txt /tmp/btlog.txt
```

ROM 特性：logcat 缓冲 ~3 分钟 / ~12k 行，**必须实时抓**。

### 关键指标 grep

```bash
grep -c "sendIsoData: ISO is not supported in HAL v1.0" /tmp/btlog.txt   # 必须 0
grep -c "remove_iso_data_path: No such iso connection" /tmp/btlog.txt   # 必须 0
grep -c "__fortify_fatal" /tmp/btlog.txt                                # 必须 0
grep -c "Invalid packet type rcvd 0x5" /tmp/btlog.txt                   # 必须 0
grep -c "avc: denied" /tmp/btlog.txt                                    # 必须 0
adb -s $SERIAL shell logcat -b crash -d -v threadtime | tail -20
```

---

## 6. 健康状态检查

```bash
SER=$SERIAL
# 进程年龄（ELAPSED 小 = 刚崩过重启）
adb -s $SER shell 'ps -A -o PID,ELAPSED,NAME | grep -iE "bluetooth"'
# HIDL 双注册（必须 @1.0 + @1.1 都 DM,FC Y）
adb -s $SER shell lshal list -i | grep IBluetoothHci
# VINTF @1.1 声明
adb -s $SER shell 'vintf dm | grep -A3 android.hardware.bluetooth'
# SELinux
adb -s $SER shell 'ls -Z /vendor/lib64/hw/ | grep -i "impl-qti\|bluetooth"'
# 白名单模块（只读）
for m in zygisk_vector hma_oss_zygisk zygisksu; do
  adb -s $SER shell su -c "test -e /data/adb/modules/$m/disable" && echo "$m=disabled" || echo "$m=enabled"
done
```

---

## 7. 事故复盘

### 事故一：VINTF fragment → bootloop（2026-10-05）

**触发**：部署 `vendor/etc/vintf/manifest/android.hardware.bluetooth@1.1.xml` fragment 后重启。

**现象**：system_server 崩溃循环 → RescueParty 触发 → 卡厂商 logo，
`adb devices` / `fastboot devices` 均空。

**根因**：libvintf 两条规则任一命中都会让**整份** device manifest 解析失败
（fatal，所有依赖 manifest 的 system 进程拿不到 manifest → system_server 崩）：

1. 跨文件 FqInstance 冲突：
   ```
   Cannot add manifest fragment ...: HAL "android.hardware.bluetooth" has a conflict:
   Conflicting FqInstance: @1.0::IBluetoothHci/default (from manifest_ukee.xml)
   vs. @1.1::IBluetoothHci/default (from fragment)
   ```
2. 同一 `<hal>` 块内重复 major：
   ```
   Illformed file: Duplicated major version: 1.0 vs 1.1
   ```

**救援**：

1. KernelSU 安全模式（开机画面后连按音量减 3 次，按-松）→ 系统正常启动
2. safe mode 下 `su: inaccessible or not found` → 用 `adb root` 拿 uid=0
3. 发现 `/data/adb/modules/leaudio_marble_fix/disable` 已被 safe mode **自动创建**
   （mtime 1973-04-30 06:32, size 0）—— 这就是救援入口
4. recovery 里救不了：`/data` 是 f2fs + `fileencryption=ice` + metadata encryption，
   `mount /data` 返回 `Invalid argument`，`/data/adb` 不存在。**必须进安全模式**
5. 备份原文件 → 删掉 fragment → 推正确格式

**预防**：改 manifest 前必做第 4 节的 bind mount 预验证。

### 事故二：`adb push` 直推模块目录 → HAL 无法加载（2026-10-06）

见 root-causes.md 根因 5。修复 = `chcon` + `stop/start vendor.bluetooth-1-0-qti`，
**不需要重启**。

### 事故三：音频 HAL 补丁碰了 paciasp → crash loop（2026-10-06 下午）

把 `audio.bluetooth.default.so` 的 `UpdateSinkMetadata` 入口 `paciasp` 改成 `ret` →
PLT 间接调用没有 BTI 落点 → 每次开 BLE 输入都 `SIGILL (ILL_ILLOPC)`，音频 HAL crash loop
~15 次（约 1 分钟）。**不重启就热修**：`cat 新文件 > 模块内文件`（同 inode，magic mount
即时可见）+ init 自动重启的音频 HAL 加载新代码，~30s 内自愈。
教训：改函数入口必须保留 `paciasp`/`bti c`（见 architecture.md 2.2、dead-ends.md 7.4）。

---

## 8. 重启设备流程（AGENTS.md 要求）

```text
设备即将重启。重启后需要你输入锁屏密码；在你确认已解锁、设备已进入系统前，
我不会继续执行命令。
```

用户确认后才执行：

```bash
adb -s $SERIAL reboot
adb -s $SERIAL wait-for-device
adb -s $SERIAL get-state
```

`wait-for-device` / `get-state=device` 只代表 ADB 已重连，**不代表用户已解锁**。此时必须停下：

```text
设备已重新连接，但可能仍停留在锁屏界面。请先输入锁屏密码并回复"已解锁"。
```

用户确认后才检查启动完成：

```bash
adb -s $SERIAL shell getprop sys.boot_completed    # 1
adb -s $SERIAL shell getprop dev.bootcomplete      # 1
adb -s $SERIAL shell getprop init.svc.bootanim     # stopped
```

**不得通过 ADB 输入、读取或绕过锁屏密码。**

---

## 9. 白名单模块（只读，永不改状态）

`zygisk_vector` / `hma_oss_zygisk` / `zygisksu`。

- 每次检查必须先读 `module.prop` 确认 ID/名称/版本，再查 `disable` 文件
- 报告时明确写 `enabled` 或 `disabled`
- 不得卸载、删除、清空数据、临时禁用（除非用户明确确认并说明影响）
- 恢复状态前必须先检查原状态
