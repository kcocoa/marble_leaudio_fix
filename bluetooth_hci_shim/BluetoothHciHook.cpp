//
// BluetoothHciHook.cpp — v3 (ABI-correct shadow vtables)
//
// Shim library replacing vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so
// Adds android.hardware.bluetooth@1.1 (sendIsoData / initialize_1_1) support on top
// of the stock Qualcomm 1.0 passthrough HAL, entirely in userspace.
//
// Mechanism (all slot indices verified against on-device binaries):
//   1. HIDL_FETCH_IBluetoothHci returns the stock Qualcomm object with its primary
//      vtable shadowed: slot 1 (interfaceChain) hooked to report the 1.1+1.0+IBase
//      chain, so hwservicemanager registers the service under the 1.1 name too.
//   2. BnConstructorMap["...@1.0::IBluetoothHci"] hooked: the real 1.0 BnHw binder
//      is created, then its primary vtable is shadowed with slot 11 (onTransact)
//      hooked to handle transaction codes 6 (initialize_1_1) and 7 (sendIsoData).
//   3. Shadow vtables copy the full Itanium-ABI prefix (vbase_offset at vptr[-3],
//      offset-to-top at vptr[-2], RTTI at vptr[-1]) from the *live* object vptr,
//      so vbase adjustments (RefBase::incStrong etc.) keep working.
//
// ABI facts (verified via disassembly of device /vendor/lib64 binaries):
//   sp<T>          = 8 bytes (single pointer)
//   hidl_string    = 16 bytes {const char* buf; uint32 size; bool owns}
//   hidl_vec<T>    = 16 bytes {T* buf; uint32 size; bool owns; uint8 pad[3]}
//   Status         = 32 bytes, Status::ok() zeroes all 32 bytes via sret
//   Return<void>   = 40 bytes via sret, all-zero == OK
//   BnHw object    : primary vptr @ +0, secondary vptr @ +0x20, mImpl sp @ +0x78
//   HCI impl vptr  : [0]=isRemote [1]=interfaceChain [11]=D1 [12]=D0 [13]=initialize
//                    [14]=sendHciCommand [15]=sendAclData [16]=sendScoData
//   BnHw vptr      : [0]=transact ... [11]=onTransact
//   std::function  : callable __func object at +0x20, operator() at vptr[6]
//

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <string>
#include <functional>
#include <android/log.h>
#include <hidl/ConcurrentMap.h>

// bionic property API (resolved from libc at runtime)
extern "C" int __system_property_get(const char* name, char* value);
#define PROP_VALUE_MAX 92

#define LOG_TAG "BluetoothHciHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Minimal ABI-verified type declarations. These declarations produce the same
// mangled names as the real platform classes, so the linker binds them to the
// already-loaded libhidlbase / libutils implementations.
// ---------------------------------------------------------------------------
namespace android {

class RefBase {
public:
    void incStrong(const void* id) const;
    void decStrong(const void* id) const;
};

template <typename T>
struct sp {  // 8 bytes; REAL sp is ::android::sp from utils/StrongPointer.h.
    // The real sp<T> has a non-trivial dtor (decStrong), which makes it
    // "non-trivial for calls" — returned via x8 (sret) even at 8 bytes.
    // The empty user-provided dtor below reproduces that ABI. Copies are
    // shallow (no refcount changes), keeping the shim fully transparent.
    T* m_ptr = nullptr;
    ~sp() {}
};

namespace hardware {

struct IBinder;  // opaque

class Parcel {
public:
    bool enforceInterface(const char* interfaceName) const;
    int32_t readBuffer(size_t buffer_size, size_t* buffer_handle, const void** buffer_out) const;
    int32_t readNullableEmbeddedBuffer(size_t buffer_size, size_t* buffer_handle,
                                       size_t parent_buffer_handle, size_t parent_offset,
                                       const void** buffer_out) const;
    int32_t readNullableStrongBinder(::android::sp<IBinder>* binder) const;
    // v3.3: raw buffer accessors from libhidlbase (used by the binder transact hook
    // to scan outbound hciEventReceived parcels for Number-of-Completed-Packets events)
    const uint8_t* data() const;
    size_t dataSize() const;
    // v3.4: parcel cursor (needed to read a parcel and then rewind so the stock
    // generated code can parse it normally).
    size_t dataPosition() const;
    int32_t setDataPosition(size_t pos) const;
};

struct Status {  // 32 bytes
    uint8_t bytes[32];
    static Status ok();
};

int32_t writeToParcel(const Status& status, Parcel* parcel);

// details::getBnConstructorMap lives in libhidlbase (already linked at build time)
namespace details {
struct FakeIBase;  // ::android::hidl::base::V1_0::IBase — unused, real map key is std::string
using BnConstructorMapType =
    ConcurrentMap<std::string, std::function<sp<IBinder>(void*)>>;
BnConstructorMapType& getBnConstructorMap();
}  // namespace details

}  // namespace hardware
}  // namespace android

// ---------------------------------------------------------------------------
// ABI types used by hooks (no linkage involved — pure layout)
// ---------------------------------------------------------------------------
namespace {

struct HidlString {  // 16 bytes
    const char* mBuffer;
    uint32_t mSize;
    bool mOwnsBuffer;
    uint8_t mPad[3];
};
static_assert(sizeof(HidlString) == 16, "hidl_string layout");

struct HidlVec {  // 16 bytes — layout of hidl_vec<uint8_t> and hidl_vec<hidl_string>
    void* mBuffer;
    uint32_t mSize;
    bool mOwnsBuffer;
    uint8_t mPad[3];
};
static_assert(sizeof(HidlVec) == 16, "hidl_vec layout");

struct HidlReturnVoid40 {  // Return<void> = details::return_status, 40 bytes, sret
    uint8_t bytes[40];
};
static_assert(sizeof(HidlReturnVoid40) == 40, "Return<void> layout");

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
constexpr const char* kHci11Descriptor = "android.hardware.bluetooth@1.1::IBluetoothHci";
constexpr const char* kHci10Descriptor = "android.hardware.bluetooth@1.0::IBluetoothHci";
constexpr const char* kBaseDescriptor = "android.hidl.base@1.0::IBase";

// ISO credit multiplication (v3.2):
// The hastings firmware reports only 3 ISO buffers and returns NCP credits in
// ~78ms batches, capping sustained throughput at ~38 SDU/s while LC3 48_4
// needs 100 SDU/s. We hook hciEventReceived on the callbacks wrapper and
// multiply the "Number of Completed Packets" count for ISO handles, letting
// the stack keep a deeper in-flight pipeline.
// Runtime knob: persist.vendor.leaudio.isocred.mult (default 6; 1 = disabled)
// ---------------------------------------------------------------------------
static int GetIsoCreditMult() {
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.vendor.leaudio.isocred.mult", v) > 0) {
        int m = atoi(v);
        if (m >= 1 && m <= 64) return m;
    }
    return 6;
}

