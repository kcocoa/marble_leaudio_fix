#!/usr/bin/env python3
"""Build libbluetooth_qti_real.so from the stock QTI HAL
(android.hardware.bluetooth@1.0-impl-qti.so): rename its SONAME and route
vendor-dropped HCI ISO packets (type 5) through the
IBluetoothHciCallbacks::scoDataReceived vtable slot (0x80). Idempotent.

Root cause
----------
The vendor dispatch function (prologue at 0x3ba8c) handles HCI packet types
2 (ACL, slot 0x78) and 4 (Event, slot 0x70) only. Type 5 (ISO) falls into the
"Unexpected event type %d" error branch at 0x3bdec, which merely logs and drops
the packet. The Bluetooth stack therefore never receives ISO RX data -> LE Audio
CIS cannot be maintained -> ACL supervision timeout -> teardown crash in
remove_iso_data_path (No such iso connection: 0xffff).

Fix
---
Replace the 9-instruction error block at 0x3bdec (0x24 bytes) with a trampoline
that, for type 5 only, calls the callbacks wrapper's vtable slot 0x80
(scoDataReceived, a slot the vendor dispatch NEVER invokes for any packet type).
The BluetoothHciHook shim intercepts that binder transaction (code 4) and
rewrites it in place into the @1.1 isoDataReceived call (code 5).

Verified register state at 0x3bdec (from prologue at 0x3ba8c):
    x20 = this (ctx; wrapper at this+0x10)
    x19 = data (const hidl_vec<uint8_t>*)
    w21 = HCI packet type
    x25 = logger holder, x23 = TLS, canary at [x29,-8] -> all still valid.
"""
import struct
import sys

TRAMPOLINE_OFF = 0x3BDEC   # start of the error block (replaced)
TRAMPOLINE_LEN = 0x24      # 0x3be10 - 0x3bdec = 36 bytes = 9 instructions
EVT_PATH = 0x3BB4C         # type-4 path (pattern source, not a target here)
CLEANUP   = 0x3BE10        # shared epilogue (canary check + restore + ret)

# --- v3.12 static RX patches (must be applied to the same file) ------------
# UartController::OnDataReady valid-packet-type mask: mov w9, #0x501e -> #0x503e
# (bit 5 declares HCI ISO type 5 as a legal H4 indicator; without it the HAL
#  treats every incoming ISO packet as UART desync and triggers SSR.)
RX_MASK_OFF   = 0x503D8
RX_MASK_FROM  = 0x528A03C9
RX_MASK_TO    = 0x528A07C9
# HciPacketizer header size table: type 5 header 0 -> 4 bytes (2B handle + 2B len)
RX_HDR_OFF    = 0x2F560
RX_HDR_TO     = 4
# HciPacketizer jump table: type 5 entry 34 -> 30 (ACL length parse rule)
RX_JUMP_OFF   = 0x2DD52
RX_JUMP_FROM  = 34
RX_JUMP_TO    = 30

