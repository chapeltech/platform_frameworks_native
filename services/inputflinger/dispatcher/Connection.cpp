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

#include "Connection.h"

#include <binder/IBinder.h>
#include <input/Input.h>
#include <input/InputTransport.h>
#include <utils/StrongPointer.h>
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <algorithm>
#include <log/log.h>
#include <tuple>
#endif

namespace android::inputdispatcher {

Connection::Connection(std::unique_ptr<InputChannel> inputChannel, bool isFocusMonitor,
                       const IdGenerator& idGenerator)
      : status(Status::NORMAL),
        isFocusMonitor(isFocusMonitor),
        inputPublisher(std::move(inputChannel)),
        inputState(idGenerator)
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
        , mExternalDesktopIdGenerator(idGenerator), mExternalDesktopDeliveredMotionState(idGenerator)
#endif
        {}

sp<IBinder> Connection::getToken() const {
    return inputPublisher.getChannel().getConnectionToken();
};

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
bool Connection::preparedExternalDesktopHoverWithdrawal(const DispatchEntry& packet,
                                              const ExternalDesktopPointerStreamKey& key,
                                              int32_t pointerId) const {
    const auto& token = packet.externalDesktopHoverRetirement;
    if (pointerId < 0 || pointerId > MAX_POINTER_ID || !key.valid() || !token ||
            token->mConnection.lock().get() != this || !token->mRemovedOwners ||
            !externalDesktopPublishedMotion.validHoverRetirement(packet, &externalDesktopPublishedMotion)) return false;
    // A key may be shared by several pointer IDs. A removed key alone cannot
    // record a withdrawal for a different, still-live row of that same key.
    return token->mRemovedOwners->ids().test(pointerId) &&
            ExternalDesktopPointerOwnership::keyAt(token->mRemovedOwners, pointerId) == key;
}

bool Connection::pruneExternalDesktopQuiescentReplay() {
    const auto& publisher = *mExternalDesktopPublisherDestruction.mState;
    if (status != Status::NORMAL || !mExternalDesktopDeliveredMotionComplete || !publisher.mComplete ||
            publisher.mLocalFdDestroyed.load(std::memory_order_acquire)) return false;
    if (mExternalDesktopDeliveredMotionRevision == std::numeric_limits<uint64_t>::max()) {
        mExternalDesktopDeliveredMotionComplete = false; // never recycle a prepared-view revision
        return false;
    }
    if (outboundQueue.size() > kExternalDesktopMaxPreparedQueuePackets ||
            externalDesktopSuspendedQueue.size() > kExternalDesktopMaxPreparedQueuePackets ||
            waitQueue.size() > kExternalDesktopMaxPreparedQueuePackets) return false;
    const auto sameStream = [](const MotionEntry& a, const MotionEntry& b) {
        return a.deviceId == b.deviceId && a.source == b.source &&
                (isMouseOrTouchpad(a.source) || a.displayId == b.displayId);
    };
    bool changed = false;
    for (auto& cached : mExternalDesktopNativePackets) {
        if (!cached.resolved || !cached.physical) continue;
        // Do not use getPointersOfLastEvent: that public accessor can return
        // no match merely because ownership differs. The actual physical
        // memento lookup includes ordinary/null peers and both hover/touch.
        if (mExternalDesktopDeliveredMotionState.findMotionMemento(*cached.resolved, false) >= 0 ||
                mExternalDesktopDeliveredMotionState.findMotionMemento(*cached.resolved, true) >= 0) continue;
        const auto queued = [&](const auto& queue) {
            return std::any_of(queue.begin(), queue.end(), [&](const auto& entry) {
                if (entry->eventEntry->type != EventEntry::Type::MOTION) return false;
                const auto& resolved = static_cast<const MotionEntry&>(*entry->eventEntry);
                return sameStream(resolved, *cached.resolved) ||
                        (entry->externalDesktopPhysicalMotionOrigin &&
                         sameStream(*entry->externalDesktopPhysicalMotionOrigin, *cached.physical));
            });
        };
        if (queued(outboundQueue) || queued(externalDesktopSuspendedQueue) || queued(waitQueue)) continue;
        // No current client contact or queued/sent packet needs this replay
        // context. Raw/held/cold owners elsewhere remain intact and continue
        // to block InputDispatcher's independent physical claim barrier.
        cached = {};
        changed = true;
    }
    if (changed) ++mExternalDesktopDeliveredMotionRevision;
    return changed;
}

bool Connection::sameExternalDesktopPhysicalStream(const DispatchEntry& a, const DispatchEntry& b) {
    if (a.eventEntry->type != EventEntry::Type::MOTION || b.eventEntry->type != EventEntry::Type::MOTION) return false;
    const auto& x = a.externalDesktopPhysicalMotionOrigin ? *a.externalDesktopPhysicalMotionOrigin :
            static_cast<const MotionEntry&>(*a.eventEntry);
    const auto& y = b.externalDesktopPhysicalMotionOrigin ? *b.externalDesktopPhysicalMotionOrigin :
            static_cast<const MotionEntry&>(*b.eventEntry);
    // Match actual InputState's physical client-stream identity. Its mouse /
    // touchpad stream spans display changes, so those cannot bypass each other.
    return x.deviceId == y.deviceId && x.source == y.source &&
            (isMouseOrTouchpad(x.source) || x.displayId == y.displayId);
}

bool Connection::hasExternalDesktopSuspendedStream(const DispatchEntry& packet) const {
    return std::any_of(externalDesktopSuspendedQueue.begin(), externalDesktopSuspendedQueue.end(),
            [&](const auto& prior) { return sameExternalDesktopPhysicalStream(*prior, packet); });
}

bool Connection::deferExternalDesktopFrontPacket() {
    if (outboundQueue.empty() || externalDesktopSuspendedQueue.size() >= kExternalDesktopMaxSuspendedPackets) return false;
    externalDesktopSuspendedQueue.push_back(std::move(outboundQueue.front()));
    outboundQueue.pop_front();
    return true; // exact object/seq/roster/action/foreground reference retained
}

void Connection::restoreExternalDesktopSuspendedPackets() {
    while (!externalDesktopSuspendedQueue.empty()) {
        outboundQueue.push_front(std::move(externalDesktopSuspendedQueue.back()));
        externalDesktopSuspendedQueue.pop_back();
    }
}

android::base::Result<Connection::ExternalDesktopPreparedSentTail> Connection::prepareExternalDesktopSentTail(
        const DispatchEntry& packet, std::span<const std::shared_ptr<ExternalDesktopSentTail>> tails) const {
    ExternalDesktopPreparedSentTail result;
    result.owner = this;
    auto& state = *mExternalDesktopPublisherDestruction.mState;
    result.revision = state.mRevision;
    if (tails.empty()) return result; // ordinary path allocates no journal entry
    if (!state.mComplete || state.mLocalFdDestroyed.load(std::memory_order_acquire) ||
            !packet.seq || state.mRevision == std::numeric_limits<uint64_t>::max() ||
            state.mSent.size() >= ExternalDesktopPublisherLifetime::kMaxSent || tails.size() > 256) {
        return android::base::Error() << "Bounded successful-send journal unavailable";
    }
    for (const auto& prior : state.mSent) if (prior->seq == packet.seq) {
        return android::base::Error() << "Sequence still belongs to an unacknowledged send";
    }
    result.sent = std::make_unique<ExternalDesktopPublisherLifetime::Sent>();
    result.sent->seq = packet.seq;
    result.sent->packet = &packet;
    result.sent->event = packet.eventEntry.get();
    result.sent->tails.reserve(tails.size());
    for (const auto& tail : tails) {
        if (!tail || tail->mSent == std::numeric_limits<size_t>::max() ||
                std::find(result.sent->tails.begin(), result.sent->tails.end(), tail) != result.sent->tails.end()) {
            return android::base::Error() << "Duplicate/unrepresentable successful-send referent";
        }
        result.sent->tails.push_back(tail);
    }
    state.mSent.reserve(state.mSent.size() + 1); // no allocation after publish succeeds
    return result;
}

bool Connection::commitExternalDesktopSentTail(ExternalDesktopPreparedSentTail&& prepared) {
    auto& state = *mExternalDesktopPublisherDestruction.mState;
    if (prepared.owner != this || prepared.revision != state.mRevision) return false;
    prepared.owner = nullptr;
    if (!prepared.sent) return true;
    if (!state.mComplete || state.mLocalFdDestroyed.load(std::memory_order_acquire) ||
            state.mRevision == std::numeric_limits<uint64_t>::max() ||
            state.mSent.size() >= state.mSent.capacity() ||
            prepared.sent->hoverWithdrawals.size() > prepared.sent->tails.size()) return false;
    for (const auto* withdrawn : prepared.sent->hoverWithdrawals) {
        if (!withdrawn || std::count_if(prepared.sent->tails.begin(), prepared.sent->tails.end(),
                [&](const auto& tail) { return tail.get() == withdrawn; }) != 1 ||
                std::count(prepared.sent->hoverWithdrawals.begin(), prepared.sent->hoverWithdrawals.end(),
                           withdrawn) != 1) return false;
    }
    for (const auto& tail : prepared.sent->tails) ++tail->mSent;
    for (auto* withdrawn : prepared.sent->hoverWithdrawals) withdrawn->mHoverWithdrawalPublished = true;
    state.mSent.push_back(std::move(prepared.sent));
    ++state.mRevision;
    return true;
}

bool Connection::acknowledgeExternalDesktopSentTail(const DispatchEntry& packet) {
    auto& state = *mExternalDesktopPublisherDestruction.mState;
    const auto found = std::find_if(state.mSent.begin(), state.mSent.end(),
            [&](const auto& sent) { return sent->seq == packet.seq; });
    if (found == state.mSent.end()) return true; // untracked ordinary packet/duplicate ACK
    const auto& sent = **found;
    if (sent.packet != &packet || sent.event != packet.eventEntry.get() ||
            !state.mComplete || state.mRevision == std::numeric_limits<uint64_t>::max()) {
        state.mComplete = false;
        return false; // mismatched identity never consumes old transport evidence
    }
    for (const auto& tail : sent.tails) if (!tail->mSent) {
        state.mComplete = false;
        return false;
    }
    if (sent.hoverWithdrawals.size() > sent.tails.size()) {
        state.mComplete = false;
        return false;
    }
    for (const auto* withdrawn : sent.hoverWithdrawals) {
        if (!withdrawn ||
                std::count_if(sent.tails.begin(), sent.tails.end(),
                    [&](const auto& tail) { return tail.get() == withdrawn; }) != 1 ||
                std::count(sent.hoverWithdrawals.begin(), sent.hoverWithdrawals.end(), withdrawn) != 1 ||
                !withdrawn->mHoverWithdrawalPublished) {
            state.mComplete = false;
            return false;
        }
    }
    for (const auto& tail : sent.tails) --tail->mSent;
    for (auto* withdrawn : sent.hoverWithdrawals) withdrawn->mHoverWithdrawalAcknowledged = true;
    state.mSent.erase(found);
    ++state.mRevision;
    return true;
}

Connection::ExternalDesktopPreparedUnsentRemoval::Stamp::Stamp(const DispatchEntry& value)
      : packet(&value), seq(value.seq), event(value.eventEntry.get()),
        ownership(value.externalDesktopPointerOwnership), retirement(value.externalDesktopHoverRetirement), arrival(value.externalDesktopRebindArrival),
        physicalOrigin(value.externalDesktopPhysicalMotionOrigin), submission(value.externalDesktopSubmissionReservation),
        closingTail(value.externalDesktopClosingTail), transform(value.transform),
        rawTransform(value.rawTransform), scale(value.globalScaleFactor),
        flags(value.resolvedMotionFlags), targetFlags(value.targetFlags), uid(value.targetUid), vsyncId(value.vsyncId),
        windowId(value.windowId) {}

bool Connection::ExternalDesktopPreparedUnsentRemoval::Stamp::matches(const DispatchEntry& value) const {
    return packet == &value && seq == value.seq && event == value.eventEntry.get() &&
            ownership == value.externalDesktopPointerOwnership && retirement == value.externalDesktopHoverRetirement && arrival == value.externalDesktopRebindArrival &&
            physicalOrigin == value.externalDesktopPhysicalMotionOrigin && submission == value.externalDesktopSubmissionReservation &&
            closingTail == value.externalDesktopClosingTail && transform == value.transform &&
            rawTransform == value.rawTransform && scale == value.globalScaleFactor &&
            flags == value.resolvedMotionFlags && targetFlags == value.targetFlags && uid == value.targetUid &&
            vsyncId == value.vsyncId && windowId == value.windowId;
}

android::base::Result<std::unique_ptr<Connection::ExternalDesktopPreparedUnsentRemoval>>
Connection::prepareExternalDesktopUnsentRemoval(const ExternalDesktopPointerStreamKey& key) const {
    return prepareExternalDesktopQueueChange(key, /*cancelCurrent=*/false);
}

android::base::Result<std::unique_ptr<Connection::ExternalDesktopPreparedUnsentRemoval>>
Connection::prepareExternalDesktopSavedCancellation(const ExternalDesktopPointerStreamKey& key) const {
    return prepareExternalDesktopQueueChange(key, /*cancelCurrent=*/true);
}

android::base::Result<std::unique_ptr<Connection::ExternalDesktopPreparedUnsentRemoval>>
Connection::prepareExternalDesktopQueueChange(const ExternalDesktopPointerStreamKey& key, bool cancelCurrent,
                                    ExternalDesktopPointerOwnership::Ids selectedIds) const {
    const InputState* delivered = externalDesktopDeliveredMotionForReplay();
    if (!key.valid() || !delivered || (!cancelCurrent &&
        (delivered->hasExternalDesktopStream(key) || externalDesktopPublishedMotion.livePointers(key) != 0)) ||
        mExternalDesktopDeliveredMotionRevision == std::numeric_limits<uint64_t>::max()) {
        return android::base::Error() << "Native queue change lacks complete successful checkpoint";
    }
    if (outboundQueue.size() > kExternalDesktopMaxPreparedQueuePackets ||
        waitQueue.size() > kExternalDesktopMaxPreparedQueuePackets) {
        return android::base::Error() << "Bounded private queue preparation unavailable";
    }
    auto result = std::unique_ptr<ExternalDesktopPreparedUnsentRemoval>(
            new ExternalDesktopPreparedUnsentRemoval(this, mExternalDesktopDeliveredMotionRevision,
                    externalDesktopPublishedMotion.revision(), key, cancelCurrent));
    result->mOriginalOutbound.reserve(outboundQueue.size());
    result->mOriginalWait.reserve(waitQueue.size());
    constexpr size_t maxCanceledPackets = ExternalDesktopPublishedMotion::kMaxMementos * ExternalDesktopPointerOwnership::kMaxIds;
    const size_t preparedCapacity = outboundQueue.size() + (cancelCurrent ? maxCanceledPackets : 0);
    result->mReuse.reserve(preparedCapacity);
    result->mFreshPackets.reserve(preparedCapacity);
    result->mDerived.reserve(preparedCapacity);
    for (const auto& packet : waitQueue) {
        if (!packet || (!cancelCurrent && ExternalDesktopPublishedMotion::packetContains(*packet, key))) {
            return android::base::Error() << "Sent ownership requires published cancellation, never unsent erasure";
        }
        result->mOriginalWait.emplace_back(*packet);
    }
    auto original = delivered->copyExternalDesktopMotionProjection();
    auto replay = delivered->copyExternalDesktopMotionProjection();
    auto enqueue = inputState.copyExternalDesktopMotionProjection();
    if (!original.ok() || !replay.ok() || !enqueue.ok()) {
        return android::base::Error() << "Sticky delivered coverage unavailable for queue replay";
    }
    result->mOriginalEnqueue = std::move(*enqueue);
    result->mReplay = std::move(*replay);
    auto futurePublication = externalDesktopPublishedMotion;
    struct Physical {
        DeviceId device;
        uint32_t source;
        ui::LogicalDisplayId display;
        bool matches(const MotionEntry& event) const {
            return device == event.deviceId && source == event.source &&
                    (isMouseOrTouchpad(source) || display == event.displayId);
        }
    };
    // Validate the ENTIRE original queue before inheriting any prior private
    // retirement intent. The capability is never trusted merely because a
    // field is present; both actual owner classes must replay it successfully.
    auto originalPublication=externalDesktopPublishedMotion;
    std::vector<ExternalDesktopHoverRetirementPtr> inheritedRetirements;
    inheritedRetirements.reserve(outboundQueue.size());
    for(const auto& packet:outboundQueue){
        if(!packet) return android::base::Error() << "Missing original queue owner";
        if (packet->externalDesktopRebindArrival && packet->externalDesktopRebindArrival->references(key)) {
            return android::base::Error() << "Queued trusted handoff still owns this old/new key tail";
        }
        if(packet->eventEntry->type==EventEntry::Type::MOTION){
            const auto& event=static_cast<const MotionEntry&>(*packet->eventEntry);
            ExternalDesktopPointerOwnership::Ids ids;
            const int32_t action=MotionEvent::getActionMasked(event.action);
            if(!ExternalDesktopPointerOwnership::idsOf(event.pointerProperties,ids) ||
               event.pointerProperties.size()!=event.pointerCoords.size() ||
               ((action==AMOTION_EVENT_ACTION_DOWN || action==AMOTION_EVENT_ACTION_UP) && event.getPointerCount()!=1) ||
               ((action==AMOTION_EVENT_ACTION_POINTER_DOWN || action==AMOTION_EVENT_ACTION_POINTER_UP) &&
                (event.getPointerCount()<2 || MotionEvent::getActionIndex(event.action)>=event.getPointerCount())) ||
               packet->externalDesktopPointerOwnership!=event.externalDesktopPointerOwnership) {
                return android::base::Error() << "Original motion shape/ownership differs";
            }
        }
        auto publication=originalPublication.prepareForReplay(*packet,externalDesktopPublishedMotion);
        if(!publication.ok()) return android::base::Error() << "Original publication intent is not valid";
        if(packet->eventEntry->type==EventEntry::Type::MOTION){
            const auto& event=static_cast<const MotionEntry&>(*packet->eventEntry);
            auto next=packet->externalDesktopRebindArrival ? (*original)->prepareExternalDesktopRebindArrival(*packet) :
                    packet->externalDesktopHoverRetirement ? (*original)->prepareExternalDesktopHoverRetirement(*packet) :
                    (*original)->prepareExternalDesktopPublishedMotion(event,packet->resolvedMotionFlags);
            if(!next.ok()) return android::base::Error() << "Original motion replay is not complete";
            original=std::move(next);
        }
        if(!originalPublication.commit(std::move(*publication))) return android::base::Error() << "Original publication preparation changed";
        if(packet->externalDesktopHoverRetirement) inheritedRetirements.push_back(packet->externalDesktopHoverRetirement);
    }
    if(!(*original)->sameExternalDesktopMotionProjection(*result->mOriginalEnqueue)) {
        return android::base::Error() << "Original delivered plus queued motion does not cover advanced state";
    }
    std::vector<Physical> affected;
    affected.reserve(InputState::kExternalDesktopMaxPublishedMotionMementos);
    for (const auto& packet : outboundQueue) {
        if (!packet) return android::base::Error() << "Missing outbound queue owner";
        result->mOriginalOutbound.emplace_back(*packet);
        if (!ExternalDesktopPublishedMotion::packetContains(*packet, key)) continue;
        if (packet->eventEntry->type != EventEntry::Type::MOTION) {
            return android::base::Error() << "Private ownership on non-motion packet";
        }
        const auto& event = static_cast<const MotionEntry&>(*packet->eventEntry);
        if (std::none_of(affected.begin(), affected.end(), [&](const auto& value) { return value.matches(event); })) {
            if (affected.size() == InputState::kExternalDesktopMaxPublishedMotionMementos) {
                return android::base::Error() << "Bounded affected physical containers unavailable";
            }
            affected.push_back({event.deviceId, event.source, event.displayId});
        }
    }

    // One cold transaction uses the REAL publication/replay classes. All
    // allocations happen here, before any queue/input-state mutation.
    auto appendFuture = [&](std::unique_ptr<DispatchEntry> replacement, size_t sourceIndex,
                            const DispatchEntry* originalPacket,
                            const ExternalDesktopPointerOwnershipPtr& retirementIntent) -> android::base::Result<void> {
        if(retirementIntent){
            const auto self=weak_from_this();
            if(!replacement || self.expired() || self.lock().get()!=this) {
                return android::base::Error() << "Missing exact shared native connection capability";
            }
            auto bound=futurePublication.bindHoverRetirement(*replacement,retirementIntent,self,externalDesktopPublishedMotion);
            if(!bound.ok()) return android::base::Error() << bound.error();
        }
        const auto arrival = originalPacket ? originalPacket->externalDesktopRebindArrival : ExternalDesktopRebindArrivalPtr{};
        if (arrival) {
            if (!replacement) {
                replacement=std::make_unique<DispatchEntry>(originalPacket->eventEntry,originalPacket->targetFlags,
                        originalPacket->transform,originalPacket->rawTransform,originalPacket->globalScaleFactor,
                        originalPacket->targetUid,originalPacket->vsyncId,originalPacket->windowId);
                replacement->resolvedMotionFlags=originalPacket->resolvedMotionFlags;
                replacement->externalDesktopPointerOwnership=originalPacket->externalDesktopPointerOwnership;
                replacement->externalDesktopPhysicalMotionOrigin=originalPacket->externalDesktopPhysicalMotionOrigin;
                replacement->externalDesktopSubmissionReservation=originalPacket->externalDesktopSubmissionReservation;
            }
            replacement->externalDesktopRebindArrival=arrival;
            auto bound=futurePublication.bindRebindArrival(*replacement);
            if (!bound.ok()) return android::base::Error() << bound.error();
        }
        const DispatchEntry& candidate = replacement ? *replacement : *originalPacket;
        auto publication = futurePublication.prepareForReplay(candidate,externalDesktopPublishedMotion);
        if(!publication.ok()) return android::base::Error() << "Changed publication intent is not valid";
        if (candidate.eventEntry->type == EventEntry::Type::MOTION) {
            const auto& motion = static_cast<const MotionEntry&>(*candidate.eventEntry);
            auto next = candidate.externalDesktopRebindArrival ? result->mReplay->prepareExternalDesktopRebindArrival(candidate) :
                    candidate.externalDesktopHoverRetirement ? result->mReplay->prepareExternalDesktopHoverRetirement(candidate) :
                    result->mReplay->prepareExternalDesktopPublishedMotion(motion, candidate.resolvedMotionFlags);
            if (!next.ok()) return android::base::Error() << "Changed queue cannot replay through actual InputState";
            result->mReplay = std::move(*next);
        }
        if (!futurePublication.commit(std::move(*publication))) {
            return android::base::Error() << "Changed queue lacks representable future publication state";
        }
        if (replacement) {
            result->mFreshPackets.push_back(replacement.get());
            result->mQueue.push_back(std::move(replacement));
            result->mReuse.push_back(kExternalDesktopMaxPreparedQueuePackets);
        } else {
            result->mQueue.push_back(nullptr);
            result->mReuse.push_back(sourceIndex);
        }
        return {};
    };
    struct PendingCancellation {
        size_t beforeOriginalIndex;
        ExternalDesktopPublishedMotion::SavedCancellation saved;
    };
    std::vector<PendingCancellation> pending;
    if (cancelCurrent) {
        auto cancellations = externalDesktopPublishedMotion.prepareCancellations(key, mExternalDesktopIdGenerator,
                std::span<const ExternalDesktopHoverRetirementPtr>(inheritedRetirements), selectedIds);
        if (!cancellations.ok()) return android::base::Error() << cancellations.error();
        if (delivered->hasExternalDesktopStream(key) != !cancellations->empty()) {
            return android::base::Error() << "Saved private geometry and successful roster disagree";
        }
        pending.reserve(cancellations->size());
        for (auto& saved : *cancellations) {
            ExternalDesktopPointerOwnershipPtr deliveredOwners;
            const auto deliveredPointers = delivered->getPointersOfLastEvent(
                    *saved.traceOrigin, saved.hovering, &deliveredOwners);
            ExternalDesktopPointerOwnership::Ids deliveredIds;
            if (!deliveredPointers ||
                !ExternalDesktopPointerOwnership::idsOf(deliveredPointers->first, deliveredIds) ||
                deliveredIds != saved.originalLiveIds) {
                return android::base::Error() << "Saved successful cancellation roster differs from delivered checkpoint";
            }
            for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
                if (deliveredIds.test(id) &&
                    ExternalDesktopPointerOwnership::keyAt(deliveredOwners, static_cast<int32_t>(id)) !=
                    ExternalDesktopPointerOwnership::keyAt(saved.originalOwnership, static_cast<int32_t>(id))) {
                    return android::base::Error() << "Saved successful cancellation changed a pointer owner";
                }
            }
            const Physical physical{saved.event->deviceId, saved.event->source, saved.event->displayId};
            size_t anchor = outboundQueue.size();
            for (size_t i = 0; i < outboundQueue.size(); ++i) {
                if (outboundQueue[i]->eventEntry->type != EventEntry::Type::MOTION) continue;
                const auto& event = static_cast<const MotionEntry&>(*outboundQueue[i]->eventEntry);
                if (!physical.matches(event)) continue;
                if (event.eventTime < saved.event->eventTime) {
                    return android::base::Error() << "Cannot insert saved cancellation before an older physical timestamp";
                }
                anchor = std::min(anchor, i);
            }
            pending.push_back({anchor, std::move(saved)});
        }
        std::stable_sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) {
            if (a.beforeOriginalIndex != b.beforeOriginalIndex) return a.beforeOriginalIndex < b.beforeOriginalIndex;
            return a.saved.event->eventTime < b.saved.event->eventTime;
        });
    }
    auto appendCancellations = [&](size_t index) -> android::base::Result<void> {
        for (auto& value : pending) {
            if (value.beforeOriginalIndex != index) continue;
            result->mCancellations.push_back(value.saved.packet.get());
            result->mDerived.push_back({value.saved.event, value.saved.traceOrigin});
            auto appendStatus = appendFuture(std::move(value.saved.packet),
                    kExternalDesktopMaxPreparedQueuePackets, nullptr,value.saved.removedHoverOwners);
            if (!appendStatus.ok()) return android::base::Error() << appendStatus.error();
        }
        return {};
    };
    for (size_t index = 0; index < outboundQueue.size(); ++index) {
        auto cancellationStatus = appendCancellations(index);
        if (!cancellationStatus.ok()) return android::base::Error() << cancellationStatus.error();
        const DispatchEntry& packet = *outboundQueue[index];
        std::unique_ptr<DispatchEntry> replacement;
        bool remove = false;
        ExternalDesktopPointerOwnershipPtr retainedHoverIntent;
        if(packet.externalDesktopHoverRetirement){
            const auto& intent=packet.externalDesktopHoverRetirement;
            bool absorbed=false;
            for(const auto& next:pending){
                if(!next.saved.hovering || !next.saved.removedHoverOwners) continue;
                const auto& event=*intent->mEvent;
                const Physical physical{next.saved.event->deviceId,next.saved.event->source,next.saved.event->displayId};
                if(!physical.matches(event)) continue;
                bool contained=true;
                for(size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id){
                    if(intent->mRemovedOwners->ids().test(id) &&
                       ExternalDesktopPointerOwnership::keyAt(intent->mRemovedOwners,static_cast<int32_t>(id))!=
                       ExternalDesktopPointerOwnership::keyAt(next.saved.removedHoverOwners,static_cast<int32_t>(id))) contained=false;
                }
                absorbed|=contained;
            }
            if(absorbed) continue; // replaced by one combined authorized intent; tails move atomically.
            // Even an unrelated old intent needs a fresh expected revision if
            // an earlier cancellation changed the predicted publication order.
            replacement=std::make_unique<DispatchEntry>(packet.eventEntry,packet.targetFlags,
                    packet.transform,packet.rawTransform,packet.globalScaleFactor,
                    packet.targetUid,packet.vsyncId,packet.windowId);
            replacement->resolvedMotionFlags=packet.resolvedMotionFlags;
            replacement->externalDesktopPointerOwnership=packet.externalDesktopPointerOwnership;
            replacement->externalDesktopPhysicalMotionOrigin=packet.externalDesktopPhysicalMotionOrigin;
            replacement->externalDesktopSubmissionReservation=packet.externalDesktopSubmissionReservation;
            retainedHoverIntent=intent->mRemovedOwners;
        }
        if (packet.eventEntry->type != EventEntry::Type::MOTION && packet.externalDesktopPointerOwnership) {
            return android::base::Error() << "Private ownership on non-motion packet";
        }
        if (packet.eventEntry->type == EventEntry::Type::MOTION) {
            const auto origin = std::static_pointer_cast<const MotionEntry>(packet.eventEntry);
            const MotionEntry& event = *origin;
            ExternalDesktopPointerOwnership::Ids allIds;
            const int32_t originalAction = MotionEvent::getActionMasked(event.action);
            if (!ExternalDesktopPointerOwnership::idsOf(event.pointerProperties, allIds) ||
                event.pointerProperties.size() != event.pointerCoords.size() ||
                ((originalAction == AMOTION_EVENT_ACTION_DOWN || originalAction == AMOTION_EVENT_ACTION_UP) &&
                 event.getPointerCount() != 1) ||
                ((originalAction == AMOTION_EVENT_ACTION_POINTER_DOWN || originalAction == AMOTION_EVENT_ACTION_POINTER_UP) &&
                 (event.getPointerCount() < 2 || MotionEvent::getActionIndex(event.action) >= event.getPointerCount()))) {
                return android::base::Error() << "Malformed motion before stock replay/split";
            }
            if (packet.externalDesktopPointerOwnership != event.externalDesktopPointerOwnership) {
                return android::base::Error() << "Packet/raw motion ownership mismatch";
            }
            const bool selected = ExternalDesktopPublishedMotion::packetContains(packet, key);
            const bool affectedContact = std::any_of(affected.begin(), affected.end(),
                    [&](const auto& value) { return value.matches(event); });
            int32_t action = event.action;
            nsecs_t downTime = event.downTime;
            std::vector<PointerProperties> properties;
            std::vector<PointerCoords> coords;
            if (selected) {
                ExternalDesktopPointerOwnership::Ids keep;
                if (!event.externalDesktopPointerOwnership ||
                    !event.externalDesktopPointerOwnership->matches(event.pointerProperties) ||
                    event.pointerProperties.size() != event.pointerCoords.size()) {
                    return android::base::Error() << "Malformed private queued pointer roster";
                }
                for (const auto& property : event.pointerProperties) {
                    if (!selectedIds.test(property.id) ||
                        ExternalDesktopPointerOwnership::keyAt(event.externalDesktopPointerOwnership, property.id) != key) keep.set(property.id);
                }
                remove = keep.none();
                if (!remove) {
                    // Use stock action/index/CANCELED splitting, not a second
                    // gesture state machine or manually patched raw packet.
                    auto split = MotionEvent::split(event.action, event.flags, 0,
                            event.pointerProperties, event.pointerCoords, keep,
                            [&]() { return event.getDescription(); });
                    if (!split.ok()) return android::base::Error() << "Unrepresentable unsent pointer split";
                    action = std::get<0>(*split);
                    properties = std::move(std::get<1>(*split));
                    coords = std::move(std::get<2>(*split));
                }
            }
            const int32_t masked = MotionEvent::getActionMasked(action);
            const bool hover = masked == AMOTION_EVENT_ACTION_HOVER_ENTER ||
                    masked == AMOTION_EVENT_ACTION_HOVER_MOVE || masked == AMOTION_EVENT_ACTION_HOVER_EXIT;
            if (!remove && affectedContact && !hover) {
                if (masked == AMOTION_EVENT_ACTION_DOWN) downTime = event.eventTime;
                else if (auto previous = result->mReplay->externalDesktopMotionDownTime(event)) downTime = *previous;
            }
            if (!remove && (selected || downTime != event.downTime)) {
                if (!selected) { properties = event.pointerProperties; coords = event.pointerCoords; }
                auto ownership = ExternalDesktopPointerOwnership::select(event.externalDesktopPointerOwnership, properties);
                if (!ownership.ok()) return android::base::Error() << "Unsent ownership projection failed";
                auto copy = std::make_shared<MotionEntry>(mExternalDesktopIdGenerator.nextId(), event.injectionState,
                        event.eventTime, event.deviceId, event.source, event.displayId, event.policyFlags,
                        action, event.actionButton, event.flags, event.metaState, event.buttonState,
                        event.classification, event.xPrecision, event.yPrecision, event.xCursorPosition,
                        event.yCursorPosition, downTime, properties, coords);
                copy->externalDesktopPointerOwnership = std::move(*ownership);
                replacement = std::make_unique<DispatchEntry>(copy, packet.targetFlags,
                        packet.transform, packet.rawTransform, packet.globalScaleFactor,
                        packet.targetUid, packet.vsyncId, packet.windowId);
                replacement->resolvedMotionFlags = packet.resolvedMotionFlags;
                replacement->externalDesktopPhysicalMotionOrigin = packet.externalDesktopPhysicalMotionOrigin;
                replacement->externalDesktopSubmissionReservation = packet.externalDesktopSubmissionReservation;
                result->mDerived.push_back({std::move(copy), origin});
            }
        }
        if (remove) continue;
        auto appendStatus = appendFuture(std::move(replacement), index, &packet,retainedHoverIntent);
        if (!appendStatus.ok()) return android::base::Error() << appendStatus.error();
    }
    auto cancellationStatus = appendCancellations(outboundQueue.size());
    if (!cancellationStatus.ok()) return android::base::Error() << cancellationStatus.error();
    if (!(*original)->sameExternalDesktopMotionProjection(*result->mOriginalEnqueue)) {
        // A missing queued predecessor, transfer-only state or lost tracking
        // cannot be silently replaced by an apparently empty replay. Keep all
        // history untouched; this substep does not manufacture quiescence.
        return android::base::Error() << "Original delivered-plus-unsent queue does not cover enqueue state";
    }
    return result;
}