static uint16_t gIsoHandles[16] = {0};
static uint32_t gIsoHandleTick[16] = {0};
static uint32_t gIsoSendTick = 0;
// v3.4: synth NCP is opt-in (persist.vendor.leaudio.ncpsynth=1). The v3.3j
// implementation corrupts the event stream (13-byte CC rewritten into a 6-byte
// NCP without fixing the length/size fields) -> the stack parses a bogus
// "Hardware Error 0x0f" event and aborts (SIGABRT). Kept for experiments only.
static bool NcpSynthEnabled() {
    static int sVal = -1;
    if (sVal < 0) {
        char v[PROP_VALUE_MAX] = {0};
        sVal = (__system_property_get("persist.vendor.leaudio.ncpsynth", v) > 0 && v[0] == '1') ? 1 : 0;
    }
    return sVal == 1;
}
// v3.3j: per-handle outstanding-send accounting for honest NCP synthesis.
// Each sendIsoData(h) increments pending[h]; each CC-0x1407 (= one firmware
// completion) is attributed FIFO to the oldest pending handle and reported
// as a standard NCP with count=1. Fluoride's used_credits can never underflow
// this way (each synthesized NCP maps to a real outstanding send).
static uint32_t gIsoPending[16] = {0};
// v3.7: credit-window accounting. The stack starts with a tiny credit pool
// (3) and only refills from NCPs, which the QCI HAL swallows. Measured
// steady-state throughput = initial_credits x grant_rate = 3 x 10/s = 30/s,
// which matches the observed wall exactly. Echoing the sent count back (v3.6)
// cannot raise it, because grants == sends keeps the pool constant. Instead we
// keep a WINDOW of credits in flight (the controller really completes every
// packet 1:1 and has 155 buffers, so a window of ~12/handle is well within
// its capacity).
static uint32_t gIsoSent[16] = {0};
static uint32_t gIsoGranted[16] = {0};
static int IsoCreditWindow() {
    // v3.9: re-read every call (grant path runs ~10/s) so the window can be
    // swept live with setprop, no HAL restart needed.
    char v[PROP_VALUE_MAX] = {0};
    int n = __system_property_get("persist.vendor.leaudio.isocred.window", v);
    int w = 12;
    if (n > 0) {
        int t = atoi(v);
        if (t >= 0 && t <= 200) w = t;
    }
    return w;
}
static int FindIsoHandleSlot(uint16_t h) {
    for (int i = 0; i < 16; i++)
        if (gIsoHandles[i] == h) return i;
    return -1;
}

static void RecordIsoHandle(uint16_t h) {
    gIsoSendTick++;
    int slot = FindIsoHandleSlot(h);
    if (slot < 0 || gIsoHandles[slot] != h) {
        // unknown handle: find a free slot or overwrite the oldest
        slot = -1;
        for (int i = 0; i < 16; i++)
            if (gIsoHandles[i] == 0) { slot = i; break; }
        if (slot < 0) {
            int oldest = 0;
            for (int i = 1; i < 16; i++)
                if (gIsoHandleTick[i] < gIsoHandleTick[oldest]) { oldest = i; }
            slot = oldest;
        }
        gIsoHandles[slot] = h;
        gIsoPending[slot] = 0;
        gIsoSent[slot] = 0;
        gIsoGranted[slot] = 0;
    }
    gIsoHandleTick[slot] = gIsoSendTick;
    gIsoPending[slot]++;
    gIsoSent[slot]++;
}

static bool IsIsoHandle(uint16_t h) {
    for (int i = 0; i < 16; i++) {
        if (gIsoHandles[i] == h && gIsoHandles[i] != 0 &&
            (gIsoSendTick - gIsoHandleTick[i]) < 4096) {
            return true;
        }
    }
    return false;
}

void* gStockHci = nullptr;
typedef void (*SendDataToControllerFn)(void* thiz, int packetType, const HidlVec* data);
SendDataToControllerFn gSendDataToController = nullptr;

// v3.8: fix the QTI ring buffer's refusal of HCI ISO packets (type 5).
//
// Root cause of the user's "Bluetooth keeps crashing": PacketBuff::AddBuffNode
// dispatches on HciPacketType via a byte table in .rodata
// (AddBuffNode @0x81bbc: adrp x9,0x2e000; add x9,x9,#0x4ac; ldrb; br).
// Dumping it: type 1->CMD, 2->ACL, 3->SCO, 4->EVT, and type 5 -> 0x81ce8 =
// the "Received packet with wrong packet type" error path. So every ISO packet
// the stack produces is DROPPED inside the HAL and never reaches the
// controller. The controller's ISO path then starves and the firmware asserts:
//   HW error event 0x10 code 0x0f -> SSR -> HAL dies -> stack aborts
//   -> audio connection drops ("蓝牙闪退"), ~4s after each ISO burst.
// Verified: 4/4 ring-buffer error bursts (14 msgs each) preceded an SSR by 4s.
//
// ISO and ACL HCI framing are identical (handle:2, length:2, payload), so the
// ACL entry's length rule (16-bit len @off 2, +5) is exactly right for ISO.
// We patch the table entry for type 5 to the ACL entry value. The table lives
// in the real lib's .rodata in *this* process, so it is a runtime memory patch
// only: no partition is touched and a HAL restart reverts it.
static bool RingIsoPatchEnabled() {
    static int sV = -1;
    if (sV < 0) {
        char v[PROP_VALUE_MAX] = {0};
        sV = (__system_property_get("persist.vendor.leaudio.iso.patchring", v) > 0 &&
              v[0] == '0') ? 0 : 1;
    }
    return sV == 1;
}

static bool PatchRingBufferIsoType() {
    if (gSendDataToController == nullptr) return false;
    Dl_info info;
    if (dladdr((void*)gSendDataToController, &info) == 0 || info.dli_fbase == nullptr) {
        LOGE("ring-patch: dladdr failed");
        return false;
    }
    const uintptr_t kTableOff = 0x2e4ac;  // from AddBuffNode's adrp/add
    const uint8_t kAclEntry = 25;         // entry[type-1] for type 2 (ACL)
    uint8_t* entry = (uint8_t*)((uintptr_t)info.dli_fbase + kTableOff + 4);  // type 5
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    void* page = (void*)((uintptr_t)entry & ~(uintptr_t)(pg - 1));
    if (mprotect(page, (size_t)pg, PROT_READ | PROT_WRITE) != 0) {
        LOGE("ring-patch: mprotect failed: %s", strerror(errno));
        return false;
    }
    uint8_t before = *entry;
    *entry = kAclEntry;
    __builtin___clear_cache((char*)entry, (char*)entry + 1);
    mprotect(page, (size_t)pg, PROT_READ);
    LOGI("ring-patch: ISO(type5) table entry %u -> %u at %p (base %p)", before, *entry,
         (void*)entry, info.dli_fbase);
    return before != kAclEntry;
}

// BpHwBluetoothHciCallbacks ctor from the 1.0 interface lib: (this, const sp<IBinder>&)
typedef void (*BpCallbacksCtorFn)(void* thiz, const void* spBinder);
BpCallbacksCtorFn gBpCallbacksCtor = nullptr;
void* gHciEventReceivedSym = nullptr;  // set in ResolveSymbols()

// hciEventReceived hook (on the Bp callbacks wrapper) — see NCP multiplication above.
typedef HidlReturnVoid40 (*HciEventReceivedFn)(void* thiz, const HidlVec* event);
HciEventReceivedFn gOrigHciEventReceived = nullptr;
HciEventReceivedFn gOrigHciEventReceivedAlt = nullptr;
void* gBpShadowStorage[64] = {nullptr};   // prefix + slots 0..59
bool gBpHookInstalled = false;
static uint32_t sMultPackets = 0;

