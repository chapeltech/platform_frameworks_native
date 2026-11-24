/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "InputState.h"

#include <binder/IBinder.h>
#include <input/Input.h>
#include <input/InputTransport.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

#include <deque>
#include <memory>
#include <string>

#include "Entry.h"
#include "InputState.h"
#include "ExternalDesktopPublishedMotion.h"

namespace android::inputdispatcher {

struct DispatchEntry;

/* Manages the dispatch state associated with a single input channel. */
class Connection
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
      : public std::enable_shared_from_this<Connection>
#endif
{
public:
    enum class Status {
        // Everything is peachy.
        NORMAL,
        // An unrecoverable communication error has occurred.
        BROKEN,
        // The input channel has been unregistered.
        ZOMBIE,

        ftl_first = NORMAL,
        ftl_last = ZOMBIE,
    };

    Status status;
    // True if this connection is for a focus monitor, which behaves differently in various ways and
    // is usually associated with a display and not with a window.
    bool isFocusMonitor;
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
private:
    // Destruction order is part of the local-FD proof: keep BEFORE publisher.
    ExternalDesktopPublisherDestruction mExternalDesktopPublisherDestruction;
public:
#endif
    InputPublisher inputPublisher;
    InputState inputState;

    // True if this connection is responsive.
    // If this connection is not responsive, avoid publishing more events to it until the
    // application consumes some of the input.
    bool responsive = true;

    // Queue of events that need to be published to the connection.
    std::deque<std::unique_ptr<DispatchEntry>> outboundQueue;

    // Queue of events that have been published to the connection but that have not
    // yet received a "finished" response from the application.
    std::deque<std::unique_ptr<DispatchEntry>> waitQueue;

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    // Whole physical packets only: never split a suspended owned contact out
    // of a recipient's atomic roster. Independent physical streams may pass.
    static constexpr size_t kExternalDesktopMaxSuspendedPackets = 1024;
    std::deque<std::unique_ptr<DispatchEntry>> externalDesktopSuspendedQueue;
    static bool sameExternalDesktopPhysicalStream(const DispatchEntry&, const DispatchEntry&);
    bool hasExternalDesktopSuspendedStream(const DispatchEntry&) const;
    bool deferExternalDesktopFrontPacket();
    void restoreExternalDesktopSuspendedPackets();
    ExternalDesktopPublishedMotion externalDesktopPublishedMotion;
    class ExternalDesktopPreparedSentTail final {
    private:
        friend class Connection;
        friend class InputDispatcher;
        const Connection* owner = nullptr;
        uint64_t revision = 0;
        std::unique_ptr<ExternalDesktopPublisherLifetime::Sent> sent;
    };
    // Preparation owns bounded storage BEFORE a real publisher call. Commit
    // is called only after publish returned zero; WOULD_BLOCK/error drops it.
    android::base::Result<ExternalDesktopPreparedSentTail> prepareExternalDesktopSentTail(
            const DispatchEntry&, std::span<const std::shared_ptr<ExternalDesktopSentTail>>) const;
    bool commitExternalDesktopSentTail(ExternalDesktopPreparedSentTail&&);
    bool acknowledgeExternalDesktopSentTail(const DispatchEntry&);
    struct ExternalDesktopDeliveredNativePacket final {
        std::shared_ptr<const MotionEntry> resolved;
        std::shared_ptr<const MotionEntry> physical;
    };

    class ExternalDesktopPreparedDeliveredMotion final {
    public:
        ExternalDesktopPreparedDeliveredMotion(ExternalDesktopPreparedDeliveredMotion&& other) noexcept
              : mOwner(std::exchange(other.mOwner, nullptr)), mRevision(other.mRevision),
                mRelevant(other.mRelevant), mInvalidate(other.mInvalidate),
                mState(std::move(other.mState)), mStateUnchanged(other.mStateUnchanged),
                mNativeIndex(other.mNativeIndex), mNativePacket(std::move(other.mNativePacket)) {}
        ExternalDesktopPreparedDeliveredMotion(const ExternalDesktopPreparedDeliveredMotion&) = delete;
        ExternalDesktopPreparedDeliveredMotion& operator=(const ExternalDesktopPreparedDeliveredMotion&) = delete;
    private:
        friend class Connection;
        ExternalDesktopPreparedDeliveredMotion(const Connection* owner, uint64_t revision)
              : mOwner(owner), mRevision(revision) {}
        const Connection* mOwner;
        uint64_t mRevision;
        bool mRelevant = false;
        bool mInvalidate = false;
        std::unique_ptr<InputState> mState;
        bool mStateUnchanged = false;
        size_t mNativeIndex = ExternalDesktopPublishedMotion::kMaxMementos;
        ExternalDesktopDeliveredNativePacket mNativePacket;
    };

    // Dispatcher lock required. Includes ordinary/null pointer predecessors,
    // unlike the private-key geometry snapshots. Coverage is relative to THIS
    // native Connection's publications, not proof of a fresh/drained client FD.
    ExternalDesktopPreparedDeliveredMotion prepareExternalDesktopDeliveredMotion(const DispatchEntry& packet) const {
        ExternalDesktopPreparedDeliveredMotion result(this, mExternalDesktopDeliveredMotionRevision);
        if (packet.eventEntry->type != EventEntry::Type::MOTION) return result;
        const auto& event = static_cast<const MotionEntry&>(*packet.eventEntry);
        if (!isFromSource(event.source, AINPUT_SOURCE_CLASS_POINTER)) return result;
        result.mRelevant = true;
        if (!mExternalDesktopDeliveredMotionComplete ||
            mExternalDesktopDeliveredMotionRevision == std::numeric_limits<uint64_t>::max()) {
            result.mInvalidate = true;
            return result;
        }
        // Retain actual successful native packet context, including ordinary
        // peers. Fixed slots are NEVER evicted on UP/timeout/ACK: without a
        // trusted quiescence/teardown proof, overflow makes private admission
        // sticky-unavailable while the ordinary publisher keeps running.
        size_t empty = mExternalDesktopNativePackets.size();
        for (size_t i = 0; i < mExternalDesktopNativePackets.size(); ++i) {
            const auto& prior = mExternalDesktopNativePackets[i].resolved;
            if (!prior) { if (empty == mExternalDesktopNativePackets.size()) empty = i; continue; }
            if (prior->deviceId == event.deviceId && prior->source == event.source &&
                (isMouseOrTouchpad(event.source) || prior->displayId == event.displayId)) {
                result.mNativeIndex = i; break;
            }
        }
        if (result.mNativeIndex == mExternalDesktopNativePackets.size()) result.mNativeIndex = empty;
        if (result.mNativeIndex == mExternalDesktopNativePackets.size()) {
            result.mInvalidate = true;
            return result;
        }
        result.mNativePacket = {std::static_pointer_cast<const MotionEntry>(packet.eventEntry),
                               packet.externalDesktopPhysicalMotionOrigin};
        // The resolver installs a derived MotionEntry before enqueue when its
        // action changes; publishMotionEvent publishes this exact entry.action.
        const int32_t action = MotionEvent::getActionMasked(event.action);
        // Match InputState's motion-state transitions. OUTSIDE/scroll/buttons
        // neither add nor remove contacts here; private delivery geometry stays
        // in ExternalDesktopPublishedMotion, not this replay-roster checkpoint.
        if (action != AMOTION_EVENT_ACTION_DOWN && action != AMOTION_EVENT_ACTION_UP &&
            action != AMOTION_EVENT_ACTION_CANCEL && action != AMOTION_EVENT_ACTION_MOVE &&
            action != AMOTION_EVENT_ACTION_POINTER_DOWN && action != AMOTION_EVENT_ACTION_POINTER_UP &&
            action != AMOTION_EVENT_ACTION_HOVER_ENTER && action != AMOTION_EVENT_ACTION_HOVER_MOVE &&
            action != AMOTION_EVENT_ACTION_HOVER_EXIT) {
            result.mStateUnchanged = true;
            return result;
        }
        auto next = packet.externalDesktopRebindArrival ?
                mExternalDesktopDeliveredMotionState.prepareExternalDesktopRebindArrival(packet) : packet.externalDesktopHoverRetirement ?
                mExternalDesktopDeliveredMotionState.prepareExternalDesktopHoverRetirement(packet) :
                mExternalDesktopDeliveredMotionState.prepareExternalDesktopPublishedMotion(event,packet.resolvedMotionFlags);
        if (!next.ok()) result.mInvalidate = true;
        else result.mState = std::move(*next);
        return result;
    }

    bool commitExternalDesktopDeliveredMotion(ExternalDesktopPreparedDeliveredMotion&& prepared) {
        if (prepared.mOwner != this || prepared.mRevision != mExternalDesktopDeliveredMotionRevision) return false;
        prepared.mOwner = nullptr;
        if (!prepared.mRelevant) return true;
        if (prepared.mInvalidate) mExternalDesktopDeliveredMotionComplete = false;
        else {
            if (prepared.mState) {
                if (!mExternalDesktopDeliveredMotionState.swapExternalDesktopMotionProjection(*prepared.mState)) return false;
            } else if (!prepared.mStateUnchanged) return false;
            mExternalDesktopNativePackets[prepared.mNativeIndex] = std::move(prepared.mNativePacket);
        }
        if (mExternalDesktopDeliveredMotionRevision != std::numeric_limits<uint64_t>::max()) {
            ++mExternalDesktopDeliveredMotionRevision;
        } else mExternalDesktopDeliveredMotionComplete = false;
        return true;
    }

    const InputState* externalDesktopDeliveredMotionForReplay() const {
        // Borrow only under dispatcher lock; copy an owned projection before
        // unlocking. This is motion replay state, NOT a geometry authority.
        // Empty coverage is not fresh-FD or channel-teardown proof.
        return status == Status::NORMAL && mExternalDesktopDeliveredMotionComplete ? &mExternalDesktopDeliveredMotionState : nullptr;
    }
    const ExternalDesktopDeliveredNativePacket* externalDesktopNativePacketForReplay(const MotionEntry& query) const {
        if (!externalDesktopDeliveredMotionForReplay()) return nullptr;
        for (const auto& packet : mExternalDesktopNativePackets) {
            if (packet.resolved && packet.resolved->deviceId == query.deviceId &&
                packet.resolved->source == query.source &&
                (isMouseOrTouchpad(query.source) || packet.resolved->displayId == query.displayId)) return &packet;
        }
        return nullptr;
    }

    void invalidateExternalDesktopDeliveredMotion() {
        mExternalDesktopDeliveredMotionComplete = false;
        mExternalDesktopDeliveredMotionState.clear();
        if (mExternalDesktopDeliveredMotionRevision != std::numeric_limits<uint64_t>::max()) ++mExternalDesktopDeliveredMotionRevision;
    }

    // Prepared cold queue changes. The original entry point still requires
    // CURRENT delivered/live/sent absence (not historical/quiescent proof).
    // The separate saved-cancellation entry point retains successful geometry;
    // neither operation means canceled/drained/claim release at queue commit.
    static constexpr size_t kExternalDesktopMaxPreparedQueuePackets = 1024;
    class ExternalDesktopPreparedUnsentRemoval final {
    public:
        ExternalDesktopPreparedUnsentRemoval(const ExternalDesktopPreparedUnsentRemoval&) = delete;
        ExternalDesktopPreparedUnsentRemoval& operator=(const ExternalDesktopPreparedUnsentRemoval&) = delete;
    private:
        friend class Connection;
        friend class InputDispatcher;
        friend class InputDispatcherTest;
        struct Stamp {
            const DispatchEntry* packet;
            uint32_t seq;
            const EventEntry* event;
            ExternalDesktopPointerOwnershipPtr ownership;
            ExternalDesktopHoverRetirementPtr retirement;
            ExternalDesktopRebindArrivalPtr arrival;
            std::shared_ptr<const MotionEntry> physicalOrigin;
            ExternalDesktopSubmissionReservationPtr submission;
            std::shared_ptr<const ExternalDesktopClosingTail> closingTail;
            ui::Transform transform;
            ui::Transform rawTransform;
            float scale;
            ftl::Flags<MotionFlag> flags;
            ftl::Flags<InputTargetFlags> targetFlags;
            gui::Uid uid;
            int64_t vsyncId;
            std::optional<int32_t> windowId;
            explicit Stamp(const DispatchEntry& value);
            bool matches(const DispatchEntry& value) const;
        };
        struct Derived {
            std::shared_ptr<MotionEntry> event;
            std::shared_ptr<const MotionEntry> original;
            bool transferSynthesized = false;
        };
        ExternalDesktopPreparedUnsentRemoval(const Connection* owner, uint64_t revision,
                                  uint64_t publicationRevision, ExternalDesktopPointerStreamKey key,
                                  bool cancelCurrent)
              : mOwner(owner), mDeliveredRevision(revision), mPublicationRevision(publicationRevision),
                mKey(key), mCancelCurrent(cancelCurrent) {}
        const Connection* mOwner;
        uint64_t mDeliveredRevision;
        uint64_t mPublicationRevision;
        ExternalDesktopPointerStreamKey mKey;
        bool mCancelCurrent;
        std::vector<Stamp> mOriginalOutbound;
        std::vector<Stamp> mOriginalWait;
        // Each null slot is filled by a move of its exact unchanged source at
        // commit. Fresh rewritten packets already own all storage/metadata.
        std::deque<std::unique_ptr<DispatchEntry>> mQueue;
        std::vector<size_t> mReuse;
        std::vector<DispatchEntry*> mFreshPackets;
        std::vector<Derived> mDerived;
        // Exact prepared saved cancellations, distinct from rewritten queued
        // physical samples. A same-Connection rebind inserts its arrival after
        // these packets without mistaking their saved origin for new movement.
        std::vector<const DispatchEntry*> mCancellations;
        std::unique_ptr<InputState> mOriginalEnqueue;
        std::unique_ptr<InputState> mReplay;
        bool mCommitted = false;
    };
    android::base::Result<std::unique_ptr<ExternalDesktopPreparedUnsentRemoval>>
    prepareExternalDesktopUnsentRemoval(const ExternalDesktopPointerStreamKey& key) const;

    // Cold saved-geometry cancellation preparation. Success means QUEUED
    // intent only, not publication/ACK/teardown or lifetime release. Partial
    // mixed-hover retirement and trusted transfer remain explicit OPEN work.
    android::base::Result<std::unique_ptr<ExternalDesktopPreparedUnsentRemoval>>
    prepareExternalDesktopSavedCancellation(const ExternalDesktopPointerStreamKey& key) const;

private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    bool preparedExternalDesktopHoverWithdrawal(const DispatchEntry&, const ExternalDesktopPointerStreamKey&,
                                      int32_t pointerId) const;
    // Dispatcher lock. Release ONLY quiescent replay context after actual
    // delivery/ACK processing; not an input/claim/resource release authority.
    bool pruneExternalDesktopQuiescentReplay();
    android::base::Result<std::unique_ptr<ExternalDesktopPreparedUnsentRemoval>>
    prepareExternalDesktopQueueChange(const ExternalDesktopPointerStreamKey& key, bool cancelCurrent,
                           ExternalDesktopPointerOwnership::Ids selected = ExternalDesktopPointerOwnership::Ids{}.set()) const;
    bool canCommitExternalDesktopUnsentRemoval(const ExternalDesktopPreparedUnsentRemoval& value) const;
    // Dispatcher lock throughout prepare/trace/commit/accounting. Commit does
    // no allocation and never changes waitQueue or successful delivery state.
    bool commitExternalDesktopUnsentRemoval(ExternalDesktopPreparedUnsentRemoval& value);
    // Re-simulate an already owned prospective queue after a trusted rebind
    // has inserted/rebuilt packets. Does not alter any Connection state.
    android::base::Result<void> replayExternalDesktopPreparedQueue(ExternalDesktopPreparedUnsentRemoval& value) const;
    // Used only after BOTH rebind plans passed canCommit under the same mLock.
    void commitExternalDesktopPreparedQueueUnchecked(ExternalDesktopPreparedUnsentRemoval& value) noexcept;

    const IdGenerator& mExternalDesktopIdGenerator;
    InputState mExternalDesktopDeliveredMotionState;
    bool mExternalDesktopDeliveredMotionComplete = true;
    uint64_t mExternalDesktopDeliveredMotionRevision = 1;
    std::array<ExternalDesktopDeliveredNativePacket, ExternalDesktopPublishedMotion::kMaxMementos> mExternalDesktopNativePackets{};
public:

    struct ExternalDesktopStreamReferences {
        bool valid = false;
        size_t queuedPackets = 0;
        size_t sentPackets = 0;
        size_t livePointers = 0;
        bool drained() const { return valid && queuedPackets == 0 && sentPackets == 0 && livePointers == 0; }
    };

    // Dispatcher lock required. Derive references from actual queue ownership;
    // a publication, one ACK or an empty callback cannot stand in for drainage.
    // This is not by itself authority to release a native root/window claim.
    ExternalDesktopStreamReferences externalDesktopStreamReferences(const ExternalDesktopPointerStreamKey& key) const {
        ExternalDesktopStreamReferences result;
        // BROKEN/ZOMBIE is not proof that the client has stopped reading its
        // already-published socket data. Actual registration/channel teardown
        // belongs to the future claim retirement barrier, not this query.
        result.valid = key.valid() && status == Status::NORMAL;
        if (!result.valid) return result;
        for (const auto& packet : outboundQueue) {
            result.queuedPackets += ExternalDesktopPublishedMotion::packetContains(*packet, key);
        }
        for (const auto& packet : externalDesktopSuspendedQueue) {
            result.queuedPackets += ExternalDesktopPublishedMotion::packetContains(*packet, key);
        }
        for (const auto& packet : waitQueue) {
            result.sentPackets += ExternalDesktopPublishedMotion::packetContains(*packet, key);
        }
        result.livePointers = externalDesktopPublishedMotion.livePointers(key);
        return result;
    }
#endif

    Connection(std::unique_ptr<InputChannel> inputChannel, bool isFocusMonitor,
               const IdGenerator& idGenerator);

    inline const std::string& getInputChannelName() const {
        return inputPublisher.getChannel().getName();
    }

    sp<IBinder> getToken() const;
};

} // namespace android::inputdispatcher
