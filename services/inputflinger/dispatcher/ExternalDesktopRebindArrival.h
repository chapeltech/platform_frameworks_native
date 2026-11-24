#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopPointerOwnership.h"
#include "InputTargetFlags.h"
#include <ftl/flags.h>
#include <gui/PidUid.h>
#include <ui/Transform.h>
#include <memory>

namespace android { class ExternalDesktopStageObjectIdentity; }
namespace android::inputdispatcher {
class Connection;
class InputDispatcher;
class InputState;
class ExternalDesktopPublishedMotion;
struct DispatchEntry;
struct MotionEntry;

// Immutable per-packet derivative of an ACTUAL trusted two-owner capability,
// never a public MotionEntry/StageTag/scalar authorization. It avoids retaining
// the capability's InputTarget -> Connection strong reference in a queue cycle.
// Old and new key references survive publication and remain on the exact sent
// entry until its actual ACK/abort, not merely until queue commit.
class ExternalDesktopRebindArrival final {
public:
    bool references(const ExternalDesktopPointerStreamKey& key) const { return key == mOldKey || key == mNewKey; }
private:
    friend class InputDispatcher;
    friend class Connection;
    friend class InputState;
    friend class ExternalDesktopPublishedMotion;
    ExternalDesktopRebindArrival(std::weak_ptr<const Connection> source,
                       std::weak_ptr<const Connection> destination,
                       std::shared_ptr<const ExternalDesktopStageObjectIdentity> submission,
                       ExternalDesktopPointerStreamKey oldKey, ExternalDesktopPointerStreamKey newKey,
                       ExternalDesktopPointerOwnership::Ids before, ExternalDesktopPointerOwnership::Ids added,
                       ExternalDesktopPointerOwnershipPtr beforeOwners,
                       std::shared_ptr<const MotionEntry> event,
                       std::shared_ptr<const MotionEntry> physicalOrigin, uint32_t seq,
                       const ExternalDesktopPublishedMotion* owner, uint64_t revision,
                       const ui::Transform& transform, const ui::Transform& rawTransform,
                       float scale, ftl::Flags<InputTargetFlags> targetFlags,
                       ftl::Flags<MotionFlag> flags, gui::Uid uid, int64_t vsyncId,
                       std::optional<int32_t> windowId)
          : mSource(std::move(source)), mDestination(std::move(destination)),
            mSubmission(std::move(submission)), mOldKey(oldKey), mNewKey(newKey),
            mBefore(before), mAdded(added), mBeforeOwners(std::move(beforeOwners)),
            mEvent(std::move(event)), mPhysicalOrigin(std::move(physicalOrigin)), mSeq(seq), mOwner(owner), mRevision(revision),
            mTransform(transform), mRawTransform(rawTransform), mScale(scale),
            mTargetFlags(targetFlags), mFlags(flags), mUid(uid), mVsyncId(vsyncId), mWindowId(windowId) {}
    std::weak_ptr<const Connection> mSource;
    std::weak_ptr<const Connection> mDestination;
    std::shared_ptr<const ExternalDesktopStageObjectIdentity> mSubmission;
    ExternalDesktopPointerStreamKey mOldKey, mNewKey;
    ExternalDesktopPointerOwnership::Ids mBefore, mAdded;
    ExternalDesktopPointerOwnershipPtr mBeforeOwners;
    std::shared_ptr<const MotionEntry> mEvent;
    std::shared_ptr<const MotionEntry> mPhysicalOrigin;
    uint32_t mSeq;
    const ExternalDesktopPublishedMotion* mOwner;
    uint64_t mRevision;
    ui::Transform mTransform, mRawTransform;
    float mScale;
    ftl::Flags<InputTargetFlags> mTargetFlags;
    ftl::Flags<MotionFlag> mFlags;
    gui::Uid mUid;
    int64_t mVsyncId;
    std::optional<int32_t> mWindowId;
};
using ExternalDesktopRebindArrivalPtr = std::shared_ptr<const ExternalDesktopRebindArrival>;
} // namespace android::inputdispatcher
#endif