HidlReturnVoid40 HookedHciEventReceivedImpl(HciEventReceivedFn orig,
                                            void* thiz, const HidlVec* event,
                                            const char* tag) {
    if (orig == nullptr || event == nullptr || event->mBuffer == nullptr) {
        HidlReturnVoid40 z; memset(z.bytes, 0, sizeof(z.bytes)); return z;
    }
    static uint32_t sAllEvents = 0;
    if (++sAllEvents <= 10 || (sAllEvents & 0x3ff) == 1) {
        const uint8_t* db = (const uint8_t*)event->mBuffer;
        LOGI("evt[%s]#%u: code=0x%02x len=%u b2..7=%02x %02x %02x %02x %02x %02x mult=%d",
             tag, sAllEvents, event->mSize >= 1 ? db[0] : 0, event->mSize,
             event->mSize >= 3 ? db[2] : 0, event->mSize >= 4 ? db[3] : 0,
             event->mSize >= 5 ? db[4] : 0, event->mSize >= 6 ? db[5] : 0,
             event->mSize >= 7 ? db[6] : 0, event->mSize >= 8 ? db[7] : 0,
             GetIsoCreditMult());
    }
    int mult = GetIsoCreditMult();
    const uint8_t* b = (const uint8_t*)event->mBuffer;
    uint32_t n = event->mSize;
    uint8_t tmp[64];
    bool modified = false;
    uint16_t repH = 0; uint32_t repFrom = 0, repTo = 0;
    // HCI Number of Completed Packets event: [0]=0x13 [1]=len [2]=num_handles,
    // then per handle: [handle:2][completed:2]
    if (mult > 1 && n >= 8 && n <= sizeof(tmp) && b[0] == 0x13) {
        uint32_t numHandles = b[2];
        if (3 + 4 * numHandles == n) {
            memcpy(tmp, b, n);
            for (uint32_t i = 0; i < numHandles; i++) {
                uint16_t h = (uint16_t)(tmp[3 + 4*i] | (tmp[4 + 4*i] << 8));
                if (IsIsoHandle(h)) {
                    uint32_t cnt = (uint32_t)(tmp[5 + 4*i] | (tmp[6 + 4*i] << 8));
                    uint32_t ncnt = cnt * (uint32_t)mult;
                    if (ncnt > 0xFFFF) ncnt = 0xFFFF;
                    tmp[5 + 4*i] = (uint8_t)(ncnt & 0xFF);
                    tmp[6 + 4*i] = (uint8_t)((ncnt >> 8) & 0xFF);
                    modified = true;
                    if (repH == 0) { repH = h; repFrom = cnt; repTo = ncnt; }
                }
            }
        }
    }
    if (modified) {
        if ((++sMultPackets & 0x3f) == 1) {
            LOGI("NCP mult x%d: hdl 0x%x %u->%u", mult, repH, repFrom, repTo);
        }
        HidlVec vec;
        vec.mBuffer = tmp;
        vec.mSize = n;
        vec.mOwnsBuffer = false;
        vec.mPad[0] = vec.mPad[1] = vec.mPad[2] = 0;
        return orig(thiz, &vec);
    }
    return orig(thiz, event);
}

HidlReturnVoid40 HookedHciEventReceived(void* thiz, const HidlVec* event) {
    return HookedHciEventReceivedImpl(gOrigHciEventReceived, thiz, event, "A");
}

HidlReturnVoid40 HookedHciEventReceivedAlt(void* thiz, const HidlVec* event) {
    return HookedHciEventReceivedImpl(gOrigHciEventReceivedAlt, thiz, event, "B");
}

// onTransact original (slot 11 of the BnHw primary vtable)
typedef int32_t (*OnTransactFn)(void* self, uint32_t code,
                                const ::android::hardware::Parcel* data,
                                ::android::hardware::Parcel* reply,
                                uint32_t flags, const void* hidlCb);
OnTransactFn gOrigOnTransact = nullptr;

// ---------------------------------------------------------------------------
// v3.3: mRemote (BpHwBinder) transact hook — the CFI-safe event interception.
//
// Vendor dispatch (lambda in DataHandler) calls the *real* callbacks wrapper
// (not ours — it re-wraps Fluoride's binder), and does so under explicit
// __cfi_slowpath — so shadowing any IBluetoothHciCallbacks vtable is unsafe.
// But every wrapper shares the same mRemote BpHwBinder (ProcessState caches one
// per handle), and its vtable slot 0 is `transact` (verified: _ZTVBpHwBinder at
// 0xa6928, vptr point 0xa6940 = BpHwBinder::transact; Bp marshal code does
// `ldr x8,[x0]; ldr x8,[x8]; blr` — slot 0, from the CFI-free iface lib).
// hciEventReceived marshals the event into the parcel and transacts with
// code 2. We scan the parcel buffer for self-consistent NCP event structures
// (0x13, param_len == 1+4n, complete handle/count tuples), multiply the counts
// for ISO handles (tracked by the sendIsoData hook), then forward.
// ---------------------------------------------------------------------------
typedef int32_t (*BinderTransactFn)(void* thiz, uint32_t code,
                                     const ::android::hardware::Parcel* data,
                                     ::android::hardware::Parcel* reply,
                                     uint32_t flags, void* onTransactDone);
BinderTransactFn gOrigBinderTransact = nullptr;
void* gBinderShadowStorage[40] = {nullptr};  // prefix(4) + slots 0..35
void* gHookedBinder = nullptr;
static uint32_t sEvtParcels = 0;
static uint32_t sCcOpcodeHist[65536] = {0};  // per-opcode CC counts (v3.3g diag)
static uint32_t sEvtMultApplied = 0;

static bool MultiplyNcpInBuffer(uint8_t* b, size_t size) {
    bool modified = false;
    const int mult = GetIsoCreditMult();
    if (mult <= 1) return false;
    // scan for: [0]=0x13 [1]=param_len==1+4n [2]=n, fully contained
    // (NCP param_len = num_handles byte + 4B per handle tuple)
    for (size_t i = 0; i + 4 <= size; i++) {
        if (b[i] != 0x13) continue;
        uint32_t n = b[i + 2];
        if (n < 1 || n > 10) continue;
        if (b[i + 1] != (uint8_t)(1 + 4 * n)) continue;
        if (i + 3 + 4 * n > size) continue;
        for (uint32_t k = 0; k < n; k++) {
            uint16_t h = (uint16_t)(b[i + 3 + 4 * k] | (b[i + 4 + 4 * k] << 8));
            if (!IsIsoHandle(h)) continue;
            uint32_t cnt = (uint32_t)(b[i + 5 + 4 * k] | (b[i + 6 + 4 * k] << 8));
            uint32_t ncnt = cnt * (uint32_t)mult;
            if (ncnt > 0xFFFF) ncnt = 0xFFFF;
            b[i + 5 + 4 * k] = (uint8_t)(ncnt & 0xFF);
            b[i + 6 + 4 * k] = (uint8_t)((ncnt >> 8) & 0xFF);
            modified = true;
            if ((++sEvtMultApplied & 0x3f) == 1) {
                LOGI("NCP mult x%d in transact: hdl 0x%x %u->%u (parcel off %zu)",
                     mult, h, cnt, ncnt, i);
            }
        }
    }
    return modified;
}