INSTRUCTIONS = [
    # cmp w21, #0x5                    — HCI type 5 (ISO)?
    struct.pack('<I', 0x71000000 | (5 << 10) | (21 << 5) | 0x1F),
    # b.ne CLEANUP                     — other types: silent drop (as before)
    struct.pack('<I', 0x54000000 | (((CLEANUP - (TRAMPOLINE_OFF + 4)) // 4) << 5) | 0x1),
    # ldr x0, [x20, #0x10]            — x0 = callbacks wrapper (this for the call)
    struct.pack('<I', 0xF9400000 | ((0x10 >> 3) << 10) | (20 << 5) | 0),
    # ldr x21, [x0]                   — its vtable
    struct.pack('<I', 0xF9400000 | (0 << 5) | 21),
    # ldr x9, [x21, #0x80]            — slot 0x80 = scoDataReceived; the vendor
    #                                   dispatch never invokes it for any type
    struct.pack('<I', 0xF9400000 | ((0x80 >> 3) << 10) | (21 << 5) | 9),
    # mov x1, x19                      — const hidl_vec<uint8_t>& data
    struct.pack('<I', 0xAA1303E1),
    # mov x8, sp                       — Return<void> sret slot. REQUIRED: the
    #                                   generated HIDL methods zero 32 bytes at
    #                                   [x8] and write x8[0x20]; without it every
    #                                   call dies on a wild write.
    struct.pack('<I', 0x910003E8),
    # blr x9                           — marshals as SCO (code 4, @1.0 token)
    struct.pack('<I', 0xD63F0120),
    # b CLEANUP                        — done
    struct.pack('<I', 0x14000000 | ((CLEANUP - (TRAMPOLINE_OFF + 32)) // 4)),
]


# Stock ships this library as android.hardware.bluetooth@1.0-impl-qti.so; the
# shim takes over that name and loads the stock code as libbluetooth_qti_real.so.
SONAME_FROM = b'android.hardware.bluetooth@1.0-impl-qti.so'
SONAME_TO   = b'libbluetooth_qti_real.so'


def patch(blob: bytearray, off: int, old: bytes, new: bytes, what: str) -> None:
    """Idempotent: already-patched bytes are left alone, anything else aborts."""
    cur = bytes(blob[off:off + len(new)])
    if cur == new:
        print(f'  [=] {what} @ {hex(off)}')
    elif cur == old:
        blob[off:off + len(new)] = new
        print(f'  [+] {what} @ {hex(off)}: {old.hex()} -> {new.hex()}')
    else:
        raise SystemExit(f'unexpected bytes at {hex(off)} ({what}): {cur.hex()}')


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f'usage: {sys.argv[0]} <stock impl-qti.so> <out.so>')
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        blob = bytearray(f.read())

    # 0) SONAME (.dynstr; the string occurs exactly once)
    pad = SONAME_TO + b'\0' * (len(SONAME_FROM) - len(SONAME_TO))
    n_old, n_new = blob.count(SONAME_FROM + b'\0'), blob.count(SONAME_TO + b'\0')
    if n_old == 1 and n_new == 0:
        off = blob.index(SONAME_FROM + b'\0')
        blob[off:off + len(pad)] = pad
        print(f'  [+] SONAME @ {hex(off)} -> {SONAME_TO.decode()}')
    elif n_old == 0 and n_new == 1:
        print('  [=] SONAME')
    else:
        raise SystemExit(f'SONAME not found exactly once (old={n_old}, new={n_new})')

    # 1) RX patches (UART mask + packetizer tables)
    patch(blob, RX_MASK_OFF, struct.pack('<I', RX_MASK_FROM), struct.pack('<I', RX_MASK_TO), 'RX mask')
    patch(blob, RX_HDR_OFF, struct.pack('<Q', 0), struct.pack('<Q', RX_HDR_TO), 'RX hdr size')
    patch(blob, RX_JUMP_OFF, bytes([RX_JUMP_FROM]), bytes([RX_JUMP_TO]), 'RX jump')

    # 2) ISO RX trampoline (replaces the "Unexpected event type" error block)
    payload = b''.join(INSTRUCTIONS)
    assert len(payload) == TRAMPOLINE_LEN, (len(payload), TRAMPOLINE_LEN)
    cur = bytes(blob[TRAMPOLINE_OFF:TRAMPOLINE_OFF + TRAMPOLINE_LEN])
    if cur == payload:
        print(f'  [=] trampoline @ {hex(TRAMPOLINE_OFF)}')
    elif cur[:4] == bytes.fromhex('21fffff0'):  # original block starts with adrp x1
        blob[TRAMPOLINE_OFF:TRAMPOLINE_OFF + TRAMPOLINE_LEN] = payload
        print(f'  [+] trampoline @ {hex(TRAMPOLINE_OFF)} ({TRAMPOLINE_LEN} bytes)')
    else:
        raise SystemExit(f'unexpected bytes at {hex(TRAMPOLINE_OFF)}: {cur[:4].hex()}')

    with open(dst, 'wb') as f:
        f.write(blob)
    print(f'patched {src} -> {dst}')


if __name__ == '__main__':
    main()
