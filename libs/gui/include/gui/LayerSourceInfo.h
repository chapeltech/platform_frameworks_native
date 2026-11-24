#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <android/gui/DropInputMode.h>
#include <gui/WindowInfo.h>

namespace android::gui {
struct LayerSourceLifetime {
    // SF-created read-only lifetime BBinder: NO Layer/SurfaceControl/FD/back-pointer.
    sp<IBinder> identity;
    // Actual originally-published handle, never recreated/promoted into
    // persistent history. Only scoped serialization/validation promotes it.
    wp<IBinder> publishedHandle;
};
// PRIVATE compositor facts, NOT RootClaim, authority, stage tags or input
// eligibility. IDs must later join actual retained SurfaceControl lifetimes.
// Requested facts and the separately composed snapshot are never conflated.
struct LayerSourceNode {
    uint32_t id, parent, relativeParent, mirrorSource, croppedBy, stopAt;
    uint32_t ownerUid, requestedInputConfig, requestedDropMode;
    bool handleAlive, hasInputToken, hasClientToken;
    // PRIVATE v6: actual FloatRect facts, never integer-truncated geometry.
    std::array<float, 4> requestedCrop;
    // Every six-float matrix is [dsdx,dtdx,dtdy,dsdy,tx,ty], i.e.
    // (a*x+b*y+tx, c*x+d*y+ty). Requested matrix is NOT applied geometry.
    std::array<float, 6> requestedTransform;
    LayerSourceLifetime lifetime;
    sp<IBinder> ownedInputBlockOrigin = nullptr; // immutable SF-created origin, NOT authority
};
struct LayerSourceApplied {
    int32_t windowId, displayId;
    uint32_t inputConfig, composedDropMode, inheritedDropMode;
    // SF's canReceiveInput() is visibility-like, NOT channel routability.
    bool hasInputToken, hasClientToken, hasInputInfo, snapshotCanReceiveInput;
    std::array<int32_t, 4> inputFrame;
    std::array<float, 4> layerBounds;
    std::array<float, 6> inputTransform, localTransform, layerTransform;
    sp<IBinder> snapshotIdentity; // NONRESOURCE actual owned snapshot observation
    // Explicit availability: historical/public fixture facts may lack this
    // projection and CANNOT be used as stock evidence by a StageLive adapter.
    bool stockProjectionComplete = false;
    uint32_t stockInputConfig = 0, stockDropMode = 0;
    bool ownedDropAll = false;
    sp<IBinder> ownedInputBlockOrigin = nullptr;
    uint64_t ownedInputBlockRevision = 0;
    int64_t ownedInputBlockEpoch = 0;
    bool operator==(const LayerSourceApplied&) const = default;
};
struct LayerSourceRoot {
    LayerSourceNode requested;
    LayerSourceApplied applied;
};
struct LayerSourceTraversal {
    uint32_t sourceId, variant;
    bool detached, relativeLoop;
    std::vector<uint32_t> mirrorRoots, relativeRoots;
    LayerSourceNode requested;
    LayerSourceApplied applied;
};
struct LayerSourceInfo {
    static constexpr size_t kMaxRows = 8192;
    static constexpr size_t kMaxAncestors = 64;
    static constexpr size_t kMaxMirrors = WindowInfo::kMaxCloneLineageDepth;
    static constexpr size_t kMaxNodes = 65536;
    static constexpr size_t kMaxPathIds = 262144; // WHOLE companion, not per row
    // Unavailable-only observer limit, NOT a promise of full large-scene
    // coverage. Repeated ancestry is bounded; deduplicated tables remain OPEN.
    static constexpr size_t kMaxBytes = 128 * 1024;
    // v6 also charges Binder24+offset8+publication serial8+main-pass8.
    static constexpr size_t kHeaderBytes = 60, kRowBytes = 260;
    // Coherent Android64 Binder: each flat object24B plus8B object offset.
    // Three objects/node (+nonresource owned-block origin), two/applied.
    static constexpr size_t kNodeBytes = 160, kAppliedBytes = 220, kTraversalBytes = 404;
    static constexpr size_t kNodeObjectOffsets = 24;
    static constexpr size_t kAppliedObjectOffsets = 16;
    uint32_t rowIndex, sourceId, variant;
    bool detached, relativeLoop;
    // Complete ordered paths, never first/last-root-only projections.
    std::vector<uint32_t> mirrorRoots, relativeRoots;
    // Leaf FIRST, then actual original-parent edges to native root. These are
    // actual RequestedLayerStates, including non-input Task/Activity nodes.
    std::vector<LayerSourceNode> ancestors;
    // Separate ACTUAL composed-snapshot parent paths, leaf first. Requested
    // parent IDs alone are never proof of applied/root membership.
    std::vector<LayerSourceTraversal> traversal;
    // One native mirror relation/snapshot for EACH ordered mirror-root prefix.
    std::vector<LayerSourceRoot> roots;
    LayerSourceApplied applied;
};

// Charged BEFORE each companion allocation, across the WHOLE exported batch.
// No requested/ancestor fact is itself evidence that a row is owned.
struct LayerSourceBudget {
    size_t rows = LayerSourceInfo::kMaxRows;
    size_t nodes = LayerSourceInfo::kMaxNodes;
    size_t pathIds = LayerSourceInfo::kMaxPathIds;
    size_t bytes = LayerSourceInfo::kMaxBytes - LayerSourceInfo::kHeaderBytes;
    static bool take(size_t& left, size_t count) {
        if (count > left) return false;
        left -= count;
        return true;
    }
};

inline std::array<float, 6> layerSourceInputMatrix(const ui::Transform& t) {
    return {t.dsdx(), t.dtdx(), t.dtdy(), t.dsdy(), t.tx(), t.ty()};
}
inline bool layerSourceStockProjectionValid(const LayerSourceApplied& value) {
    if (!value.stockProjectionComplete) {
        // Explicit unavailable, never a guessed baseline or an owned-block proof.
        return value.stockInputConfig == 0 && value.stockDropMode == 0 &&
                !value.ownedDropAll && !value.ownedInputBlockOrigin &&
                value.ownedInputBlockRevision == 0 && value.ownedInputBlockEpoch == 0;
    }
    if (value.stockDropMode > static_cast<uint32_t>(DropInputMode::OBSCURED) ||
            value.ownedInputBlockEpoch < 0 ||
            (value.ownedInputBlockOrigin ? value.ownedInputBlockRevision == 0 ||
                value.ownedInputBlockRevision > uint64_t(INT64_MAX) ||
                (!value.ownedDropAll && value.ownedInputBlockEpoch == 0) :
                value.ownedDropAll || value.ownedInputBlockRevision != 0 ||
                    value.ownedInputBlockEpoch != 0)) return false;
    uint32_t expected = value.stockInputConfig;
    if (value.ownedDropAll && !(expected & static_cast<uint32_t>(WindowInfo::InputConfig::NO_INPUT_CHANNEL))) {
        expected |= static_cast<uint32_t>(WindowInfo::InputConfig::DROP_INPUT);
    }
    return value.inputConfig == expected && value.composedDropMode ==
            (value.ownedDropAll ? static_cast<uint32_t>(DropInputMode::ALL) : value.stockDropMode);
}
inline bool layerSourceFinite(const LayerSourceApplied& value) {
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(), [](float x) { return std::isfinite(x); });
    };
    return value.snapshotIdentity && layerSourceStockProjectionValid(value) &&
            finite(value.inputTransform) && finite(value.localTransform) &&
            finite(value.layerTransform) && finite(value.layerBounds);
}
inline bool layerSourceFinite(const LayerSourceNode& value) {
    return value.lifetime.identity &&
            std::all_of(value.requestedCrop.begin(), value.requestedCrop.end(),
                       [](float x) { return std::isfinite(x); }) &&
            std::all_of(value.requestedTransform.begin(), value.requestedTransform.end(),
                       [](float x) { return std::isfinite(x); });
}
// Compare against the FINAL row in this same WindowInfosUpdate. Touchable
// regions, Binder identities and all other stock fields are consumed from that
// row itself, never reconstructed from requested facts or token-presence bits.
inline bool layerSourceMatchesWindow(const LayerSourceInfo& facts, const WindowInfo& window,
                                     size_t index) {
    const auto& applied = facts.applied;
    if (facts.rowIndex != index || facts.ancestors.empty() || facts.traversal.empty() ||
        facts.ancestors.front().id != facts.sourceId ||
        facts.traversal.front().sourceId != facts.sourceId ||
        facts.traversal.front().variant != facts.variant ||
        facts.traversal.front().mirrorRoots != facts.mirrorRoots ||
        facts.traversal.front().relativeRoots != facts.relativeRoots ||
        facts.traversal.front().detached != facts.detached ||
        facts.traversal.front().relativeLoop != facts.relativeLoop ||
        facts.traversal.front().applied.snapshotIdentity != applied.snapshotIdentity ||
        applied.windowId != window.id || applied.displayId != window.displayId.val() ||
        applied.inputConfig != window.inputConfig.get() ||
        applied.hasInputToken != (window.token != nullptr) ||
        applied.hasClientToken != (window.windowToken != nullptr) ||
        applied.inputFrame != std::array<int32_t, 4>{window.frame.left, window.frame.top,
                                                   window.frame.right, window.frame.bottom} ||
        applied.inputTransform != layerSourceInputMatrix(window.transform) ||
        !layerSourceFinite(applied)) return false;
    for (const auto& node : facts.ancestors) if (!layerSourceFinite(node)) return false;
    for (const auto& root : facts.roots) {
        if (!layerSourceFinite(root.requested) || !layerSourceFinite(root.applied)) return false;
    }
    if (facts.roots.size() != facts.mirrorRoots.size()) return false;
    for (size_t i=0;i<facts.roots.size();++i) {
        if (facts.roots[i].requested.id != facts.mirrorRoots[i]) return false;
    }
    for (const auto& edge : facts.traversal) {
        if (edge.requested.id != edge.sourceId || !layerSourceFinite(edge.requested) ||
            !layerSourceFinite(edge.applied)) return false;
    }
    if (facts.mirrorRoots.empty()) return !window.cloneLineage;
    return window.cloneLineage && window.cloneLineage->sourceLayerId == facts.sourceId &&
            window.cloneLineage->mirrorRootIds == facts.mirrorRoots;
}