int32_t HookedBinderTransact(void* thiz, uint32_t code,
                             const ::android::hardware::Parcel* data,
                             ::android::hardware::Parcel* reply,
                             uint32_t flags, void* onTransactDone) {
    if (code == 2 && data != nullptr) {  // 2 = IBluetoothHciCallbacks::hciEventReceived
        const uint8_t* buf = data->data();
        size_t sz = data->dataSize();
        if ((++sEvtParcels & 0x3ff) == 1) {
            LOGI("transact hook: evt parcel #%u (data=%p size=%zu)", sEvtParcels,
                 buf, sz);
        }
        // Wire format (verified by hexdump): token string + a series of "buffer
        // objects" of 0x28 bytes each: {magic 0x70742a85 LE, flags u32,
        //  ptr u64, size u64}. flags==1 entries point at the actual event bytes
        // on the heap — NOT inlined in the parcel. Scan the parcel for these
        // objects and multiply NCP counts inside the pointed-to event buffer.
        {
            static const uint8_t kBufObjMagic[4] = {0x85, 0x2a, 0x74, 0x70};
            static uint32_t sEvtCodeHist[256] = {0};
            bool anyFlag1 = false;
            for (size_t o = 0; o + 0x18 <= sz; o += 4) {
                if (memcmp(buf + o, kBufObjMagic, 4) != 0) continue;
                uint32_t flags = (uint32_t)buf[o + 4] | ((uint32_t)buf[o + 5] << 8) |
                                 ((uint32_t)buf[o + 6] << 16) | ((uint32_t)buf[o + 7] << 24);
                if (flags != 1) continue;  // 0 = the hidl_vec header blob itself
                uint64_t ptr = 0, bsz = 0;
                memcpy(&ptr, buf + o + 8, 8);
                memcpy(&bsz, buf + o + 0x10, 8);
                if (ptr < 0x1000 || bsz < 8 || bsz > 2048) continue;
                if ((((uintptr_t)ptr) >> 48) != 0xb400) continue;  // heap pattern guard
                const uint8_t* eb = (const uint8_t*)(uintptr_t)ptr;
                if (bsz >= 1) sEvtCodeHist[eb[0]]++;
                // v3.3g: opcode histogram for Command Complete (0x0e):
                // [0]=0x0e [1]=plen [2]=num_cmd [3..4]=opcode LE
                if (bsz >= 5 && eb[0] == 0x0e) {
                    uint16_t opc = (uint16_t)(eb[3] | (eb[4] << 8));
                    sCcOpcodeHist[opc]++;
                    static uint32_t sCcDumped = 0;
                    if (sCcDumped < 6 && (opc == 0x2062 || opc == 0x1407 || opc == 0x2058)) {
                        sCcDumped++;
                        char h[128]; int o2 = 0;
                        for (size_t q = 0; q < bsz && q < 24; q++)
                            o2 += snprintf(h + o2, sizeof(h) - o2, "%02x ", eb[q]);
                        LOGI("CC opc=0x%04x dump[%zu]: %s", opc, bsz, h);
                    }
                }
                // v3.3g: dump 0x14 events (what are they?)
                {
                    static uint32_t s14Dumped = 0;
                    if (bsz >= 2 && eb[0] == 0x14 && s14Dumped < 4) {
                        s14Dumped++;
                        char h[64]; int o2 = 0;
                        for (size_t q = 0; q < bsz && q < 12; q++)
                            o2 += snprintf(h + o2, sizeof(h) - o2, "%02x ", eb[q]);
                        LOGI("evt 0x14 dump[%zu]: %s", bsz, h);
                    }
                }
                anyFlag1 = true;
                // v3.4: honest NCP synthesis — fixes the credit starvation.
                //
                // Facts (verified with a full btsnoop capture, 2026-10-05):
                //  * transport level (HAL<->controller): the controller DOES send
                //    standard NCPs (0x13) for every ISO packet, 1:1 with our ISO
                //    data packets (handle 0x05: 686 sent / 686 completed).
                //  * stack level (what the HAL forwards to Fluoride): ZERO 0x13.
                //    The QCI HAL swallows them and only forwards its own vendor
                //    0x1407 poll Command Complete (~10/s) which carries a private
                //    credit counter. Fluoride therefore never refills iso_credits_
                //    -> it drops ~85% of the audio (the stutter).
                //  * The HAL's "vendor wraps ISO as 0x1407" theory was WRONG: ISO
                //    data goes to the controller as standard HCI ISO packets
                //    (packet type 0x05).
                //
                // Since the controller completes exactly one credit per ISO packet
                // we forwarded, crediting exactly gIsoPending[] per handle is
                // honest (not over-crediting: the v3.3i underflow came from
                // multiplying an already-correct count). We piggyback on the HAL's
                // own 0x1407 CC and report all handles at once.
                //
                // CRITICAL: the HIDL vec length field inside the parcel must be
                // updated to the shortened event size. v3.3j rewrote the 13-byte CC
                // into a 6-byte NCP without touching it, so the stack parsed the 7
                // stale trailing bytes as a bogus event -> "Hardware Error 0x0f" ->
                // SIGABRT crash loop.
                if (NcpSynthEnabled() && bsz >= 7 && eb[0] == 0x0e && eb[3] == 0x07 &&
                    eb[4] == 0x14) {
                    const int win = IsoCreditWindow();
                    int n = 0;
                    for (int q = 0; q < 16; q++) {
                        if (gIsoHandles[q] == 0) continue;
                        if (win == 0) {
                            if (gIsoPending[q] > 0) n++;   // honest echo mode
                        } else {
                            int64_t avail = 3 + (int64_t)gIsoGranted[q] - (int64_t)gIsoSent[q];
                            if ((int64_t)win - avail > 0) n++;
                        }
                    }
                    size_t need = 3 + 4 * (size_t)n;
                    if (n > 0 && need <= bsz) {
                        uint8_t* w = (uint8_t*)eb;  // event buffer is heap, writable
                        w[0] = 0x13;                // Number Of Completed Packets
                        w[1] = (uint8_t)(1 + 4 * n);
                        w[2] = (uint8_t)n;
                        size_t o2 = 3;
                        uint32_t total = 0;
                        for (int q = 0; q < 16; q++) {
                            if (gIsoHandles[q] == 0) continue;
                            int64_t c;
                            if (win == 0) {
                                if (gIsoPending[q] == 0) continue;
                                c = (int64_t)gIsoPending[q];
                            } else {
                                int64_t avail = 3 + (int64_t)gIsoGranted[q] - (int64_t)gIsoSent[q];
                                c = (int64_t)win - avail;
                                if (c <= 0) continue;
                            }
                            if (c > 0xFFFF) c = 0xFFFF;
                            uint16_t h = gIsoHandles[q];
                            w[o2++] = (uint8_t)(h & 0xFF);
                            w[o2++] = (uint8_t)(h >> 8);
                            w[o2++] = (uint8_t)((uint16_t)c & 0xFF);
                            w[o2++] = (uint8_t)(((uint16_t)c >> 8) & 0xFF);
                            total += (uint32_t)c;
                            gIsoGranted[q] += (uint32_t)c;
                            gIsoPending[q] = 0;
                        }
                        // shrink the hidl_vec so the stack reads exactly our event
                        uint64_t nsz = (uint64_t)need;
                        memcpy((void*)(uintptr_t)(buf + o + 0x10), &nsz, 8);
                        static uint32_t sSynthNcp = 0;
                        if ((++sSynthNcp & 0x07) == 1) {
                            LOGI("NCP synth win=%d: %d handle(s), %u credit(s) (bsz %zu->%zu) (#%u)",
                                 win, n, total, bsz, need, sSynthNcp);
                        }
                    }
                }
                MultiplyNcpInBuffer((uint8_t*)eb, (size_t)bsz);
            }
            // periodic histogram: which event codes flow through hciEventReceived?
            if ((sEvtParcels & 0x3ff) == 0 && anyFlag1) {
                char h[720]; int off = 0;
                for (int e = 0; e < 256 && off < (int)sizeof(h) - 24; e++) {
                    if (sEvtCodeHist[e])
                        off += snprintf(h + off, sizeof(h) - off, "%02x:%u ", e, sEvtCodeHist[e]);
                }
                LOGI("evt code hist after %u parcels: %s", sEvtParcels, h);
                char h2[640]; off = 0;
                for (int o = 0; o < 65536 && off < (int)sizeof(h2) - 20; o++) {
                    if (sCcOpcodeHist[o])
                        off += snprintf(h2 + off, sizeof(h2) - off, "%04x:%u ", o, sCcOpcodeHist[o]);
                }
                LOGI("CC opcode hist: %s", h2);
            }
        }
        if (buf != nullptr && sz >= 8 && sz <= 8192) {
            MultiplyNcpInBuffer((uint8_t*)buf, sz);  // legacy inline fallback
        }
    }
    return gOrigBinderTransact(thiz, code, data, reply, flags, onTransactDone);
}

void InstallBinderTransactHook(void* binder) {
    if (binder == nullptr) return;
    void** liveVptr = *(void***)binder;
    if (liveVptr == nullptr) return;
    if (binder == gHookedBinder && *(void***)binder == &gBinderShadowStorage[4]) {
        return;  // already hooked (same binder re-registered)
    }
    // copy prefix + slots: liveVptr[-4 .. +35]
    memcpy(gBinderShadowStorage, liveVptr - 4, sizeof(gBinderShadowStorage));
    gOrigBinderTransact = (BinderTransactFn)liveVptr[0];
    gBinderShadowStorage[4 + 0] = (void*)&HookedBinderTransact;
    gHookedBinder = binder;
    *(void***)binder = &gBinderShadowStorage[4];
    LOGI("mRemote transact hook installed: binder=%p vptr=%p orig=%p",
         binder, (void*)liveVptr, (void*)gOrigBinderTransact);
}

