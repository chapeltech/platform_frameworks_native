#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "Entry.h"

#include <android-base/result.h>

#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace android::inputdispatcher {

// PRIVATE delivery evidence, not a stream issuer, claim, routing decision or
// lifetime retirement API. The connection's dispatcher lock protects this owner.
// Unlike InputState (which advances at enqueue), this advances ONLY after the
// real InputPublisher reports success. Queue entries themselves retain queued
// and sent packet ownership until actual removal/ACK/abort, independently.
class ExternalDesktopPublishedMotion final {
public:
    static constexpr size_t kMaxMementos = 64;
    using Ids = ExternalDesktopPointerOwnership::Ids;

    struct Snapshot final {
        DeviceId deviceId;
        uint32_t source;
        ui::LogicalDisplayId displayId;
        bool hovering;
        Ids liveIds;
        std::vector<PointerProperties> properties;
        std::vector<PointerCoords> coords;
        ExternalDesktopPointerOwnershipPtr ownership;
        ui::Transform transform;
        ui::Transform rawTransform;
        float globalScaleFactor;
        ftl::Flags<MotionFlag> flags;
        float xPrecision;
        float yPrecision;
        float xCursorPosition;
        float yCursorPosition;
        nsecs_t downTime;
        uint32_t policyFlags;
        gui::Uid targetUid;
        int64_t vsyncId;
        std::optional<int32_t> windowId;
        // Exact successful packet context. ZERO_COORDS controls scaling in the
        // publisher; reconstructing this from today's WindowInfo is unsafe.
        ftl::Flags<InputTargetFlags> targetFlags;
        nsecs_t eventTime;
        std::shared_ptr<const MotionEntry> traceOrigin;
        // Actual native pre-target/pre-split origin, distinct from the
        // resolved trace envelope and any receiver-side batched history.
        std::shared_ptr<const MotionEntry> physicalOrigin;
        ExternalDesktopSubmissionReservationPtr submissionReservation;

        Ids idsFor(const ExternalDesktopPointerStreamKey& key) const {
            Ids result;
            if (!key.valid()) return result;
            for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
                if (liveIds.test(id) && ExternalDesktopPointerOwnership::keyAt(ownership, static_cast<int32_t>(id)) == key) {
                    result.set(id);
                }
            }
            return result;
        }
    };

    // All allocation/copying happens BEFORE the publisher call. The successful
    // publication commit is a no-allocation move into an existing bounded slot.
    // A WOULD_BLOCK/error destroys this preparation without changing evidence.
    class Prepared final {
    public:
        Prepared(Prepared&& other) noexcept
              : mOwner(std::exchange(other.mOwner, nullptr)), mRevision(other.mRevision),
                mIndex(other.mIndex), mSnapshot(std::move(other.mSnapshot)) {}
        Prepared& operator=(Prepared&& other) noexcept {
            if (this != &other) {
                mOwner = std::exchange(other.mOwner, nullptr);
                mRevision = other.mRevision;
                mIndex = other.mIndex;
                mSnapshot = std::move(other.mSnapshot);
            }
            return *this;
        }
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class ExternalDesktopPublishedMotion;
        Prepared(const ExternalDesktopPublishedMotion* owner, uint64_t revision, size_t index,
                 std::shared_ptr<const Snapshot> snapshot)
              : mOwner(owner), mRevision(revision), mIndex(index), mSnapshot(std::move(snapshot)) {}
        const ExternalDesktopPublishedMotion* mOwner;
        uint64_t mRevision;
        size_t mIndex;
        std::shared_ptr<const Snapshot> mSnapshot;
    };

    android::base::Result<Prepared> prepare(const DispatchEntry& packet) const {
        return preparePacket(packet,this);
    }

