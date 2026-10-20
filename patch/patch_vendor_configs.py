#!/usr/bin/env python3
"""patch_vendor_configs.py — 修改从设备导出的原厂 vendor 配置，产出模块用的版本。

输入（先由 scripts/dump_device_binaries.sh 导出原厂文件）:
  module/vendor/etc/vintf/manifest_ukee.xml
  module/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml
  module/apex/com.android.bt/etc/bluetooth/le_audio/audio_set_scenarios.json   （有才改）

改动:
  1. manifest: IBluetoothHci 声明 @1.0 -> @1.1（<version>+<interface> 写法；
     其他写法会让 libvintf 解析失败导致 bootloop，见 docs/root-causes.md 第 6 节）
  2. 音频策略: 追加 bluetooth 模块（A2DP / 助听器 / BLE 输出 + BLE 输入）。
     BLE 输入必须声明，否则 LE Audio 设备在 AudioPolicy 中不可用（docs/dead-ends.md 第 6 节）
  3. 音频策略: 去掉 primary 模块 "A2DP In" 端口的 encodedFormats="AUDIO_FORMAT_LC3"
     （原厂为高通硬件 offload 所写；与已验证可用的配置保持一致）
  4. LE Audio 预设: Media 列表里把两条 155 B 预设提前，单耳和双耳都选 124 kbps。
     两只耳机的 Sink PAC 声明每帧最多 155 B（48 kHz、10 ms 即 124 kbps），这是它们支持的最高码率。
     Android 17 原厂把双耳的 VND_Two-OneChan…155octs 排在 120 B 的 Two-OneChan 48_4 之后，
     把单耳的 VND_One-OneChan…155octs 排在 120 B 的 One-OneChan 48_4 之后，两条都轮不到。
     单耳若先匹配到 155 B、第二只耳机加入时双耳重配成 120 B，
     AddCisToStreamConfiguration 断言 155!=120，蓝牙进程 abort（docs/root-causes.md 第 15 节）。
     双耳那条挪到 Media 最前；单耳那条挪到 One-TwoChan 24_2 之前（紧挨在 One-OneChan 48_* 前面）。
     已知代价：单耳播放时放入第二只耳机，控制器有时拒绝追加第二个 CIS（0x1e），表现为"在播放但没声音"，
     暂停再播放即可（两个 CIS 一起建不受影响）。
     想要稳一点就删掉 Media 里的 VND_One-OneChan…155octs，单双耳都回到 120 B。

就地修改，可重复执行（已改过的文件跳过）；结构与预期不符时中止，不做猜测性修改。

用法: python3 patch/patch_vendor_configs.py [--module-dir <路径>]
"""
import argparse
import json
import os
import re
import sys

