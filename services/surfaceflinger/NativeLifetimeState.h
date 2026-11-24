#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>

namespace android::surfaceflinger {

// Only nonresource value state. The number of RESOURCE leases is independent
// from shared_ptr/Binder reference counts; retained observations cannot delay it.
template<class Publication> class NativeLifetimeState {
public:
    struct Value {
        bool available=true, completed=false;
        uint64_t removalPass=0;
        Publication publication{};
    };
    bool acquire() {
        std::lock_guard lock(mLock);
        if (!mValue.available || mValue.completed || mLeases==UINT32_MAX) {
            mValue.available=false; return false;
        }
        ++mLeases; return true;
    }
    void release() {
        std::function<void()> wake;
        {
            std::lock_guard lock(mLock);
            if (!mLeases) { mValue.available=false; return; }
            if (--mLeases==0) { mValue.completed=true; if (mRemoved) wake=mWake; }
        }
        if (wake) wake(); // NEVER while holding this value-state mutex.
    }
    void removed(std::function<void()> wake) {
        std::lock_guard lock(mLock); mRemoved=true; mWake=std::move(wake);
    }
    bool fenceAfterCommit(uint64_t pass) {
        std::lock_guard lock(mLock);
        if (!mValue.available || !mRemoved || !mValue.completed || !pass || mValue.removalPass) return false;
        mValue.removalPass=pass; return true;
    }
    bool publish(const Publication& value) {
        std::lock_guard lock(mLock);
        if (!mValue.available || !mValue.completed || !mValue.removalPass ||
            !value.available() || value.mainPass<mValue.removalPass || mValue.publication.available()) return false;
        mValue.publication=value; return true;
    }
    void unavailable() { std::lock_guard lock(mLock); mValue.available=false; }
    Value value() const {
        std::lock_guard lock(mLock);
        return mValue.available ? mValue : Value{false,false,0,{}};
    }
private:
    mutable std::mutex mLock;
    uint32_t mLeases=0;
    bool mRemoved=false;
    Value mValue;
    std::function<void()> mWake; // Production closure captures ONLY weak SF, no Layer/SC/FD.
};
} // namespace android::surfaceflinger
