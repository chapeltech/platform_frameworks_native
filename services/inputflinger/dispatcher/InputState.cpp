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

#include "DebugConfig.h"
#include "input/Input.h"
#include "input/InputDevice.h"

#include "InputState.h"

#include <cinttypes>
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <bit>
#endif
#include "InputDispatcher.h"

namespace android::inputdispatcher {

InputState::InputState(const IdGenerator& idGenerator) : mIdGenerator(idGenerator) {}

InputState::~InputState() {}

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
static bool boundedExternalDesktopPointerCoords(const std::vector<PointerCoords>& coords) {
    for (const auto& point : coords) {
        if (BitSet64::count(point.bits) > point.values.size()) return false;
    }
    return true;
}

bool InputState::hasBoundedExternalDesktopMotionProjection() const {
    if (mMotionMementos.size() > kExternalDesktopMaxPublishedMotionMementos) return false;
    for (const MotionMemento& memento : mMotionMementos) {
        if (memento.pointerProperties.size() > MAX_POINTERS ||
            memento.pointerProperties.size() != memento.pointerCoords.size() ||
            !boundedExternalDesktopPointerCoords(memento.pointerCoords)) return false;
        ExternalDesktopPointerOwnership::Ids ids;
        if (!ExternalDesktopPointerOwnership::idsOf(memento.pointerProperties, ids) ||
            (memento.externalDesktopPointerOwnership &&
             !memento.externalDesktopPointerOwnership->matches(memento.pointerProperties))) return false;
    }
    return true;
}

android::base::Result<std::optional<InputState::ExternalDesktopPhysicalMotionSnapshot>>
InputState::copyExternalDesktopPhysicalMotionSnapshotForOwner(const MotionEntry& query,
                                                    bool hovering) const {
    if (!hasBoundedExternalDesktopMotionProjection()) {
        return android::base::Error() << "Physical owner snapshot coverage unavailable";
    }
    const ssize_t index = findMotionMemento(query, hovering);
    if (index < 0) return std::optional<ExternalDesktopPhysicalMotionSnapshot>{};
    const MotionMemento& memento = mMotionMementos[index];
    return std::optional<ExternalDesktopPhysicalMotionSnapshot>{ExternalDesktopPhysicalMotionSnapshot{
            memento.pointerProperties, memento.pointerCoords,
            memento.externalDesktopPointerOwnership, memento.downTime}};
}

android::base::Result<std::unique_ptr<InputState>> InputState::prepareExternalDesktopPublishedMotion(
        const MotionEntry& entry, ftl::Flags<MotionFlag> flags) const {
    if (!hasBoundedExternalDesktopMotionProjection() || entry.pointerProperties.size() > MAX_POINTERS ||
        entry.pointerProperties.size() != entry.pointerCoords.size() ||
        !boundedExternalDesktopPointerCoords(entry.pointerCoords)) {
        return android::base::Error() << "Unbounded delivered motion checkpoint";
    }
    const int32_t action = MotionEvent::getActionMasked(entry.action);
    const bool hover = action == AMOTION_EVENT_ACTION_HOVER_ENTER ||
            action == AMOTION_EVENT_ACTION_HOVER_MOVE;
    const bool addsMemento = action == AMOTION_EVENT_ACTION_DOWN || hover;
    if (addsMemento && findMotionMemento(entry, hover) < 0 &&
        mMotionMementos.size() == kExternalDesktopMaxPublishedMotionMementos) {
        return android::base::Error() << "Delivered motion checkpoint capacity exhausted";
    }
    // Reuse the actual stock/PhaseA InputState transition rules, not a second
    // gesture model. All vector allocation happens before the publisher call.
    auto prepared = std::make_unique<InputState>(mIdGenerator);
    prepared->mMotionMementos = mMotionMementos;
    if (!prepared->trackMotion(entry, flags) || !prepared->hasBoundedExternalDesktopMotionProjection()) {
        return android::base::Error() << "Delivered motion cannot be represented exactly";
    }
    return prepared;
}

bool InputState::swapExternalDesktopMotionProjection(InputState& prepared) noexcept {
    if (&mIdGenerator != &prepared.mIdGenerator) return false;
    mMotionMementos.swap(prepared.mMotionMementos);
    return true;
}

android::base::Result<std::unique_ptr<InputState>> InputState::prepareExternalDesktopHoverRetirement(
        const DispatchEntry& packet) const {
    const auto& capability = packet.externalDesktopHoverRetirement;
    if (!capability || capability->mConnection.expired() || !hasBoundedExternalDesktopMotionProjection() ||
        packet.eventEntry->type != EventEntry::Type::MOTION ||
        packet.eventEntry.get() != capability->mEvent.get() || packet.seq != capability->mSeq) {
        return android::base::Error() << "Missing exact native hover retirement packet";
    }
    const auto& entry = static_cast<const MotionEntry&>(*packet.eventEntry);
    if (!boundedExternalDesktopPointerCoords(entry.pointerCoords)) {
        return android::base::Error() << "Scoped hover coordinate axis capacity exceeded";
    }
    const ssize_t index = findMotionMemento(entry, /*hovering=*/true);
    if (index < 0 || entry.externalDesktopPointerOwnership != packet.externalDesktopPointerOwnership) {
        return android::base::Error() << "Hover retirement lacks delivered physical predecessor";
    }
    const auto& before = mMotionMementos[index];
    ExternalDesktopPointerOwnership::Ids beforeIds,afterIds;
    if (!ExternalDesktopPointerOwnership::idsOf(before.pointerProperties,beforeIds) ||
        beforeIds != capability->mBefore || capability->mAfter == beforeIds ||
        (capability->mAfter & ~beforeIds).any()) {
        return android::base::Error() << "Hover retirement predecessor differs";
    }
    for(size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id){
        if(beforeIds.test(id) && ExternalDesktopPointerOwnership::keyAt(before.externalDesktopPointerOwnership,static_cast<int32_t>(id)) !=
                ExternalDesktopPointerOwnership::keyAt(capability->mBeforeOwners,static_cast<int32_t>(id))) {
            return android::base::Error() << "Hover retirement changed an existing owner";
        }
    }
    if(capability->mAfter.none()) {
        if(entry.action != AMOTION_EVENT_ACTION_HOVER_EXIT) {
            return android::base::Error() << "Last hover owner requires exact EXIT";
        }
        return prepareExternalDesktopPublishedMotion(entry,packet.resolvedMotionFlags);
    }
    if(entry.action != AMOTION_EVENT_ACTION_HOVER_MOVE ||
       !ExternalDesktopPointerOwnership::idsOf(entry.pointerProperties,afterIds) || afterIds != capability->mAfter ||
       entry.pointerProperties.size() != entry.pointerCoords.size() ||
       (entry.externalDesktopPointerOwnership && !entry.externalDesktopPointerOwnership->matches(entry.pointerProperties))) {
        return android::base::Error() << "Scoped hover retirement requires exact surviving MOVE roster";
    }
    for(size_t id=0;id<ExternalDesktopPointerOwnership::kMaxIds;++id){
        if(afterIds.test(id) && ExternalDesktopPointerOwnership::keyAt(entry.externalDesktopPointerOwnership,static_cast<int32_t>(id)) !=
                ExternalDesktopPointerOwnership::keyAt(before.externalDesktopPointerOwnership,static_cast<int32_t>(id))) {
            return android::base::Error() << "Scoped hover retirement retagged a survivor";
        }
        if(beforeIds.test(id) && !afterIds.test(id) &&
           (!ExternalDesktopPointerOwnership::keyAt(capability->mRemovedOwners,static_cast<int32_t>(id)) ||
            ExternalDesktopPointerOwnership::keyAt(capability->mRemovedOwners,static_cast<int32_t>(id)) !=
            ExternalDesktopPointerOwnership::keyAt(before.externalDesktopPointerOwnership,static_cast<int32_t>(id)))) {
            return android::base::Error() << "Scoped hover retirement lacks removed private owner witness";
        }
    }
    auto prepared=copyExternalDesktopMotionProjection();
    if(!prepared.ok()) return android::base::Error() << prepared.error();
    auto& after=(*prepared)->mMotionMementos[index];
    // Match stock HOVER_MOVE's replacement values without weakening its normal
    // continuity checks or erasing/readding another contact/hover container.
    after.flags=packet.resolvedMotionFlags;
    after.xPrecision=entry.xPrecision;after.yPrecision=entry.yPrecision;
    after.xCursorPosition=entry.xCursorPosition;after.yCursorPosition=entry.yCursorPosition;
    after.downTime=entry.downTime;after.policyFlags=entry.policyFlags;
    after.displayId=entry.displayId;
    after.pointerProperties=entry.pointerProperties;after.pointerCoords=entry.pointerCoords;
    after.externalDesktopPointerOwnership=entry.externalDesktopPointerOwnership;
    after.firstNewPointerIdx=INVALID_POINTER_INDEX;
    return prepared;
}

android::base::Result<std::unique_ptr<InputState>> InputState::prepareExternalDesktopRebindArrival(
        const DispatchEntry& packet) const {
    const auto& token = packet.externalDesktopRebindArrival;
    if (!token || token->mSource.expired() || token->mDestination.expired() || !token->mSubmission ||
        token->mEvent.get() != packet.eventEntry.get() || token->mSeq != packet.seq ||
        !token->mPhysicalOrigin || token->mPhysicalOrigin != packet.externalDesktopPhysicalMotionOrigin ||
        !hasBoundedExternalDesktopMotionProjection() || packet.eventEntry->type != EventEntry::Type::MOTION) {
        return android::base::Error() << "Missing exact native two-owner arrival packet";
    }
    const auto& event = static_cast<const MotionEntry&>(*packet.eventEntry);
    if (!boundedExternalDesktopPointerCoords(event.pointerCoords)) return android::base::Error() << "Arrival axes invalid";
    const int32_t action = MotionEvent::getActionMasked(event.action);
    const bool hover = action == AMOTION_EVENT_ACTION_HOVER_ENTER || action == AMOTION_EVENT_ACTION_HOVER_MOVE;
    ExternalDesktopPointerOwnershipPtr owners;
    const auto previous = getPointersOfLastEvent(event, hover, &owners);
    ExternalDesktopPointerOwnership::Ids before, after;
    if ((previous && !ExternalDesktopPointerOwnership::idsOf(previous->first, before)) || before != token->mBefore ||
        token->mAdded.none() || (token->mAdded & before).any() ||
        !ExternalDesktopPointerOwnership::idsOf(event.pointerProperties, after) || after != (before | token->mAdded) ||
        event.pointerProperties.size() != event.pointerCoords.size() ||
        packet.externalDesktopPointerOwnership != event.externalDesktopPointerOwnership) {
        return android::base::Error() << "Arrival changed an unselected predecessor roster";
    }
    for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
        if (before.test(id) && (ExternalDesktopPointerOwnership::keyAt(owners, static_cast<int32_t>(id)) !=
                ExternalDesktopPointerOwnership::keyAt(token->mBeforeOwners, static_cast<int32_t>(id)) ||
                ExternalDesktopPointerOwnership::keyAt(event.externalDesktopPointerOwnership, static_cast<int32_t>(id)) !=
                ExternalDesktopPointerOwnership::keyAt(owners, static_cast<int32_t>(id)))) {
            return android::base::Error() << "Arrival retagged an ordinary/private peer";
        }
        if (token->mAdded.test(id) && ExternalDesktopPointerOwnership::keyAt(event.externalDesktopPointerOwnership,
                static_cast<int32_t>(id)) != token->mNewKey) {
            return android::base::Error() << "Arrival lacks exact new presentation lifetime";
        }
    }
    if (!hover || before.none()) return prepareExternalDesktopPublishedMotion(event, packet.resolvedMotionFlags);
    if (action != AMOTION_EVENT_ACTION_HOVER_MOVE) return android::base::Error() << "Aggregate hover addition must MOVE";
    const ssize_t index = findMotionMemento(event, true);
    if (index < 0) return android::base::Error() << "Hover arrival lacks actual predecessor";
    auto result = copyExternalDesktopMotionProjection();
    if (!result.ok()) return android::base::Error() << result.error();
    auto& memento = (*result)->mMotionMementos[index];
    memento.flags = packet.resolvedMotionFlags;
    memento.xPrecision = event.xPrecision; memento.yPrecision = event.yPrecision;
    memento.xCursorPosition = event.xCursorPosition; memento.yCursorPosition = event.yCursorPosition;
    memento.downTime = event.downTime; memento.policyFlags = event.policyFlags;
    memento.displayId = event.displayId;
    memento.pointerProperties = event.pointerProperties; memento.pointerCoords = event.pointerCoords;
    memento.externalDesktopPointerOwnership = event.externalDesktopPointerOwnership;
    memento.firstNewPointerIdx = INVALID_POINTER_INDEX;
    return result;
}