bool Connection::canCommitExternalDesktopUnsentRemoval(const ExternalDesktopPreparedUnsentRemoval& value) const {
    if (value.mOwner != this || value.mCommitted || !value.mReplay ||
        !value.mOriginalEnqueue || !inputState.sameExternalDesktopMotionProjection(*value.mOriginalEnqueue) ||
        value.mDeliveredRevision != mExternalDesktopDeliveredMotionRevision ||
        value.mPublicationRevision != externalDesktopPublishedMotion.revision() ||
        !externalDesktopDeliveredMotionForReplay() || (!value.mCancelCurrent &&
        (mExternalDesktopDeliveredMotionState.hasExternalDesktopStream(value.mKey) || externalDesktopPublishedMotion.livePointers(value.mKey) != 0)) ||
        value.mOriginalOutbound.size() != outboundQueue.size() ||
        value.mOriginalWait.size() != waitQueue.size()) return false;
    for (size_t i = 0; i < outboundQueue.size(); ++i) {
        if (!outboundQueue[i] || !value.mOriginalOutbound[i].matches(*outboundQueue[i])) return false;
    }
    for (size_t i = 0; i < waitQueue.size(); ++i) {
        if (!waitQueue[i] || !value.mOriginalWait[i].matches(*waitQueue[i])) return false;
    }
    return true;
}