BT_MODULE = """\
    <!-- Bluetooth Audio HAL (AIDL: android.hardware.bluetooth.audio) -->
        <module name="bluetooth" halVersion="2.0">
            <mixPorts>
                <mixPort name="a2dp output" role="source">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="44100,48000,88200,96000" channelMasks="AUDIO_CHANNEL_OUT_STEREO" />
                </mixPort>
                <mixPort name="hearing aid output" role="source">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="16000,24000" channelMasks="AUDIO_CHANNEL_OUT_STEREO" />
                </mixPort>
                <mixPort name="ble output" role="source">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                    <profile name="" format="AUDIO_FORMAT_PCM_24_BIT_PACKED" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                    <profile name="" format="AUDIO_FORMAT_PCM_32_BIT" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                    <profile name="" format="AUDIO_FORMAT_LC3" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                </mixPort>
                <mixPort name="ble input" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="8000,16000,24000,32000,48000" channelMasks="AUDIO_CHANNEL_IN_MONO,AUDIO_CHANNEL_IN_STEREO" />
                    <profile name="" format="AUDIO_FORMAT_LC3" samplingRates="8000,16000,24000,32000,48000" channelMasks="AUDIO_CHANNEL_IN_MONO,AUDIO_CHANNEL_IN_STEREO" />
                </mixPort>
            </mixPorts>
            <devicePorts>
                <devicePort tagName="BT A2DP Out" type="AUDIO_DEVICE_OUT_BLUETOOTH_A2DP" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="44100,48000,88200,96000" channelMasks="AUDIO_CHANNEL_OUT_STEREO" />
                </devicePort>
                <devicePort tagName="BT A2DP Headphones" type="AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="44100,48000,88200,96000" channelMasks="AUDIO_CHANNEL_OUT_STEREO" />
                </devicePort>
                <devicePort tagName="BT A2DP Speaker" type="AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="44100,48000,88200,96000" channelMasks="AUDIO_CHANNEL_OUT_STEREO" />
                </devicePort>
                <devicePort tagName="BT Hearing Aid Out" type="AUDIO_DEVICE_OUT_HEARING_AID" role="sink" />
                <devicePort tagName="BLE Headset Out" type="AUDIO_DEVICE_OUT_BLE_HEADSET" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                    <profile name="" format="AUDIO_FORMAT_PCM_24_BIT_PACKED" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                    <profile name="" format="AUDIO_FORMAT_PCM_32_BIT" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                </devicePort>
                <devicePort tagName="BLE Speaker Out" type="AUDIO_DEVICE_OUT_BLE_SPEAKER" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                </devicePort>
                <devicePort tagName="BLE Broadcast Out" type="AUDIO_DEVICE_OUT_BLE_BROADCAST" role="sink">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="8000,16000,24000,32000,44100,48000" channelMasks="AUDIO_CHANNEL_OUT_STEREO,AUDIO_CHANNEL_OUT_MONO" />
                </devicePort>
                <devicePort tagName="BLE Headset In" type="AUDIO_DEVICE_IN_BLE_HEADSET" role="source">
                    <profile name="" format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="8000,16000,24000,32000,48000" channelMasks="AUDIO_CHANNEL_IN_MONO,AUDIO_CHANNEL_IN_STEREO" />
                </devicePort>
            </devicePorts>
            <routes>
                <route type="mix" sink="BT A2DP Out" sources="a2dp output" />
                <route type="mix" sink="BT A2DP Headphones" sources="a2dp output" />
                <route type="mix" sink="BT A2DP Speaker" sources="a2dp output" />
                <route type="mix" sink="BT Hearing Aid Out" sources="hearing aid output" />
                <route type="mix" sink="BLE Headset Out" sources="ble output" />
                <route type="mix" sink="BLE Speaker Out" sources="ble output" />
                <route type="mix" sink="BLE Broadcast Out" sources="ble output" />
                <route type="mix" sink="ble input" sources="BLE Headset In" />
            </routes>
        </module>
"""

HCI_10 = re.compile(
    r"(<name>android\.hardware\.bluetooth</name>\s*<transport>hwbinder</transport>\s*?\n)"
    r"([ \t]*)<fqname>@1\.0::IBluetoothHci/default</fqname>"
)


def die(msg):
    print(f"[x] {msg}", file=sys.stderr)
    sys.exit(1)


def patch_manifest(path):
    s = open(path, encoding="utf-8").read()
    if "<name>IBluetoothHci</name>" in s and "<version>1.1</version>" in s:
        print(f"[=] 已是 @1.1，跳过: {path}")
        return
    if len(HCI_10.findall(s)) != 1:
        die(f"{path}: 未找到唯一的 @1.0::IBluetoothHci/default 声明，确认是原厂文件")
    s = HCI_10.sub(
        lambda m: (m.group(1)
                   + f"{m.group(2)}<version>1.1</version>\n"
                   + f"{m.group(2)}<interface>\n"
                   + f"{m.group(2)}    <name>IBluetoothHci</name>\n"
                   + f"{m.group(2)}    <instance>default</instance>\n"
                   + f"{m.group(2)}</interface>"),
        s)
    open(path, "w", encoding="utf-8").write(s)
    print(f"[✓] IBluetoothHci @1.0 -> @1.1: {path}")


A2DP_IN_LC3 = re.compile(r'(<devicePort tagName="A2DP In"[^>]*?)\s+encodedFormats="AUDIO_FORMAT_LC3"')