android::base::Result<std::unique_ptr<InputState>> InputState::copyExternalDesktopMotionProjection() const {
    if (!hasBoundedExternalDesktopMotionProjection()) {
        return android::base::Error() << "Unbounded motion projection cannot seed queue replay";
    }
    auto result = std::make_unique<InputState>(mIdGenerator);
    result->mMotionMementos = mMotionMementos;
    return result;
}

bool InputState::sameExternalDesktopMotionProjection(const InputState& other) const {
    if (!hasBoundedExternalDesktopMotionProjection() || !other.hasBoundedExternalDesktopMotionProjection()) return false;
    if (&mIdGenerator != &other.mIdGenerator || mMotionMementos.size() != other.mMotionMementos.size()) return false;
    const auto sameFloat = [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); };
    for (size_t i = 0; i < mMotionMementos.size(); ++i) {
        const auto& a = mMotionMementos[i];
        const auto& b = other.mMotionMementos[i];
        if (a.deviceId != b.deviceId || a.source != b.source || a.displayId != b.displayId ||
            a.flags != b.flags || !sameFloat(a.xPrecision,b.xPrecision) || !sameFloat(a.yPrecision,b.yPrecision) ||
            !sameFloat(a.xCursorPosition,b.xCursorPosition) || !sameFloat(a.yCursorPosition,b.yCursorPosition) ||
            a.downTime != b.downTime || a.pointerProperties != b.pointerProperties ||
            a.firstNewPointerIdx != b.firstNewPointerIdx || a.hovering != b.hovering ||
            a.policyFlags != b.policyFlags || a.externalDesktopPointerOwnership != b.externalDesktopPointerOwnership ||
            a.pointerCoords.size() != b.pointerCoords.size()) return false;
        for (size_t p = 0; p < a.pointerCoords.size(); ++p) {
            const auto& x = a.pointerCoords[p];
            const auto& y = b.pointerCoords[p];
            if (x.bits != y.bits || x.isResampled != y.isResampled) return false;
            const uint32_t axes = BitSet64::count(x.bits);
            if (axes > x.values.size() || axes > y.values.size()) return false;
            for (uint32_t axis = 0; axis < axes; ++axis) {
                if (!sameFloat(x.values[axis],y.values[axis])) return false;
            }
        }
    }
    return true;
}