bool Connection::commitExternalDesktopUnsentRemoval(ExternalDesktopPreparedUnsentRemoval& value) {
    if (!canCommitExternalDesktopUnsentRemoval(value)) return false;
    commitExternalDesktopPreparedQueueUnchecked(value);
    return true;
}

void Connection::commitExternalDesktopPreparedQueueUnchecked(ExternalDesktopPreparedUnsentRemoval& value) noexcept {
    // Same IdGenerator was checked by successful prepare/replay. Swapping
    // vectors/deques/unique_ptrs here cannot allocate or have a partial refusal.
    const bool swapped = inputState.swapExternalDesktopMotionProjection(*value.mReplay);
    LOG_ALWAYS_FATAL_IF(!swapped, "Native prepared motion owner changed under dispatcher lock");
    for (size_t i = 0; i < value.mReuse.size(); ++i) {
        if (value.mReuse[i] != kExternalDesktopMaxPreparedQueuePackets) {
            value.mQueue[i] = std::move(outboundQueue[value.mReuse[i]]);
        }
    }
    outboundQueue.swap(value.mQueue);
    value.mOwner = nullptr;
    value.mCommitted = true;
    // value.mQueue now owns ONLY retired originals (plus moved-out null slots).
    // Dispatcher must increment replacement injection references before using
    // its real releaseDispatchEntry on each retired original, under the lock.
}

