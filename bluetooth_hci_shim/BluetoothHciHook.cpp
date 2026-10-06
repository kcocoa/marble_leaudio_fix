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
#include <pthread.h>
#include <time.h>
#include <string>
#include <functional>
#include <android/log.h>
#include <stdarg.h>
#include <hidl/ConcurrentMap.h>

// bionic property API (resolved from libc at runtime)
extern "C" int __system_property_get(const char* name, char* value);
#define PROP_VALUE_MAX 92

#define LOG_TAG "BluetoothHciHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Truncation-safe formatted append.
//
// snprintf() returns the length it *would* have written (excluding the NUL),
// NOT the number of bytes actually stored. Accumulating that return value
// drives the offset past the end of the buffer, at which point
// `sizeof(buf) - off` wraps to a huge size_t and bionic's FORTIFY turns it
// into __fortify_fatal() -> abort(). That exact bug aborted the HAL on the
// very first forwarded ISO RX packet: char h[96] + 8 rounds of
// a 17-char format = 136 > 96, and round 7 got size == (size_t)-6.
// See git log "fix(shim): hexdump 缓冲区溢出" for the full tombstone.
//
// Always clamp the offset and stop; never trust the return value.
// ---------------------------------------------------------------------------
__attribute__((format(printf, 4, 5)))
static inline void hexCat(char* buf, size_t cap, int* off, const char* fmt, ...) {
    if (buf == nullptr || off == nullptr) return;
    size_t o = (size_t)*off;
    if (o + 1 >= cap) {           // no room left for even a NUL: stop
        *off = (int)cap - 1;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + o, cap - o, fmt, ap);
    va_end(ap);
    if (n < 0) return;            // encoding error: leave buffer as-is
    if ((size_t)n >= cap - o) {   // output was truncated: clamp and stop
        *off = (int)cap - 1;
        return;
    }
    *off += n;
}

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
// hciEventReceived marshals the event into the parcel and transacts with code 2;
// those parcels are only observed here (event / Command Complete histograms) and
// forwarded unchanged. The other reason for this hook is the ISO RX rewrite
// above, which is what actually has to happen.
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
static uint32_t sIsoRxForwarded = 0;  // ISO packets rewritten sco->iso (v3.13)
static uint32_t sIsoRxNoToken = 0;    // parcels whose token could not be patched

// ---------------------------------------------------------------------------
// v3.13 ISO RX forwarding.
//
// The vendor dispatch (patched trampoline in libbluetooth_qti_real.so at
// 0x3bdec) routes controller ISO packets (HCI type 5) through the callbacks
// wrapper's vtable slot 0x80 = scoDataReceived (a slot the vendor dispatch
// never invokes itself), so the packet is marshalled as an ordinary
// @1.0::IBluetoothHciCallbacks transaction with code 4.
//
// We rewrite that outgoing parcel here: the interface token becomes
// "android.hardware.bluetooth@1.1::IBluetoothHciCallbacks" (same length, one
// byte differs) and the transaction code becomes 5 (isoDataReceived), so the
// Bluetooth stack finally receives the ISO RX data it needs to keep the CIS
// alive. Without it the earbuds time out, the ACL drops, and LE Audio teardown
// hits the "No such iso connection: 0xffff" assert in btm_iso_impl.h.
// ---------------------------------------------------------------------------
static const char kCb10Token[] = "android.hardware.bluetooth@1.0::IBluetoothHciCallbacks";

static bool PatchCbTokenTo11(::android::hardware::Parcel* data) {
    if (data == nullptr) return false;
    uint8_t* buf = const_cast<uint8_t*>(data->data());  // parcel body is writable
    size_t sz = data->dataSize();
    if (buf == nullptr || sz < 64) return false;
    const size_t n = sizeof(kCb10Token) - 1;
    // The interface token is the first object in the parcel; scan a generous
    // window in case a length prefix precedes the string.
    size_t limit = sz < n + 128 ? sz - n : n + 128;
    for (size_t i = 0; i < limit; i++) {
        if (memcmp(buf + i, kCb10Token, n) != 0) continue;
        if (buf[i + n] != '\0') continue;  // NUL-terminated in the parcel
        const size_t verDigit = i + 29;   // "...bluetooth@1.0::..."
        if (buf[verDigit] != '0') continue;
        buf[verDigit] = '1';
        return true;
    }
    return false;
}

