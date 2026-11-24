#pragma once

#include <binder/Binder.h>
#include <gui/NativeLifetimeFacts.h>
#include <array>
#include "NativeLifetimeState.h"

namespace android::surfaceflinger {
class NativeLifetimeToken final : public BBinder {
public:
    using State=NativeLifetimeState<gui::NativePublication>;
    explicit NativeLifetimeToken(gui::NativeLifetimeKind kind,
            sp<NativeLifetimeToken> resourceGroup={})
          : mKind(kind),mState(std::make_shared<State>()),mResourceGroup(std::move(resourceGroup)) {}
    const std::shared_ptr<State>& state() const { return mState; }
    gui::NativeLifetimeKind kind() const { return mKind; }
    const sp<NativeLifetimeToken>& resourceGroup() const { return mResourceGroup; }
protected:
    status_t onTransact(uint32_t code,const Parcel& request,Parcel* reply,uint32_t flags=0) override {
        if (code!=gui::kNativeLifetimeQuery) return BBinder::onTransact(code,request,reply,flags);
        uint32_t version;
        if (!reply || (flags&IBinder::FLAG_ONEWAY) || request.readUint32(&version)!=OK ||
                request.dataAvail()!=0) return BAD_VALUE;
        const State* state=mState.get();auto kind=mKind;
        if (version==gui::kNativeRootResourceGroupQueryVersion) {
            if (mKind!=gui::NativeLifetimeKind::Layer || !mResourceGroup ||
                mResourceGroup->kind()!=gui::NativeLifetimeKind::RootResourceGroup) return BAD_VALUE;
            state=mResourceGroup->state().get();kind=gui::NativeLifetimeKind::RootResourceGroup;
        } else if (version!=gui::kNativeLifetimeQueryVersion ||
                   mKind==gui::NativeLifetimeKind::RootResourceGroup) return BAD_VALUE;
        const auto v=state->value();
        return gui::writeNativeLifetimeFact(reply,{kind,v.available,v.completed,v.removalPass,v.publication});
    }
private:
    const gui::NativeLifetimeKind mKind;
    const std::shared_ptr<State> mState; // NO resource lease or Layer/SC/FD/back-pointer.
    const sp<NativeLifetimeToken> mResourceGroup; // NONRESOURCE; retaining it is NOT a lease.
};

// Must be the FIRST base (first VIRTUAL base for Layer). Thus its destructor
// runs after all real resource-bearing members/bases, not in the destructor body.
class NativeLifetimeGuard {
public:
    NativeLifetimeGuard() = default; // Root/default snapshots explicitly have no identity.
    explicit NativeLifetimeGuard(gui::NativeLifetimeKind kind) { attach(sp<NativeLifetimeToken>::make(kind)); }
    NativeLifetimeGuard(const NativeLifetimeGuard& other) { copy(other); }
    // A moved-from C++ object's bases/members still need not be resource-free.
    // Keep a lease on BOTH objects until both actual destructors complete.
    NativeLifetimeGuard(NativeLifetimeGuard&& other) noexcept { copy(other); }
    NativeLifetimeGuard& operator=(const NativeLifetimeGuard&)=delete;
    NativeLifetimeGuard& operator=(NativeLifetimeGuard&&)=delete;
    ~NativeLifetimeGuard() {
        if (mToken) mToken->state()->release();
        for (size_t i=0;i<mGroupCount;++i) if (mGroups[i].acquired) mGroups[i].token->state()->release();
    }
protected:
    static constexpr size_t kMaxResourceGroups=64; // bounded actual-object leases, NOT history.
    void attachResourceGroup(uint32_t creationId,const sp<NativeLifetimeToken>& token) {
        if (!token || token->kind()!=gui::NativeLifetimeKind::RootResourceGroup) {
            unavailableResourceGroups();return;
        }
        for (size_t i=0;i<mGroupCount;++i) if (mGroups[i].token==token) {
            if (mGroups[i].creationId!=creationId) unavailableResourceGroups();
            return;
        }
        if (!mGroupsAvailable || mGroupCount==kMaxResourceGroups) {
            token->state()->unavailable();unavailableResourceGroups();return;
        }
        const bool acquired=token->state()->acquire();
        mGroups[mGroupCount++]={token,creationId,acquired};
        if (!acquired) unavailableResourceGroups();
    }
    template<class RootIds> void inheritResourceGroups(const NativeLifetimeGuard& parent,
                                                        const RootIds& roots) {
        if (!parent.mGroupsAvailable) unavailableResourceGroups();
        for (uint32_t id:roots) {
            if (id==UINT32_MAX) continue; // synthetic hierarchy ROOT, never a real Layer.
            bool found=false;
            for (size_t i=0;i<parent.mGroupCount;++i) if (parent.mGroups[i].creationId==id) {
                found=true;attachResourceGroup(id,parent.mGroups[i].token);
            }
            if (!found) {
                // Never silently omit a known parent's coverage. This path is
                // impossible for the actual ordered hierarchy construction.
                for (size_t i=0;i<parent.mGroupCount;++i) parent.mGroups[i].token->state()->unavailable();
                unavailableResourceGroups();
            }
        }
    }
    void attach(const sp<NativeLifetimeToken>& token) {
        if (mToken) { mToken->state()->unavailable(); return; }
        mToken=token; if (mToken) mToken->state()->acquire();
    }
    const sp<NativeLifetimeToken>& nativeLifetimeToken() const { return mToken; }
private:
    struct GroupLease { sp<NativeLifetimeToken> token;uint32_t creationId=0;bool acquired=false; };
    void copy(const NativeLifetimeGuard& other) {
        attach(other.mToken);mGroupsAvailable=other.mGroupsAvailable;
        for (size_t i=0;i<other.mGroupCount;++i) attachResourceGroup(other.mGroups[i].creationId,other.mGroups[i].token);
    }
    void unavailableResourceGroups() {
        mGroupsAvailable=false;
        for (size_t i=0;i<mGroupCount;++i) mGroups[i].token->state()->unavailable();
    }
    sp<NativeLifetimeToken> mToken;
    std::array<GroupLease,kMaxResourceGroups> mGroups{};
    size_t mGroupCount=0;
    bool mGroupsAvailable=true;
};
} // namespace android::surfaceflinger