android::base::Result<void> Connection::replayExternalDesktopPreparedQueue(
        ExternalDesktopPreparedUnsentRemoval& value) const {
    const auto* delivered = externalDesktopDeliveredMotionForReplay();
    if (!delivered || value.mOwner != this || value.mQueue.size() != value.mReuse.size() ||
        value.mQueue.size() > kExternalDesktopMaxPreparedQueuePackets) {
        return android::base::Error() << "Incomplete bounded destination queue plan";
    }
    auto replay = delivered->copyExternalDesktopMotionProjection();
    if (!replay.ok()) return android::base::Error() << replay.error();
    auto publication = externalDesktopPublishedMotion;
    std::vector<DispatchEntry*> fresh;
    fresh.reserve(value.mQueue.size());
    for (size_t i = 0; i < value.mQueue.size(); ++i) {
        auto& replacement = value.mQueue[i];
        const DispatchEntry* packet = replacement ? replacement.get() :
                (value.mReuse[i] < outboundQueue.size() ? outboundQueue[value.mReuse[i]].get() : nullptr);
        if (!packet) return android::base::Error() << "Missing exact queue source";
        if (packet->externalDesktopHoverRetirement) {
            // Earlier rebind packets change publication revisions. Reissue an
            // exact packet witness from the prior validated retirement intent;
            // never leave an old expected revision on a changed queue.
            const auto intent = packet->externalDesktopHoverRetirement->mRemovedOwners;
            if (!replacement) {
                replacement = std::make_unique<DispatchEntry>(packet->eventEntry, packet->targetFlags,
                        packet->transform, packet->rawTransform, packet->globalScaleFactor,
                        packet->targetUid, packet->vsyncId, packet->windowId);
                replacement->resolvedMotionFlags = packet->resolvedMotionFlags;
                replacement->externalDesktopPointerOwnership = packet->externalDesktopPointerOwnership;
                replacement->externalDesktopPhysicalMotionOrigin = packet->externalDesktopPhysicalMotionOrigin;
                replacement->externalDesktopSubmissionReservation = packet->externalDesktopSubmissionReservation;
                value.mReuse[i] = kExternalDesktopMaxPreparedQueuePackets;
            }
            auto bound = publication.bindHoverRetirement(*replacement, intent, weak_from_this(),
                                                         externalDesktopPublishedMotion);
            if (!bound.ok()) return android::base::Error() << bound.error();
            packet = replacement.get();
        }
        if (packet->externalDesktopRebindArrival) {
            const auto arrival = packet->externalDesktopRebindArrival;
            if (!replacement) {
                replacement = std::make_unique<DispatchEntry>(packet->eventEntry, packet->targetFlags,
                        packet->transform, packet->rawTransform, packet->globalScaleFactor,
                        packet->targetUid, packet->vsyncId, packet->windowId);
                replacement->resolvedMotionFlags = packet->resolvedMotionFlags;
                replacement->externalDesktopPointerOwnership = packet->externalDesktopPointerOwnership;
                replacement->externalDesktopPhysicalMotionOrigin = packet->externalDesktopPhysicalMotionOrigin;
                replacement->externalDesktopSubmissionReservation = packet->externalDesktopSubmissionReservation;
                replacement->externalDesktopRebindArrival = arrival;
                value.mReuse[i] = kExternalDesktopMaxPreparedQueuePackets;
            }
            auto bound = publication.bindRebindArrival(*replacement);
            if (!bound.ok()) return android::base::Error() << bound.error();
            packet = replacement.get();
        }
        auto prepared = publication.prepareForReplay(*packet, externalDesktopPublishedMotion);
        if (!prepared.ok()) return android::base::Error() << prepared.error();
        if (packet->eventEntry->type == EventEntry::Type::MOTION) {
            const auto& motion = static_cast<const MotionEntry&>(*packet->eventEntry);
            auto next = packet->externalDesktopRebindArrival ? (*replay)->prepareExternalDesktopRebindArrival(*packet) :
                    packet->externalDesktopHoverRetirement ? (*replay)->prepareExternalDesktopHoverRetirement(*packet) :
                    (*replay)->prepareExternalDesktopPublishedMotion(motion, packet->resolvedMotionFlags);
            if (!next.ok()) return android::base::Error() << next.error();
            replay = std::move(next);
        }
        if (!publication.commit(std::move(*prepared))) {
            return android::base::Error() << "Prospective publication owner changed";
        }
        if (replacement) fresh.push_back(replacement.get());
    }
    value.mReplay = std::move(*replay);
    value.mFreshPackets = std::move(fresh);
    return {};
}
#endif

} // namespace android::inputdispatcher
