#!/usr/bin/env python3
"""patch_bt_jni_odd_octets.py — 修补 libbluetooth_jni.so 里 LE Audio 软件编码对奇数帧长丢 1 字节的 bug。

根因（AOSP system/bta/le_audio/codec_interface.cc 和 client.cc）
  LC3 编码输出写进 std::vector<int16_t>：
    Encode():  channel_samples = (out_offset + out_size) / 2      // 字节数 -> int16 个数，向下取整
    client.cc: SendIsoData(handle, vec.data(), vec.size() * 2)    // 发送长度也取自 vector
  帧长是奇数（如 AOSP qpr2 的 155 B 预设，标准里也有 45 B、75 B）时，每帧少发 1 字节，
  耳机端 LC3 解码出错，听感是单耳"风声"。偶数帧长不受影响。

补丁（只改 APEX com.android.bt@361099999 里的这一个库，偏移对应该版本）
  1. Encode：samples = size - size/2，即向上取整（一条指令：sub w9,w8,w8,lsr #1），
     缓冲区够装完整的编码帧，偶数帧长行为不变。
  2. 三个发送点（单 CIS、双 CIS 的左和右）：编译器每处都调用了两次 GetDecodedSamples()，
     第二次纯属多余。去掉它，腾出的指令位用来算真实长度：
        L = (vector 字节数 / 2 == byte_count) ? vector 字节数 : byte_count
     前者是单 CIS 里一个 CIS 装两个声道（长度 2*byte_count，必为偶数），后者是其余情况。
     byte_count 即 octets_per_codec_frame，在栈上 [sp,#108]。

带原始字节校验，可重复执行：已打过补丁则跳过；原始字节对不上（版本不同）则中止，不改文件。

用法: python3 patch/patch_bt_jni_odd_octets.py <输入 libbluetooth_jni.so> [<输出>]   # 省略输出则原地
"""
import hashlib
import struct
import sys

PATCHES = [
    # (说明, 文件偏移, 原始指令字, 新指令字)
    ('Encode: samples = ceil(size/2)', 0x510d2c,
     [0x53017d09],
     [0x4b480509]),
    ('single-CIS send', 0x44053c,
     [0xf9410368, 0xf9400017, 0xf9400109, 0xaa0803e0, 0xf9402929, 0xd63f0120, 0xf94002c8, 0xb9400809, 0x2a1503e1],
     [0xf9400017, 0xb9400809, 0x7940dbeb, 0x4b170129, 0x6b49057f, 0x1a8b0123, 0xf94002c8, 0x2a1503e1, 0xd503201f]),
    ('left-CIS send', 0x440674,
     [0xf9410368, 0xf9400018, 0xf9400109, 0xaa0803e0, 0xf9402929, 0xd63f0120, 0xf94002e8, 0xb9400809, 0x2a1603e1, 0xb940000a, 0xaa1703e0, 0xaa1803e2, 0xf9403508, 0x4b0a0129, 0x121f7923],
     [0xf9400018, 0xb9400809, 0x7940dbeb, 0x4b180129, 0x6b49057f, 0x1a8b0123, 0xf94002e8, 0x2a1603e1, 0xaa1703e0, 0xaa1803e2, 0xf9403508, 0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f]),
    ('right-CIS send', 0x4406d8,
     [0xf9410768, 0xf9400017, 0xf9400109, 0xaa0803e0, 0xf9402929, 0xd63f0120, 0xf94002c8, 0xb9400809, 0x2a1503e1],
     [0xf9400017, 0xb9400809, 0x7940dbeb, 0x4b170129, 0x6b49057f, 0x1a8b0123, 0xf94002c8, 0x2a1503e1, 0xd503201f]),
    ('shared tail: drop ldr w10', 0x4406fc,
     [0xb940000a],
     [0xd503201f]),
    ('shared tail: drop sub/and', 0x440708,
     [0x4b0a0129, 0x121f7923],
     [0xd503201f, 0xd503201f]),
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
        sys.exit("[x] 原始指令对不上（不是 com.android.bt@361099999 的库？）。未修改任何文件。")

    for name, off, old, new in PATCHES:
        for i, w in enumerate(new):
            struct.pack_into("<I", data, off + 4 * i, w)
        print("[*] %-30s @0x%x  %d 条指令" % (name, off, len(new)))
    open(dst, "wb").write(data)
    print("[✓] %s  md5 %s" % (dst, hashlib.md5(data).hexdigest()))


if __name__ == "__main__":
    main()