// Exact companion wire size (all fields use explicit Parcel primitives). This
// preflight also validates final exported-row correspondence before writing.
inline bool layerSourcesValid(const std::vector<LayerSourceInfo>& rows,
                              const std::vector<WindowInfo>& windows, bool complete) {
    if (!complete) return rows.empty();
    if (rows.size() != windows.size() || rows.size() > LayerSourceInfo::kMaxRows) return false;
    LayerSourceBudget budget;
    const auto ids = [&](const auto& values, size_t maximum) {
        return values.size() <= maximum && LayerSourceBudget::take(budget.pathIds, values.size()) &&
                LayerSourceBudget::take(budget.bytes, values.size() * sizeof(uint32_t));
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (!layerSourceMatchesWindow(row, windows[i], i) ||
            row.ancestors.size() > LayerSourceInfo::kMaxAncestors ||
            row.roots.size() != row.mirrorRoots.size() ||
            row.traversal.size() > LayerSourceInfo::kMaxAncestors ||
            !LayerSourceBudget::take(budget.rows, 1) ||
            !LayerSourceBudget::take(budget.bytes, LayerSourceInfo::kRowBytes + LayerSourceInfo::kAppliedObjectOffsets) ||
            !ids(row.mirrorRoots, LayerSourceInfo::kMaxMirrors) ||
            !ids(row.relativeRoots, LayerSourceInfo::kMaxAncestors) ||
            !LayerSourceBudget::take(budget.nodes, row.ancestors.size() + row.roots.size() + row.traversal.size()) ||
            !LayerSourceBudget::take(budget.bytes, row.ancestors.size() * (LayerSourceInfo::kNodeBytes + LayerSourceInfo::kNodeObjectOffsets)) ||
            !LayerSourceBudget::take(budget.bytes, row.roots.size() * (LayerSourceInfo::kNodeBytes + LayerSourceInfo::kAppliedBytes + LayerSourceInfo::kNodeObjectOffsets + LayerSourceInfo::kAppliedObjectOffsets)) ||
            !LayerSourceBudget::take(budget.bytes, row.traversal.size() * (LayerSourceInfo::kTraversalBytes + LayerSourceInfo::kNodeObjectOffsets + LayerSourceInfo::kAppliedObjectOffsets))) return false;
        for (const auto& edge : row.traversal) {
            if (!ids(edge.mirrorRoots, LayerSourceInfo::kMaxMirrors) ||
                !ids(edge.relativeRoots, LayerSourceInfo::kMaxAncestors)) return false;
        }
    }
    return true;
}
} // namespace android::gui
