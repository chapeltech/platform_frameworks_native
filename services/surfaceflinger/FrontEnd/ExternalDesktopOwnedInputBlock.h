#pragma once

#include <cstring>
#include <cstdint>
#include <binder/IBinder.h>
#include <gui/WindowInfo.h>
#include <private/android_filesystem_config.h>
#include <sys/types.h>

namespace android::surfaceflinger {
// PRIVATE restrictive-root creation provenance only. This is NOT a lease,
// stage admission, tag issuer, permission to clear DROP or claim authority.
// Both values come from the kernel Binder transaction, never layer metadata.
inline bool externalDesktopOwnedBlockCreator(uid_t uid, const char* sid) {
    return uid == AID_SYSTEM && sid != nullptr &&
            std::strcmp(sid, "u:r:system_server:s0") == 0;
}

// Per-ACTUAL-root mutable restriction, separate from its immutable origin.
// Serialized by SF's main-thread lifecycle owner. ARM/PARK always advances the
// revision, even when already blocked: its real input update is the fresh B1.
struct ExternalDesktopOwnedInputBlockControl {
    enum class Operation : int32_t { REPARK_CLEAR_TAG = 0, ARM_TAGGED_DROP = 1, ENABLE_SAME_TAG = 2 };
    bool blocked = true;
    uint64_t revision = 1;
    int64_t epoch = 0; // creation is unarmed; an unarmed root cannot be enabled
    std::optional<gui::WindowInfo::StageTag> stageTag;
    bool accepts(uint64_t expectedRevision, int64_t nextEpoch, bool nextBlocked) const {
        return !stageTag && expectedRevision == revision && revision < uint64_t(INT64_MAX) &&
                nextEpoch > 0 && (nextBlocked ? nextEpoch >= epoch : nextEpoch == epoch);
    }
    void advance(int64_t nextEpoch, bool nextBlocked) noexcept {
        ++revision; epoch = nextEpoch; blocked = nextBlocked;
    }
    static bool validTag(const gui::WindowInfo::StageTag& tag) {
        return tag.authorityGeneration > 0 && tag.atlasRevision >= 0 && tag.geometryRevision >= 0 &&
                tag.epoch > 0 && tag.presentationKey != 0;
    }
    bool acceptsState(uint64_t expectedRevision, int64_t nextEpoch, Operation operation,
                      const std::optional<gui::WindowInfo::StageTag>& tag) const {
        if (expectedRevision != revision || revision >= uint64_t(INT64_MAX) || nextEpoch <= 0) return false;
        switch (operation) {
            case Operation::REPARK_CLEAR_TAG:
                return !tag && nextEpoch >= epoch;
            case Operation::ARM_TAGGED_DROP:
                return blocked && !stageTag && tag && validTag(*tag) && tag->epoch == nextEpoch && nextEpoch >= epoch;
            case Operation::ENABLE_SAME_TAG:
                return blocked && stageTag && tag == stageTag && nextEpoch == epoch;
        }
        return false;
    }
    void advanceState(int64_t nextEpoch, Operation operation,
                      const std::optional<gui::WindowInfo::StageTag>& tag) noexcept {
        ++revision; epoch = nextEpoch;
        blocked = operation != Operation::ENABLE_SAME_TAG;
        stageTag = operation == Operation::REPARK_CLEAR_TAG ? std::nullopt : tag;
    }
};

// Derived traversal state, never a setter for the creation-only origin.
// Multiple independent origins stay actually blocked but have unavailable
// provenance until a separately reviewed multi-origin consumer exists.
struct ExternalDesktopOwnedInputBlockState {
    bool all = false;
    bool covered = true;
    sp<IBinder> origin;
    uint64_t revision = 0;
    int64_t epoch = 0;
    uint32_t root = 0;
    std::optional<gui::WindowInfo::StageTag> stageTag;
    void inherit(const ExternalDesktopOwnedInputBlockState& parent, const sp<IBinder>& ownOrigin,
                 const ExternalDesktopOwnedInputBlockControl& control = {}, uint32_t ownRoot = 0) {
        all = parent.all || (ownOrigin != nullptr && control.blocked);
        covered = parent.covered;
        origin = parent.origin;
        revision = parent.revision; epoch = parent.epoch;
        root = parent.root; stageTag = parent.stageTag;
        if (ownOrigin && origin && ownOrigin != origin) covered = false;
        if (!covered) { all = true; origin.clear(); revision = 0; epoch = 0; root = 0; stageTag.reset(); }
        else if (ownOrigin) {
            origin = ownOrigin; revision = control.revision; epoch = control.epoch;
            root = ownRoot; stageTag = control.stageTag;
        }
        if (stageTag && (!origin || root == 0)) {
            covered = false; all = true; origin.clear(); revision = 0; epoch = 0; root = 0; stageTag.reset();
        }
        // Never discard parent.all or infer an enable from missing metadata.
    }
};
} // namespace android::surfaceflinger
