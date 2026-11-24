/*
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_TAG "WindowInfo"
#define LOG_NDEBUG 0

#include <type_traits>

#include <binder/Parcel.h>
#include <gui/WindowInfo.h>

#include <log/log.h>

namespace android::gui {

namespace {

constexpr size_t kMaxBytesName = 1024;

// The legacy tail discriminators 0/1 remain byte-identical. Only records with
// new metadata use 2, followed by an explicitly bounded primitive-only payload.
constexpr int32_t kStagedAtlasExtension = 2;
constexpr uint32_t kStagedAtlasVersion = 1;
constexpr uint32_t kHasLineage = 1u << 0;
constexpr uint32_t kHasRegion = 1u << 1;
constexpr uint32_t kHasTag = 1u << 2;
constexpr uint32_t kKnownStagedAtlasFields = kHasLineage | kHasRegion | kHasTag;
constexpr size_t kMaxStagedAtlasPayloadBytes =
        2 * sizeof(uint32_t) +
        (2 + WindowInfo::kMaxCloneLineageDepth) * sizeof(uint32_t) +
        (1 + 4 * WindowInfo::kMaxPreViewportRegionRects) * sizeof(int32_t) +
        5 * sizeof(uint64_t);

bool stagedAtlasRectValid(const Rect& rect) {
    constexpr int32_t limit = WindowInfo::kMaxStagedAtlasCoordinate;
    const int64_t width = int64_t{rect.right} - rect.left;
    const int64_t height = int64_t{rect.bottom} - rect.top;
    return rect.left >= -limit && rect.left <= limit && rect.top >= -limit &&
            rect.top <= limit && rect.right >= -limit && rect.right <= limit &&
            rect.bottom >= -limit && rect.bottom <= limit && width > 0 && width <= limit &&
            height > 0 && height <= limit;
}

size_t regionRectCount(const Region& region) {
    return region.isEmpty() ? 0 : static_cast<size_t>(region.end() - region.begin());
}

bool stagedAtlasRegionValid(const Region& region) {
    if (region.isEmpty()) return region.hasSameRects(Region());
    if (regionRectCount(region) > WindowInfo::kMaxPreViewportRegionRects) return false;
    for (const Rect& rect : region) {
        if (!stagedAtlasRectValid(rect)) return false;
    }
    return true;
}

status_t writeCloneLineage(Parcel* parcel, const WindowInfo::CloneLineage& lineage) {
    status_t status = parcel->writeUint32(lineage.sourceLayerId)
            ?: parcel->writeInt32(static_cast<int32_t>(lineage.mirrorRootIds.size()));
    for (uint32_t root : lineage.mirrorRootIds) status = status ?: parcel->writeUint32(root);
    return status;
}

status_t readCloneLineage(const Parcel* parcel, WindowInfo::CloneLineage& lineage) {
    int32_t count;
    status_t status = parcel->readUint32(&lineage.sourceLayerId) ?: parcel->readInt32(&count);
    if (status != OK) return status;
    if (count <= 0 || count > static_cast<int32_t>(WindowInfo::kMaxCloneLineageDepth)) {
        return BAD_VALUE;
    }
    lineage.mirrorRootIds.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        uint32_t root;
        status = parcel->readUint32(&root);
        if (status != OK) return status;
        lineage.mirrorRootIds.push_back(root);
    }
    return OK;
}

status_t writeStagedAtlasPayload(Parcel* parcel, const WindowInfo& info) {
    const uint32_t fields = (info.cloneLineage ? kHasLineage : 0) |
            (info.preViewportTouchableRegion ? kHasRegion : 0) | (info.stageTag ? kHasTag : 0);
    status_t status = parcel->writeUint32(kStagedAtlasVersion) ?: parcel->writeUint32(fields);
    if (info.cloneLineage) status = status ?: writeCloneLineage(parcel, *info.cloneLineage);
    if (info.preViewportTouchableRegion) {
        const Region& region = *info.preViewportTouchableRegion;
        status = status ?: parcel->writeUint32(static_cast<uint32_t>(regionRectCount(region)));
        if (!region.isEmpty()) {
            for (const Rect& rect : region) {
                status = status ?: parcel->writeInt32(rect.left) ?: parcel->writeInt32(rect.top)
                        ?: parcel->writeInt32(rect.right) ?: parcel->writeInt32(rect.bottom);
            }
        }
    }
    if (info.stageTag) {
        const auto& tag = *info.stageTag;
        status = status ?: parcel->writeInt64(tag.authorityGeneration)
                ?: parcel->writeInt64(tag.atlasRevision) ?: parcel->writeInt64(tag.geometryRevision)
                ?: parcel->writeInt64(tag.epoch) ?: parcel->writeUint64(tag.presentationKey);
    }
    return status;
}

status_t readStagedAtlasPayload(const Parcel* parcel, WindowInfo& info) {
    uint32_t byteCount;
    status_t status = parcel->readUint32(&byteCount);
    if (status != OK) return status;
    if (byteCount < 2 * sizeof(uint32_t) || byteCount > kMaxStagedAtlasPayloadBytes ||
        byteCount > parcel->dataAvail() || byteCount % sizeof(uint32_t) != 0) {
        return BAD_VALUE;
    }
    // The bounded subparcel prevents malformed counts from consuming a following record.
    const size_t payloadStart = parcel->dataPosition();
    Parcel payload;
    status = payload.appendFrom(parcel, payloadStart, byteCount);
    if (status != OK) return status;
    if (payload.objectsCount() != 0) return BAD_VALUE;
    payload.setDataPosition(0);
    uint32_t version, fields;
    status = payload.readUint32(&version) ?: payload.readUint32(&fields);
    if (status != OK) return status;
    if (version != kStagedAtlasVersion || (fields & ~kKnownStagedAtlasFields) != 0 ||
        (fields & (kHasRegion | kHasTag)) == 0) {
        return BAD_VALUE;
    }
    std::optional<WindowInfo::CloneLineage> lineage;
    std::optional<Region> region;
    std::optional<WindowInfo::StageTag> tag;
    if (fields & kHasLineage) {
        lineage.emplace();
        status = readCloneLineage(&payload, *lineage);
        if (status != OK) return status;
    }
    if (fields & kHasRegion) {
        uint32_t count;
        status = payload.readUint32(&count);
        if (status != OK) return status;
        if (count > WindowInfo::kMaxPreViewportRegionRects ||
            count > payload.dataAvail() / (4 * sizeof(int32_t))) {
            return BAD_VALUE;
        }
        region.emplace();
        for (uint32_t i = 0; i < count; ++i) {
            Rect rect;
            status = payload.readInt32(&rect.left) ?: payload.readInt32(&rect.top)
                    ?: payload.readInt32(&rect.right) ?: payload.readInt32(&rect.bottom);
            if (status != OK) return status;
            if (!stagedAtlasRectValid(rect)) return BAD_VALUE;
            region->orSelf(rect);
            // Union may split rectangles: bound the canonical result at every step too.
            if (regionRectCount(*region) > WindowInfo::kMaxPreViewportRegionRects) return BAD_VALUE;
        }
    }
    if (fields & kHasTag) {
        tag.emplace();
        status = payload.readInt64(&tag->authorityGeneration) ?: payload.readInt64(&tag->atlasRevision)
                ?: payload.readInt64(&tag->geometryRevision) ?: payload.readInt64(&tag->epoch)
                ?: payload.readUint64(&tag->presentationKey);
        if (status != OK) return status;
    }
    if (payload.dataAvail() != 0) return BAD_VALUE;
    parcel->setDataPosition(payloadStart + byteCount);
    info.cloneLineage = std::move(lineage);
    info.preViewportTouchableRegion = std::move(region);
    info.stageTag = std::move(tag);
    return OK;
}

// Avoiding pulling in ICU here.
status_t writeUtf8AsUtf16WithTruncation(Parcel* parcel, const std::string& str, size_t max_bytes) {
    if (str.size() <= max_bytes) {
        return parcel->writeUtf8AsUtf16(str);
    }
    std::string truncated;
    truncated.reserve(max_bytes);
    static constexpr std::string_view kTail = "[TRUNC]";
    size_t truncated_length = max_bytes - kTail.size();
    while (truncated_length > 0 &&
           (static_cast<unsigned char>(str[truncated_length]) & 0xC0) == 0x80) {
        truncated_length--;
    }
    truncated.append(str.substr(0, truncated_length));
    truncated.append(kTail);
    return parcel->writeUtf8AsUtf16(truncated);
}

std::ostream& operator<<(std::ostream& out, const sp<IBinder>& binder) {
    if (binder == nullptr) {
        out << "<null>";
    } else {
        out << binder.get();
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, const Region& region) {
    if (region.isEmpty()) {
        out << "<empty>";
        return out;
    }

    bool first = true;
    Region::const_iterator cur = region.begin();
    Region::const_iterator const tail = region.end();
    while (cur != tail) {
        if (first) {
            first = false;
        } else {
            out << "|";
        }
        out << "[" << cur->left << "," << cur->top << "][" << cur->right << "," << cur->bottom
            << "]";
        cur++;
    }
    return out;
}

status_t writeTransform(android::Parcel* parcel, const ui::Transform& transform) {
    return parcel->writeFloat(transform.dsdx()) ?:
            parcel->writeFloat(transform.dtdx()) ?:
            parcel->writeFloat(transform.tx()) ?:
            parcel->writeFloat(transform.dtdy()) ?:
            parcel->writeFloat(transform.dsdy()) ?:
            parcel->writeFloat(transform.ty());
}

status_t readTransform(const android::Parcel* parcel, ui::Transform& transform) {
    float dsdx, dtdx, tx, dtdy, dsdy, ty;

    const status_t status = parcel->readFloat(&dsdx) ?:
            parcel->readFloat(&dtdx) ?:
            parcel->readFloat(&tx) ?:
            parcel->readFloat(&dtdy) ?:
            parcel->readFloat(&dsdy) ?:
            parcel->readFloat(&ty);
    if (status != OK) {
        return status;
    }

    transform.set({dsdx, dtdx, tx, dtdy, dsdy, ty, 0, 0, 1});
    return OK;
}

} // namespace

void WindowInfo::setInputConfig(ftl::Flags<InputConfig> config, bool value) {
    if (value) {
        inputConfig |= config;
        return;
    }
    inputConfig &= ~config;
}

void WindowInfo::addTouchableRegion(const Rect& region) {
    touchableRegion.orSelf(region);
}

bool WindowInfo::isSpy() const {
    return inputConfig.test(InputConfig::SPY);
}

bool WindowInfo::interceptsStylus() const {
    return inputConfig.test(InputConfig::INTERCEPTS_STYLUS);
}

bool WindowInfo::overlaps(const WindowInfo* other) const {
    return !frame.isEmpty() && frame.left < other->frame.right && frame.right > other->frame.left &&
            frame.top < other->frame.bottom && frame.bottom > other->frame.top;
}

bool WindowInfo::operator==(const WindowInfo& info) const {
    return info.token == token && info.id == id && info.name == name &&
            info.dispatchingTimeout == dispatchingTimeout && info.frame == frame &&
            info.contentSize == contentSize && info.surfaceInset == surfaceInset &&
            info.globalScaleFactor == globalScaleFactor && info.transform == transform &&
            info.touchableRegion.hasSameRects(touchableRegion) &&
            info.touchOcclusionMode == touchOcclusionMode && info.ownerPid == ownerPid &&
            info.ownerUid == ownerUid && info.packageName == packageName &&
            info.inputConfig == inputConfig && info.displayId == displayId &&
            info.replaceTouchableRegionWithCrop == replaceTouchableRegionWithCrop &&
            info.applicationInfo == applicationInfo && info.layoutParamsType == layoutParamsType &&
            info.layoutParamsFlags == layoutParamsFlags &&
            info.canOccludePresentation == canOccludePresentation &&
            info.cloneLineage == cloneLineage && info.stageTag == stageTag &&
            info.preViewportTouchableRegion.has_value() ==
                    preViewportTouchableRegion.has_value() &&
            (!preViewportTouchableRegion ||
             info.preViewportTouchableRegion->hasSameRects(*preViewportTouchableRegion));
}

status_t WindowInfo::writeToParcel(android::Parcel* parcel) const {
    if (parcel == nullptr) {
        ALOGE("%s: Null parcel", __func__);
        return BAD_VALUE;
    }
    if (name.empty()) {
        parcel->writeInt32(0);
        return OK;
    }
    if (cloneLineage &&
        (cloneLineage->mirrorRootIds.empty() ||
         cloneLineage->mirrorRootIds.size() > kMaxCloneLineageDepth)) {
        return BAD_VALUE;
    }
    if (preViewportTouchableRegion && !stagedAtlasRegionValid(*preViewportTouchableRegion)) {
        return BAD_VALUE;
    }
    parcel->writeInt32(1);

    // Ensure that the size of custom types are what we expect for writing into the parcel.
    static_assert(sizeof(inputConfig) == 4u);
    static_assert(sizeof(ownerPid.val()) == 4u);
    static_assert(sizeof(ownerUid.val()) == 4u);

    // clang-format off
    status_t status = parcel->writeStrongBinder(token) ?:
        parcel->writeInt64(dispatchingTimeout.count()) ?:
        parcel->writeInt32(id) ?:
        writeUtf8AsUtf16WithTruncation(parcel, name, kMaxBytesName) ?:
        parcel->writeInt32(layoutParamsFlags.get()) ?:
        parcel->writeInt32(
                static_cast<std::underlying_type_t<WindowInfo::Type>>(layoutParamsType)) ?:
        parcel->write(frame) ?:
        parcel->writeInt32(contentSize.width) ?:
        parcel->writeInt32(contentSize.height) ?:
        parcel->writeInt32(surfaceInset) ?:
        parcel->writeFloat(globalScaleFactor) ?:
        parcel->writeFloat(alpha) ?:
        writeTransform(parcel, transform) ?:
        parcel->writeInt32(static_cast<int32_t>(touchOcclusionMode)) ?:
        parcel->writeInt32(ownerPid.val()) ?:
        parcel->writeInt32(ownerUid.val()) ?:
        parcel->writeUtf8AsUtf16(packageName) ?:
        parcel->writeInt32(inputConfig.get()) ?:
        parcel->writeInt32(displayId.val()) ?:
        applicationInfo.writeToParcel(parcel) ?:
        parcel->write(touchableRegion) ?:
        parcel->writeBool(replaceTouchableRegionWithCrop) ?:
        parcel->writeStrongBinder(touchableRegionCropHandle.promote()) ?:
        parcel->writeStrongBinder(windowToken) ?:
        parcel->writeStrongBinder(focusTransferTarget) ?:
        parcel->writeBool(canOccludePresentation) ?:
        parcel->writeBool(cloneLayerStackTransform.has_value());
    // clang-format on
    if (cloneLayerStackTransform) {
        status = status ?: writeTransform(parcel, *cloneLayerStackTransform);
    }
    if (preViewportTouchableRegion || stageTag) {
        Parcel payload;
        status = status ?: writeStagedAtlasPayload(&payload, *this);
        if (status != OK) return status;
        if (payload.dataSize() > kMaxStagedAtlasPayloadBytes) return BAD_VALUE;
        return parcel->writeInt32(kStagedAtlasExtension)
                ?: parcel->writeUint32(static_cast<uint32_t>(payload.dataSize()))
                ?: parcel->appendFrom(&payload, 0, payload.dataSize());
    }
    status = status ?: parcel->writeInt32(cloneLineage.has_value() ? 1 : 0);
    if (status != OK || !cloneLineage) {
        return status;
    }
    return writeCloneLineage(parcel, *cloneLineage);
}

status_t WindowInfo::readFromParcel(const android::Parcel* parcel) {
    // A reused destination must not retain any private metadata on an absent record or failure.
    cloneLineage.reset();
    preViewportTouchableRegion.reset();
    stageTag.reset();
    cloneLayerStackTransform.reset();
    if (parcel == nullptr) {
        ALOGE("%s: Null parcel", __func__);
        return BAD_VALUE;
    }
    if (parcel->readInt32() == 0) {
        return OK;
    }

    token = parcel->readStrongBinder();
    dispatchingTimeout = static_cast<decltype(dispatchingTimeout)>(parcel->readInt64());
    status_t status = parcel->readInt32(&id) ?: parcel->readUtf8FromUtf16(&name);
    if (status != OK) {
        return status;
    }

    int32_t lpFlags, lpType, touchOcclusionModeInt, inputConfigInt, ownerPidInt, ownerUidInt,
            displayIdInt;
    sp<IBinder> touchableRegionCropHandleSp;
    bool hasCloneLayerStackTransform = false;

    // clang-format off
    status = parcel->readInt32(&lpFlags) ?:
        parcel->readInt32(&lpType) ?:
        parcel->read(frame) ?:
        parcel->readInt32(&contentSize.width) ?:
        parcel->readInt32(&contentSize.height) ?:
        parcel->readInt32(&surfaceInset) ?:
        parcel->readFloat(&globalScaleFactor) ?:
        parcel->readFloat(&alpha) ?:
        readTransform(parcel, /*byRef*/ transform) ?:
        parcel->readInt32(&touchOcclusionModeInt) ?:
        parcel->readInt32(&ownerPidInt) ?:
        parcel->readInt32(&ownerUidInt) ?:
        parcel->readUtf8FromUtf16(&packageName) ?:
        parcel->readInt32(&inputConfigInt) ?:
        parcel->readInt32(&displayIdInt) ?:
        applicationInfo.readFromParcel(parcel) ?:
        parcel->read(touchableRegion) ?:
        parcel->readBool(&replaceTouchableRegionWithCrop) ?:
        parcel->readNullableStrongBinder(&touchableRegionCropHandleSp) ?:
        parcel->readNullableStrongBinder(&windowToken) ?:
        parcel->readNullableStrongBinder(&focusTransferTarget) ?:
        parcel->readBool(&canOccludePresentation)?:
        parcel->readBool(&hasCloneLayerStackTransform);
    // clang-format on

    if (status != OK) {
        return status;
    }

    layoutParamsFlags = ftl::Flags<Flag>(lpFlags);
    layoutParamsType = static_cast<Type>(lpType);
    touchOcclusionMode = static_cast<TouchOcclusionMode>(touchOcclusionModeInt);
    inputConfig = ftl::Flags<InputConfig>(inputConfigInt);
    ownerPid = Pid{ownerPidInt};
    ownerUid = Uid{static_cast<uid_t>(ownerUidInt)};
    touchableRegionCropHandle = touchableRegionCropHandleSp;
    displayId = ui::LogicalDisplayId{displayIdInt};

    std::optional<ui::Transform> cloneTransform =
            hasCloneLayerStackTransform ? std::make_optional<ui::Transform>() : std::nullopt;
    if (cloneTransform) {
        status = readTransform(parcel, /*byRef*/ *cloneTransform);
        if (status != OK) {
            return status;
        }
    }

    int32_t hasCloneLineage;
    status = parcel->readInt32(&hasCloneLineage);
    if (status != OK || hasCloneLineage == 0) {
        if (status == OK) cloneLayerStackTransform = std::move(cloneTransform);
        return status;
    }
    if (hasCloneLineage == kStagedAtlasExtension) {
        status = readStagedAtlasPayload(parcel, *this);
        if (status == OK) cloneLayerStackTransform = std::move(cloneTransform);
        return status;
    }
    if (hasCloneLineage != 1) {
        return BAD_VALUE;
    }
    CloneLineage lineage;
    status = readCloneLineage(parcel, lineage);
    if (status != OK) return status;
    cloneLineage = std::move(lineage);
    cloneLayerStackTransform = std::move(cloneTransform);
    return OK;
}