// v3.3b diagnostics: walk the DataHandler's known members after initialize to
// identify the REAL callbacks object the vendor dispatch uses (the wrapper
// the vendor built from Fluoride's binder — not our own wrapper).
// All pointer derefs go through /proc/self/mem pread probes: invalid pointers
// return EIO instead of raising SIGSEGV (the v3.3 bare-deref version crashed
// the HAL service — fault addr 0x34cf000000018112, crash loop).
static int gSelfMemFd = -1;
static void OpenSelfMem() {
    if (gSelfMemFd < 0) gSelfMemFd = open("/proc/self/mem", O_RDONLY);
}
// Safely read 8 bytes at addr. Returns true on success.
static bool SafeReadQ(void* addr, uint64_t* out) {
    if (addr == nullptr) return false;
    uintptr_t a = (uintptr_t)addr;
    if (a < 0x1000 || (a & 7) != 0) return false;
    if (gSelfMemFd >= 0) {
        ssize_t r = pread(gSelfMemFd, out, 8, (off_t)a);
        if (r == 8) return true;
        static bool sLogged = false;
        if (!sLogged) {
            sLogged = true;
            LOGW("pread(%p) -> %zd errno=%d", addr, r, errno);
        }
    }
    // fallback: Android Scudo heap pointers here look like 0xb4xxxxxxxxxxxxxx
    if ((a >> 48) == 0xb400) {
        *out = *(uint64_t*)a;
        return true;
    }
    return false;
}

void DumpVendorCallbackPath(void* impl) {
    if (impl == nullptr) return;
    OpenSelfMem();
    uint64_t dh = 0, cb1010 = 0, b8 = 0, fobj = 0, fn140 = 0, fnCb = 0, fnCbVptr = 0;
    bool ok = SafeReadQ((char*)impl + 0x10, &dh);
    if (!ok || dh < 0x1000) { LOGW("dh unreadable"); return; }
    SafeReadQ((char*)(uintptr_t)dh + 0x10, &cb1010);
    SafeReadQ((char*)(uintptr_t)dh + 0xb8, &b8);
    SafeReadQ((char*)(uintptr_t)dh + 0x140, &fobj);
    if (fobj >= 0x1000) {
        SafeReadQ((void*)(uintptr_t)fobj, &fn140);                       // __func vptr
        SafeReadQ((char*)(uintptr_t)fobj + 0x10, &fnCb);                // captured cb sp
        if (fnCb >= 0x1000) SafeReadQ((void*)(uintptr_t)fnCb, &fnCbVptr);
    }
    LOGI("dh=%llx [+0x10]=%llx [+0xb8]=%llx [__func@+0x140]=%llx (vptr=%llx) cb=%llx cbvptr=%llx (memfd=%d)",
         (unsigned long long)dh, (unsigned long long)cb1010, (unsigned long long)b8,
         (unsigned long long)fobj, (unsigned long long)fn140,
         (unsigned long long)fnCb, (unsigned long long)fnCbVptr, gSelfMemFd);
}

bool gBnHwHookInstalled = false;

// Shadow vtables: [0..3] = Itanium prefix copy (vbase_offset, offset_to_top, RTTI
// at [1..3] relative to live vptr[-3..-1]); live vptr slot i lives at storage[4+i].
void* gHciShadowStorage[32] = {nullptr};   // HCI impl: prefix + slots 0..27
void* gBnHwShadowStorage[48] = {nullptr};  // BnHw:     prefix + slots 0..43

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// RefBase subobject pointer via the vtable prefix vbase_offset (vptr[-3]).
void* RefBaseOf(void* obj) {
    void** vptr = *(void***)obj;
    intptr_t vbase = ((const intptr_t*)vptr)[-3];
    return (char*)obj + vbase;
}

void IncStrong(void* obj, const void* id) {
    ((const ::android::RefBase*)RefBaseOf(obj))->incStrong(id);
}

void DecStrong(void* obj, const void* id) {
    ((const ::android::RefBase*)RefBaseOf(obj))->decStrong(id);
}

void WriteOkStatus(::android::hardware::Parcel* reply) {
    if (reply == nullptr) return;
    ::android::hardware::Status st = ::android::hardware::Status::ok();  // 32B zeroed via sret
    ::android::hardware::writeToParcel(st, reply);
}

// Invoke std::function<void(const hidl_vec<hidl_string>&)> — ABI: callable object
// (__func) pointer at cb+0x20, its operator() at vtable slot 6.
void InvokeChainCb(const void* cbObj, const HidlString* strs, uint32_t count) {
    if (cbObj == nullptr) return;
    void* fobj = *(void**)((char*)cbObj + 0x20);
    if (fobj == nullptr) return;
    HidlVec vec;
    vec.mBuffer = (void*)strs;
    vec.mSize = count;
    vec.mOwnsBuffer = false;
    vec.mPad[0] = vec.mPad[1] = vec.mPad[2] = 0;
    void** vptr = *(void***)fobj;
    auto invoke = (void (*)(void*, const HidlVec*))vptr[6];
    invoke(fobj, &vec);
}

// ---------------------------------------------------------------------------
// Hook 1: interfaceChain (HCI impl vptr[1]) — sret(40) / this / &cb
// ---------------------------------------------------------------------------
HidlReturnVoid40 HookedInterfaceChain(void* /*thiz*/, const void* cbObj) {
    static const char* kChain[3] = {
        "android.hardware.bluetooth@1.1::IBluetoothHci",
        "android.hardware.bluetooth@1.0::IBluetoothHci",
        "android.hidl.base@1.0::IBase",
    };
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("interfaceChain hooked: reporting 1.1 + 1.0 + IBase");
    }
    HidlString strs[3];
    for (int i = 0; i < 3; ++i) {
        strs[i].mBuffer = kChain[i];
        strs[i].mSize = (uint32_t)strlen(kChain[i]);
        strs[i].mOwnsBuffer = false;
        strs[i].mPad[0] = strs[i].mPad[1] = strs[i].mPad[2] = 0;
    }
    InvokeChainCb(cbObj, strs, 3);
    HidlReturnVoid40 r;
    memset(r.bytes, 0, sizeof(r.bytes));  // all-zero Return<void> == OK
    return r;
}