bool InputState::hasExternalDesktopStream(const ExternalDesktopPointerStreamKey& key) const {
    for (const auto& memento : mMotionMementos) {
        for (const auto& property : memento.pointerProperties) {
            if (ExternalDesktopPointerOwnership::keyAt(memento.externalDesktopPointerOwnership, property.id) == key) {
                return true;
            }
        }
    }
    return false;
}

std::optional<nsecs_t> InputState::externalDesktopMotionDownTime(const MotionEntry& entry) const {
    const ssize_t index = findMotionMemento(entry, /*hovering=*/false);
    return index < 0 ? std::nullopt : std::optional(mMotionMementos[index].downTime);
}
#endif

bool InputState::isHovering(DeviceId deviceId, uint32_t source,
                            ui::LogicalDisplayId displayId
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
                            , const ExternalDesktopPointerOwnershipPtr& ownership
#endif
                            ) const {
    for (const MotionMemento& memento : mMotionMementos) {
        if (memento.deviceId == deviceId && memento.source == source &&
            memento.displayId == displayId && memento.hovering) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
            if (memento.externalDesktopPointerOwnership || ownership) {
                if (!memento.externalDesktopPointerOwnership || !ownership ||
                    memento.externalDesktopPointerOwnership->ids() != ownership->ids()) continue;
                bool same = true;
                for (const auto& property : memento.pointerProperties) {
                    same &= ExternalDesktopPointerOwnership::keyAt(memento.externalDesktopPointerOwnership, property.id) ==
                            ExternalDesktopPointerOwnership::keyAt(ownership, property.id);
                }
                if (!same) continue;
            }
#endif
            return true;
        }
    }
    return false;
}

bool InputState::trackKey(const KeyEntry& entry, int32_t flags) {
    switch (entry.action) {
        case AKEY_EVENT_ACTION_UP: {
            if (entry.flags & AKEY_EVENT_FLAG_FALLBACK) {
                std::erase_if(mFallbackKeys,
                              [&entry](const auto& item) { return item.second == entry.keyCode; });
            }
            ssize_t index = findKeyMemento(entry);
            if (index >= 0) {
                mKeyMementos.erase(mKeyMementos.begin() + index);
                return true;
            }
            /* FIXME: We can't just drop the key up event because that prevents creating
             * popup windows that are automatically shown when a key is held and then
             * dismissed when the key is released.  The problem is that the popup will
             * not have received the original key down, so the key up will be considered
             * to be inconsistent with its observed state.  We could perhaps handle this
             * by synthesizing a key down but that will cause other problems.
             *
             * So for now, allow inconsistent key up events to be dispatched.
             *
    #if DEBUG_OUTBOUND_EVENT_DETAILS
            ALOGD("Dropping inconsistent key up event: deviceId=%d, source=%08x, "
                    "keyCode=%d, scanCode=%d",
                    entry.deviceId, entry.source, entry.keyCode, entry.scanCode);
    #endif
            return false;
            */
            return true;
        }

        case AKEY_EVENT_ACTION_DOWN: {
            ssize_t index = findKeyMemento(entry);
            if (index >= 0) {
                mKeyMementos.erase(mKeyMementos.begin() + index);
            }
            addKeyMemento(entry, flags);
            return true;
        }

        default:
            return true;
    }
}

