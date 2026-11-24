/*
 * Copyright 2023 The Android Open Source Project
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

#include <gui/WindowInfosUpdate.h>
#include <private/gui/ParcelUtils.h>
#if defined(__ANDROID__)
#include <linux/android/binder.h>
#endif

namespace android::gui {
namespace {
constexpr uint32_t kLayerSourceVersion = 6; // PRIVATE explicit publication/lifetime-query contract
static_assert(sizeof(binder_size_t) == 8);
#if defined(__ANDROID__)
static_assert(sizeof(flat_binder_object) == 24);
#endif
status_t writeNode(Parcel* p, const LayerSourceNode& n) {
    if (!layerSourceFinite(n)) return BAD_VALUE;
    for (uint32_t value : {n.id, n.parent, n.relativeParent, n.mirrorSource, n.croppedBy,
                          n.stopAt, n.ownerUid, n.requestedInputConfig, n.requestedDropMode}) {
        SAFE_PARCEL(p->writeUint32, value);
    }
    for (bool value : {n.handleAlive, n.hasInputToken, n.hasClientToken}) {
        SAFE_PARCEL(p->writeUint32, value ? 1u : 0u);
    }
    for (float value : n.requestedCrop) SAFE_PARCEL(p->writeFloat, value);
    for (float value : n.requestedTransform) SAFE_PARCEL(p->writeFloat, value);
    if (!n.lifetime.identity) return BAD_VALUE;
    SAFE_PARCEL(p->writeStrongBinder, n.lifetime.identity);
    const auto existing = n.lifetime.publishedHandle.promote();
    SAFE_PARCEL(p->writeStrongBinder, existing); // scoped promotion ONLY
    SAFE_PARCEL(p->writeStrongBinder, n.ownedInputBlockOrigin);
    return OK;
}
status_t readBoolean(const Parcel* p, bool& out) {
    uint32_t value; SAFE_PARCEL(p->readUint32, &value);
    if (value > 1) return BAD_VALUE;
    out = value != 0; return OK;
}
status_t readNode(const Parcel* p, LayerSourceNode& n) {
    for (uint32_t* value : {&n.id, &n.parent, &n.relativeParent, &n.mirrorSource, &n.croppedBy,
                           &n.stopAt, &n.ownerUid, &n.requestedInputConfig, &n.requestedDropMode}) {
        SAFE_PARCEL(p->readUint32, value);
    }
    for (bool* value : {&n.handleAlive, &n.hasInputToken, &n.hasClientToken}) SAFE_PARCEL(readBoolean, p, *value);
    for (float& value : n.requestedCrop) SAFE_PARCEL(p->readFloat, &value);
    for (float& value : n.requestedTransform) SAFE_PARCEL(p->readFloat, &value);
    SAFE_PARCEL(p->readStrongBinder, &n.lifetime.identity);
    sp<IBinder> existing;
    SAFE_PARCEL(p->readNullableStrongBinder, &existing);
    n.lifetime.publishedHandle = existing; // persistent observation stays WEAK
    SAFE_PARCEL(p->readNullableStrongBinder, &n.ownedInputBlockOrigin);
    return layerSourceFinite(n) ? OK : BAD_VALUE;
}
status_t writeApplied(Parcel* p, const LayerSourceApplied& a) {
    SAFE_PARCEL(p->writeInt32, a.windowId); SAFE_PARCEL(p->writeInt32, a.displayId);
    for (uint32_t value : {a.inputConfig, a.composedDropMode, a.inheritedDropMode}) SAFE_PARCEL(p->writeUint32, value);
    for (bool value : {a.hasInputToken, a.hasClientToken, a.hasInputInfo, a.snapshotCanReceiveInput}) SAFE_PARCEL(p->writeUint32, value ? 1u : 0u);
    for (int32_t value : a.inputFrame) SAFE_PARCEL(p->writeInt32, value);
    for (float value : a.layerBounds) SAFE_PARCEL(p->writeFloat, value);
    for (const auto* matrix : {&a.inputTransform, &a.localTransform, &a.layerTransform}) {
        for (float value : *matrix) SAFE_PARCEL(p->writeFloat, value);
    }
    if (!a.snapshotIdentity) return BAD_VALUE;
    SAFE_PARCEL(p->writeStrongBinder, a.snapshotIdentity);
    SAFE_PARCEL(p->writeUint32, a.stockProjectionComplete ? 1u : 0u);
    SAFE_PARCEL(p->writeUint32, a.stockInputConfig);
    SAFE_PARCEL(p->writeUint32, a.stockDropMode);
    SAFE_PARCEL(p->writeUint32, a.ownedDropAll ? 1u : 0u);
    SAFE_PARCEL(p->writeStrongBinder, a.ownedInputBlockOrigin);
    SAFE_PARCEL(p->writeUint64, a.ownedInputBlockRevision);
    SAFE_PARCEL(p->writeInt64, a.ownedInputBlockEpoch);
    return OK;
}
status_t readApplied(const Parcel* p, LayerSourceApplied& a) {
    SAFE_PARCEL(p->readInt32, &a.windowId); SAFE_PARCEL(p->readInt32, &a.displayId);
    for (uint32_t* value : {&a.inputConfig, &a.composedDropMode, &a.inheritedDropMode}) SAFE_PARCEL(p->readUint32, value);
    for (bool* value : {&a.hasInputToken, &a.hasClientToken, &a.hasInputInfo, &a.snapshotCanReceiveInput}) SAFE_PARCEL(readBoolean, p, *value);
    for (int32_t& value : a.inputFrame) SAFE_PARCEL(p->readInt32, &value);
    for (float& value : a.layerBounds) SAFE_PARCEL(p->readFloat, &value);
    for (auto* matrix : {&a.inputTransform, &a.localTransform, &a.layerTransform}) {
        for (float& value : *matrix) SAFE_PARCEL(p->readFloat, &value);
    }
    SAFE_PARCEL(p->readStrongBinder, &a.snapshotIdentity);
    SAFE_PARCEL(readBoolean, p, a.stockProjectionComplete);
    SAFE_PARCEL(p->readUint32, &a.stockInputConfig);
    SAFE_PARCEL(p->readUint32, &a.stockDropMode);
    SAFE_PARCEL(readBoolean, p, a.ownedDropAll);
    SAFE_PARCEL(p->readNullableStrongBinder, &a.ownedInputBlockOrigin);
    SAFE_PARCEL(p->readUint64, &a.ownedInputBlockRevision);
    SAFE_PARCEL(p->readInt64, &a.ownedInputBlockEpoch);
    return OK;
}
status_t readIds(const Parcel* p, std::vector<uint32_t>& out, size_t maximum, size_t& totalIds,
                 size_t& bytesLeft) {
    uint32_t count; SAFE_PARCEL(p->readUint32, &count);
    if (count > maximum || count > LayerSourceInfo::kMaxPathIds - totalIds ||
        !LayerSourceBudget::take(bytesLeft, count * sizeof(uint32_t))) return BAD_VALUE;
    totalIds += count;
    out.resize(count);
    for (auto& id : out) SAFE_PARCEL(p->readUint32, &id);
    return OK;
}
status_t writeIds(Parcel* p, const std::vector<uint32_t>& ids, size_t maximum, size_t& totalIds) {
    if (ids.size() > maximum || ids.size() > LayerSourceInfo::kMaxPathIds - totalIds) return BAD_VALUE;
    totalIds += ids.size();
    SAFE_PARCEL(p->writeUint32, static_cast<uint32_t>(ids.size()));
    for (auto id : ids) SAFE_PARCEL(p->writeUint32, id);
    return OK;
}
status_t readSources(const Parcel* p, const std::vector<WindowInfo>& windows, bool& complete,
                     std::vector<LayerSourceInfo>& out, NativePublication& publication) {
    uint32_t version, count; bool available;
    SAFE_PARCEL(p->readUint32, &version);
    if (version != kLayerSourceVersion) return BAD_VALUE;
    NativePublication candidatePublication;
    SAFE_PARCEL(readNativePublication,p,candidatePublication);
    SAFE_PARCEL(readBoolean, p, available);
    if (!available && candidatePublication.available()) return BAD_VALUE;
    SAFE_PARCEL(p->readUint32, &count);
    size_t bytesLeft = LayerSourceInfo::kMaxBytes - LayerSourceInfo::kHeaderBytes;
    if (count > LayerSourceInfo::kMaxRows || (available ? count != windows.size() : count != 0) ||
        !LayerSourceBudget::take(bytesLeft, count * (LayerSourceInfo::kRowBytes + LayerSourceInfo::kAppliedObjectOffsets))) return BAD_VALUE;
    std::vector<LayerSourceInfo> candidate(count);
    size_t totalNodes = 0;
    size_t totalIds = 0;
    for (size_t i = 0; i < count; ++i) {
        auto& info = candidate[i];
        SAFE_PARCEL(p->readUint32, &info.rowIndex); SAFE_PARCEL(p->readUint32, &info.sourceId);
        SAFE_PARCEL(p->readUint32, &info.variant);
        SAFE_PARCEL(readBoolean, p, info.detached); SAFE_PARCEL(readBoolean, p, info.relativeLoop);
        SAFE_PARCEL(readIds, p, info.mirrorRoots, LayerSourceInfo::kMaxMirrors, totalIds, bytesLeft);
        SAFE_PARCEL(readIds, p, info.relativeRoots, LayerSourceInfo::kMaxAncestors, totalIds, bytesLeft);
        uint32_t ancestors, roots, traversal;
        SAFE_PARCEL(p->readUint32, &ancestors);
        if (info.rowIndex != i || ancestors == 0 || ancestors > LayerSourceInfo::kMaxAncestors ||
            ancestors > LayerSourceInfo::kMaxNodes - totalNodes ||
            !LayerSourceBudget::take(bytesLeft, ancestors * (LayerSourceInfo::kNodeBytes + LayerSourceInfo::kNodeObjectOffsets))) return BAD_VALUE;
        totalNodes += ancestors; info.ancestors.resize(ancestors);
        for (auto& node : info.ancestors) SAFE_PARCEL(readNode, p, node);
        SAFE_PARCEL(p->readUint32, &roots);
        if (roots != info.mirrorRoots.size() || roots > LayerSourceInfo::kMaxMirrors ||
            roots > LayerSourceInfo::kMaxNodes - totalNodes ||
            !LayerSourceBudget::take(bytesLeft, roots * (LayerSourceInfo::kNodeBytes + LayerSourceInfo::kAppliedBytes + LayerSourceInfo::kNodeObjectOffsets + LayerSourceInfo::kAppliedObjectOffsets))) return BAD_VALUE;
        totalNodes += roots; info.roots.resize(roots);
        for (auto& root : info.roots) {
            SAFE_PARCEL(readNode, p, root.requested); SAFE_PARCEL(readApplied, p, root.applied);
        }
        SAFE_PARCEL(p->readUint32, &traversal);
        if (traversal == 0 || traversal > LayerSourceInfo::kMaxAncestors ||
            traversal > LayerSourceInfo::kMaxNodes - totalNodes ||
            !LayerSourceBudget::take(bytesLeft, traversal * (LayerSourceInfo::kTraversalBytes + LayerSourceInfo::kNodeObjectOffsets + LayerSourceInfo::kAppliedObjectOffsets))) return BAD_VALUE;
        totalNodes += traversal; info.traversal.resize(traversal);
        for (auto& edge : info.traversal) {
            SAFE_PARCEL(p->readUint32, &edge.sourceId); SAFE_PARCEL(p->readUint32, &edge.variant);
            SAFE_PARCEL(readBoolean, p, edge.detached); SAFE_PARCEL(readBoolean, p, edge.relativeLoop);
            SAFE_PARCEL(readIds, p, edge.mirrorRoots, LayerSourceInfo::kMaxMirrors, totalIds, bytesLeft);
            SAFE_PARCEL(readIds, p, edge.relativeRoots, LayerSourceInfo::kMaxAncestors, totalIds, bytesLeft);
            SAFE_PARCEL(readNode, p, edge.requested);
            SAFE_PARCEL(readApplied, p, edge.applied);
        }
        SAFE_PARCEL(readApplied, p, info.applied);
        if (!layerSourceMatchesWindow(info, windows[i], i)) return BAD_VALUE;
    }
    complete = available; out = std::move(candidate); // only complete valid payload commits
    publication=std::move(candidatePublication);
    return OK;
}
status_t writeSources(Parcel* p, const std::vector<WindowInfo>& windows, bool complete,
                      const std::vector<LayerSourceInfo>& rows, const NativePublication& publication) {
    if (!layerSourcesValid(rows, windows, complete) || !publication.valid() ||
            (!complete && publication.available())) return BAD_VALUE;
    SAFE_PARCEL(p->writeUint32, kLayerSourceVersion);
    SAFE_PARCEL(writeNativePublication,p,publication);
    SAFE_PARCEL(p->writeUint32, complete ? 1u : 0u);
    SAFE_PARCEL(p->writeUint32, static_cast<uint32_t>(rows.size()));
    size_t totalNodes = 0;
    size_t totalIds = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& info = rows[i];
        if (info.rowIndex != i || info.ancestors.empty() || info.ancestors.size() > LayerSourceInfo::kMaxAncestors ||
            info.roots.size() != info.mirrorRoots.size() || info.roots.size() > LayerSourceInfo::kMaxMirrors ||
            info.traversal.empty() || info.traversal.size() > LayerSourceInfo::kMaxAncestors ||
            info.ancestors.size() + info.roots.size() + info.traversal.size() > LayerSourceInfo::kMaxNodes - totalNodes) return BAD_VALUE;
        totalNodes += info.ancestors.size() + info.roots.size() + info.traversal.size();
        SAFE_PARCEL(p->writeUint32, info.rowIndex); SAFE_PARCEL(p->writeUint32, info.sourceId); SAFE_PARCEL(p->writeUint32, info.variant);
        SAFE_PARCEL(p->writeUint32, info.detached ? 1u : 0u); SAFE_PARCEL(p->writeUint32, info.relativeLoop ? 1u : 0u);
        SAFE_PARCEL(writeIds, p, info.mirrorRoots, LayerSourceInfo::kMaxMirrors, totalIds);
        SAFE_PARCEL(writeIds, p, info.relativeRoots, LayerSourceInfo::kMaxAncestors, totalIds);
        SAFE_PARCEL(p->writeUint32, static_cast<uint32_t>(info.ancestors.size()));
        for (const auto& node : info.ancestors) SAFE_PARCEL(writeNode, p, node);
        SAFE_PARCEL(p->writeUint32, static_cast<uint32_t>(info.roots.size()));
        for (const auto& root : info.roots) {
            SAFE_PARCEL(writeNode, p, root.requested); SAFE_PARCEL(writeApplied, p, root.applied);
        }
        SAFE_PARCEL(p->writeUint32, static_cast<uint32_t>(info.traversal.size()));
        for (const auto& edge : info.traversal) {
            SAFE_PARCEL(p->writeUint32, edge.sourceId); SAFE_PARCEL(p->writeUint32, edge.variant);
            SAFE_PARCEL(p->writeUint32, edge.detached ? 1u : 0u); SAFE_PARCEL(p->writeUint32, edge.relativeLoop ? 1u : 0u);
            SAFE_PARCEL(writeIds, p, edge.mirrorRoots, LayerSourceInfo::kMaxMirrors, totalIds);
            SAFE_PARCEL(writeIds, p, edge.relativeRoots, LayerSourceInfo::kMaxAncestors, totalIds);
            SAFE_PARCEL(writeNode, p, edge.requested);
            SAFE_PARCEL(writeApplied, p, edge.applied);
        }
        SAFE_PARCEL(writeApplied, p, info.applied);
    }
    return OK;
}
} // namespace

status_t WindowInfosUpdate::readFromParcel(const android::Parcel* parcel) {
    layerSourcesComplete = false;
    layerSources.clear(); // no stale companion on reuse/decode failure
    nativePublication={};
    if (parcel == nullptr) {
        ALOGE("%s: Null parcel", __func__);
        return BAD_VALUE;
    }

    uint32_t size;

    SAFE_PARCEL(parcel->readUint32, &size);
    windowInfos.reserve(size);
    for (uint32_t i = 0; i < size; i++) {
        windowInfos.push_back({});
        SAFE_PARCEL(windowInfos.back().readFromParcel, parcel);
    }

    SAFE_PARCEL(parcel->readUint32, &size);
    displayInfos.reserve(size);
    for (uint32_t i = 0; i < size; i++) {
        displayInfos.push_back({});
        SAFE_PARCEL(displayInfos.back().readFromParcel, parcel);
    }

    SAFE_PARCEL(parcel->readInt64, &vsyncId);
    SAFE_PARCEL(parcel->readInt64, &timestamp);
    SAFE_PARCEL(readSources, parcel, windowInfos, layerSourcesComplete, layerSources,nativePublication);

    return OK;
}

status_t WindowInfosUpdate::writeToParcel(android::Parcel* parcel) const {
    if (parcel == nullptr) {
        ALOGE("%s: Null parcel", __func__);
        return BAD_VALUE;
    }

    SAFE_PARCEL(parcel->writeUint32, static_cast<uint32_t>(windowInfos.size()));
    for (auto& windowInfo : windowInfos) {
        SAFE_PARCEL(windowInfo.writeToParcel, parcel);
    }

    SAFE_PARCEL(parcel->writeUint32, static_cast<uint32_t>(displayInfos.size()));
    for (auto& displayInfo : displayInfos) {
        SAFE_PARCEL(displayInfo.writeToParcel, parcel);
    }

    SAFE_PARCEL(parcel->writeInt64, vsyncId);
    SAFE_PARCEL(parcel->writeInt64, timestamp);
    SAFE_PARCEL(writeSources, parcel, windowInfos, layerSourcesComplete, layerSources,nativePublication);

    return OK;
}

} // namespace android::gui