// ---------------------------------------------------------------------------
// Transaction code 6: initialize_1_1 — mirror of _hidl_initialize with the 1.1
// descriptor and the remote-callbacks path (BpHwBluetoothHciCallbacks).
// ---------------------------------------------------------------------------
int32_t HandleInitialize11(void* self,
                           const ::android::hardware::Parcel& data,
                           ::android::hardware::Parcel* reply) {
    typedef ::android::hardware::Parcel P;
    if (!data.enforceInterface(kHci11Descriptor)) {
        LOGE("initialize_1_1: enforceInterface(1.1) failed");
        return (int32_t)0x80000001u;  // stock error code
    }
    ::android::sp<::android::hardware::IBinder> binderSp = {};
    int32_t err = data.readNullableStrongBinder(&binderSp);
    if (err != 0) {
        LOGE("initialize_1_1: readNullableStrongBinder err=%d", err);
        return err;
    }
    void* binder = binderSp.m_ptr;

    void* callbacks = nullptr;  // sp<V1_0::IBluetoothHciCallbacks>
    if (binder != nullptr) {
        // localBinder() is vtable slot 7; non-null means a same-process client.
        void** bVptr = *(void***)binder;
        auto localBinder = (void* (*)(void*))bVptr[7];
        void* local = localBinder(binder);
        if (local != nullptr) {
            LOGE("initialize_1_1: same-process callbacks unsupported");
            return (int32_t)0x80000001u;
        }
        // Remote path: callbacks = new BpHwBluetoothHciCallbacks(binderSp)
        if (gBpCallbacksCtor == nullptr) {
            LOGE("initialize_1_1: Bp ctor unresolved");
            return (int32_t)0x80000001u;
        }
        void* cbObj = malloc(216);  // sizeof(BpHwBluetoothHciCallbacks)
        gBpCallbacksCtor(cbObj, &binderSp);
        // Diagnostic (v3.2e): dump the full 216-byte wrapper object to enumerate
        // every embedded vptr (primary @ +0, possible secondary @ +0x20/RefBase).
        // Decoded on host against the 1.0 interface lib's vtables.
        {
            void** p = (void**)cbObj;
            char buf[600];
            int off = 0;
            for (int i = 0; i < 27; i++) {  // 27 * 8 = 216 bytes
                off += snprintf(buf + off, sizeof(buf) - off, "%llx ", (unsigned long long)p[i]);
                if (off >= (int)sizeof(buf) - 24) break;
            }
            LOGI("wrapper dump [0..26] (216B): %s", buf);
        }
        // Shadow the wrapper's primary vtable and hook hciEventReceived for
        // ISO NCP credit multiplication. The vendor may dispatch through the
        // raw function slot (found by dlsym match) or the adjacent this-adjusting
        // thunk slot (offset 0x78), so we hook BOTH.
        if (gBpCallbacksCtor != nullptr && !gBpHookInstalled && gHciEventReceivedSym != nullptr) {
            void** liveVptr = *(void***)cbObj;
            int slot = -1;
            for (int i = 0; i < 60; i++) {
                if (liveVptr[i] == gHciEventReceivedSym) { slot = i; break; }
            }
            if (slot >= 0) {
                memcpy(gBpShadowStorage, liveVptr - 4, sizeof(gBpShadowStorage));
                gOrigHciEventReceived = (HciEventReceivedFn)liveVptr[slot];
                gOrigHciEventReceivedAlt = (HciEventReceivedFn)liveVptr[slot + 1];
                gBpShadowStorage[4 + slot] = (void*)&HookedHciEventReceived;
                if (slot + 1 < 60) {
                    gBpShadowStorage[4 + slot + 1] = (void*)&HookedHciEventReceivedAlt;
                }
                gBpHookInstalled = true;
                LOGI("hciEventReceived hooks installed (slots %d+%d, sym=%p ctor=%p)",
                     slot, slot + 1, gHciEventReceivedSym, (void*)gBpCallbacksCtor);
                LOGI("live vptr=%p [12..18]: %p %p %p %p %p %p %p", (void*)liveVptr,
                     liveVptr[12], liveVptr[13], liveVptr[14], liveVptr[15],
                     liveVptr[16], liveVptr[17], liveVptr[18]);
            } else {
                LOGE("hciEventReceived symbol not found in wrapper vtable");
            }
        }
        IncStrong(cbObj, &callbacks);
        callbacks = cbObj;
        // v3.3: hook the shared mRemote BpHwBinder (slot 0 = transact) — the
        // CFI-safe interception point for ALL outbound hciEventReceived calls,
        // regardless of which wrapper the vendor dispatches through.
        void* mRemote = *(void**)((char*)cbObj + 0x18);
        InstallBinderTransactHook(mRemote);
        LOGI("initialize_1_1: callbacks wrapped as BpHwBluetoothHciCallbacks %p (mRemote=%p)",
             cbObj, mRemote);
    }
    // destroy the temporary sp<IBinder>
    if (binder != nullptr) {
        DecStrong(binder, &binderSp);
        binderSp.m_ptr = nullptr;
    }

    // mImpl (BnHwBase::__hidl__mImpl at +0x78), take a strong ref like the stock does
    void* impl = *(void**)((char*)self + 0x78);
    if (impl != nullptr) IncStrong(impl, &impl);

    // impl->initialize(callbacks): vptr[13]
    // ABI (verified from stock prologue at 0x3a90c):
    //   x8 = sret pointer (40-byte Return<void>), x0 = this, x1 = const sp& callbacks
    // A 40-byte struct return in C++ is classified MEMORY -> clang passes x8 sret
    // automatically (same pattern as HookedInterfaceChain, which works).
    int32_t ret = 0;
    if (impl != nullptr) {
        typedef HidlReturnVoid40 (*InitializeFn)(void* thiz, void* spCallbacks);
        void** vptr = *(void***)impl;
        InitializeFn initialize = (InitializeFn)vptr[13];
        HidlReturnVoid40 rv = initialize(impl, &callbacks);
        // Diagnostic: dump the impl object's pointer-sized fields to see where
        // the callbacks pointer landed (which sp member holds it).
        {
            void** p = (void**)impl;
            char buf[512];
            int off = 0;
            for (int i = 0; i < 32; i++) {
                off += snprintf(buf + off, sizeof(buf) - off, "%llx ", (unsigned long long)p[i]);
                if (off >= (int)sizeof(buf) - 24) break;
            }
            LOGI("impl dump [0..31]: %s", buf);
        }
        // Return<void> layout: bytes[0..3] = return_status enum (0 == OK).
        // (Stock writes only 33 bytes; the tail 7 bytes are padding — do not check them.)
        uint32_t status = 0;
        memcpy(&status, rv.bytes, sizeof(status));
        if (status != 0) {
            LOGE("initialize_1_1: stock initialize returned error %u", status);
            ret = (int32_t)0x80000001u;
        }
        LOGI("initialize_1_1: stock initialize() called on impl %p (status=%u)", impl, status);
        DumpVendorCallbackPath(impl);
    } else {
        LOGE("initialize_1_1: mImpl is null");
        ret = (int32_t)0x80000001u;
    }

    if (impl != nullptr) DecStrong(impl, &impl);

    if (ret == 0) WriteOkStatus(reply);

    // destroy our callbacks sp
    if (callbacks != nullptr) {
        DecStrong(callbacks, &callbacks);
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Transaction code 7: sendIsoData — mirror of _hidl_sendAclData with the 1.1
// descriptor; data forwarded to stock sendDataToController(HciPacketType::ISO).
// ---------------------------------------------------------------------------
static bool IsoStrip4Enabled() {
    static int sV = -1;
    if (sV < 0) {
        char v[PROP_VALUE_MAX] = {0};
        sV = (__system_property_get("persist.vendor.leaudio.iso.strip4", v) > 0 &&
              v[0] == '0') ? 0 : 1;
    }
    return sV == 1;
}

int32_t HandleSendIsoData(const ::android::hardware::Parcel& data,
                          ::android::hardware::Parcel* reply) {
    typedef ::android::hardware::Parcel P;
    if (!data.enforceInterface(kHci11Descriptor)) {
        LOGE("sendIsoData: enforceInterface(1.1) failed");
        return (int32_t)0x80000001u;
    }
    const HidlVec* vec = nullptr;
    size_t parent = 0;
    int32_t err = data.readBuffer(sizeof(HidlVec), &parent, (const void**)&vec);
    if (err != 0 || vec == nullptr) {
        LOGE("sendIsoData: readBuffer err=%d", err);
        return err != 0 ? err : (int32_t)0x80000001u;
    }
    const void* buf = nullptr;
    size_t handle = 0;
    err = data.readNullableEmbeddedBuffer(vec->mSize, &handle, parent, 0, &buf);
    if (err != 0) {
        LOGE("sendIsoData: readNullableEmbeddedBuffer err=%d", err);
        return err;
    }
    HidlVec isoVec;
    isoVec.mBuffer = (void*)buf;
    isoVec.mSize = vec->mSize;
    isoVec.mOwnsBuffer = false;
    isoVec.mPad[0] = isoVec.mPad[1] = isoVec.mPad[2] = 0;
    // Track the ISO connection handle (bits 11-0 of the first halfword) so the
    // NCP multiplication hook only boosts credits for ISO handles.
    if (buf != nullptr && vec->mSize >= 2) {
        RecordIsoHandle((uint16_t)(((const uint8_t*)buf)[0] |
                                   (((const uint8_t*)buf)[1] << 8)) & 0x0FFF);
    }
    if (gSendDataToController != nullptr && gStockHci != nullptr) {
        // v3.11: strip the 4-byte framed-SDU header (root cause of the crash).
        //
        // btsnoop of a streaming attempt (CIG: Framing=0 i.e. UNFRAMED,
        // Max_SDU_C2P=0x009b=155) shows every SDU is emitted as two HCI ISO
        // packets:
        //   len=160 PB=0 hcilen=155  05 05 00 9b 00 | 00 00 | 9a 00 | frame...
        //   len=  8 PB=3 hcilen=3    05 05 30 03 00 | 34 ...
        // The first fragment starts with the AOSP framed-SDU header
        // [seq:2][len:2] where len=0x009a=154, so the SDU is 4+154 = 158 bytes
        // while the CIS was negotiated UNFRAMED with Max_SDU=155 (the LC3 frame
        // size). The controller asserts on that: HW error 0x0f -> SSR ~4s later
        // -> HAL dies -> stack aborts -> "no sound + Bluetooth restarts".
        // Removing the 4-byte header leaves 151+3 = 154 bytes = exactly the LC3
        // frame, i.e. <= Max_SDU, and consistent with the unframed CIS.
        uint8_t stripBuf[1024];
        const uint8_t* p = (const uint8_t*)buf;
        if (IsoStrip4Enabled() && p != nullptr && vec->mSize >= 8) {
            uint32_t hf = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            uint32_t pb = (hf >> 12) & 0x3;   // 0=first frag, 3=complete SDU
            uint32_t l = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
            if ((pb == 0 || pb == 3) && l >= 8 && (uint64_t)l + 4 == vec->mSize) {
                uint32_t newlen = l - 4;
                memcpy(stripBuf, p, 4);
                stripBuf[2] = (uint8_t)(newlen & 0xFF);
                stripBuf[3] = (uint8_t)((newlen >> 8) & 0xFF);
                memcpy(stripBuf + 4, p + 8, newlen);
                isoVec.mBuffer = stripBuf;
                isoVec.mSize = 4u + newlen;
                static uint32_t sStripLog = 0;
                if ((++sStripLog & 0x3f) == 1)
                    LOGI("ISO strip: pb=%u hcilen %u -> %u (sdu hdr seq=%u len=%u)", pb, l,
                         newlen, (unsigned)(p[4] | (p[5] << 8)), (unsigned)(p[6] | (p[7] << 8)));
            }
        }
        gSendDataToController(gStockHci, 5 /* HciPacketType::ISO_DATA */, &isoVec);
    } else {
        LOGE("sendIsoData: controller/impl unresolved, dropping %u bytes", vec->mSize);
        return (int32_t)0x80000001u;
    }
    static uint32_t sIsoPackets = 0;
    if ((++sIsoPackets & 0x3f) == 1) {  // log roughly every 64th packet
        LOGI("sendIsoData: forwarded %u bytes (orig %u) (pkt #%u) h0=0x%02x h1=0x%02x",
             (unsigned)isoVec.mSize, (unsigned)vec->mSize, sIsoPackets,
             vec->mSize >= 1 ? ((const uint8_t*)buf)[0] : 0,
             vec->mSize >= 2 ? ((const uint8_t*)buf)[1] : 0);
    }
    // sendIsoData is *not* oneway: the reply must carry Status::ok()
    WriteOkStatus(reply);
    return 0;
}

// ---------------------------------------------------------------------------
// v3.4 diag: HCI command capture (read-only, parcel position restored).
//
// Purpose: learn the *real* CIS parameters the stack programs into the
// controller (ISO_Interval / framing / RTN / max transport latency / PHY / BN-FT)
// and confirm the host->controller packet rate (one HCI ISO packet per SDU).
// This is what decides whether "fewer, bigger packets" (aggregation) is even
// allowed by the negotiated CIS, and how much room the ISO interval has.
// ---------------------------------------------------------------------------
void DumpCigParamsCommand(const uint8_t* p, size_t plen) {
    if (plen < 17) return;
    uint8_t cig = p[0];
    uint32_t sdu_c2p = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16);
    uint32_t sdu_p2c = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16);
    uint8_t sca = p[7], packing = p[8], framing = p[9];
    uint16_t lat_c2p = (uint16_t)(p[10] | (p[11] << 8));
    uint16_t lat_p2c = (uint16_t)(p[12] | (p[13] << 8));
    uint8_t rtn_c2p = p[14], rtn_p2c = p[15], num = p[16];
    LOGI("LE_SET_CIG_PARAMS: cig=%u sdu_c2p=%uus sdu_p2c=%uus sca=%u packing=%u framing=%u",
         cig, sdu_c2p, sdu_p2c, sca, packing, framing);
    LOGI("  lat_c2p=%u lat_p2c=%u rtn_c2p=%u rtn_p2c=%u num_cis=%u", lat_c2p, lat_p2c,
         rtn_c2p, rtn_p2c, num);
    size_t o = 17;
    for (uint8_t i = 0; i < num && o + 11 <= plen; i++) {
        uint8_t cis_id = p[o];
        uint16_t ms_c2p = (uint16_t)(p[o + 1] | (p[o + 2] << 8));
        uint16_t ms_p2c = (uint16_t)(p[o + 3] | (p[o + 4] << 8));
        LOGI("  CIS[%u] id=%u maxsdu_c2p=%u maxsdu_p2c=%u phy_c2p=0x%02x phy_p2c=0x%02x "
             "bn=%u/%u ft=%u/%u",
             i, cis_id, ms_c2p, ms_p2c, p[o + 5], p[o + 6], p[o + 7], p[o + 8], p[o + 9],
             p[o + 10]);
        o += 11;
    }
    if (o + 2 <= plen) {
        uint16_t iso_iv = (uint16_t)(p[o] | (p[o + 1] << 8));
        LOGI("  ** ISO_Interval=%u (x1.25ms = %.2f ms) **", iso_iv, iso_iv * 1.25);
    }
}

