#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <dlfcn.h>
#include <functional>
#include <android/log.h>
#include <utils/RefBase.h>

#define LOG_TAG "BluetoothHciShim"

namespace android {
    namespace hardware {
        struct hidl_string {
            const char* mBuffer;
            uint32_t mSize;
            bool mOwnsBuffer;
        };

        template<typename T>
        struct hidl_vec {
            T* mBuffer;
            uint32_t mSize;
            bool mOwnsBuffer;
            uint8_t mPad[3];

            size_t size() const { return mSize; }
            const T* data() const { return mBuffer; }
            T* data() { return mBuffer; }
        };

        struct hidl_handle {
            void* mHandle;
            bool mOwnsHandle;
        };

        struct hidl_death_recipient {};

        template<typename T>
        struct Return {
            int32_t mStatus;
            T mVal;
            bool isOk() const { return true; }
        };

        template<>
        struct Return<void> {
            int32_t mStatus;
            bool isOk() const { return true; }
        };

        static inline Return<void> Void() {
            Return<void> r;
            r.mStatus = 0;
            return r;
        }

        namespace bluetooth {
            namespace V1_0 {
                enum class Status : int32_t {
                    SUCCESS = 0,
                    TRANSPORT_ERROR = 1,
                    INITIALIZATION_ERROR = 2,
                    UNKNOWN = 3,
                };
            }
        }
    }
}

namespace android::hidl::base::V1_0 {
    struct DebugInfo {};

    struct IBase : virtual public ::android::RefBase {
        // Slot 0 in vtable
        virtual bool isRemote() const { return false; }

        // Slots 1 to 10
        virtual ::android::hardware::Return<void> interfaceChain(std::function<void(const ::android::hardware::hidl_vec<::android::hardware::hidl_string>&)> _hidl_cb) = 0;
        virtual ::android::hardware::Return<void> debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options) = 0;
        virtual ::android::hardware::Return<void> interfaceDescriptor(std::function<void(const ::android::hardware::hidl_string&)> _hidl_cb) = 0;
        virtual ::android::hardware::Return<void> getHashChain(std::function<void(const ::android::hardware::hidl_vec<uint8_t>&)> _hidl_cb) = 0;
        virtual ::android::hardware::Return<void> setHALInstrumentation() = 0;
        virtual ::android::hardware::Return<bool> linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie) = 0;
        virtual ::android::hardware::Return<void> ping() = 0;
        virtual ::android::hardware::Return<void> getDebugInfo(std::function<void(const DebugInfo&)> _hidl_cb) = 0;
        virtual ::android::hardware::Return<void> notifySyspropsChanged() = 0;
        virtual ::android::hardware::Return<bool> unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient) = 0;

        // Slots 11 & 12
        virtual ~IBase() = default;
    };
}

namespace android::hardware::bluetooth {
    namespace V1_0 {
        struct IBluetoothHciCallbacks : virtual public ::android::hidl::base::V1_0::IBase {
            virtual ::android::hardware::Return<void> initializationComplete(Status status) = 0;
            virtual ::android::hardware::Return<void> hciEventReceived(const ::android::hardware::hidl_vec<uint8_t>& event) = 0;
            virtual ::android::hardware::Return<void> aclDataReceived(const ::android::hardware::hidl_vec<uint8_t>& data) = 0;
            virtual ::android::hardware::Return<void> scoDataReceived(const ::android::hardware::hidl_vec<uint8_t>& data) = 0;
        };

        struct IBluetoothHci : virtual public ::android::hidl::base::V1_0::IBase {
            virtual ::android::hardware::Return<void> initialize(const ::android::sp<IBluetoothHciCallbacks>& callback) = 0;
            virtual ::android::hardware::Return<void> sendHciCommand(const ::android::hardware::hidl_vec<uint8_t>& command) = 0;
            virtual ::android::hardware::Return<void> sendAclData(const ::android::hardware::hidl_vec<uint8_t>& data) = 0;
            virtual ::android::hardware::Return<void> sendScoData(const ::android::hardware::hidl_vec<uint8_t>& data) = 0;
            virtual ::android::hardware::Return<void> close() = 0;
        };
    }

    namespace V1_1 {
        struct IBluetoothHciCallbacks : virtual public V1_0::IBluetoothHciCallbacks {
            virtual ::android::hardware::Return<void> isoDataReceived(const ::android::hardware::hidl_vec<uint8_t>& data) = 0;
        };

        struct IBluetoothHci : virtual public V1_0::IBluetoothHci {
            virtual ::android::hardware::Return<void> initialize_1_1(const ::android::sp<IBluetoothHciCallbacks>& callback) = 0;
            virtual ::android::hardware::Return<void> sendIsoData(const ::android::hardware::hidl_vec<uint8_t>& data) = 0;
        };
    }
}

