#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopPointerOwnership.h"

#include <gui/WindowInfo.h>
#include <memory>

namespace android { class ExternalDesktopStageObjectIdentity; }
namespace android::inputdispatcher {
class Connection;
class InputTarget;
class InputDispatcher;
class ExternalDesktopStageOwner;
class InputDispatcherTest;
struct MotionEntry;

// PRIVATE TCB capability, not a wire value or a second authority. In particular
// an APK, WindowInfo/StageTag, channel token, geometry stamp or scalar key cannot
// construct it. The production factory belongs to the still-UNIMPLEMENTED
// StageOwner full-claim/stream issuer. No such factory/caller is added here.
//
// Actual endpoint referents and an exact successful pre-target native packet
// are retained, not reconstructed from advanced InputState or receiver history.
// Destination InputTarget is an owned, trusted, per-pointer resolved projection
// for this exact native frame. It is never looked up from mutable geometry at
// transaction commit. A physical release/closing-key/drain fence is separate.
class ExternalDesktopTrustedTransfer final {
public:
    ExternalDesktopTrustedTransfer(const ExternalDesktopTrustedTransfer&) = delete;
    ExternalDesktopTrustedTransfer& operator=(const ExternalDesktopTrustedTransfer&) = delete;

private:
    friend class ExternalDesktopStageOwner;
    friend class InputDispatcher;
    friend class InputDispatcherTest; // explicit fixture double, never issuance
    struct Endpoint final {
        sp<gui::WindowInfoHandle> handle;
        gui::WindowInfo immutableInfo;
        std::shared_ptr<const ExternalDesktopStageObjectIdentity> window;
        std::shared_ptr<const ExternalDesktopStageObjectIdentity> presentation;
    };
    ExternalDesktopTrustedTransfer(std::weak_ptr<Connection> source, std::weak_ptr<Connection> destination,
                         Endpoint from, Endpoint to,
                         std::shared_ptr<const ExternalDesktopStageObjectIdentity> submission,
                         std::shared_ptr<const MotionEntry> physicalOrigin,
                         std::shared_ptr<const MotionEntry> resolvedOrigin,
                         std::shared_ptr<const InputTarget> target,
                         ExternalDesktopPointerOwnership::Ids selected, ExternalDesktopPointerStreamKey oldKey,
                         ExternalDesktopPointerStreamKey newKey, uint64_t sourceRevision,
                         uint64_t destinationRevision, uint64_t sourceDeliveredRevision,
                         uint64_t destinationDeliveredRevision, int64_t destinationVsyncId)
          : mSource(std::move(source)), mDestination(std::move(destination)),
            mFrom(std::move(from)), mTo(std::move(to)), mSubmission(std::move(submission)),
            mPhysicalOrigin(std::move(physicalOrigin)), mResolvedOrigin(std::move(resolvedOrigin)),
            mDestinationTarget(std::move(target)), mSelected(selected), mOldKey(oldKey),
            mNewKey(newKey), mSourceRevision(sourceRevision),
            mDestinationRevision(destinationRevision), mSourceDeliveredRevision(sourceDeliveredRevision),
            mDestinationDeliveredRevision(destinationDeliveredRevision), mDestinationVsyncId(destinationVsyncId) {}

    std::weak_ptr<Connection> mSource;
    std::weak_ptr<Connection> mDestination;
    Endpoint mFrom;
    Endpoint mTo;
    std::shared_ptr<const ExternalDesktopStageObjectIdentity> mSubmission;
    std::shared_ptr<const MotionEntry> mPhysicalOrigin;
    std::shared_ptr<const MotionEntry> mResolvedOrigin;
    std::shared_ptr<const InputTarget> mDestinationTarget;
    ExternalDesktopPointerOwnership::Ids mSelected;
    ExternalDesktopPointerStreamKey mOldKey;
    ExternalDesktopPointerStreamKey mNewKey;
    uint64_t mSourceRevision;
    uint64_t mDestinationRevision;
    uint64_t mSourceDeliveredRevision;
    uint64_t mDestinationDeliveredRevision;
    int64_t mDestinationVsyncId;
    bool mConsumed = false;
};
} // namespace android::inputdispatcher
#endif