void DumpHciCommandIfInteresting(const ::android::hardware::Parcel& data, uint32_t code) {
    const size_t savedPos = data.dataPosition();
    const HidlVec* vec = nullptr;
    size_t parent = 0;
    int32_t err = data.readBuffer(sizeof(HidlVec), &parent, (const void**)&vec);
    if (err == 0 && vec != nullptr && vec->mSize >= 3 && vec->mSize <= 4096) {
        const void* buf = nullptr;
        size_t handle = 0;
        err = data.readNullableEmbeddedBuffer(vec->mSize, &handle, parent, 0, &buf);
        if (err == 0 && buf != nullptr) {
            const uint8_t* c = (const uint8_t*)buf;
            size_t n = vec->mSize;
            uint16_t opcode = (uint16_t)(c[0] | (c[1] << 8));
            uint8_t plen = c[2];
            // v3.4diag: log each newly-seen opcode once (capped) so we learn
            // which transaction code carries HCI commands, without flooding.
            static uint32_t sSeen[65536 / 32] = {0};
            static uint32_t sSeenLogged = 0;
            bool first = (sSeen[opcode >> 5] & (1u << (opcode & 31))) == 0;
            if (first) {
                sSeen[opcode >> 5] |= (1u << (opcode & 31));
                if (sSeenLogged < 40) {
                    sSeenLogged++;
                    char h[128]; int o2 = 0;
                    for (size_t q = 0; q < n && q < 16; q++)
                        o2 += snprintf(h + o2, sizeof(h) - o2, "%02x ", c[q]);
                    LOGI("hci opcode new: 0x%04x (txcode=%u len=%u): %s", opcode, code,
                         (unsigned)n, h);
                }
            }
            if (opcode == 0x2062) {  // LE Set CIG Parameters
                DumpCigParamsCommand(c + 3, (size_t)plen <= n - 3 ? plen : n - 3);
            } else if (opcode == 0x2064 || opcode == 0x2065 || opcode == 0x2066 ||
                       opcode == 0x2061 || opcode == 0x2063) {
                char h[160]; int o2 = 0;
                for (size_t q = 0; q < n && q < 32; q++)
                    o2 += snprintf(h + o2, sizeof(h) - o2, "%02x ", c[q]);
                LOGI("LE_CIS_CMD opcode=0x%04x len=%u: %s", opcode, (unsigned)n, h);
            }
        }
    }
    data.setDataPosition(savedPos);
}

