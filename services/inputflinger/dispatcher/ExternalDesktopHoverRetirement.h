#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "InputTargetFlags.h"
#include "ExternalDesktopPointerOwnership.h"

#include <ftl/flags.h>
#include <gui/PidUid.h>
#include <memory>
#include <ui/Transform.h>
#include <utility>

namespace android::inputdispatcher {
class Connection;
class InputState;
class ExternalDesktopPublishedMotion;
struct MotionEntry;

// PRIVATE packet capability, not a stream/lifetime/authority issuer. Only the
// native owner can create/rebind it after validating its existing queue and a
// saved successful snapshot. No raw MotionEntry, APK or wire setter carries it.
class ExternalDesktopHoverRetirement final {
public:
    bool references(const ExternalDesktopPointerStreamKey& key) const {
        for (size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id) {
            if (ExternalDesktopPointerOwnership::keyAt(mRemovedOwners,static_cast<int32_t>(id))==key) return true;
        }
        return false;
    }
private:
    friend class Connection;
    friend class InputState;
    friend class ExternalDesktopPublishedMotion;
    ExternalDesktopHoverRetirement(std::weak_ptr<const Connection> connection,
            const ExternalDesktopPublishedMotion* owner,uint64_t revision,
            ExternalDesktopPointerOwnership::Ids before,ExternalDesktopPointerOwnership::Ids after,
            ExternalDesktopPointerOwnershipPtr beforeOwners,ExternalDesktopPointerOwnershipPtr removedOwners,
            std::shared_ptr<const MotionEntry> event,uint32_t seq,
            const ui::Transform& transform,const ui::Transform& rawTransform,float scale,
            ftl::Flags<InputTargetFlags> targetFlags,ftl::Flags<MotionFlag> flags,
            gui::Uid uid,int64_t vsyncId,std::optional<int32_t> windowId)
          : mConnection(std::move(connection)),mOwner(owner),mRevision(revision),mBefore(before),
            mAfter(after),mBeforeOwners(std::move(beforeOwners)),mRemovedOwners(std::move(removedOwners)),
            mEvent(std::move(event)),mSeq(seq),mTransform(transform),mRawTransform(rawTransform),
            mScale(scale),mTargetFlags(targetFlags),mFlags(flags),mUid(uid),mVsyncId(vsyncId),mWindowId(windowId) {}
    const std::weak_ptr<const Connection> mConnection;
    const ExternalDesktopPublishedMotion* const mOwner;
    const uint64_t mRevision;
    const ExternalDesktopPointerOwnership::Ids mBefore,mAfter;
    const ExternalDesktopPointerOwnershipPtr mBeforeOwners,mRemovedOwners;
    const std::shared_ptr<const MotionEntry> mEvent;
    const uint32_t mSeq;
    const ui::Transform mTransform,mRawTransform;
    const float mScale;
    const ftl::Flags<InputTargetFlags> mTargetFlags;
    const ftl::Flags<MotionFlag> mFlags;
    const gui::Uid mUid;
    const int64_t mVsyncId;
    const std::optional<int32_t> mWindowId;
};

using ExternalDesktopHoverRetirementPtr=std::shared_ptr<const ExternalDesktopHoverRetirement>;
} // namespace android::inputdispatcher
#endif