/**
 * Return:
 *  true if the incoming event was correctly tracked,
 *  false if the incoming event should be dropped.
 */
bool InputState::trackMotion(const MotionEntry& entry, ftl::Flags<MotionFlag> flags) {
    // Don't track non-pointer events
    if (!isFromSource(entry.source, AINPUT_SOURCE_CLASS_POINTER)) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
        if (entry.externalDesktopPointerOwnership) return false;
#endif
        // This is a focus-dispatched event; we don't track its state.
        return true;
    }

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    if (!canTrackExternalDesktopMotion(entry)) return false;
#endif

    if (!input_flags::enable_multi_device_same_window_stream()) {
        if (!mMotionMementos.empty()) {
            const MotionMemento& lastMemento = mMotionMementos.back();
            if (isStylusEvent(lastMemento.source, lastMemento.pointerProperties) &&
                !isStylusEvent(entry.source, entry.pointerProperties)) {
                // We already have a stylus stream, and the new event is not from stylus.
                return false;
            }
        }
    }

    int32_t actionMasked = entry.action & AMOTION_EVENT_ACTION_MASK;
    switch (actionMasked) {
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/false);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
                return true;
            }

            return false;
        }

        case AMOTION_EVENT_ACTION_DOWN: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/false);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
            }
            addMotionMemento(entry, flags, /*hovering=*/false);
            return true;
        }

        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_MOVE: {
            if (entry.source & AINPUT_SOURCE_CLASS_NAVIGATION) {
                // Trackballs can send MOVE events with a corresponding DOWN or UP. There's no need
                // to generate cancellation events for these since they're based in relative rather
                // than absolute units.
                return true;
            }

            ssize_t index = findMotionMemento(entry, /*hovering=*/false);

            if (entry.source & AINPUT_SOURCE_CLASS_JOYSTICK) {
                // Joysticks can send MOVE events without a corresponding DOWN or UP. Since all
                // joystick axes are normalized to [-1, 1] we can trust that 0 means it's neutral.
                // Any other value and we need to track the motion so we can send cancellation
                // events for anything generating fallback events (e.g. DPad keys for joystick
                // movements).
                if (index >= 0) {
                    if (entry.pointerCoords[0].isEmpty()) {
                        mMotionMementos.erase(mMotionMementos.begin() + index);
                    } else {
                        MotionMemento& memento = mMotionMementos[index];
                        if (!memento.setPointers(entry)) return false;
                    }
                } else if (!entry.pointerCoords[0].isEmpty()) {
                    addMotionMemento(entry, flags, /*hovering=*/false);
                }

                // Joysticks and trackballs can send MOVE events without corresponding DOWN or UP.
                return true;
            }

            if (index >= 0) {
                MotionMemento& memento = mMotionMementos[index];
                if (memento.firstNewPointerIdx < 0) {
                    if (!memento.setPointers(entry)) return false;
                    return true;
                }
            }

            return false;
        }

        case AMOTION_EVENT_ACTION_HOVER_EXIT: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/true);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
                return true;
            }

            return false;
        }

        case AMOTION_EVENT_ACTION_HOVER_ENTER:
        case AMOTION_EVENT_ACTION_HOVER_MOVE: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/true);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
            }
            addMotionMemento(entry, flags, /*hovering=*/true);
            return true;
        }

        default:
            return true;
    }
}

std::optional<std::pair<std::vector<PointerProperties>, std::vector<PointerCoords>>>
InputState::getPointersOfLastEvent(const MotionEntry& entry, bool hovering
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
                                 , ExternalDesktopPointerOwnershipPtr* ownership
#endif
                                 ) const {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    if (ownership) ownership->reset();
#endif
    ssize_t index = findMotionMemento(entry, hovering);
    if (index == -1) {
        return std::nullopt;
    }
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    if (!compatibleExternalDesktopPointers(mMotionMementos[index], entry)) return std::nullopt;
    if (ownership) *ownership = mMotionMementos[index].externalDesktopPointerOwnership;
#endif
    return std::make_pair(mMotionMementos[index].pointerProperties,
                          mMotionMementos[index].pointerCoords);
}

ssize_t InputState::findKeyMemento(const KeyEntry& entry) const {
    for (size_t i = 0; i < mKeyMementos.size(); i++) {
        const KeyMemento& memento = mKeyMementos[i];
        if (memento.deviceId == entry.deviceId && memento.source == entry.source &&
            memento.displayId == entry.displayId && memento.keyCode == entry.keyCode &&
            memento.scanCode == entry.scanCode) {
            return i;
        }
    }
    return -1;
}

ssize_t InputState::findMotionMemento(const MotionEntry& entry, bool hovering) const {
    // This locates the physical client-stream container, NOT private authority.
    // Its per-pointer owners are checked before any private update/removal.
    // On connected displays a mouse can move between displays and displayId may change
    // while a gesture is in-progress.
    const bool skipDisplayCheck = isMouseOrTouchpad(entry.source);
    for (size_t i = 0; i < mMotionMementos.size(); i++) {
        const MotionMemento& memento = mMotionMementos[i];
        if (memento.deviceId == entry.deviceId && memento.source == entry.source &&
            memento.hovering == hovering &&
            (skipDisplayCheck || memento.displayId == entry.displayId)) {
            return i;
        }
    }
    return -1;
}

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
bool InputState::compatibleExternalDesktopPointers(const MotionMemento& memento,
                                         const MotionEntry& entry) {
    const auto& old = memento.externalDesktopPointerOwnership;
    const auto& incoming = entry.externalDesktopPointerOwnership;
    if (!old && !incoming) return true;
    ExternalDesktopPointerOwnership::Ids oldIds, incomingIds;
    if (!ExternalDesktopPointerOwnership::idsOf(memento.pointerProperties, oldIds) ||
        !ExternalDesktopPointerOwnership::idsOf(entry.pointerProperties, incomingIds) ||
        (old && !old->matches(memento.pointerProperties)) ||
        (incoming && !incoming->matches(entry.pointerProperties))) return false;
    for (size_t id = 0; id < ExternalDesktopPointerOwnership::kMaxIds; ++id) {
        const auto oldKey = ExternalDesktopPointerOwnership::keyAt(old, static_cast<int32_t>(id));
        const auto newKey = ExternalDesktopPointerOwnership::keyAt(incoming, static_cast<int32_t>(id));
        // Missing fields cannot erase an owned live pointer. Conversely an
        // already-ordinary pointer cannot be relabelled in mid-stream.
        if (oldIds.test(id) && (oldKey || (incomingIds.test(id) && newKey)) &&
            (!incomingIds.test(id) || oldKey != newKey)) return false;
    }
    return true;
}