private:
    friend class Connection;
    android::base::Result<Prepared> prepareForReplay(const DispatchEntry& packet,
                                                    const ExternalDesktopPublishedMotion& realOwner) const {
        return preparePacket(packet,&realOwner);
    }
    android::base::Result<Prepared> preparePacket(const DispatchEntry& packet,
                                                 const ExternalDesktopPublishedMotion* realOwner) const {
        constexpr size_t noChange = kMaxMementos;
        auto unchanged = [&]() { return Prepared(this, mRevision, noChange, nullptr); };
        if (packet.eventEntry->type != EventEntry::Type::MOTION) {
            if(packet.externalDesktopHoverRetirement || packet.externalDesktopRebindArrival) return android::base::Error() << "Native motion capability on non-motion packet";
            return unchanged();
        }
        const auto& event = static_cast<const MotionEntry&>(*packet.eventEntry);
        const auto& ownership = packet.externalDesktopPointerOwnership;
        const int32_t action = MotionEvent::getActionMasked(event.action);
        bool hovering = action == AMOTION_EVENT_ACTION_HOVER_ENTER ||
                action == AMOTION_EVENT_ACTION_HOVER_MOVE || action == AMOTION_EVENT_ACTION_HOVER_EXIT;
        const bool sampleOnly = action == AMOTION_EVENT_ACTION_SCROLL ||
                action == AMOTION_EVENT_ACTION_BUTTON_PRESS || action == AMOTION_EVENT_ACTION_BUTTON_RELEASE;
        size_t previous = find(event, hovering);
        // OUTSIDE does not update an ongoing stream. Ordinary keys/unclaimed
        // input never depend on a private bookkeeping revision/capacity budget.
        if (action == AMOTION_EVENT_ACTION_OUTSIDE && !packet.externalDesktopHoverRetirement) return unchanged();
        if (!packet.externalDesktopHoverRetirement && !sampleOnly && !ownership && previous == noChange) return unchanged();
        if (!packet.externalDesktopHoverRetirement && sampleOnly && !ownership && previous == noChange && find(event, true) == noChange) {
            return unchanged();
        }
        if (ownership != event.externalDesktopPointerOwnership ||
            event.getPointerCount() > MAX_POINTERS ||
            event.pointerProperties.size() != event.pointerCoords.size() ||
            (ownership && !ownership->matches(event.pointerProperties))) {
            return android::base::Error() << "Private published packet roster mismatch";
        }
        Ids liveIds;
        if (!ExternalDesktopPointerOwnership::idsOf(event.pointerProperties, liveIds)) {
            return android::base::Error() << "Invalid published pointer IDs";
        }
        for (const auto& point : event.pointerCoords) {
            if (BitSet64::count(point.bits) > point.values.size()) {
                return android::base::Error() << "Private published coordinate axis capacity exceeded";
            }
        }
        if(packet.externalDesktopHoverRetirement && !validHoverRetirement(packet,realOwner)) {
            return android::base::Error() << "Untrusted/stale hover roster-retirement capability";
        }
        if (packet.externalDesktopRebindArrival && !validRebindArrival(packet, realOwner, hovering)) {
            return android::base::Error() << "Untrusted/stale native rebind-arrival capability";
        }
        if (sampleOnly) {
            const size_t contact = find(event, false), hover = find(event, true);
            if (contact == noChange && hover == noChange) return unchanged();
            const auto matches = [&](size_t index) {
                if (index == noChange || mSnapshots[index]->liveIds != liveIds) return false;
                for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
                    if (liveIds.test(id) &&
                        ExternalDesktopPointerOwnership::keyAt(mSnapshots[index]->ownership, static_cast<int32_t>(id)) !=
                        ExternalDesktopPointerOwnership::keyAt(ownership, static_cast<int32_t>(id))) return false;
                }
                return true;
            };
            const bool contactMatches = matches(contact), hoverMatches = matches(hover);
            if (contactMatches == hoverMatches) {
                // Do not keep stale geometry or guess another live owner's
                // coordinates. Scoped partial/ambiguous samples need the
                // per-pointer geometry mechanism before complete activation.
                return android::base::Error() << "Ambiguous/incomplete private published sample";
            }
            previous = contactMatches ? contact : hover;
            hovering = mSnapshots[previous]->hovering;
        }
        if (mRevision == std::numeric_limits<uint64_t>::max()) {
            return android::base::Error() << "Private publication owner revision exhausted";
        }
        if (!sampleOnly && action != AMOTION_EVENT_ACTION_DOWN && action != AMOTION_EVENT_ACTION_UP &&
            action != AMOTION_EVENT_ACTION_MOVE && action != AMOTION_EVENT_ACTION_CANCEL &&
            action != AMOTION_EVENT_ACTION_POINTER_DOWN && action != AMOTION_EVENT_ACTION_POINTER_UP &&
            action != AMOTION_EVENT_ACTION_HOVER_ENTER && action != AMOTION_EVENT_ACTION_HOVER_MOVE &&
            action != AMOTION_EVENT_ACTION_HOVER_EXIT) {
            return android::base::Error() << "Invalid private publication action";
        }
        if ((action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_DOWN) &&
            event.getPointerCount() != 1) {
            return android::base::Error() << "Invalid private terminal/initial pointer count";
        }
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN || action == AMOTION_EVENT_ACTION_POINTER_UP) {
            const size_t index = MotionEvent::getActionIndex(event.action);
            if (event.getPointerCount() < 2 || index >= event.getPointerCount()) {
                return android::base::Error() << "Invalid private pointer transition index";
            }
        }
        if (previous != noChange && !packet.externalDesktopHoverRetirement &&
            !(packet.externalDesktopRebindArrival && hovering)) {
            const Snapshot& old = *mSnapshots[previous];
            for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
                if (!old.liveIds.test(id)) continue;
                const auto before = ExternalDesktopPointerOwnership::keyAt(old.ownership, static_cast<int32_t>(id));
                const auto after = ExternalDesktopPointerOwnership::keyAt(ownership, static_cast<int32_t>(id));
                if (!liveIds.test(id) || before != after) {
                    return android::base::Error() << "Published packet changed a surviving owner";
                }
            }
            if (action == AMOTION_EVENT_ACTION_DOWN ||
                (action != AMOTION_EVENT_ACTION_POINTER_DOWN && old.liveIds != liveIds)) {
                return android::base::Error() << "Published packet replaced a live pointer roster";
            }
            if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                const int32_t added = event.pointerProperties[MotionEvent::getActionIndex(event.action)].id;
                if (old.liveIds.test(added) || (liveIds ^ old.liveIds).count() != 1) {
                    return android::base::Error() << "Invalid private pointer addition";
                }
            }
        }
        if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL ||
            action == AMOTION_EVENT_ACTION_HOVER_EXIT) {
            return previous == noChange ? unchanged() : Prepared(this, mRevision, previous, nullptr);
        }
        if (action == AMOTION_EVENT_ACTION_POINTER_UP) {
            const size_t index = MotionEvent::getActionIndex(event.action);
            if (index >= event.getPointerCount()) {
                return android::base::Error() << "Invalid published POINTER_UP index";
            }
            liveIds.reset(event.pointerProperties[index].id);
        }
        bool hasLiveOwner = false;
        for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
            hasLiveOwner |= liveIds.test(id) &&
                    ExternalDesktopPointerOwnership::keyAt(ownership, static_cast<int32_t>(id)).has_value();
        }
        if (!hasLiveOwner) {
            return previous == noChange ? unchanged() : Prepared(this, mRevision, previous, nullptr);
        }
        size_t slot = previous;
        if (slot == noChange) {
            for (size_t i = 0; i < mSnapshots.size(); ++i) {
                if (!mSnapshots[i]) { slot = i; break; }
            }
        }
        if (slot == noChange) {
            // Admission must reserve this bounded capacity before private
            // selection is activated. Never silently evict another live stream.
            return android::base::Error() << "Private published-memento capacity exhausted";
        }
        auto reservation = snapshotReservation(packet, liveIds);
        if (!reservation.ok()) return android::base::Error() << reservation.error();
        auto next = std::make_shared<const Snapshot>(Snapshot{
                event.deviceId, event.source, event.displayId, hovering, liveIds,
                event.pointerProperties, event.pointerCoords, ownership,
                packet.transform, packet.rawTransform, packet.globalScaleFactor,
                packet.resolvedMotionFlags, event.xPrecision, event.yPrecision,
                event.xCursorPosition, event.yCursorPosition, event.downTime, event.policyFlags,
                packet.targetUid, packet.vsyncId, packet.windowId,
                packet.targetFlags, event.eventTime,
                std::static_pointer_cast<const MotionEntry>(packet.eventEntry),
                packet.externalDesktopPhysicalMotionOrigin, std::move(*reservation)});
        return Prepared(this, mRevision, slot, std::move(next));
    }

    static android::base::Result<ExternalDesktopSubmissionReservationPtr> snapshotReservation(
            const DispatchEntry& packet, const Ids& liveIds) {
        if (!packet.externalDesktopSubmissionReservation) return ExternalDesktopSubmissionReservationPtr{};
        auto retained = std::shared_ptr<ExternalDesktopSubmissionReservation>(new ExternalDesktopSubmissionReservation);
        for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
            if (!liveIds.test(id)) continue;
            const auto key = ExternalDesktopPointerOwnership::keyAt(packet.externalDesktopPointerOwnership,
                                                         static_cast<int32_t>(id));
            if (!key) continue; // Ordinary/null peers never acquire a private row.
            const auto& row = packet.externalDesktopSubmissionReservation->mRows[id];
            if (!row || row->mId != static_cast<int32_t>(id) || row->mKey != *key) {
                return android::base::Error() << "Snapshot preparation lost its exact surviving row";
            }
            retained->mRows[id] = row; // SAME existing row, no issuance or physical-lifetime change.
        }
        // Preparation-only allocation: the actual queued/sent packet and
        // closing handle keep their FULL owners through real publication/ACK.
        return ExternalDesktopSubmissionReservationPtr(std::move(retained));
    }

    bool validRebindArrival(const DispatchEntry& packet, const ExternalDesktopPublishedMotion* realOwner,
                            bool hovering) const {
        const auto& token = packet.externalDesktopRebindArrival;
        if (!token || token->mOwner != realOwner || token->mRevision != mRevision ||
            token->mSource.expired() || token->mDestination.expired() || !token->mSubmission ||
            token->mEvent.get() != packet.eventEntry.get() || token->mSeq != packet.seq ||
            !token->mPhysicalOrigin || token->mPhysicalOrigin != packet.externalDesktopPhysicalMotionOrigin ||
            token->mTransform != packet.transform || token->mRawTransform != packet.rawTransform ||
            token->mScale != packet.globalScaleFactor || token->mTargetFlags != packet.targetFlags ||
            token->mFlags != packet.resolvedMotionFlags || token->mUid != packet.targetUid ||
            token->mVsyncId != packet.vsyncId || token->mWindowId != packet.windowId ||
            !token->mOldKey.valid() || !token->mNewKey.valid()) return false;
        const auto& event = static_cast<const MotionEntry&>(*packet.eventEntry);
        Ids ids;
        if (!ExternalDesktopPointerOwnership::idsOf(event.pointerProperties, ids) ||
            token->mAdded.none() || (token->mBefore & token->mAdded).any() ||
            ids != (token->mBefore | token->mAdded)) return false;
        const auto previous = find(event, hovering);
        if (previous != kMaxMementos && mSnapshots[previous]->liveIds != token->mBefore) return false;
        for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
            if (token->mBefore.test(id)) {
                const auto owner = ExternalDesktopPointerOwnership::keyAt(token->mBeforeOwners, static_cast<int32_t>(id));
                if ((previous == kMaxMementos && owner) ||
                    (previous != kMaxMementos && owner != ExternalDesktopPointerOwnership::keyAt(
                            mSnapshots[previous]->ownership, static_cast<int32_t>(id))) ||
                    owner != ExternalDesktopPointerOwnership::keyAt(packet.externalDesktopPointerOwnership, static_cast<int32_t>(id))) return false;
            }
            if (token->mAdded.test(id) && ExternalDesktopPointerOwnership::keyAt(packet.externalDesktopPointerOwnership,
                    static_cast<int32_t>(id)) != token->mNewKey) return false;
        }
        const int32_t action = MotionEvent::getActionMasked(event.action);
        if (hovering) return token->mBefore.none() ? action == AMOTION_EVENT_ACTION_HOVER_ENTER :
                action == AMOTION_EVENT_ACTION_HOVER_MOVE;
        if (token->mAdded.count() != 1) return false;
        if (token->mBefore.none()) return action == AMOTION_EVENT_ACTION_DOWN;
        return action == AMOTION_EVENT_ACTION_POINTER_DOWN &&
                MotionEvent::getActionIndex(event.action) < event.getPointerCount() &&
                token->mAdded.test(event.pointerProperties[MotionEvent::getActionIndex(event.action)].id);
    }

    android::base::Result<void> bindRebindArrival(DispatchEntry& packet) const {
        const auto& old = packet.externalDesktopRebindArrival;
        if (!old || old->mSource.expired() || old->mDestination.expired() || !old->mSubmission ||
            old->mEvent.get() != packet.eventEntry.get() || old->mTransform != packet.transform ||
            !old->mPhysicalOrigin || old->mPhysicalOrigin != packet.externalDesktopPhysicalMotionOrigin ||
            old->mRawTransform != packet.rawTransform || old->mScale != packet.globalScaleFactor ||
            old->mTargetFlags != packet.targetFlags || old->mFlags != packet.resolvedMotionFlags ||
            old->mUid != packet.targetUid || old->mVsyncId != packet.vsyncId || old->mWindowId != packet.windowId) {
            return android::base::Error() << "Arrival packet no longer matches trusted native projection";
        }
        packet.externalDesktopRebindArrival = ExternalDesktopRebindArrivalPtr(new ExternalDesktopRebindArrival(
                old->mSource, old->mDestination, old->mSubmission, old->mOldKey, old->mNewKey,
                old->mBefore, old->mAdded, old->mBeforeOwners, old->mEvent, old->mPhysicalOrigin, packet.seq,
                old->mOwner, mRevision, packet.transform, packet.rawTransform, packet.globalScaleFactor,
                packet.targetFlags, packet.resolvedMotionFlags, packet.targetUid, packet.vsyncId, packet.windowId));
        return {};
    }

    static bool sameSavedCoordinate(const PointerCoords& a,const PointerCoords& b) {
        const uint32_t count=BitSet64::count(a.bits);
        if(a.bits!=b.bits || a.isResampled!=b.isResampled ||
           count>a.values.size() || count>b.values.size()) return false;
        for(uint32_t i=0;i<count;++i) {
            if(std::bit_cast<uint32_t>(a.values[i])!=std::bit_cast<uint32_t>(b.values[i])) return false;
        }
        return true;
    }
    bool validHoverRetirement(const DispatchEntry& packet,const ExternalDesktopPublishedMotion* realOwner) const {
        const auto& token=packet.externalDesktopHoverRetirement;
        if(!token || token->mConnection.expired() || token->mOwner!=realOwner || token->mRevision!=mRevision ||
           token->mSeq!=packet.seq || token->mEvent.get()!=packet.eventEntry.get() ||
           packet.eventEntry->type!=EventEntry::Type::MOTION || packet.transform!=token->mTransform ||
           packet.rawTransform!=token->mRawTransform || packet.globalScaleFactor!=token->mScale ||
           packet.targetFlags!=token->mTargetFlags || packet.resolvedMotionFlags!=token->mFlags ||
           packet.targetUid!=token->mUid || packet.vsyncId!=token->mVsyncId || packet.windowId!=token->mWindowId) return false;
        const auto& event=static_cast<const MotionEntry&>(*packet.eventEntry);
        const size_t index=find(event,true);
        if(index==kMaxMementos || mSnapshots[index]->liveIds!=token->mBefore) return false;
        const auto& before=*mSnapshots[index];
        Ids eventIds;
        if(!ExternalDesktopPointerOwnership::idsOf(event.pointerProperties,eventIds) ||
           event.pointerProperties.size()!=event.pointerCoords.size() ||
           token->mAfter==token->mBefore || (token->mAfter&~token->mBefore).any() ||
           !token->mRemovedOwners || token->mRemovedOwners->ids()!=(token->mBefore&~token->mAfter)) return false;
        if(token->mAfter.none()) {
            if(event.action!=AMOTION_EVENT_ACTION_HOVER_EXIT || eventIds!=token->mBefore) return false;
        } else if(event.action!=AMOTION_EVENT_ACTION_HOVER_MOVE || eventIds!=token->mAfter) return false;
        const auto sameFloat=[](float a,float b){return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);};
        if(event.deviceId!=before.deviceId || event.source!=before.source || event.displayId!=before.displayId ||
           event.eventTime!=before.eventTime || event.downTime!=before.downTime || event.policyFlags!=before.policyFlags ||
           event.actionButton!=0 || event.metaState!=AMETA_NONE || event.buttonState!=0 ||
           event.classification!=MotionClassification::NONE || event.flags!=before.flags ||
           !sameFloat(event.xPrecision,before.xPrecision) || !sameFloat(event.yPrecision,before.yPrecision) ||
           !sameFloat(event.xCursorPosition,before.xCursorPosition) || !sameFloat(event.yCursorPosition,before.yCursorPosition)) return false;
        for(size_t i=0;i<event.pointerProperties.size();++i){
            const int32_t id=event.pointerProperties[i].id;
            auto old=std::find_if(before.properties.begin(),before.properties.end(),[id](const auto& p){return p.id==id;});
            if(old==before.properties.end()) return false;
            const size_t oldIndex=static_cast<size_t>(old-before.properties.begin());
            if(oldIndex>=before.coords.size() || *old!=event.pointerProperties[i] ||
               !sameSavedCoordinate(before.coords[oldIndex],event.pointerCoords[i])) return false;
        }
        for(size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id){
            if(!token->mBefore.test(id)) continue;
            const auto key=ExternalDesktopPointerOwnership::keyAt(before.ownership,static_cast<int32_t>(id));
            if(key!=ExternalDesktopPointerOwnership::keyAt(token->mBeforeOwners,static_cast<int32_t>(id))) return false;
            if(token->mAfter.test(id)) {
                if(key!=ExternalDesktopPointerOwnership::keyAt(event.externalDesktopPointerOwnership,static_cast<int32_t>(id))) return false;
            } else if(!key || key!=ExternalDesktopPointerOwnership::keyAt(token->mRemovedOwners,static_cast<int32_t>(id))) return false;
        }
        return true;
    }

    // Only Connection may rebind an already-validated queued intent or a new
    // cancellation selected from its successful owner. Ordinary packet fields
    // cannot authorize shrinkage. Rebinding changes NEW packet storage only.
    android::base::Result<void> bindHoverRetirement(DispatchEntry& packet,
            const ExternalDesktopPointerOwnershipPtr& removed,std::weak_ptr<const Connection> connection,
            const ExternalDesktopPublishedMotion& realOwner) const {
        if(connection.expired() || !removed || packet.eventEntry->type!=EventEntry::Type::MOTION) {
            return android::base::Error() << "Missing native hover retirement owner";
        }
        const auto event=std::static_pointer_cast<const MotionEntry>(packet.eventEntry);
        const size_t index=find(*event,true);
        if(index==kMaxMementos) return android::base::Error() << "No successful private hover predecessor";
        const auto& snapshot=*mSnapshots[index];
        const Ids after=snapshot.liveIds&~removed->ids();
        Ids eventIds;
        if((removed->ids()&~snapshot.liveIds).any() || after==snapshot.liveIds ||
           !ExternalDesktopPointerOwnership::idsOf(event->pointerProperties,eventIds) ||
           event->pointerProperties.size()!=event->pointerCoords.size() ||
           (after.any() ? event->action!=AMOTION_EVENT_ACTION_HOVER_MOVE || eventIds!=after :
                          event->action!=AMOTION_EVENT_ACTION_HOVER_EXIT || eventIds!=snapshot.liveIds) ||
           packet.targetFlags!=snapshot.targetFlags || packet.transform!=snapshot.transform ||
           packet.rawTransform!=snapshot.rawTransform || packet.globalScaleFactor!=snapshot.globalScaleFactor ||
           packet.resolvedMotionFlags!=snapshot.flags || packet.targetUid!=snapshot.targetUid ||
           packet.vsyncId!=snapshot.vsyncId || packet.windowId!=snapshot.windowId ||
           event->eventTime!=snapshot.eventTime || event->downTime!=snapshot.downTime) {
            return android::base::Error() << "Hover retirement is not the exact saved projection";
        }
        for(size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id){
            if(removed->ids().test(id) &&
               (!ExternalDesktopPointerOwnership::keyAt(removed,static_cast<int32_t>(id)) ||
                ExternalDesktopPointerOwnership::keyAt(removed,static_cast<int32_t>(id))!=
                ExternalDesktopPointerOwnership::keyAt(snapshot.ownership,static_cast<int32_t>(id)))) {
                return android::base::Error() << "Hover retirement removed a different owner";
            }
        }
        for(size_t i=0;i<event->pointerProperties.size();++i){
            const int32_t id=event->pointerProperties[i].id;
            auto old=std::find_if(snapshot.properties.begin(),snapshot.properties.end(),
                    [id](const auto& p){return p.id==id;});
            if(old==snapshot.properties.end()) return android::base::Error() << "Missing saved survivor";
            const size_t oldIndex=static_cast<size_t>(old-snapshot.properties.begin());
            if(oldIndex>=snapshot.coords.size() || *old!=event->pointerProperties[i] ||
               !sameSavedCoordinate(snapshot.coords[oldIndex],event->pointerCoords[i])) {
                return android::base::Error() << "Hover retirement changed saved survivor geometry";
            }
        }
        std::vector<PointerProperties> live;
        live.reserve(snapshot.liveIds.count());
        for(const auto& property:snapshot.properties) if(snapshot.liveIds.test(property.id)) live.push_back(property);
        auto beforeOwners=ExternalDesktopPointerOwnership::select(snapshot.ownership,live);
        if(!beforeOwners.ok()) return android::base::Error() << beforeOwners.error();
        packet.externalDesktopHoverRetirement=ExternalDesktopHoverRetirementPtr(new ExternalDesktopHoverRetirement(std::move(connection),
                &realOwner,mRevision,snapshot.liveIds,after,std::move(*beforeOwners),removed,event,packet.seq,
                packet.transform,packet.rawTransform,packet.globalScaleFactor,packet.targetFlags,
                packet.resolvedMotionFlags,packet.targetUid,packet.vsyncId,packet.windowId));
        if(!validHoverRetirement(packet,&realOwner)) return android::base::Error() << "Constructed hover capability refused";
        return {};
    }