class BluetoothHciShim : public ::android::hardware::bluetooth::V1_1::IBluetoothHci {
private:
    void* mStockLibHandle = nullptr;
    ::android::hardware::bluetooth::V1_0::IBluetoothHci* mStockHci = nullptr;
    using SendDataToControllerFunc = void (*)(void* thiz, int type, const ::android::hardware::hidl_vec<uint8_t>& data);
    SendDataToControllerFunc mSendDataToController = nullptr;

public:
    BluetoothHciShim() {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Initializing BluetoothHciShim...");
        
        // Open stock Qualcomm 1.0 implementation
        mStockLibHandle = dlopen("/vendor/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so", RTLD_NOW);
        if (!mStockLibHandle) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "dlopen stock impl failed: %s", dlerror());
            return;
        }

        using FetchFunc = void* (*)(const char*);
        FetchFunc fetch = (FetchFunc)dlsym(mStockLibHandle, "HIDL_FETCH_IBluetoothHci");
        if (!fetch) {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "dlsym HIDL_FETCH_IBluetoothHci failed: %s", dlerror());
            return;
        }

        mStockHci = (::android::hardware::bluetooth::V1_0::IBluetoothHci*)fetch("default");
        mSendDataToController = (SendDataToControllerFunc)dlsym(mStockLibHandle,
            "_ZN7android8hardware9bluetooth4V1_014implementation12BluetoothHci20sendDataToControllerE13HciPacketTypeRKNS0_8hidl_vecIhEE");

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
            "BluetoothHciShim initialized! mStockHci=%p, mSendDataToController=%p",
            mStockHci, mSendDataToController);
    }

    virtual ~BluetoothHciShim() {
        if (mStockLibHandle) {
            dlclose(mStockLibHandle);
        }
    }

    // Slot 0
    bool isRemote() const override { return false; }

    // Slots 1 to 10
    ::android::hardware::Return<void> interfaceChain(std::function<void(const ::android::hardware::hidl_vec<::android::hardware::hidl_string>&)> _hidl_cb) override {
        static const char* chains[3] = {
            "android.hardware.bluetooth@1.1::IBluetoothHci",
            "android.hardware.bluetooth@1.0::IBluetoothHci",
            "android.hidl.base@1.0::IBase"
        };
        ::android::hardware::hidl_string vec[3];
        for (int i = 0; i < 3; ++i) {
            vec[i].mBuffer = chains[i];
            vec[i].mSize = strlen(chains[i]);
            vec[i].mOwnsBuffer = false;
        }
        ::android::hardware::hidl_vec<::android::hardware::hidl_string> res;
        res.mBuffer = vec;
        res.mSize = 3;
        res.mOwnsBuffer = false;
        _hidl_cb(res);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options) override {
        if (mStockHci) return mStockHci->debug(fd, options);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> interfaceDescriptor(std::function<void(const ::android::hardware::hidl_string&)> _hidl_cb) override {
        ::android::hardware::hidl_string desc;
        desc.mBuffer = "android.hardware.bluetooth@1.1::IBluetoothHci";
        desc.mSize = 45;
        desc.mOwnsBuffer = false;
        _hidl_cb(desc);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> getHashChain(std::function<void(const ::android::hardware::hidl_vec<uint8_t>&)> _hidl_cb) override {
        if (mStockHci) return mStockHci->getHashChain(_hidl_cb);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> setHALInstrumentation() override {
        if (mStockHci) return mStockHci->setHALInstrumentation();
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<bool> linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie) override {
        if (mStockHci) return mStockHci->linkToDeath(recipient, cookie);
        ::android::hardware::Return<bool> r;
        r.mStatus = 0;
        r.mVal = false;
        return r;
    }

    ::android::hardware::Return<void> ping() override {
        if (mStockHci) return mStockHci->ping();
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> getDebugInfo(std::function<void(const ::android::hidl::base::V1_0::DebugInfo&)> _hidl_cb) override {
        if (mStockHci) return mStockHci->getDebugInfo(_hidl_cb);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> notifySyspropsChanged() override {
        if (mStockHci) return mStockHci->notifySyspropsChanged();
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<bool> unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient) override {
        if (mStockHci) return mStockHci->unlinkToDeath(recipient);
        ::android::hardware::Return<bool> r;
        r.mStatus = 0;
        r.mVal = false;
        return r;
    }

    // Slots 13 to 17 (V1_0 overrides)
    ::android::hardware::Return<void> initialize(const ::android::sp<::android::hardware::bluetooth::V1_0::IBluetoothHciCallbacks>& cb) override {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "initialize(1.0) called");
        if (mStockHci) return mStockHci->initialize(cb);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> sendHciCommand(const ::android::hardware::hidl_vec<uint8_t>& command) override {
        if (mStockHci) return mStockHci->sendHciCommand(command);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> sendAclData(const ::android::hardware::hidl_vec<uint8_t>& data) override {
        if (mStockHci) return mStockHci->sendAclData(data);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> sendScoData(const ::android::hardware::hidl_vec<uint8_t>& data) override {
        if (mStockHci) return mStockHci->sendScoData(data);
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> close() override {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "close() called");
        if (mStockHci) return mStockHci->close();
        return ::android::hardware::Void();
    }

    // Slots 18 & 19 (V1_1 overrides)
    ::android::hardware::Return<void> initialize_1_1(const ::android::sp<::android::hardware::bluetooth::V1_1::IBluetoothHciCallbacks>& cb) override {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "initialize_1_1 called! Delegating to stock initialize with callback wrapper");
        if (mStockHci) {
            return mStockHci->initialize((::android::hardware::bluetooth::V1_0::IBluetoothHciCallbacks*)cb.get());
        }
        return ::android::hardware::Void();
    }

    ::android::hardware::Return<void> sendIsoData(const ::android::hardware::hidl_vec<uint8_t>& data) override {
        if (mSendDataToController && mStockHci) {
            mSendDataToController(mStockHci, 5, data);
        } else {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "sendIsoData: mSendDataToController or mStockHci is null!");
        }
        return ::android::hardware::Void();
    }
};

extern "C" __attribute__((visibility("default")))
void* HIDL_FETCH_IBluetoothHci(const char* name) {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HIDL_FETCH_IBluetoothHci called for: %s", name ? name : "(null)");
    return new BluetoothHciShim();
}
