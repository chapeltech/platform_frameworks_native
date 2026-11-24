#pragma once

#include <algorithm>
#include <vector>
#include "NativeLifetimeToken.h"

namespace android::surfaceflinger {
// SF-main-only pending weak observations, never a Layer/SC registry. Published
// facts live in their already-issued nonresource tokens, not an unbounded log.
class NativeRetirementTracker {
public:
    static constexpr size_t kMaxPending=8192;
    void removed(const sp<NativeLifetimeToken>& token,const std::function<void()>& wake) {
        if (!token) return;
        prune();
        for (const auto& weak:mPending) if (weak.promote()==token) return;
        if (!mAvailable || mPending.size()>=kMaxPending) { token->state()->unavailable(); return; }
        token->state()->removed(wake); mPending.emplace_back(token);
    }
    bool afterCommit() {
        if (!mAvailable) return false;
        if (mPass==UINT64_MAX) {
            mAvailable=false;
            for (const auto& weak:mPending) if (auto t=weak.promote()) t->state()->unavailable();
            return false;
        }
        ++mPass; prune(); bool changed=false;
        for (const auto& weak:mPending) if (auto t=weak.promote()) changed|=t->state()->fenceAfterCommit(mPass);
        return changed;
    }
    uint64_t pass() const { return mAvailable ? mPass:0; }
    std::vector<sp<NativeLifetimeToken>> candidates() {
        prune();std::vector<sp<NativeLifetimeToken>> out;
        if (!mAvailable) return out;
        for (const auto& weak:mPending) if (auto t=weak.promote()) {
            const auto v=t->state()->value();
            if (v.available && v.completed && v.removalPass && !v.publication.available()) out.push_back(std::move(t));
        }
        return out;
    }
private:
    void prune() {
        std::erase_if(mPending,[](const auto& weak) {
            auto t=weak.promote();
            // Expiry is ONLY garbage collection: it never sets completed/fenced.
            return !t || t->state()->value().publication.available();
        });
    }
    bool mAvailable=true;
    uint64_t mPass=0;
    std::vector<wp<NativeLifetimeToken>> mPending;
};
} // namespace android::surfaceflinger
