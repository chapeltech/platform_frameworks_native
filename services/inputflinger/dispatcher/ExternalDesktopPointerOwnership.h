#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <android-base/result.h>
#include <input/Input.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace android::inputdispatcher {

// PRIVATE transport values, NOT authority or an allocator. A future trusted
// native lifetime/stream registry must supply them. Never derive these from a
// StageTag, geometry epoch, downTime, raw pointer, APK or channel token alone.
struct ExternalDesktopPointerStreamKey {
    uint64_t domain;
    int64_t generation;
    uint64_t windowLifetime;
    uint64_t presentationLifetime;
    uint64_t streamInstance;

    bool valid() const {
        return domain != 0 && generation > 0 && windowLifetime != 0 &&
                presentationLifetime != 0 && streamInstance != 0;
    }
    bool operator==(const ExternalDesktopPointerStreamKey&) const = default;
};

class ExternalDesktopPointerOwnership;
using ExternalDesktopPointerOwnershipPtr = std::shared_ptr<const ExternalDesktopPointerOwnership>;

// Owns the COMPLETE pointer roster when present; a row may explicitly be
// ordinary/null. nullptr is the ordinary path, without per-event allocation.
// The object is immutable after construction and never borrows caller buffers.
class ExternalDesktopPointerOwnership final {
public:
    static constexpr size_t kMaxIds = 32;
    static_assert(MAX_POINTER_ID + 1 == kMaxIds);
    using Ids = std::bitset<kMaxIds>;
    struct Row {
        int32_t id;
        std::optional<ExternalDesktopPointerStreamKey> key;
    };

    static android::base::Result<ExternalDesktopPointerOwnershipPtr> create(std::span<const Row> rows) {
        if (rows.empty() || rows.size() > kMaxIds) {
            return android::base::Error() << "Bounded nonempty pointer ownership roster required";
        }
        Ids ids;
        std::array<std::optional<ExternalDesktopPointerStreamKey>, kMaxIds> keys{};
        bool owned = false;
        for (const Row& row : rows) {
            if (row.id < 0 || row.id > MAX_POINTER_ID || ids.test(row.id) ||
                (row.key && !row.key->valid())) {
                return android::base::Error() << "Duplicate/invalid private pointer ownership";
            }
            ids.set(row.id);
            keys[row.id] = row.key;
            owned |= row.key.has_value();
        }
        if (!owned) return ExternalDesktopPointerOwnershipPtr{};
        return ExternalDesktopPointerOwnershipPtr(new ExternalDesktopPointerOwnership(ids, keys));
    }

    static bool idsOf(const std::vector<PointerProperties>& properties, Ids& ids) {
        ids.reset();
        if (properties.empty() || properties.size() > kMaxIds) return false;
        for (const auto& property : properties) {
            if (property.id < 0 || property.id > MAX_POINTER_ID || ids.test(property.id)) {
                return false;
            }
            ids.set(property.id);
        }
        return true;
    }

    bool matches(const std::vector<PointerProperties>& properties) const {
        Ids ids;
        return idsOf(properties, ids) && ids == mIds;
    }
    const Ids& ids() const { return mIds; }
    std::optional<ExternalDesktopPointerStreamKey> key(int32_t id) const {
        if (id < 0 || id > MAX_POINTER_ID || !mIds.test(id)) return std::nullopt;
        return mKeys[static_cast<size_t>(id)];
    }

    static std::optional<ExternalDesktopPointerStreamKey> keyAt(const ExternalDesktopPointerOwnershipPtr& value,
                                                     int32_t id) {
        return value ? value->key(id) : std::nullopt;
    }

    static android::base::Result<ExternalDesktopPointerOwnershipPtr> select(
            const ExternalDesktopPointerOwnershipPtr& value,
            const std::vector<PointerProperties>& properties) {
        if (!value) return ExternalDesktopPointerOwnershipPtr{};
        Ids ids;
        if (!idsOf(properties, ids) || (ids & ~value->mIds).any()) {
            return android::base::Error() << "Derived pointer ownership roster mismatch";
        }
        if (ids == value->mIds) return value;
        std::array<Row, kMaxIds> rows{};
        size_t count = 0;
        for (size_t id = 0; id < kMaxIds; ++id) {
            if (ids.test(id)) rows[count++] = Row{static_cast<int32_t>(id), value->mKeys[id]};
        }
        return create(std::span<const Row>(rows.data(), count));
    }

    static android::base::Result<ExternalDesktopPointerOwnershipPtr> merge(
            const ExternalDesktopPointerOwnershipPtr& left, Ids leftIds,
            const ExternalDesktopPointerOwnershipPtr& right, Ids rightIds) {
        if (!left && !right) return ExternalDesktopPointerOwnershipPtr{};
        if ((left && leftIds != left->mIds) || (right && rightIds != right->mIds) ||
            (leftIds & rightIds).any() || (leftIds | rightIds).none()) {
            return android::base::Error() << "Pointer aggregation ownership mismatch";
        }
        std::array<Row, kMaxIds> rows{};
        size_t count = 0;
        for (size_t id = 0; id < kMaxIds; ++id) {
            if (leftIds.test(id)) rows[count++] = Row{static_cast<int32_t>(id), keyAt(left, static_cast<int32_t>(id))};
            if (rightIds.test(id)) rows[count++] = Row{static_cast<int32_t>(id), keyAt(right, static_cast<int32_t>(id))};
        }
        return create(std::span<const Row>(rows.data(), count));
    }

private:
    ExternalDesktopPointerOwnership(Ids ids,
                          std::array<std::optional<ExternalDesktopPointerStreamKey>, kMaxIds> keys)
          : mIds(ids), mKeys(keys) {}
    const Ids mIds;
    const std::array<std::optional<ExternalDesktopPointerStreamKey>, kMaxIds> mKeys;
};

} // namespace android::inputdispatcher
#endif