WindowInfoHandle::WindowInfoHandle() {}

WindowInfoHandle::~WindowInfoHandle() {}

WindowInfoHandle::WindowInfoHandle(const WindowInfoHandle& other) : mInfo(other.mInfo) {}

WindowInfoHandle::WindowInfoHandle(const WindowInfo& other) : mInfo(other) {}

status_t WindowInfoHandle::writeToParcel(android::Parcel* parcel) const {
    return mInfo.writeToParcel(parcel);
}

status_t WindowInfoHandle::readFromParcel(const android::Parcel* parcel) {
    return mInfo.readFromParcel(parcel);
}

void WindowInfoHandle::releaseChannel() {
    mInfo.token.clear();
}

sp<IBinder> WindowInfoHandle::getToken() const {
    return mInfo.token;
}

void WindowInfoHandle::updateFrom(sp<WindowInfoHandle> handle) {
    mInfo = handle->mInfo;
}

std::ostream& operator<<(std::ostream& out, const WindowInfo& info) {
    out << "name=" << info.name << ", id=" << info.id << ", displayId=" << info.displayId
        << ", inputConfig=" << info.inputConfig.string() << ", alpha=" << info.alpha << ", frame=["
        << info.frame.left << "," << info.frame.top << "][" << info.frame.right << ","
        << info.frame.bottom << "], globalScale=" << info.globalScaleFactor
        << ", applicationInfo.name=" << info.applicationInfo.name
        << ", applicationInfo.token=" << info.applicationInfo.token
        << ", touchableRegion=" << info.touchableRegion << ", ownerPid=" << info.ownerPid.toString()
        << ", ownerUid=" << info.ownerUid.toString() << ", dispatchingTimeout="
        << std::chrono::duration_cast<std::chrono::milliseconds>(info.dispatchingTimeout).count()
        << "ms, token=" << info.token.get()
        << ", touchOcclusionMode=" << ftl::enum_string(info.touchOcclusionMode);
    if (info.canOccludePresentation) out << ", canOccludePresentation";
    if (info.cloneLineage) {
        out << ", cloneLineage={sourceLayerId=" << info.cloneLineage->sourceLayerId << ", roots=[";
        bool first = true;
        for (uint32_t rootId : info.cloneLineage->mirrorRootIds) {
            if (!first) out << ",";
            out << rootId;
            first = false;
        }
        out << "]}";
    }
    std::string transform;
    info.transform.dump(transform, "transform", "    ");
    out << "\n" << transform;
    return out;
}

std::ostream& operator<<(std::ostream& out, const WindowInfoHandle& window) {
    const WindowInfo& info = *window.getInfo();
    out << info;
    return out;
}

} // namespace android::gui