// ---------------------------------------------------------------------------
// Hook 2: BnHwBluetoothHci::onTransact (BnHw primary vptr[11])
// ---------------------------------------------------------------------------
int32_t HookedOnTransact(void* self, uint32_t code,
                         const ::android::hardware::Parcel* data,
                         ::android::hardware::Parcel* reply,
                         uint32_t flags, const void* hidlCb) {
    static uint32_t sTxCount = 0;
    static bool sInit11Seen = false;
    if (code == 6) sInit11Seen = true;
    if (code == 1 && sInit11Seen) {
        // Hypothesis d: a second (1.0) initialize after our 1.1 path would make
        // the impl swap mCb to a stock un-hooked Bp wrapper -> event hook dead.
        LOGW("SECOND INITIALIZE (code 1) after 1.1 init — hypothesis d CONFIRMED");
    }
    if (code != 7) {  // log everything except the hot ISO path
        if (++sTxCount <= 24 || (sTxCount & 0xff) == 0) {
            LOGI("onTransact code=%u (#%u)", code, sTxCount);
        }
    }
    if (code == 7) {
        return HandleSendIsoData(*data, reply);
    }
    if (code == 6) {
        LOGI("onTransact: initialize_1_1 (code 6)");
        return HandleInitialize11(self, *data, reply);
    }
    return gOrigOnTransact(self, code, data, reply, flags, hidlCb);
}

// ---------------------------------------------------------------------------
// Hook 3: BnConstructorMap — build the real 1.0 BnHw binder, then shadow its
// primary vtable (slot 11 = onTransact). Registered for the 1.0 descriptor
// (getDescriptor() of the passthrough impl is hardcoded to 1.0) and 1.1.
// ---------------------------------------------------------------------------
::android::sp<::android::hardware::IBinder> MakeHookedBnHw(
    std::function<::android::sp<::android::hardware::IBinder>(void*)> origFunc,
    void* impl) {
    auto binder = origFunc(impl);
    if (binder.m_ptr == nullptr) return binder;
    if (!gBnHwHookInstalled) {
        void** liveVptr = *(void***)binder.m_ptr;
        gOrigOnTransact = (OnTransactFn)liveVptr[11];
        // copy Itanium prefix + slots: liveVptr[-4 .. +43] -> storage[0..47]
        memcpy(gBnHwShadowStorage, liveVptr - 4, sizeof(gBnHwShadowStorage));
        gBnHwShadowStorage[4 + 11] = (void*)&HookedOnTransact;
        gBnHwHookInstalled = true;
        LOGI("BnHw onTransact hook installed: binder=%p orig=%p hook=%p",
             binder.m_ptr, (void*)gOrigOnTransact, (void*)&HookedOnTransact);
    }
    *(void***)binder.m_ptr = &gBnHwShadowStorage[4];
    return binder;
}

// ---------------------------------------------------------------------------
// Library init
// ---------------------------------------------------------------------------
// BpHwBluetoothHciCallbacks ctor and hciEventReceived live in the 1.0 interface library.

void ResolveSymbols() {
    // BpHwBluetoothHciCallbacks ctor lives in the 1.0 interface library.
    void* ifaceLib = dlopen("/vendor/lib64/android.hardware.bluetooth@1.0.so", RTLD_NOW);
    if (ifaceLib != nullptr) {
        gBpCallbacksCtor = (BpCallbacksCtorFn)dlsym(
            ifaceLib,
            "_ZN7android8hardware9bluetooth4V1_025BpHwBluetoothHciCallbacksC1ERKNS_2spINS0_7IBinderEEE");
        gHciEventReceivedSym = dlsym(
            ifaceLib,
            "_ZN7android8hardware9bluetooth4V1_025BpHwBluetoothHciCallbacks16hciEventReceivedERKNS0_8hidl_vecIhEE");
    }
    if (gBpCallbacksCtor == nullptr) {
        LOGW("BpHwBluetoothHciCallbacks ctor not resolved: %s", dlerror());
    }
}

void RegisterBnConstructorHooks() {
    auto& map = ::android::hardware::details::getBnConstructorMap();
    auto orig10 = map.get(kHci10Descriptor, nullptr);
    if (!orig10) {
        LOGE("1.0 constructor not found in BnConstructorMap");
        return;
    }
    auto hook = [orig10](void* impl) -> ::android::sp<::android::hardware::IBinder> {
        return MakeHookedBnHw(orig10, impl);
    };
    map.set(kHci10Descriptor, hook);
    map.set(kHci11Descriptor, hook);
    LOGI("BnConstructorMap hooks registered (1.0 + 1.1)");
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point invoked by ServiceManagement passthrough loader
// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
void* HIDL_FETCH_IBluetoothHci(const char* name) {
    LOGI("HIDL_FETCH_IBluetoothHci(%s)", name ? name : "(null)");

    // dlopen by SONAME first: dedups with the DT_NEEDED instance (single DataHandler)
    void* realLib = dlopen("libbluetooth_qti_real.so", RTLD_NOW | RTLD_NOLOAD);
    if (realLib == nullptr) {
        realLib = dlopen("libbluetooth_qti_real.so", RTLD_NOW);
    }
    if (realLib == nullptr) {
        realLib = dlopen("/vendor/lib64/hw/libbluetooth_qti_real.so", RTLD_NOW);
    }
    if (realLib == nullptr) {
        LOGE("dlopen real impl failed: %s", dlerror());
        return nullptr;
    }
    typedef void* (*FetchFn)(const char*);
    FetchFn realFetch = (FetchFn)dlsym(realLib, "HIDL_FETCH_IBluetoothHci");
    if (realFetch == nullptr) {
        LOGE("dlsym real HIDL_FETCH failed: %s", dlerror());
        return nullptr;
    }
    gStockHci = realFetch(name);
    gSendDataToController = (SendDataToControllerFn)dlsym(
        realLib,
        "_ZN7android8hardware9bluetooth4V1_014implementation12BluetoothHci20sendDataToControllerE"
        "13HciPacketTypeRKNS0_8hidl_vecIhEE");
    LOGI("stock HAL loaded: impl=%p sendDataToController=%p",
         gStockHci, (void*)gSendDataToController);
    if (RingIsoPatchEnabled()) PatchRingBufferIsoType();

    if (gStockHci != nullptr) {
        void** liveVptr = *(void***)gStockHci;
        if (liveVptr != &gHciShadowStorage[4]) {
            // copy Itanium prefix + slots: liveVptr[-4 .. +27] -> storage[0..31]
            memcpy(gHciShadowStorage, liveVptr - 4, sizeof(gHciShadowStorage));
            gHciShadowStorage[4 + 1] = (void*)&HookedInterfaceChain;
            *(void***)gStockHci = &gHciShadowStorage[4];
            LOGI("interfaceChain hook installed on impl %p (orig vtable %p)", gStockHci, (void*)liveVptr);
        }
    }
    return gStockHci;
}

// ---------------------------------------------------------------------------
// Constructor: runs at dlopen() inside the HAL service process
// ---------------------------------------------------------------------------
__attribute__((constructor)) static void ShimInit() {
    LOGI("BluetoothHciHook v3.11 init (ring ISO patch + PB-based framed-SDU header strip + live credit window)");
    ResolveSymbols();
    RegisterBnConstructorHooks();
}
