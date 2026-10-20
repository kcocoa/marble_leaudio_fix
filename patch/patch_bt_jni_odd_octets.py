#!/usr/bin/env python3
"""patch_bt_jni_odd_octets.py — 修补 libbluetooth_jni.so 里 LE Audio 软件编码对奇数帧长丢 1 字节的 bug。

根因（AOSP system/bta/le_audio/codec_interface.cc）
  LC3 编码输出写进 std::vector<int16_t>：
    Encode():  channel_samples = (out_offset + out_size) / 2      // 字节数 -> int16 个数，向下取整
  帧长是奇数（如 155 B 的厂商预设，标准里也有 45 B、75 B）时，缓冲区少 1 字节，
  lc3_encode 写出界，最后 1 字节是垃圾。耳机端 LC3 解码出错，听感是单耳"风声"。
  偶数帧长不受影响。

com.android.bt@370399999（Android 17）和 361099999（Android 16）的差别
  Android 16 的 client.cc 用 vector.size()*2 当发送长度，奇数帧会再少发 1 字节，
  所以旧补丁还改了三个发送点。Android 17 这版编译器已经不这么做：
    双 CIS 的左/右、以及单声道：SendIsoData 的长度直接是 octets_per_codec_frame
      （stream_config 的半字，函数入口存到 [sp,#0x24]）
    单 CIS 装两个声道：长度是 num_of_channels * octets_per_codec_frame（2*byte_count，偶数）
  发送长度已经是真实帧长，不能再套 Android 16 的发送点补丁。只改 Encode 即可：
  缓冲区按向上取整分配（155 B → 78 个 int16 = 156 B），发送仍是 155 B。

补丁（只改 APEX com.android.bt@370399999 里的这一个库，偏移对应该版本）
  Encode @0x4cf6a4：lsr w9, w8, #1  →  sub w9, w8, w8, lsr #1
  即 samples = size - size/2（向上取整）。前面的 add w8, w19, w5, uxth 是 out_offset+out_size，
  紧跟着 ldr x8, [x24, #0x48]（output_channel_samples_）作比较。偶数帧长行为不变。

带原始字节校验，可重复执行：已打过补丁则跳过；原始字节对不上（版本不同）则中止，不改文件。

用法: python3 patch/patch_bt_jni_odd_octets.py <输入 libbluetooth_jni.so> [<输出>]   # 省略输出则原地
"""
import hashlib
import struct
import sys

# (说明, 文件偏移, 原始指令字, 新指令字)
PATCHES = [
    ('Encode: samples = ceil(size/2)', 0x4cf6a4,
     [0x53017d09],
     [0x4b480509]),
]


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) == 3 else src
    data = bytearray(open(src, "rb").read())
    if data[:4] != b"\x7fELF" or struct.unpack_from("<H", data, 18)[0] != 183:
        sys.exit("[x] 不是 aarch64 ELF")

    word = lambda off: struct.unpack_from("<I", data, off)[0]
    state = []
    for name, off, old, new in PATCHES:
        cur = [word(off + 4 * i) for i in range(len(old))]
        state.append("old" if cur == old else "new" if cur == new else "bad")
    if all(s == "new" for s in state):
        print("[=] 已经打过补丁")
        if dst != src:
            open(dst, "wb").write(data)
        return
    if any(s != "old" for s in state):
        for (name, off, _, _), s in zip(PATCHES, state):
            print("   %-30s @0x%x  %s" % (name, off, s))
        sys.exit("[x] 原始指令对不上（不是 com.android.bt@370399999 的库？）。未修改任何文件。")

    for name, off, old, new in PATCHES:
        for i, w in enumerate(new):
            struct.pack_into("<I", data, off + 4 * i, w)
        print("[*] %-30s @0x%x  %d 条指令" % (name, off, len(new)))
    open(dst, "wb").write(data)
    print("[✓] %s  md5 %s" % (dst, hashlib.md5(data).hexdigest()))


if __name__ == "__main__":
    main()