int32_t HookedBinderTransact(void* thiz, uint32_t code,
                             const ::android::hardware::Parcel* data,
                             ::android::hardware::Parcel* reply,
                             uint32_t flags, void* onTransactDone) {
    // v3.13: ISO RX — see PatchCbTokenTo11 above.
    if (code == 4 && data != nullptr && gOrigBinderTransact != nullptr) {
        ::android::hardware::Parcel* w =
            const_cast<::android::hardware::Parcel*>(data);
        if (PatchCbTokenTo11(w)) {
            int32_t r = gOrigBinderTransact(thiz, 5, data, reply, flags,
                                            onTransactDone);
            if ((++sIsoRxForwarded & 0x7f) == 1) {
                const uint8_t* db = data->data();
                size_t sz = data->dataSize();
                char h[160] = {};
                int o2 = 0;
                for (size_t q = 0; q + 16 <= sz && q < 64; q += 8)
                    hexCat(h, sizeof(h), &o2, "%02x%02x%02x%02x%02x%02x%02x%02x ",
                           db[q], db[q+1], db[q+2], db[q+3], db[q+4], db[q+5], db[q+6], db[q+7]);
                LOGI("ISO RX forwarded as isoDataReceived (#%u, %zu bytes): %s",
                     sIsoRxForwarded, sz, h);
            }
            return r;
        }
        if (++sIsoRxNoToken == 1)
            LOGE("ISO RX: code-4 parcel without @1.0::IBluetoothHciCallbacks token (%zu bytes)",
                 data->dataSize());
    }
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
        // objects to histogram the event codes and Command Complete opcodes.
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
                // v4.0: >= 3, not >= 8 — a 1-handle NCP is exactly 7 bytes.
                if (ptr < 0x1000 || bsz < 3 || bsz > 2048) continue;
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
                        char h[128] = {}; int o2 = 0;
                        for (size_t q = 0; q < bsz && q < 24; q++)
                            hexCat(h, sizeof(h), &o2, "%02x ", eb[q]);
                        LOGI("CC opc=0x%04x dump[%zu]: %s", opc, bsz, h);
                    }
                }
                // v3.3g: dump 0x14 events (what are they?)
                {
                    static uint32_t s14Dumped = 0;
                    if (bsz >= 2 && eb[0] == 0x14 && s14Dumped < 4) {
                        s14Dumped++;
                        char h[64] = {}; int o2 = 0;
                        for (size_t q = 0; q < bsz && q < 12; q++)
                            hexCat(h, sizeof(h), &o2, "%02x ", eb[q]);
                        LOGI("evt 0x14 dump[%zu]: %s", bsz, h);
                    }
                }
                anyFlag1 = true;
            }
            // periodic histogram: which event codes flow through hciEventReceived?
            if ((sEvtParcels & 0x3ff) == 0 && anyFlag1) {
                char h[720] = {}; int off = 0;
                for (int e = 0; e < 256; e++) {
                    if (sEvtCodeHist[e])
                        hexCat(h, sizeof(h), &off, "%02x:%u ", e, sEvtCodeHist[e]);
                }
                LOGI("evt code hist after %u parcels: %s", sEvtParcels, h);
                char h2[640] = {}; off = 0;
                for (int o = 0; o < 65536; o++) {
                    if (sCcOpcodeHist[o])
                        hexCat(h2, sizeof(h2), &off, "%04x:%u ", o, sCcOpcodeHist[o]);
                }
                LOGI("CC opcode hist: %s", h2);
            }
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
            char buf[512] = {};
            int off = 0;
            for (int i = 0; i < 32; i++)
                hexCat(buf, sizeof(buf), &off, "%llx ", (unsigned long long)p[i]);
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
int32_t HandleSendIsoData(const ::android::hardware::Parcel& data,
                          ::android::hardware::Parcel* reply) {
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
    if (gSendDataToController != nullptr && gStockHci != nullptr) {
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
// v4.1: cap Max_Transport_Latency in LE Set CIG Parameters (0x2062).
//
// For MEDIA the stack asks for up to 100 ms. Measured on 00680 (btsnoop, the
// LE CIS Established event): without the cap the controller picks
// ISO_Interval=40ms / transport latency 84570us, the in-flight queue saturates
// at the controller's 22 buffers, and the first 6-8 packets of a stream are
// dropped (btm_iso_impl.h "dropping ISO packet, iso credits: 0") because the
// credit pool is 0 until the first NCP arrives one ISO interval later.
// Capping it to 10 ms makes the controller choose 10 ms / 7210us, which runs
// with 8/22 in flight and zero drops.
// Incoming binder buffers are read-only, so the command is copied, patched and
// sent via the stock sendDataToController(COMMAND), exactly what the stock
// sendHciCommand does. Prop persist.vendor.leaudio.cig.maxlat (ms, default 10,
// 0 = off). Returns true if the transaction was fully handled here.
// ---------------------------------------------------------------------------
// Re-read on every LE Set CIG Parameters (rare), so it can be tuned with
// setprop + stream restart, no HAL restart.
static int PropInt(const char* name, int def, int lo, int hi) {
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, v) > 0) {
        int t = atoi(v);
        if (t >= lo && t <= hi) return t;
    }
    return def;
}
static int CigMaxLatencyCap() { return PropInt("persist.vendor.leaudio.cig.maxlat", 10, 0, 4000); }