bool InputState::canTrackExternalDesktopMotion(const MotionEntry& entry) const {
    if (!entry.externalDesktopPointerOwnership && !hasExternalDesktopPointerOwnership()) return true;
    if (entry.pointerCoords.size() != entry.pointerProperties.size() ||
        (entry.externalDesktopPointerOwnership &&
         !entry.externalDesktopPointerOwnership->matches(entry.pointerProperties))) return false;
    const int32_t action = MotionEvent::getActionMasked(entry.action);
    const bool hovering = action == AMOTION_EVENT_ACTION_HOVER_ENTER ||
            action == AMOTION_EVENT_ACTION_HOVER_MOVE || action == AMOTION_EVENT_ACTION_HOVER_EXIT;
    const ssize_t index = findMotionMemento(entry, hovering);
    if (index < 0) return true;
    const auto& memento = mMotionMementos[index];
    if (!compatibleExternalDesktopPointers(memento, entry)) return false;
    if (memento.externalDesktopPointerOwnership || entry.externalDesktopPointerOwnership) {
        ExternalDesktopPointerOwnership::Ids oldIds, incomingIds;
        if (!ExternalDesktopPointerOwnership::idsOf(memento.pointerProperties, oldIds) ||
            !ExternalDesktopPointerOwnership::idsOf(entry.pointerProperties, incomingIds)) return false;
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            const size_t added = MotionEvent::getActionIndex(entry.action);
            if (added >= entry.getPointerCount()) return false;
            const auto id = entry.pointerProperties[added].id;
            if (oldIds.test(id) || (incomingIds ^ oldIds).count() != 1 ||
                !incomingIds.test(id)) return false;
        } else if (action != AMOTION_EVENT_ACTION_DOWN && incomingIds != oldIds) {
            // A late private UP/CANCEL must not erase a different surviving
            // ordinary pointer which happens to reuse this physical memento.
            return false;
        }
    }
    // A duplicate DOWN cannot silently replace an outstanding private stream.
    return action != AMOTION_EVENT_ACTION_DOWN ||
            (!memento.externalDesktopPointerOwnership && !entry.externalDesktopPointerOwnership);
}

bool InputState::hasExternalDesktopPointerOwnership() const {
    return std::any_of(mMotionMementos.begin(), mMotionMementos.end(),
                       [](const auto& memento) { return bool(memento.externalDesktopPointerOwnership); });
}
#endif

void InputState::addKeyMemento(const KeyEntry& entry, int32_t flags) {
    KeyMemento memento;
    memento.deviceId = entry.deviceId;
    memento.source = entry.source;
    memento.displayId = entry.displayId;
    memento.keyCode = entry.keyCode;
    memento.scanCode = entry.scanCode;
    memento.metaState = entry.metaState;
    memento.flags = flags;
    memento.downTime = entry.downTime;
    memento.policyFlags = entry.policyFlags;
    mKeyMementos.push_back(memento);
}

void InputState::addMotionMemento(const MotionEntry& entry, ftl::Flags<MotionFlag> flags,
                                  bool hovering) {
    MotionMemento memento;
    memento.deviceId = entry.deviceId;
    memento.source = entry.source;
    memento.displayId = entry.displayId;
    memento.flags = flags;
    memento.xPrecision = entry.xPrecision;
    memento.yPrecision = entry.yPrecision;
    memento.xCursorPosition = entry.xCursorPosition;
    memento.yCursorPosition = entry.yCursorPosition;
    memento.downTime = entry.downTime;
    LOG_ALWAYS_FATAL_IF(!memento.setPointers(entry), "Private pointer validation escaped trackMotion");
    memento.hovering = hovering;
    memento.policyFlags = entry.policyFlags;
    mMotionMementos.push_back(memento);
}

bool InputState::MotionMemento::setPointers(const MotionEntry& entry) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    if (entry.externalDesktopPointerOwnership || externalDesktopPointerOwnership) {
        if (!entry.externalDesktopPointerOwnership ||
            !entry.externalDesktopPointerOwnership->matches(entry.pointerProperties) ||
            entry.pointerCoords.size() != entry.pointerProperties.size()) return false;
        std::vector<PointerProperties> properties;
        std::vector<PointerCoords> coords;
        const bool pointerUp = MotionEvent::getActionMasked(entry.action) == AMOTION_EVENT_ACTION_POINTER_UP;
        const size_t up = MotionEvent::getActionIndex(entry.action);
        if (pointerUp && up >= entry.getPointerCount()) return false;
        for (size_t i = 0; i < entry.getPointerCount(); ++i) {
            if (pointerUp && i == up) continue;
            properties.push_back(entry.pointerProperties[i]);
            coords.push_back(entry.pointerCoords[i]);
        }
        auto selected = ExternalDesktopPointerOwnership::select(entry.externalDesktopPointerOwnership, properties);
        if (!selected.ok()) return false;
        pointerProperties = std::move(properties);
        pointerCoords = std::move(coords);
        externalDesktopPointerOwnership = std::move(*selected);
        return true;
    }
#endif
    pointerProperties.clear();
    pointerCoords.clear();

    for (uint32_t i = 0; i < entry.getPointerCount(); i++) {
        if (MotionEvent::getActionMasked(entry.action) == AMOTION_EVENT_ACTION_POINTER_UP) {
            // In POINTER_UP events, the pointer is leaving. Since the action is not stored,
            // this departing pointer should not be recorded.
            const uint8_t actionIndex = MotionEvent::getActionIndex(entry.action);
            if (i == actionIndex) {
                continue;
            }
        }
        pointerProperties.push_back(entry.pointerProperties[i]);
        pointerCoords.push_back(entry.pointerCoords[i]);
    }
    return true;
}

