/*
 * Copyright 2018 The Android Open Source Project
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <limits>

#include <binder/Binder.h>
#include <binder/Parcel.h>

#include <gui/WindowInfo.h>
#include <gui/WindowInfosUpdate.h>

using std::chrono_literals::operator""s;

namespace android {

using gui::InputApplicationInfo;
using gui::TouchOcclusionMode;
using gui::WindowInfo;
using ui::Size;

namespace test {

namespace {

WindowInfo lineageWindow(std::vector<uint32_t> roots = {3, 14}) {
    WindowInfo info;
    // Empty names serialize as absent records and would not exercise the lineage codec.
    info.name = "LineageWindow";
    info.id = 123;
    info.alpha = 1.f;
    info.cloneLineage =
            WindowInfo::CloneLineage{.sourceLayerId = 111, .mirrorRootIds = std::move(roots)};
    return info;
}

// Obtain the real codec's existing-field prefix, without its final lineage-presence word.
status_t writeBeforeLineage(Parcel& parcel) {
    WindowInfo info = lineageWindow();
    info.cloneLineage.reset();
    status_t status = info.writeToParcel(&parcel);
    if (status != OK) return status;
    status = parcel.setDataSize(parcel.dataSize() - sizeof(int32_t));
    if (status == OK) parcel.setDataPosition(parcel.dataSize());
    return status;
}

WindowInfo stagedWindow() {
    WindowInfo info = lineageWindow();
    info.preViewportTouchableRegion = Region(Rect{-20, -10, 80, 90});
    info.preViewportTouchableRegion->orSelf(Rect{100, 100, 110, 110});
    // Values are informational, not authority checks; preserve signed/unsigned bits exactly.
    info.stageTag = WindowInfo::StageTag{-1, 2, -3, 4, std::numeric_limits<uint64_t>::max()};
    info.cloneLayerStackTransform = ui::Transform();
    return info;
}

status_t writeStagedRecord(Parcel& record, const Parcel& payload, uint32_t declaredBytes) {
    return writeBeforeLineage(record) ?: record.writeInt32(2)
            ?: record.writeUint32(declaredBytes)
            ?: record.appendFrom(&payload, 0, payload.dataSize());
}

void expectPrivateMetadataCleared(const WindowInfo& info) {
    EXPECT_FALSE(info.cloneLineage);
    EXPECT_FALSE(info.preViewportTouchableRegion);
    EXPECT_FALSE(info.stageTag);
    EXPECT_FALSE(info.cloneLayerStackTransform);
}

void expectStagedPayloadRejected(const Parcel& payload) {
    Parcel record;
    ASSERT_EQ(OK, writeStagedRecord(record, payload, static_cast<uint32_t>(payload.dataSize())));
    const size_t followingStart = record.dataPosition();
    ASSERT_EQ(OK, lineageWindow({5}).writeToParcel(&record));
    record.setDataPosition(0);
    WindowInfo decoded = stagedWindow();
    EXPECT_NE(OK, decoded.readFromParcel(&record));
    expectPrivateMetadataCleared(decoded);
    // A primitive read in an invalid extension must never consume the following WindowInfo.
    EXPECT_LE(record.dataPosition(), followingStart);
}

} // namespace

TEST(WindowInfo, CloneLineageParcellingAndAbsentReset) {
    WindowInfo info = lineageWindow(); // Intentionally null channel token, but a named record.
    info.cloneLayerStackTransform = ui::Transform();
    Parcel parcel;
    ASSERT_EQ(OK, info.writeToParcel(&parcel));
    parcel.setDataPosition(0);
    WindowInfo decoded;
    ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
    EXPECT_EQ(info.cloneLineage, decoded.cloneLineage);
    EXPECT_EQ(info.cloneLayerStackTransform, decoded.cloneLayerStackTransform);
    EXPECT_EQ(nullptr, decoded.token);
    EXPECT_EQ(0u, parcel.dataAvail());

    info.cloneLineage.reset();
    Parcel originalParcel;
    ASSERT_EQ(OK, info.writeToParcel(&originalParcel));
    originalParcel.setDataPosition(0);
    ASSERT_EQ(OK, decoded.readFromParcel(&originalParcel));
    EXPECT_FALSE(decoded.cloneLineage);

    decoded.cloneLineage = lineageWindow().cloneLineage;
    WindowInfo empty;
    Parcel emptyParcel;
    ASSERT_EQ(OK, empty.writeToParcel(&emptyParcel));
    emptyParcel.setDataPosition(0);
    ASSERT_EQ(OK, decoded.readFromParcel(&emptyParcel));
    EXPECT_FALSE(decoded.cloneLineage);
}

TEST(WindowInfo, CloneLineageFullDepthAndUnsignedIds) {
    std::vector<uint32_t> roots;
    for (size_t i = 0; i < WindowInfo::kMaxCloneLineageDepth; ++i) {
        roots.push_back(0x80000000u + static_cast<uint32_t>(i));
    }
    WindowInfo info = lineageWindow(roots);
    info.cloneLineage->sourceLayerId = 0x80000100u;
    Parcel parcel;
    ASSERT_EQ(OK, info.writeToParcel(&parcel));
    parcel.setDataPosition(0);
    WindowInfo decoded;
    ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
    EXPECT_EQ(info.cloneLineage, decoded.cloneLineage);
    EXPECT_EQ(0u, parcel.dataAvail());
}

TEST(WindowInfo, CloneLineageRejectsInvalidWriteDepth) {
    for (size_t count : {size_t{0}, WindowInfo::kMaxCloneLineageDepth + 1}) {
        SCOPED_TRACE(count);
        WindowInfo info = lineageWindow(std::vector<uint32_t>(count, 3));
        Parcel parcel;
        EXPECT_EQ(BAD_VALUE, info.writeToParcel(&parcel));
        EXPECT_EQ(0u, parcel.dataSize());
    }
}

TEST(WindowInfo, CloneLineageRejectsMalformedPresenceAndDepth) {
    for (int32_t presence : {-1, 3}) {
        SCOPED_TRACE(presence);
        Parcel parcel;
        ASSERT_EQ(OK, writeBeforeLineage(parcel));
        ASSERT_EQ(OK, parcel.writeInt32(presence));
        parcel.setDataPosition(0);
        WindowInfo decoded = lineageWindow();
        EXPECT_EQ(BAD_VALUE, decoded.readFromParcel(&parcel));
        EXPECT_FALSE(decoded.cloneLineage);
    }
    for (int32_t count : {-1, 0, static_cast<int32_t>(WindowInfo::kMaxCloneLineageDepth + 1)}) {
        SCOPED_TRACE(count);
        Parcel parcel;
        ASSERT_EQ(OK, writeBeforeLineage(parcel));
        ASSERT_EQ(OK, parcel.writeInt32(1));
        ASSERT_EQ(OK, parcel.writeUint32(111));
        ASSERT_EQ(OK, parcel.writeInt32(count));
        parcel.setDataPosition(0);
        WindowInfo decoded = lineageWindow();
        EXPECT_EQ(BAD_VALUE, decoded.readFromParcel(&parcel));
        EXPECT_FALSE(decoded.cloneLineage);
    }
}

TEST(WindowInfo, CloneLineageRejectsEveryTruncatedPayload) {
    const WindowInfo info = lineageWindow();
    const size_t payloadSize = (3 + info.cloneLineage->mirrorRootIds.size()) * sizeof(uint32_t);
    for (size_t removed = 1; removed <= payloadSize; ++removed) {
        SCOPED_TRACE(removed);
        Parcel parcel;
        ASSERT_EQ(OK, info.writeToParcel(&parcel));
        ASSERT_EQ(OK, parcel.setDataSize(parcel.dataSize() - removed));
        parcel.setDataPosition(0);
        WindowInfo decoded = lineageWindow({99});
        EXPECT_NE(OK, decoded.readFromParcel(&parcel));
        EXPECT_FALSE(decoded.cloneLineage);
    }
}

TEST(WindowInfo, CloneLineageCopyAndHandleUpdate) {
    const WindowInfo original = lineageWindow();
    WindowInfo copied(original);
    WindowInfo assigned;
    assigned = original;
    auto handle = sp<gui::WindowInfoHandle>::make(original);
    auto copiedHandle = sp<gui::WindowInfoHandle>::make(*handle);
    auto updatedHandle = sp<gui::WindowInfoHandle>::make();
    updatedHandle->updateFrom(handle);
    EXPECT_EQ(original.cloneLineage, copied.cloneLineage);
    EXPECT_EQ(original.cloneLineage, assigned.cloneLineage);
    EXPECT_EQ(original.cloneLineage, copiedHandle->getInfo()->cloneLineage);
    EXPECT_EQ(original.cloneLineage, updatedHandle->getInfo()->cloneLineage);
    handle->editInfo()->cloneLineage->mirrorRootIds.front() = 99;
    EXPECT_EQ(original.cloneLineage, copiedHandle->getInfo()->cloneLineage);
    EXPECT_EQ(original.cloneLineage, updatedHandle->getInfo()->cloneLineage);
}

TEST(WindowInfo, CloneLineageEqualityUsesFullOrderedPath) {
    const WindowInfo original = lineageWindow();
    WindowInfo changed = original;
    EXPECT_EQ(original, changed);
    changed.cloneLineage.reset();
    EXPECT_NE(original, changed);
    changed = original;
    ++changed.cloneLineage->sourceLayerId;
    EXPECT_NE(original, changed);
    changed = original;
    changed.cloneLineage->mirrorRootIds = {14, 3};
    EXPECT_NE(original, changed);
    changed = original;
    changed.cloneLineage->mirrorRootIds.push_back(5);
    EXPECT_NE(original, changed);
}

TEST(WindowInfo, CloneLineageWindowInfosUpdateFraming) {
    WindowInfo first = lineageWindow();
    WindowInfo second = lineageWindow({4});
    second.id = 124;
    second.name = "SecondLineageWindow";
    WindowInfo original = lineageWindow();
    original.id = 125;
    original.cloneLineage.reset();
    gui::DisplayInfo display;
    display.displayId = ui::LogicalDisplayId{7};
    display.logicalWidth = 1280;
    display.logicalHeight = 720;
    gui::WindowInfosUpdate update({first, second, original}, {display}, 123456, 789012);
    Parcel parcel;
    ASSERT_EQ(OK, update.writeToParcel(&parcel));
    parcel.setDataPosition(0);
    gui::WindowInfosUpdate decoded;
    ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
    ASSERT_EQ(3u, decoded.windowInfos.size());
    EXPECT_EQ(first, decoded.windowInfos[0]);
    EXPECT_EQ(second, decoded.windowInfos[1]);
    EXPECT_EQ(original, decoded.windowInfos[2]);
    ASSERT_EQ(1u, decoded.displayInfos.size());
    EXPECT_EQ(display.displayId, decoded.displayInfos[0].displayId);
    EXPECT_EQ(display.logicalWidth, decoded.displayInfos[0].logicalWidth);
    EXPECT_EQ(display.logicalHeight, decoded.displayInfos[0].logicalHeight);
    EXPECT_EQ(update.vsyncId, decoded.vsyncId);
    EXPECT_EQ(update.timestamp, decoded.timestamp);
    EXPECT_EQ(0u, parcel.dataAvail());
}

TEST(WindowInfo, CloneLineageMalformedUpdateStopsBeforeFollowingRecord) {
    Parcel parcel;
    ASSERT_EQ(OK, parcel.writeUint32(2));
    ASSERT_EQ(OK, writeBeforeLineage(parcel));
    ASSERT_EQ(OK, parcel.writeInt32(1));
    ASSERT_EQ(OK, parcel.writeUint32(111));
    ASSERT_EQ(OK, parcel.writeInt32(static_cast<int32_t>(WindowInfo::kMaxCloneLineageDepth + 1)));
    const WindowInfo following = lineageWindow({4});
    ASSERT_EQ(OK, following.writeToParcel(&parcel));
    ASSERT_EQ(OK, parcel.writeUint32(0)); // No displays.
    ASSERT_EQ(OK, parcel.writeInt64(123456));
    ASSERT_EQ(OK, parcel.writeInt64(789012));
    parcel.setDataPosition(0);
    gui::WindowInfosUpdate decoded;
    EXPECT_EQ(BAD_VALUE, decoded.readFromParcel(&parcel));
    ASSERT_EQ(1u, decoded.windowInfos.size());
    EXPECT_FALSE(decoded.windowInfos[0].cloneLineage);
    EXPECT_TRUE(decoded.displayInfos.empty());
    EXPECT_GT(parcel.dataAvail(), 0u);
}

TEST(WindowInfo, StagedMetadataAbsentKeepsLegacyTailBytes) {
    for (bool hasLineage : {false, true}) {
        WindowInfo info = lineageWindow();
        if (!hasLineage) info.cloneLineage.reset();
        Parcel actual, expected;
        ASSERT_EQ(OK, info.writeToParcel(&actual));
        ASSERT_EQ(OK, writeBeforeLineage(expected));
        ASSERT_EQ(OK, expected.writeInt32(hasLineage ? 1 : 0));
        if (hasLineage) {
            ASSERT_EQ(OK, expected.writeUint32(111));
            ASSERT_EQ(OK, expected.writeInt32(2));
            ASSERT_EQ(OK, expected.writeUint32(3));
            ASSERT_EQ(OK, expected.writeUint32(14));
        }
        ASSERT_EQ(expected.dataSize(), actual.dataSize());
        EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), actual.dataSize()));
    }
}

TEST(WindowInfo, StagedMetadataEveryOptionalCombinationRoundTrips) {
    for (uint32_t fields = 0; fields < 8; ++fields) {
        SCOPED_TRACE(fields);
        WindowInfo info = stagedWindow();
        if (!(fields & 1)) info.cloneLineage.reset();
        if (!(fields & 2)) info.preViewportTouchableRegion.reset();
        if (!(fields & 4)) info.stageTag.reset();
        Parcel parcel;
        ASSERT_EQ(OK, info.writeToParcel(&parcel));
        parcel.setDataPosition(0);
        WindowInfo decoded = stagedWindow();
        ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
        EXPECT_EQ(info, decoded);
        EXPECT_EQ(0u, parcel.dataAvail());
        EXPECT_TRUE(decoded.touchableRegion.hasSameRects(info.touchableRegion));
    }
}

TEST(WindowInfo, StagedMetadataExactPrimitivePayloadBytes) {
    WindowInfo info = lineageWindow();
    info.preViewportTouchableRegion = Region(Rect{-1, -2, 3, 4});
    info.stageTag = WindowInfo::StageTag{-1, 2, -3, 4, std::numeric_limits<uint64_t>::max()};
    Parcel actual, expected, payload;
    ASSERT_EQ(OK, info.writeToParcel(&actual));
    ASSERT_EQ(OK, payload.writeUint32(1)); // Private version.
    ASSERT_EQ(OK, payload.writeUint32(7)); // Lineage, region, tag.
    ASSERT_EQ(OK, payload.writeUint32(111));
    ASSERT_EQ(OK, payload.writeInt32(2));
    ASSERT_EQ(OK, payload.writeUint32(3));
    ASSERT_EQ(OK, payload.writeUint32(14));
    ASSERT_EQ(OK, payload.writeUint32(1)); // Rectangle count.
    for (int32_t edge : {-1, -2, 3, 4}) ASSERT_EQ(OK, payload.writeInt32(edge));
    for (int64_t value : {-1, 2, -3, 4}) ASSERT_EQ(OK, payload.writeInt64(value));
    ASSERT_EQ(OK, payload.writeUint64(std::numeric_limits<uint64_t>::max()));
    ASSERT_EQ(84u, payload.dataSize());
    ASSERT_EQ(OK, writeStagedRecord(expected, payload, 84));
    ASSERT_EQ(expected.dataSize(), actual.dataSize());
    EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), actual.dataSize()));
}

TEST(WindowInfo, StagedMetadataEmptyRegionDiffersFromAbsentAndResets) {
    WindowInfo empty = lineageWindow();
    empty.cloneLineage.reset();
    empty.preViewportTouchableRegion.emplace();
    WindowInfo absent = empty;
    absent.preViewportTouchableRegion.reset();
    EXPECT_NE(empty, absent);
    Parcel parcel;
    ASSERT_EQ(OK, empty.writeToParcel(&parcel));
    parcel.setDataPosition(0);
    WindowInfo decoded = stagedWindow();
    ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
    ASSERT_TRUE(decoded.preViewportTouchableRegion);
    EXPECT_TRUE(decoded.preViewportTouchableRegion->isEmpty());
    EXPECT_FALSE(decoded.cloneLineage);
    EXPECT_FALSE(decoded.stageTag);
    for (bool absentRecord : {false, true}) {
        Parcel next;
        ASSERT_EQ(OK, (absentRecord ? WindowInfo() : absent).writeToParcel(&next));
        next.setDataPosition(0);
        decoded = stagedWindow();
        ASSERT_EQ(OK, decoded.readFromParcel(&next));
        expectPrivateMetadataCleared(decoded);
    }
    decoded = stagedWindow();
    EXPECT_EQ(BAD_VALUE, decoded.readFromParcel(nullptr));
    expectPrivateMetadataCleared(decoded);
}

TEST(WindowInfo, StagedMetadataCopyHandleUpdateAndEquality) {
    const WindowInfo original = stagedWindow();
    WindowInfo copied(original), assigned;
    assigned = original;
    auto source = sp<gui::WindowInfoHandle>::make(original);
    auto copiedHandle = sp<gui::WindowInfoHandle>::make(*source);
    auto updatedHandle = sp<gui::WindowInfoHandle>::make();
    updatedHandle->updateFrom(source);
    source->editInfo()->preViewportTouchableRegion->clear();
    source->editInfo()->stageTag->epoch++;
    EXPECT_EQ(original, copied);
    EXPECT_EQ(original, assigned);
    EXPECT_EQ(original, *copiedHandle->getInfo());
    EXPECT_EQ(original, *updatedHandle->getInfo());
    for (int field = 0; field < 7; ++field) {
        WindowInfo changed = original;
        switch (field) {
            case 0: changed.stageTag->authorityGeneration++; break;
            case 1: changed.stageTag->atlasRevision++; break;
            case 2: changed.stageTag->geometryRevision++; break;
            case 3: changed.stageTag->epoch++; break;
            case 4: changed.stageTag->presentationKey--; break;
            case 5: changed.preViewportTouchableRegion->clear(); break;
            case 6: changed.stageTag.reset(); break;
        }
        EXPECT_NE(original, changed);
    }
    updatedHandle->updateFrom(sp<gui::WindowInfoHandle>::make(lineageWindow()));
    EXPECT_FALSE(updatedHandle->getInfo()->preViewportTouchableRegion);
    EXPECT_FALSE(updatedHandle->getInfo()->stageTag);
}

TEST(WindowInfo, StagedMetadataMaximumRegionDepthAndSignedBoundsRoundTrip) {
    WindowInfo info = stagedWindow();
    info.cloneLineage->mirrorRootIds.resize(WindowInfo::kMaxCloneLineageDepth, UINT32_MAX);
    info.preViewportTouchableRegion.emplace();
    for (size_t i = 0; i < WindowInfo::kMaxPreViewportRegionRects; ++i) {
        const int32_t x = -32768 + static_cast<int32_t>(i) * 2;
        info.preViewportTouchableRegion->orSelf(Rect{x, -32768, x + 1, 0});
    }
    Parcel parcel;
    ASSERT_EQ(OK, info.writeToParcel(&parcel));
    parcel.setDataPosition(0);
    WindowInfo decoded;
    ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
    EXPECT_EQ(info, decoded);
    EXPECT_EQ(0u, parcel.dataAvail());
}

TEST(WindowInfo, StagedMetadataRejectsOversizedOrOutOfBoundsWriteBeforeRecord) {
    WindowInfo info = stagedWindow();
    info.preViewportTouchableRegion.emplace();
    for (size_t i = 0; i <= WindowInfo::kMaxPreViewportRegionRects; ++i) {
        const int32_t x = static_cast<int32_t>(i) * 2;
        info.preViewportTouchableRegion->orSelf(Rect{x, 0, x + 1, 1});
    }
    Parcel oversized;
    EXPECT_EQ(BAD_VALUE, info.writeToParcel(&oversized));
    EXPECT_EQ(0u, oversized.dataSize());
    for (const Rect rect : {Rect{-32769, 0, 1, 1}, Rect{0, 0, 32769, 1},
                            Rect{-1, 0, 32768, 1}, Rect{0, -32769, 1, 1},
                            Rect{0, -1, 1, 32768}}) {
        SCOPED_TRACE(testing::Message() << rect.left << "," << rect.top << "," << rect.right
                                        << "," << rect.bottom);
        info.preViewportTouchableRegion = Region(rect);
        Parcel invalid;
        EXPECT_EQ(BAD_VALUE, info.writeToParcel(&invalid));
        EXPECT_EQ(0u, invalid.dataSize());
    }
}

TEST(WindowInfo, StagedMetadataRejectsUnknownVersionFlagsAndNoncanonicalExtension) {
    for (uint32_t version : {0u, 2u, UINT32_MAX}) {
        Parcel payload;
        ASSERT_EQ(OK, payload.writeUint32(version));
        ASSERT_EQ(OK, payload.writeUint32(2));
        ASSERT_EQ(OK, payload.writeUint32(0));
        expectStagedPayloadRejected(payload);
    }
    for (uint32_t fields : {0u, 1u, 8u, 10u, UINT32_MAX}) {
        Parcel payload;
        ASSERT_EQ(OK, payload.writeUint32(1));
        ASSERT_EQ(OK, payload.writeUint32(fields));
        ASSERT_EQ(OK, payload.writeUint32(0));
        expectStagedPayloadRejected(payload);
    }
}

TEST(WindowInfo, StagedMetadataRejectsMalformedLengthsAndCounts) {
    Parcel payload;
    ASSERT_EQ(OK, payload.writeUint32(1));
    ASSERT_EQ(OK, payload.writeUint32(2));
    ASSERT_EQ(OK, payload.writeUint32(0));
    for (uint32_t bytes : {0u, 4u, 7u, 9u, 16u, UINT32_MAX}) {
        SCOPED_TRACE(bytes);
        Parcel record;
        ASSERT_EQ(OK, writeStagedRecord(record, payload, bytes));
        record.setDataPosition(0);
        WindowInfo decoded = stagedWindow();
        EXPECT_NE(OK, decoded.readFromParcel(&record));
        expectPrivateMetadataCleared(decoded);
    }
    for (uint32_t count : {1025u, UINT32_MAX, 1u}) {
        Parcel badCount;
        ASSERT_EQ(OK, badCount.writeUint32(1));
        ASSERT_EQ(OK, badCount.writeUint32(2));
        ASSERT_EQ(OK, badCount.writeUint32(count));
        expectStagedPayloadRejected(badCount);
    }
    for (int32_t count : {-1, 0, 51}) {
        Parcel badLineage;
        ASSERT_EQ(OK, badLineage.writeUint32(1));
        ASSERT_EQ(OK, badLineage.writeUint32(3));
        ASSERT_EQ(OK, badLineage.writeUint32(111));
        ASSERT_EQ(OK, badLineage.writeInt32(count));
        expectStagedPayloadRejected(badLineage);
    }
}

TEST(WindowInfo, StagedMetadataRejectsInvalidRectanglesAndTrailingBytes) {
    for (const Rect rect : {Rect{0, 0, 0, 1}, Rect{1, 0, 0, 1}, Rect{-32769, 0, 0, 1},
                            Rect{0, 0, 1, 32769}, Rect{-1, 0, 32768, 1}}) {
        Parcel payload;
        ASSERT_EQ(OK, payload.writeUint32(1));
        ASSERT_EQ(OK, payload.writeUint32(2));
        ASSERT_EQ(OK, payload.writeUint32(1));
        ASSERT_EQ(OK, payload.writeInt32(rect.left));
        ASSERT_EQ(OK, payload.writeInt32(rect.top));
        ASSERT_EQ(OK, payload.writeInt32(rect.right));
        ASSERT_EQ(OK, payload.writeInt32(rect.bottom));
        expectStagedPayloadRejected(payload);
    }
    Parcel trailing;
    ASSERT_EQ(OK, trailing.writeUint32(1));
    ASSERT_EQ(OK, trailing.writeUint32(2));
    ASSERT_EQ(OK, trailing.writeUint32(0));
    ASSERT_EQ(OK, trailing.writeUint32(123));
    expectStagedPayloadRejected(trailing);
}

TEST(WindowInfo, StagedMetadataRejectsBinderObjectsInPrimitivePayload) {
    Parcel payload;
    ASSERT_EQ(OK, payload.writeUint32(1));
    ASSERT_EQ(OK, payload.writeUint32(4));
    ASSERT_EQ(OK, payload.writeStrongBinder(sp<BBinder>::make()));
    ASSERT_GT(payload.objectsCount(), 0u);
    expectStagedPayloadRejected(payload);
}

TEST(WindowInfo, StagedMetadataRejectsEveryTruncatedExtensionAndCommitsNothing) {
    Parcel prefix;
    ASSERT_EQ(OK, writeBeforeLineage(prefix));
    Parcel complete;
    ASSERT_EQ(OK, stagedWindow().writeToParcel(&complete));
    const size_t extensionSize = complete.dataSize() - prefix.dataSize();
    for (size_t removed = 1; removed <= extensionSize; ++removed) {
        SCOPED_TRACE(removed);
        Parcel truncated;
        ASSERT_EQ(OK, stagedWindow().writeToParcel(&truncated));
        ASSERT_EQ(OK, truncated.setDataSize(truncated.dataSize() - removed));
        truncated.setDataPosition(0);
        WindowInfo decoded = stagedWindow();
        EXPECT_NE(OK, decoded.readFromParcel(&truncated));
        expectPrivateMetadataCleared(decoded);
    }
}

TEST(WindowInfo, StagedMetadataMixedWindowInfosUpdateFraming) {
    WindowInfo first = stagedWindow();
    WindowInfo second = lineageWindow();
    second.id++;
    WindowInfo third = lineageWindow();
    third.id += 2;
    third.cloneLineage.reset();
    third.preViewportTouchableRegion.emplace();
    WindowInfo fourth = third;
    fourth.id++;
    fourth.preViewportTouchableRegion.reset();
    gui::DisplayInfo display;
    display.displayId = ui::LogicalDisplayId{7};
    display.logicalWidth = 1280;
    display.logicalHeight = 720;
    gui::WindowInfosUpdate update({first, second, third, fourth}, {display}, 123456, 789012);
    Parcel parcel;
    ASSERT_EQ(OK, update.writeToParcel(&parcel));
    parcel.setDataPosition(0);
    gui::WindowInfosUpdate decoded;
    ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
    ASSERT_EQ(4u, decoded.windowInfos.size());
    EXPECT_EQ(first, decoded.windowInfos[0]);
    EXPECT_EQ(second, decoded.windowInfos[1]);
    EXPECT_EQ(third, decoded.windowInfos[2]);
    EXPECT_EQ(fourth, decoded.windowInfos[3]);
    ASSERT_EQ(1u, decoded.displayInfos.size());
    EXPECT_EQ(display.displayId, decoded.displayInfos[0].displayId);
    EXPECT_EQ(update.vsyncId, decoded.vsyncId);
    EXPECT_EQ(update.timestamp, decoded.timestamp);
    EXPECT_EQ(0u, parcel.dataAvail());
}

TEST(WindowInfo, StagedMetadataMalformedUpdateCannotReadFollowingRecord) {
    Parcel parcel, payload;
    ASSERT_EQ(OK, parcel.writeUint32(2));
    ASSERT_EQ(OK, payload.writeUint32(1));
    ASSERT_EQ(OK, payload.writeUint32(4)); // Tag promised, but missing all five scalars.
    ASSERT_EQ(OK, writeStagedRecord(parcel, payload, static_cast<uint32_t>(payload.dataSize())));
    const size_t followingStart = parcel.dataPosition();
    ASSERT_EQ(OK, lineageWindow().writeToParcel(&parcel));
    ASSERT_EQ(OK, parcel.writeUint32(0));
    ASSERT_EQ(OK, parcel.writeInt64(123456));
    ASSERT_EQ(OK, parcel.writeInt64(789012));
    parcel.setDataPosition(0);
    gui::WindowInfosUpdate decoded;
    EXPECT_NE(OK, decoded.readFromParcel(&parcel));
    ASSERT_EQ(1u, decoded.windowInfos.size());
    expectPrivateMetadataCleared(decoded.windowInfos[0]);
    EXPECT_TRUE(decoded.displayInfos.empty());
    EXPECT_LE(parcel.dataPosition(), followingStart);
}

TEST(WindowInfo, ParcellingWithoutToken) {
    WindowInfo i, i2;
    i.token = nullptr;

    Parcel p;
    ASSERT_EQ(OK, i.writeToParcel(&p));
    p.setDataPosition(0);
    i2.readFromParcel(&p);
    ASSERT_EQ(i2.token, nullptr);
}

TEST(WindowInfo, ParcellingWithoutCloneTransform) {
    WindowInfo i, i2;
    i.cloneLayerStackTransform.reset();

    Parcel p;
    ASSERT_EQ(OK, i.writeToParcel(&p));
    p.setDataPosition(0);
    i2.readFromParcel(&p);
    ASSERT_EQ(i2.cloneLayerStackTransform, std::nullopt);
}

TEST(WindowInfo, Parcelling) {
    sp<IBinder> touchableRegionCropHandle = new BBinder();
    WindowInfo i;
    i.token = new BBinder();
    i.windowToken = new BBinder();
    i.id = 1;
    i.name = "Foobar";
    i.layoutParamsFlags = WindowInfo::Flag::SLIPPERY;
    i.layoutParamsType = WindowInfo::Type::INPUT_METHOD;
    i.dispatchingTimeout = 12s;
    i.frame = Rect(93, 34, 16, 19);
    i.contentSize = Size(10, 40);
    i.surfaceInset = 17;
    i.globalScaleFactor = 0.3;
    i.alpha = 0.7;
    i.transform.set({0.4, -1, 100, 0.5, 0, 40, 0, 0, 1});
    i.touchOcclusionMode = TouchOcclusionMode::ALLOW;
    i.ownerPid = gui::Pid{19};
    i.ownerUid = gui::Uid{24};
    i.packageName = "com.example.package";
    i.inputConfig = WindowInfo::InputConfig::NOT_FOCUSABLE;
    i.displayId = ui::LogicalDisplayId{34};
    i.replaceTouchableRegionWithCrop = true;
    i.touchableRegionCropHandle = touchableRegionCropHandle;
    i.applicationInfo.name = "ApplicationFooBar";
    i.applicationInfo.token = new BBinder();
    i.applicationInfo.dispatchingTimeoutMillis = 0x12345678ABCD;
    i.focusTransferTarget = new BBinder();
    i.cloneLayerStackTransform = ui::Transform();
    i.cloneLayerStackTransform->set({5, -1, 100, 4, 0, 40, 0, 0, 1});

    Parcel p;
    i.writeToParcel(&p);
    p.setDataPosition(0);
    WindowInfo i2;
    i2.readFromParcel(&p);
    ASSERT_EQ(i.token, i2.token);
    ASSERT_EQ(i.windowToken, i2.windowToken);
    ASSERT_EQ(i.id, i2.id);
    ASSERT_EQ(i.name, i2.name);
    ASSERT_EQ(i.layoutParamsFlags, i2.layoutParamsFlags);
    ASSERT_EQ(i.layoutParamsType, i2.layoutParamsType);
    ASSERT_EQ(i.dispatchingTimeout, i2.dispatchingTimeout);
    ASSERT_EQ(i.frame, i2.frame);
    ASSERT_EQ(i.contentSize, i2.contentSize);
    ASSERT_EQ(i.surfaceInset, i2.surfaceInset);
    ASSERT_EQ(i.globalScaleFactor, i2.globalScaleFactor);
    ASSERT_EQ(i.alpha, i2.alpha);
    ASSERT_EQ(i.transform, i2.transform);
    ASSERT_EQ(i.touchOcclusionMode, i2.touchOcclusionMode);
    ASSERT_EQ(i.ownerPid, i2.ownerPid);
    ASSERT_EQ(i.ownerUid, i2.ownerUid);
    ASSERT_EQ(i.packageName, i2.packageName);
    ASSERT_EQ(i.inputConfig, i2.inputConfig);
    ASSERT_EQ(i.displayId, i2.displayId);
    ASSERT_EQ(i.replaceTouchableRegionWithCrop, i2.replaceTouchableRegionWithCrop);
    ASSERT_EQ(i.touchableRegionCropHandle, i2.touchableRegionCropHandle);
    ASSERT_EQ(i.applicationInfo, i2.applicationInfo);
    ASSERT_EQ(i.focusTransferTarget, i2.focusTransferTarget);
    ASSERT_EQ(i.cloneLayerStackTransform, i2.cloneLayerStackTransform);
}

TEST(WindowInfo, ParcelNameTruncation) {
    WindowInfo writeInfo;
    // Construct a long string with a 3-byte UTF-8 character ('あ') to test truncation.
    // 342 characters * 3 bytes/char = 1026 bytes, which is > 1024.
    std::string_view c = "あ";
    ASSERT_EQ(3u, c.size());
    std::string longName;
    longName.reserve(342 * 3);
    for (int i = 0; i < 342; i++) {
        longName.append(c);
    }

    ASSERT_GT(longName.length(), 1024u);
    writeInfo.name = longName;

    Parcel p;
    ASSERT_EQ(OK, writeInfo.writeToParcel(&p));
    p.setDataPosition(0);

    WindowInfo readInfo;
    ASSERT_EQ(OK, readInfo.readFromParcel(&p));

    // The name should be truncated to the last valid character boundary at or before 1024 bytes.
    // 1024 / 3 = 341.33. So, 341 characters should remain. 341 * 3 = 1023 bytes.
    EXPECT_EQ(readInfo.name.length(), 1024u);
    EXPECT_THAT(readInfo.name, testing::EndsWith("[TRUNC]"));
}

TEST(InputApplicationInfo, Parcelling) {
    InputApplicationInfo i;
    i.token = new BBinder();
    i.name = "ApplicationFooBar";
    i.dispatchingTimeoutMillis = 0x12345678ABCD;

    Parcel p;
    ASSERT_EQ(i.writeToParcel(&p), OK);
    p.setDataPosition(0);
    InputApplicationInfo i2;
    ASSERT_EQ(i2.readFromParcel(&p), OK);
    ASSERT_EQ(i, i2);
}

} // namespace test
} // namespace android