def patch_policy(path):
    s = open(path, encoding="utf-8").read()
    if '<module name="bluetooth"' in s:
        if 'tagName="BLE Headset In"' not in s:
            die(f"{path}: 原文件已有不同的 bluetooth 模块，需手动合并")
        print("[=] 已含 bluetooth 模块，跳过")
    else:
        if s.count("</modules>") != 1:
            die(f"{path}: 未找到唯一的 </modules>")
        s = s.replace("</modules>", BT_MODULE + "    </modules>")
        print("[✓] 追加 bluetooth 模块")
    n = len(A2DP_IN_LC3.findall(s))
    if n > 1:
        die(f'{path}: "A2DP In" 端口不唯一')
    if n == 1:
        s = A2DP_IN_LC3.sub(r"\1", s)
        print('[✓] 去掉 "A2DP In" 的 LC3 encodedFormats')
    else:
        print('[=] "A2DP In" 无 LC3 encodedFormats，跳过')
    open(path, "w", encoding="utf-8").write(s)
    print(f"[✓] 音频策略: {path}")


SCEN_TWO = "VND_Two-OneChan-SnkAse-Lc3_48_2_155octs_High_Reliability_2"
SCEN_ONE = "VND_One-OneChan-SnkAse-Lc3_48_2_155octs_High_Reliability_2"
# 单耳 155 B 插到这条前面：它是 One-TwoChan 48_* 之后的第一条，后面才是 One-OneChan 48_4（120 B）
SCEN_ONE_BEFORE = "One-TwoChan-SnkAse-Lc3_24_2_Balanced_Reliability"


def _items(body):
    return [x.strip().rstrip(",").strip('"') for x in body.strip().splitlines() if x.strip()]


def _take_line(body, name):
    line = re.search(r'^([ \t]*)"%s",\n' % re.escape(name), body, re.M)
    if not line:
        die(f"Media 列表里找不到 {name}")
    return body.replace(line.group(0), "", 1), line.group(0)


def patch_scenarios(path):
    s = open(path, encoding="utf-8").read()
    m = re.search(r'"name":\s*"Media",\s*"configurations":\s*\[\n', s)
    if not m:
        die(f"{path}: 找不到 Media 场景")
    end = s.index("]", m.end())
    body = s[m.end():end]
    items = _items(body)
    for name in (SCEN_TWO, SCEN_ONE, SCEN_ONE_BEFORE):
        if items.count(name) != 1:
            die(f"{path}: Media 列表里 {name} 不是恰好一条")
    one_at = items.index(SCEN_ONE_BEFORE)
    if items[0] == SCEN_TWO and items[one_at - 1] == SCEN_ONE:
        print("[=] Media 预设顺序已改，跳过")
        return
    changed = []
    if items[0] != SCEN_TWO:
        body, line = _take_line(body, SCEN_TWO)
        body = line + body
        changed.append(f"{SCEN_TWO} 挪到最前")
    items = _items(body)
    if items[items.index(SCEN_ONE_BEFORE) - 1] != SCEN_ONE:
        body, line = _take_line(body, SCEN_ONE)
        anchor = re.search(r'^([ \t]*)"%s",\n' % re.escape(SCEN_ONE_BEFORE), body, re.M)
        body = body[:anchor.start()] + line + body[anchor.start():]
        changed.append(f"{SCEN_ONE} 挪到 {SCEN_ONE_BEFORE} 之前")
    s = s[:m.end()] + body + s[end:]
    json.loads(s)
    open(path, "w", encoding="utf-8").write(s)
    print("[✓] LE Audio 预设: " + "；".join(changed) + f": {path}")


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--module-dir", default=os.path.join(here, "module"))
    a = ap.parse_args()
    etc = os.path.join(a.module_dir, "vendor", "etc")
    for p in ("vintf/manifest_ukee.xml", "audio/sku_ukee/audio_policy_configuration.xml"):
        if not os.path.isfile(os.path.join(etc, p)):
            die(f"缺少 {os.path.join(etc, p)}（先跑 scripts/dump_device_binaries.sh）")
    patch_manifest(os.path.join(etc, "vintf/manifest_ukee.xml"))
    patch_policy(os.path.join(etc, "audio/sku_ukee/audio_policy_configuration.xml"))
    scen = os.path.join(a.module_dir, "apex/com.android.bt/etc/bluetooth/le_audio/audio_set_scenarios.json")
    if os.path.isfile(scen):
        patch_scenarios(scen)


if __name__ == "__main__":
    main()