public:

    bool commit(Prepared&& prepared) {
        if (prepared.mOwner != this || prepared.mRevision != mRevision) return false;
        prepared.mOwner = nullptr; // Single use, including a no-change packet.
        if (prepared.mIndex == kMaxMementos) return true;
        if (prepared.mIndex > kMaxMementos || mRevision == std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        mSnapshots[prepared.mIndex] = std::move(prepared.mSnapshot);
        ++mRevision;
        return true;
    }

    size_t livePointers(const ExternalDesktopPointerStreamKey& key) const {
        size_t count = 0;
        for (const auto& snapshot : mSnapshots) if (snapshot) count += snapshot->idsFor(key).count();
        return count;
    }

    uint64_t revision() const { return mRevision; }

    struct SavedCancellation final {
        std::unique_ptr<DispatchEntry> packet;
        std::shared_ptr<MotionEntry> event;
        std::shared_ptr<const MotionEntry> traceOrigin;
        Ids originalLiveIds;
        ExternalDesktopPointerOwnershipPtr originalOwnership;
        bool hovering;
        ExternalDesktopPointerOwnershipPtr removedHoverOwners;
    };

    // Prepare native packets from retained SUCCESSFUL geometry only. The
    // caller still has to transact queue/replay state, publish, and retain all
    // sent/ACK tails. Nothing here changes this owner or claims drainage.
    android::base::Result<std::vector<SavedCancellation>> prepareCancellations(
            const ExternalDesktopPointerStreamKey& key, const IdGenerator& generator,
            std::span<const ExternalDesktopHoverRetirementPtr> inheritedQueuedRetirements = {},
            Ids selectedIds = Ids{}.set()) const {
        if (!key.valid()) return android::base::Error() << "Invalid native cancellation key";
        std::vector<SavedCancellation> result;
        result.reserve(kMaxMementos * ExternalDesktopPointerOwnership::kMaxIds);
        const auto snapshots = mSnapshots;
        for (const auto& snapshot : snapshots) {
            if (!snapshot) continue;
            Ids selected = snapshot->idsFor(key) & selectedIds;
            if (selected.none()) continue;
            Ids propertiesIds;
            if (!ExternalDesktopPointerOwnership::idsOf(snapshot->properties, propertiesIds) ||
                snapshot->properties.size() != snapshot->coords.size() ||
                (snapshot->liveIds & ~propertiesIds).any() || snapshot->liveIds.none() ||
                !snapshot->ownership || !snapshot->ownership->matches(snapshot->properties) ||
                !snapshot->traceOrigin || snapshot->eventTime < snapshot->downTime) {
                return android::base::Error() << "Incomplete saved successful geometry";
            }
            if(snapshot->hovering) {
                // Only retained intents from the exact ORIGINAL validated
                // queue may be combined. Never infer authorization from a
                // current geometry change or a missing pointer/field.
                for(const auto& intent:inheritedQueuedRetirements){
                    if(!intent || intent->mConnection.expired() || intent->mOwner!=this || !intent->mRemovedOwners) {
                        return android::base::Error() << "Foreign queued hover retirement intent";
                    }
                    const auto& event=*intent->mEvent;
                    if(event.deviceId!=snapshot->deviceId || event.source!=snapshot->source ||
                       (!isMouseOrTouchpad(event.source) && event.displayId!=snapshot->displayId)) continue;
                    for(size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id){
                        if(!intent->mRemovedOwners->ids().test(id)) continue;
                        const auto oldKey=ExternalDesktopPointerOwnership::keyAt(intent->mRemovedOwners,static_cast<int32_t>(id));
                        if(!snapshot->liveIds.test(id) || !oldKey ||
                           oldKey!=ExternalDesktopPointerOwnership::keyAt(snapshot->ownership,static_cast<int32_t>(id))) {
                            return android::base::Error() << "Queued hover retirement changed physical pointer lifetime";
                        }
                        selected.set(id);
                    }
                }
            }
            std::vector<PointerProperties> properties;
            std::vector<PointerCoords> coords;
            properties.reserve(snapshot->liveIds.count());
            coords.reserve(snapshot->liveIds.count());
            for (size_t i = 0; i < snapshot->properties.size(); ++i) {
                if (snapshot->liveIds.test(snapshot->properties[i].id)) {
                    properties.push_back(snapshot->properties[i]);
                    coords.push_back(snapshot->coords[i]);
                }
            }
            ExternalDesktopPointerOwnershipPtr removedHoverOwners;
            if(snapshot->hovering){
                std::vector<PointerProperties> removed;
                removed.reserve(selected.count());
                for(const auto& property:properties) if(selected.test(property.id)) removed.push_back(property);
                auto owners=ExternalDesktopPointerOwnership::select(snapshot->ownership,removed);
                if(!owners.ok() || !*owners) return android::base::Error() << "Missing private removed hover owners";
                removedHoverOwners=std::move(*owners);
            }
            auto emit = [&](int32_t action) -> android::base::Result<void> {
                auto owners = ExternalDesktopPointerOwnership::select(snapshot->ownership, properties);
                if (!owners.ok()) return android::base::Error() << owners.error();
                auto flags = snapshot->flags;
                if (!snapshot->hovering) flags |= MotionFlag::CANCELED;
                auto event = std::make_shared<MotionEntry>(generator.nextId(), nullptr,
                        snapshot->eventTime, snapshot->deviceId, snapshot->source, snapshot->displayId,
                        snapshot->policyFlags, action, 0, flags, AMETA_NONE, 0,
                        MotionClassification::NONE, snapshot->xPrecision, snapshot->yPrecision,
                        snapshot->xCursorPosition, snapshot->yCursorPosition, snapshot->downTime,
                        properties, coords);
                event->externalDesktopPointerOwnership = std::move(*owners);
                auto packet = std::make_unique<DispatchEntry>(event, snapshot->targetFlags,
                        snapshot->transform, snapshot->rawTransform, snapshot->globalScaleFactor,
                        snapshot->targetUid, snapshot->vsyncId, snapshot->windowId);
                packet->resolvedMotionFlags = flags;
                packet->externalDesktopPhysicalMotionOrigin = snapshot->physicalOrigin;
                packet->externalDesktopSubmissionReservation = snapshot->submissionReservation;
                result.push_back({std::move(packet), std::move(event), snapshot->traceOrigin,
                                  snapshot->liveIds, snapshot->ownership, snapshot->hovering,removedHoverOwners});
                return {};
            };
            if (selected == snapshot->liveIds) {
                auto status = emit(snapshot->hovering ? AMOTION_EVENT_ACTION_HOVER_EXIT :
                                                      AMOTION_EVENT_ACTION_CANCEL);
                if (!status.ok()) return android::base::Error() << status.error();
                continue;
            }
            if(snapshot->hovering){
                for(size_t i=properties.size();i-- > 0;){
                    if(!selected.test(properties[i].id)) continue;
                    properties.erase(properties.begin()+static_cast<ptrdiff_t>(i));
                    coords.erase(coords.begin()+static_cast<ptrdiff_t>(i));
                }
                auto status=emit(AMOTION_EVENT_ACTION_HOVER_MOVE);
                if(!status.ok()) return android::base::Error() << status.error();
                continue;
            }
            // Descending current indices keep each full packet's departing
            // pointer and immutable key until that EXACT pointer-up is sent.
            for (size_t i = properties.size(); i-- > 0;) {
                if (!selected.test(properties[i].id)) continue;
                auto status = emit(AMOTION_EVENT_ACTION_POINTER_UP |
                        (static_cast<int32_t>(i) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT));
                if (!status.ok()) return android::base::Error() << status.error();
                properties.erase(properties.begin() + i);
                coords.erase(coords.begin() + i);
            }
        }
        return result;
    }

    template <typename Callback>
    void forEach(const ExternalDesktopPointerStreamKey& key, Callback&& callback) const {
        // Own the complete bounded query view before any callback. Reentrant
        // clear/commit cannot invalidate the callback's value or mix revisions.
        const auto snapshots = mSnapshots;
        for (const auto& snapshot : snapshots) {
            if (!snapshot) continue;
            const Ids ids = snapshot->idsFor(key);
            if (ids.any()) callback(*snapshot, ids);
        }
    }

    void clear() {
        for (auto& snapshot : mSnapshots) snapshot.reset();
        if (mRevision != std::numeric_limits<uint64_t>::max()) ++mRevision;
    }

    static bool packetContains(const DispatchEntry& packet, const ExternalDesktopPointerStreamKey& key) {
        if (!key.valid()) return false;
        if(packet.externalDesktopHoverRetirement && packet.externalDesktopHoverRetirement->references(key)) return true;
        if(packet.externalDesktopRebindArrival && packet.externalDesktopRebindArrival->references(key)) return true;
        if (!packet.externalDesktopPointerOwnership) return false;
        for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
            if (ExternalDesktopPointerOwnership::keyAt(packet.externalDesktopPointerOwnership, static_cast<int32_t>(id)) == key) return true;
        }
        return false;
    }

private:
    friend class InputDispatcherTest;
    size_t find(const MotionEntry& event, bool hovering) const {
        for (size_t i = 0; i < mSnapshots.size(); ++i) {
            const auto& snapshot = mSnapshots[i];
            if (snapshot && snapshot->deviceId == event.deviceId && snapshot->source == event.source &&
                snapshot->hovering == hovering &&
                (isMouseOrTouchpad(event.source) || snapshot->displayId == event.displayId)) return i;
        }
        return kMaxMementos;
    }

    uint64_t mRevision = 1;
    std::array<std::shared_ptr<const Snapshot>, kMaxMementos> mSnapshots{};
};

} // namespace android::inputdispatcher
#endif
