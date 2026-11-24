#pragma once

#include "LayerLifecycleManager.h"
#include "LayerSnapshotBuilder.h"
#include <gui/LayerSourceInfo.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <functional>

namespace android::surfaceflinger::frontend {
inline std::array<float, 6> layerSourceMatrix(const ui::Transform& t) {
    return {t.dsdx(), t.dtdx(), t.dtdy(), t.dsdy(), t.tx(), t.ty()};
}
using LayerSourceLifetimeResolver = std::function<std::optional<gui::LayerSourceLifetime>(uint32_t)>;
inline std::optional<gui::LayerSourceNode> layerSourceRequested(
        const RequestedLayerState& layer, const LayerSourceLifetimeResolver& resolve) {
    const auto lifetime = resolve(layer.id);
    if (!lifetime || !lifetime->identity) return {};
    const auto& input = layer.getWindowInfo();
    return gui::LayerSourceNode{layer.id, layer.parentId, layer.relativeParentId, layer.layerIdToMirror,
            layer.croppedByLayerId, layer.stopLayerId, layer.ownerUid.val(), input.inputConfig.get(),
            static_cast<uint32_t>(layer.dropInputMode), layer.handleAlive,
            input.token != nullptr, input.windowToken != nullptr,
            {layer.crop.left, layer.crop.top, layer.crop.right, layer.crop.bottom},
            layerSourceMatrix(layer.requestedTransform), *lifetime, layer.externalDesktopOwnedInputBlockOrigin};
}
inline gui::LayerSourceApplied layerSourceApplied(const LayerSnapshot& snapshot) {
    const auto& input = snapshot.inputInfo;
    const auto& bounds = snapshot.geomLayerBounds;
    return {input.id, input.displayId.val(), input.inputConfig.get(),
            static_cast<uint32_t>(snapshot.nativeOwnedInputBlock.all
                    ? gui::DropInputMode::ALL : snapshot.dropInputMode),
            static_cast<uint32_t>(snapshot.nativeParentDropInputMode),
            input.token != nullptr, input.windowToken != nullptr, snapshot.hasInputInfo(), snapshot.canReceiveInput(),
            {input.frame.left, input.frame.top, input.frame.right, input.frame.bottom},
            {bounds.left, bounds.top, bounds.right, bounds.bottom},
            layerSourceMatrix(input.transform), layerSourceMatrix(snapshot.localTransform),
            layerSourceMatrix(snapshot.geomLayerTransform), snapshot.nativeSnapshotIdentity,
            snapshot.nativeStockProjectionComplete, snapshot.nativeStockInputConfig,
            static_cast<uint32_t>(snapshot.dropInputMode), snapshot.nativeOwnedInputBlock.all,
            snapshot.nativeOwnedInputBlock.origin, snapshot.nativeOwnedInputBlock.revision,
            snapshot.nativeOwnedInputBlock.epoch};
}
inline std::optional<gui::LayerSourceInfo> buildLayerSourceInfo(
        const LayerSnapshot& snapshot, uint32_t rowIndex,
        const LayerLifecycleManager& lifecycle, const LayerSnapshotBuilder& builder,
        const LayerSourceLifetimeResolver& resolve, gui::LayerSourceBudget& budget) {
    if (!gui::LayerSourceBudget::take(budget.rows, 1) ||
        !gui::LayerSourceBudget::take(budget.bytes, gui::LayerSourceInfo::kRowBytes + gui::LayerSourceInfo::kAppliedObjectOffsets)) return {};
    gui::LayerSourceInfo result{};
    result.rowIndex = rowIndex; result.sourceId = snapshot.path.id;
    result.variant = static_cast<uint32_t>(snapshot.path.variant);
    result.detached = snapshot.path.detached; result.relativeLoop = snapshot.path.hasRelZLoop();
    if (snapshot.path.mirrorRootIds.size() > gui::LayerSourceInfo::kMaxMirrors ||
        snapshot.path.relativeRootIds.size() > gui::LayerSourceInfo::kMaxAncestors ||
        !gui::LayerSourceBudget::take(budget.pathIds,
                snapshot.path.mirrorRootIds.size() + snapshot.path.relativeRootIds.size()) ||
        !gui::LayerSourceBudget::take(budget.bytes,
                (snapshot.path.mirrorRootIds.size() + snapshot.path.relativeRootIds.size()) * sizeof(uint32_t))) return {};
    result.mirrorRoots.assign(snapshot.path.mirrorRootIds.begin(), snapshot.path.mirrorRootIds.end());
    result.relativeRoots.assign(snapshot.path.relativeRootIds.begin(), snapshot.path.relativeRootIds.end());
    uint32_t id = snapshot.path.id;
    while (id != UNASSIGNED_LAYER_ID) {
        if (result.ancestors.size() == gui::LayerSourceInfo::kMaxAncestors ||
            std::any_of(result.ancestors.begin(), result.ancestors.end(),
                        [&](const auto& parent) { return parent.id == id; })) return {};
        const auto* layer = lifecycle.getLayerFromId(id);
        if (!layer || !gui::LayerSourceBudget::take(budget.nodes, 1) ||
            !gui::LayerSourceBudget::take(budget.bytes, gui::LayerSourceInfo::kNodeBytes + gui::LayerSourceInfo::kNodeObjectOffsets)) return {};
        const auto node = layerSourceRequested(*layer, resolve);
        if (!node) return {};
        result.ancestors.push_back(*node);
        id = layer->parentId;
    }
    const LayerSnapshot* actual = &snapshot;
    while (actual->path.id != UNASSIGNED_LAYER_ID) {
        const auto& path = actual->path;
        if (result.traversal.size() == gui::LayerSourceInfo::kMaxAncestors ||
            path.mirrorRootIds.size() > gui::LayerSourceInfo::kMaxMirrors ||
            path.relativeRootIds.size() > gui::LayerSourceInfo::kMaxAncestors ||
            !gui::LayerSourceBudget::take(budget.nodes, 1) ||
            !gui::LayerSourceBudget::take(budget.bytes, gui::LayerSourceInfo::kTraversalBytes + gui::LayerSourceInfo::kNodeObjectOffsets + gui::LayerSourceInfo::kAppliedObjectOffsets) ||
            !gui::LayerSourceBudget::take(budget.pathIds,
                    path.mirrorRootIds.size() + path.relativeRootIds.size()) ||
            !gui::LayerSourceBudget::take(budget.bytes,
                    (path.mirrorRootIds.size() + path.relativeRootIds.size()) * sizeof(uint32_t))) return {};
        gui::LayerSourceTraversal edge{};
        edge.sourceId = path.id; edge.variant = static_cast<uint32_t>(path.variant);
        edge.detached = path.detached; edge.relativeLoop = path.hasRelZLoop();
        edge.mirrorRoots.assign(path.mirrorRootIds.begin(), path.mirrorRootIds.end());
        edge.relativeRoots.assign(path.relativeRootIds.begin(), path.relativeRootIds.end());
        const auto* requested = lifecycle.getLayerFromId(path.id);
        const auto node = requested ? layerSourceRequested(*requested, resolve) : std::nullopt;
        if (!node) return {};
        edge.requested = *node;
        edge.applied = layerSourceApplied(*actual);
        if (std::any_of(result.traversal.begin(), result.traversal.end(), [&](const auto& old) {
                return old.sourceId == edge.sourceId && old.variant == edge.variant &&
                        old.mirrorRoots == edge.mirrorRoots && old.relativeRoots == edge.relativeRoots &&
                        old.detached == edge.detached && old.relativeLoop == edge.relativeLoop;
            }) || !actual->nativeTraversalParent) return {};
        result.traversal.push_back(std::move(edge));
        if (actual->nativeTraversalParent->id == UNASSIGNED_LAYER_ID) break;
        actual = builder.getSnapshot(*actual->nativeTraversalParent);
        if (!actual) return {};
    }
    std::vector<uint32_t> prefix;
    for (uint32_t root : result.mirrorRoots) {
        const gui::LayerSourceTraversal* exact = nullptr;
        for (const auto& edge : result.traversal) if (edge.sourceId == root && edge.mirrorRoots == prefix) {
            if (exact) return {}; // ambiguous actual parent, never guess a path
            exact = &edge;
        }
        if (!exact || !gui::LayerSourceBudget::take(budget.nodes, 1) ||
            !gui::LayerSourceBudget::take(budget.bytes,
                    gui::LayerSourceInfo::kNodeBytes + gui::LayerSourceInfo::kAppliedBytes + gui::LayerSourceInfo::kNodeObjectOffsets + gui::LayerSourceInfo::kAppliedObjectOffsets)) return {};
        result.roots.push_back({exact->requested, exact->applied});
        prefix.push_back(root);
    }
    result.applied = layerSourceApplied(snapshot);
    if (!gui::layerSourceMatchesWindow(result, snapshot.inputInfo, rowIndex)) return {};
    return result;
}
inline std::optional<gui::LayerSourceInfo> buildLayerSourceInfo(
        const LayerSnapshot& snapshot, uint32_t rowIndex,
        const LayerLifecycleManager& lifecycle, const LayerSnapshotBuilder& builder,
        const LayerSourceLifetimeResolver& resolve) {
    gui::LayerSourceBudget budget;
    return buildLayerSourceInfo(snapshot, rowIndex, lifecycle, builder, resolve, budget);
}
} // namespace android::surfaceflinger::frontend