void InputState::MotionMemento::mergePointerStateTo(MotionMemento& other) const {
    for (uint32_t i = 0; i < getPointerCount(); i++) {
        if (other.firstNewPointerIdx < 0) {
            other.firstNewPointerIdx = other.getPointerCount();
        }
        other.pointerProperties.push_back(pointerProperties[i]);
        other.pointerCoords.push_back(pointerCoords[i]);
    }
}

size_t InputState::MotionMemento::getPointerCount() const {
    return pointerProperties.size();
}

bool InputState::shouldCancelPreviousStream(const MotionEntry& motionEntry) const {
    if (!isFromSource(motionEntry.source, AINPUT_SOURCE_CLASS_POINTER)) {
        // This is a focus-dispatched event that should not affect the previous stream.
        return false;
    }

    // New MotionEntry pointer event is coming in.

    // If this is a new gesture, and it's from a different device, then, in general, we will cancel
    // the current gesture.
    // However, because stylus should be preferred over touch, we need to treat some cases in a
    // special way.
    if (mMotionMementos.empty()) {
        // There is no ongoing pointer gesture, so there is nothing to cancel
        return false;
    }

    const MotionMemento& lastMemento = mMotionMementos.back();
    const int32_t actionMasked = MotionEvent::getActionMasked(motionEntry.action);

    // For compatibility, only one input device can be active at a time in the same window.
    if (lastMemento.deviceId == motionEntry.deviceId) {
        // In general, the same device should produce self-consistent streams so nothing needs to
        // be canceled. But there is one exception:
        // Sometimes ACTION_DOWN is received without a corresponding HOVER_EXIT. To account for
        // that, cancel the previous hovering stream
        if (actionMasked == AMOTION_EVENT_ACTION_DOWN && lastMemento.hovering) {
            return true;
        }

        // If the stream changes its source, just cancel the current gesture to be safe. It's
        // possible that the app isn't handling source changes properly
        if (motionEntry.source != lastMemento.source) {
            LOG(INFO) << "Canceling stream: last source was "
                      << inputEventSourceToString(lastMemento.source) << " and new event is "
                      << motionEntry;
            return true;
        }

        // If the injection is happening into two different displays, the same injected device id
        // could be going into both. And at this time, if mirroring is active, the same connection
        // would receive different events from each display. Since the TouchStates are per-display,
        // it's unlikely that those two streams would be consistent with each other. Therefore,
        // cancel the previous gesture if the display id changes.
        // Except when we have connected-displays where a mouse may move across display boundaries.
        const bool skipDisplayCheck = isMouseOrTouchpad(motionEntry.source);
        if (!skipDisplayCheck && motionEntry.displayId != lastMemento.displayId) {
            LOG(INFO) << "Canceling stream: last displayId was " << lastMemento.displayId
                      << " and new event is " << motionEntry;
            return true;
        }

        return false;
    }

    if (!input_flags::enable_multi_device_same_window_stream()) {
        if (isStylusEvent(lastMemento.source, lastMemento.pointerProperties)) {
            // A stylus is already active.
            if (isStylusEvent(motionEntry.source, motionEntry.pointerProperties) &&
                actionMasked == AMOTION_EVENT_ACTION_DOWN) {
                // If this new event is from a different device, then cancel the old
                // stylus and allow the new stylus to take over, but only if it's going down.
                // Otherwise, they will start to race each other.
                return true;
            }

            // Keep the current stylus gesture.
            return false;
        }

        // Cancel the current gesture if this is a start of a new gesture from a new device.
        if (actionMasked == AMOTION_EVENT_ACTION_DOWN ||
            actionMasked == AMOTION_EVENT_ACTION_HOVER_ENTER) {
            return true;
        }
    }
    // By default, don't cancel any events.
    return false;
}

std::unique_ptr<EventEntry> InputState::cancelConflictingInputStream(
        const MotionEntry& motionEntry) {
    if (!shouldCancelPreviousStream(motionEntry)) {
        return {};
    }

    const MotionMemento& memento = mMotionMementos.back();

    // Cancel the last device stream
    std::unique_ptr<MotionEntry> cancelEntry =
            createCancelEntryForMemento(memento, motionEntry.eventTime);

    if (!trackMotion(*cancelEntry, cancelEntry->flags)) {
        LOG(FATAL) << "Generated inconsistent cancel event!";
    }
    return cancelEntry;
}

std::unique_ptr<MotionEntry> InputState::createCancelEntryForMemento(const MotionMemento& memento,
                                                                     nsecs_t eventTime) const {
    const int32_t action =
            memento.hovering ? AMOTION_EVENT_ACTION_HOVER_EXIT : AMOTION_EVENT_ACTION_CANCEL;
    ftl::Flags<MotionFlag> flags = memento.flags;
    if (action == AMOTION_EVENT_ACTION_CANCEL) {
        flags |= MotionFlag::CANCELED;
    }
    auto event = std::make_unique<MotionEntry>(mIdGenerator.nextId(), /*injectionState=*/nullptr,
                                         eventTime, memento.deviceId, memento.source,
                                         memento.displayId, memento.policyFlags, action,
                                         /*actionButton=*/0, flags, AMETA_NONE,
                                         /*buttonState=*/0, MotionClassification::NONE,
                                         memento.xPrecision, memento.yPrecision,
                                         memento.xCursorPosition, memento.yCursorPosition,
                                         memento.downTime, memento.pointerProperties,
                                         memento.pointerCoords);
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    event->externalDesktopPointerOwnership = memento.externalDesktopPointerOwnership;
#endif
    return event;
}