bool MaybeRewriteCigParams(const ::android::hardware::Parcel& data,
                           ::android::hardware::Parcel* reply) {
    const int cap = CigMaxLatencyCap();
    if (cap <= 0 || gSendDataToController == nullptr || gStockHci == nullptr) return false;
    const size_t savedPos = data.dataPosition();
    if (!data.enforceInterface(kHci10Descriptor)) {
        data.setDataPosition(savedPos);
        return false;
    }
    const HidlVec* vec = nullptr;
    size_t parent = 0;
    const void* buf = nullptr;
    size_t handle = 0;
    if (data.readBuffer(sizeof(HidlVec), &parent, (const void**)&vec) != 0 || vec == nullptr ||
        vec->mSize < 3 + 17 || vec->mSize > 255 + 3 ||
        data.readNullableEmbeddedBuffer(vec->mSize, &handle, parent, 0, &buf) != 0 ||
        buf == nullptr) {
        data.setDataPosition(savedPos);
        return false;
    }
    const uint8_t* c = (const uint8_t*)buf;
    if (c[0] != 0x62 || c[1] != 0x20) {  // not LE Set CIG Parameters
        data.setDataPosition(savedPos);
        return false;
    }
    uint8_t cmd[258];
    memcpy(cmd, c, vec->mSize);
    uint8_t* p = cmd + 3;  // params
    uint16_t lc2p = (uint16_t)(p[10] | (p[11] << 8));
    uint16_t lp2c = (uint16_t)(p[12] | (p[13] << 8));
    uint16_t nc2p = lc2p > cap ? (uint16_t)cap : lc2p;
    uint16_t np2c = lp2c > cap ? (uint16_t)cap : lp2c;
    p[10] = (uint8_t)(nc2p & 0xFF); p[11] = (uint8_t)(nc2p >> 8);
    p[12] = (uint8_t)(np2c & 0xFF); p[13] = (uint8_t)(np2c >> 8);
    // params[14] = CIS_Count, then 9 bytes per CIS:
    // [id][sdu_c2p:2][sdu_p2c:2][phy_c2p][phy_p2c][rtn_c2p][rtn_p2c]
    const uint32_t plen = cmd[2];
    const uint32_t ncis = p[14];
    uint8_t rc2p = 0, rp2c = 0;
    if (15 + 9 * ncis <= plen && 3 + plen <= vec->mSize) {
        for (uint32_t i = 0; i < ncis; i++) {
            uint8_t* e = p + 15 + 9 * i;
            rc2p = e[7]; rp2c = e[8];
        }
    }
    LOGI("CIG params: max transport latency c2p %u->%u ms, p2c %u->%u ms, rtn %u/%u, "
         "%u CIS", lc2p, nc2p, lp2c, np2c, rc2p, rp2c, ncis);
    HidlVec v;
    v.mBuffer = cmd; v.mSize = vec->mSize; v.mOwnsBuffer = false;
    v.mPad[0] = v.mPad[1] = v.mPad[2] = 0;
    gSendDataToController(gStockHci, 1 /* HciPacketType::COMMAND */, &v);
    WriteOkStatus(reply);
    return true;
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
    if (code == 2 && data != nullptr && MaybeRewriteCigParams(*data, reply)) {
        return 0;  // 2 = sendHciCommand
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
// BpHwBluetoothHciCallbacks ctor lives in the 1.0 interface library.

void ResolveSymbols() {
    void* ifaceLib = dlopen("/vendor/lib64/android.hardware.bluetooth@1.0.so", RTLD_NOW);
    if (ifaceLib != nullptr) {
        gBpCallbacksCtor = (BpCallbacksCtorFn)dlsym(
            ifaceLib,
            "_ZN7android8hardware9bluetooth4V1_025BpHwBluetoothHciCallbacksC1ERKNS_2spINS0_7IBinderEEE");
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
    LOGI("BluetoothHciHook v4.5 init (ring ISO patch=%d + ISO RX forwarding, "
         "CIG max latency cap=%d ms)", RingIsoPatchEnabled() ? 1 : 0, CigMaxLatencyCap());
    ResolveSymbols();
    RegisterBnConstructorHooks();
}