std::vector<std::unique_ptr<EventEntry>> InputState::synthesizeCancellationEvents(
        nsecs_t currentTime, const CancellationOptions& options) {
    std::vector<std::unique_ptr<EventEntry>> events;
    for (KeyMemento& memento : mKeyMementos) {
        if (shouldCancelKey(memento, options)) {
            events.push_back(
                    std::make_unique<KeyEntry>(mIdGenerator.nextId(), /*injectionState=*/nullptr,
                                               currentTime, memento.deviceId, memento.source,
                                               memento.displayId, memento.policyFlags,
                                               AKEY_EVENT_ACTION_UP,
                                               memento.flags | AKEY_EVENT_FLAG_CANCELED,
                                               memento.keyCode, memento.scanCode, memento.metaState,
                                               /*repeatCount=*/0, memento.downTime));
        }
    }

    for (const MotionMemento& memento : mMotionMementos) {
        if (shouldCancelMotion(memento, options)) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
            if (options.externalDesktopStream) {
                ExternalDesktopPointerOwnership::Ids selected;
                for (const auto& property : memento.pointerProperties) {
                    if (ExternalDesktopPointerOwnership::keyAt(memento.externalDesktopPointerOwnership, property.id) ==
                        options.externalDesktopStream && (!options.pointerIds || options.pointerIds->test(property.id))) {
                        selected.set(property.id);
                    }
                }
                if (selected.none()) continue;
                // Stock hovering is one client stream. Do not broaden an
                // unrepresentable mixed-hover subset into a whole HOVER_EXIT.
                if (memento.hovering && selected.count() != memento.getPointerCount()) continue;
                auto partial = synthesizeCancellationEventsForPointers(memento, selected, currentTime);
                events.insert(events.end(), std::make_move_iterator(partial.begin()),
                              std::make_move_iterator(partial.end()));
                continue;
            }
#endif
            if (options.pointerIds == std::nullopt) {
                events.push_back(createCancelEntryForMemento(memento, currentTime));
            } else {
                std::vector<std::unique_ptr<MotionEntry>> pointerCancelEvents =
                        synthesizeCancellationEventsForPointers(memento, options.pointerIds.value(),
                                                                currentTime);
                events.insert(events.end(), std::make_move_iterator(pointerCancelEvents.begin()),
                              std::make_move_iterator(pointerCancelEvents.end()));
            }
        }
    }
    return events;
}

std::vector<std::unique_ptr<EventEntry>> InputState::synthesizePointerDownEvents(
        nsecs_t currentTime) {
    std::vector<std::unique_ptr<EventEntry>> events;
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    for (const auto& memento : mMotionMementos) {
        if (memento.externalDesktopPointerOwnership &&
            (!memento.externalDesktopPointerOwnership->matches(memento.pointerProperties) ||
             memento.pointerCoords.size() != memento.pointerProperties.size() ||
             memento.firstNewPointerIdx < INVALID_POINTER_INDEX ||
             memento.firstNewPointerIdx > static_cast<int32_t>(memento.getPointerCount()))) return {};
    }
#endif
    for (MotionMemento& memento : mMotionMementos) {
        if (!isFromSource(memento.source, AINPUT_SOURCE_CLASS_POINTER)) {
            continue;
        }

        if (memento.firstNewPointerIdx < 0) {
            continue;
        }

        std::vector<PointerProperties> pointerProperties;
        std::vector<PointerCoords> pointerCoords;

        // We will deliver all pointers the target already knows about
        for (uint32_t i = 0; i < static_cast<uint32_t>(memento.firstNewPointerIdx); i++) {
            pointerProperties.push_back(memento.pointerProperties[i]);
            pointerCoords.push_back(memento.pointerCoords[i]);
        }

        // We will send explicit events for all pointers the target doesn't know about
        for (uint32_t i = static_cast<uint32_t>(memento.firstNewPointerIdx);
             i < memento.getPointerCount(); i++) {
            pointerProperties.push_back(memento.pointerProperties[i]);
            pointerCoords.push_back(memento.pointerCoords[i]);

            const size_t pointerCount = pointerProperties.size();

            // Down only if the first pointer, pointer down otherwise
            const int32_t action = (pointerCount <= 1)
                    ? AMOTION_EVENT_ACTION_DOWN
                    : AMOTION_EVENT_ACTION_POINTER_DOWN
                            | (i << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

            events.push_back(
                    std::make_unique<MotionEntry>(mIdGenerator.nextId(), /*injectionState=*/nullptr,
                                                  currentTime, memento.deviceId, memento.source,
                                                  memento.displayId, memento.policyFlags, action,
                                                  /*actionButton=*/0, memento.flags, AMETA_NONE,
                                                  /*buttonState=*/0, MotionClassification::NONE,
                                                  memento.xPrecision, memento.yPrecision,
                                                  memento.xCursorPosition, memento.yCursorPosition,
                                                  memento.downTime, pointerProperties,
                                                  pointerCoords));
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
            auto selected = ExternalDesktopPointerOwnership::select(memento.externalDesktopPointerOwnership,
                                                           pointerProperties);
            if (!selected.ok()) return {};
            static_cast<MotionEntry&>(*events.back()).externalDesktopPointerOwnership = std::move(*selected);
#endif
        }

        memento.firstNewPointerIdx = INVALID_POINTER_INDEX;
    }

    return events;
}

std::vector<std::unique_ptr<MotionEntry>> InputState::synthesizeCancellationEventsForPointers(
        const MotionMemento& memento, std::bitset<MAX_POINTER_ID + 1> pointerIds,
        nsecs_t currentTime) {
    std::vector<std::unique_ptr<MotionEntry>> events;
    std::vector<uint32_t> canceledPointerIndices;

    for (uint32_t pointerIdx = 0; pointerIdx < memento.getPointerCount(); pointerIdx++) {
        uint32_t pointerId = uint32_t(memento.pointerProperties[pointerIdx].id);
        if (pointerIds.test(pointerId)) {
            canceledPointerIndices.push_back(pointerIdx);
        }
    }

    if (canceledPointerIndices.size() == memento.getPointerCount()) {
        // We are cancelling all pointers.
        events.emplace_back(createCancelEntryForMemento(memento, currentTime));
        return events;
    }

    // If we aren't canceling all pointers, we need to generate ACTION_POINTER_UP with
    // FLAG_CANCELED for each of the canceled pointers. For each event, we must remove the
    // previously canceled pointers from PointerProperties and PointerCoords, and update
    // pointerCount appropriately. For convenience, sort the canceled pointer indices in
    // descending order so that we can just slide the remaining pointers to the beginning of
    // the array when a pointer is canceled.
    std::sort(canceledPointerIndices.begin(), canceledPointerIndices.end(),
              std::greater<uint32_t>());

    std::vector<PointerProperties> pointerProperties = memento.pointerProperties;
    std::vector<PointerCoords> pointerCoords = memento.pointerCoords;
    for (const uint32_t pointerIdx : canceledPointerIndices) {
        if (pointerProperties.size() <= 1) {
            LOG(FATAL) << "Unexpected code path for canceling all pointers!";
        }
        const int32_t action = AMOTION_EVENT_ACTION_POINTER_UP |
                (pointerIdx << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        events.push_back(
                std::make_unique<MotionEntry>(mIdGenerator.nextId(), /*injectionState=*/nullptr,
                                              currentTime, memento.deviceId, memento.source,
                                              memento.displayId, memento.policyFlags, action,
                                              /*actionButton=*/0,
                                              memento.flags | MotionFlag::CANCELED, AMETA_NONE,
                                              /*buttonState=*/0, MotionClassification::NONE,
                                              memento.xPrecision, memento.yPrecision,
                                              memento.xCursorPosition, memento.yCursorPosition,
                                              memento.downTime, pointerProperties, pointerCoords));
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
        auto selected = ExternalDesktopPointerOwnership::select(memento.externalDesktopPointerOwnership,
                                                       pointerProperties);
        if (!selected.ok()) return {};
        events.back()->externalDesktopPointerOwnership = std::move(*selected);
#endif

        // Cleanup pointer information
        pointerProperties.erase(pointerProperties.begin() + pointerIdx);
        pointerCoords.erase(pointerCoords.begin() + pointerIdx);
    }
    return events;
}

void InputState::clear() {
    mKeyMementos.clear();
    mMotionMementos.clear();
    mFallbackKeys.clear();
}

bool InputState::mergePointerStateTo(InputState& other) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    // No private cross-window transfer/rebinding policy exists in PhaseA.
    // Refuse before changing either state or firstNewPointerIdx.
    if (hasExternalDesktopPointerOwnership() || other.hasExternalDesktopPointerOwnership()) return false;
#endif
    for (size_t i = 0; i < mMotionMementos.size(); i++) {
        MotionMemento& memento = mMotionMementos[i];
        // Since we support split pointers we need to merge touch events
        // from the same source + device + screen.
        if (isFromSource(memento.source, AINPUT_SOURCE_CLASS_POINTER)) {
            bool merged = false;
            for (size_t j = 0; j < other.mMotionMementos.size(); j++) {
                MotionMemento& otherMemento = other.mMotionMementos[j];
                if (memento.deviceId == otherMemento.deviceId &&
                    memento.source == otherMemento.source &&
                    memento.displayId == otherMemento.displayId) {
                    memento.mergePointerStateTo(otherMemento);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                memento.firstNewPointerIdx = 0;
                other.mMotionMementos.push_back(memento);
            }
        }
    }
    return true;
}

std::optional<int32_t> InputState::getFallbackKey(int32_t originalKeyCode) {
    auto it = mFallbackKeys.find(originalKeyCode);
    if (it == mFallbackKeys.end()) {
        return {};
    }
    return it->second;
}

void InputState::setFallbackKey(int32_t originalKeyCode, int32_t fallbackKeyCode) {
    mFallbackKeys.insert_or_assign(originalKeyCode, fallbackKeyCode);
}

void InputState::removeFallbackKey(int32_t originalKeyCode) {
    mFallbackKeys.erase(originalKeyCode);
}

bool InputState::shouldCancelKey(const KeyMemento& memento, const CancellationOptions& options) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    if (options.externalDesktopStream) return false;
#endif
    if (options.keyCode && memento.keyCode != options.keyCode.value()) {
        return false;
    }

    if (options.deviceId && memento.deviceId != options.deviceId.value()) {
        return false;
    }

    if (options.displayId && memento.displayId != options.displayId.value()) {
        return false;
    }

    switch (options.mode) {
        case CancellationOptions::Mode::CANCEL_ALL_EVENTS:
        case CancellationOptions::Mode::CANCEL_NON_POINTER_EVENTS:
            return true;
        case CancellationOptions::Mode::CANCEL_FALLBACK_EVENTS:
            return memento.flags & AKEY_EVENT_FLAG_FALLBACK;
        default:
            return false;
    }
}

bool InputState::shouldCancelMotion(const MotionMemento& memento,
                                    const CancellationOptions& options) {
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
    if (options.externalDesktopStream && (!options.externalDesktopStream->valid() || !memento.externalDesktopPointerOwnership)) {
        return false;
    }
#endif
    if (options.deviceId && memento.deviceId != options.deviceId.value()) {
        return false;
    }

    if (options.displayId && memento.displayId != options.displayId.value()) {
        return false;
    }

    switch (options.mode) {
        case CancellationOptions::Mode::CANCEL_ALL_EVENTS:
            return true;
        case CancellationOptions::Mode::CANCEL_POINTER_EVENTS:
            return memento.source & AINPUT_SOURCE_CLASS_POINTER;
        case CancellationOptions::Mode::CANCEL_NON_POINTER_EVENTS:
            return !(memento.source & AINPUT_SOURCE_CLASS_POINTER);
        case CancellationOptions::Mode::CANCEL_HOVER_EVENTS:
            return memento.hovering;
        default:
            return false;
    }
}

std::ostream& operator<<(std::ostream& out, const InputState& state) {
    if (!state.mMotionMementos.empty()) {
        out << "mMotionMementos: ";
        for (const InputState::MotionMemento& memento : state.mMotionMementos) {
            out << "{deviceId=" << memento.deviceId
                << ", hovering=" << std::to_string(memento.hovering)
                << ", downTime=" << memento.downTime << "ns}, ";
        }
    }
    return out;
}

} // namespace android::inputdispatcher
