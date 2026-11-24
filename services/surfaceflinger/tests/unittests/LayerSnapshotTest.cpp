/*
 * Copyright 2022 The Android Open Source Project
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

#include <cmath>
#include <limits>
#include <map>

#include <binder/Parcel.h>
#include <com_android_graphics_libgui_flags.h>
#include <common/test/FlagUtils.h>
#include <flag_macros.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gui/WindowInfosUpdate.h>
#include <renderengine/mock/FakeExternalTexture.h>

#include "FrontEnd/LayerHierarchy.h"
#include "FrontEnd/LayerLifecycleManager.h"
#include "FrontEnd/LayerSnapshotBuilder.h"
#include "FrontEnd/LayerSourceInfoBuilder.h"
#include "Layer.h"
#include "LayerHierarchyTest.h"
#include "ui/GraphicTypes.h"

#define UPDATE_AND_VERIFY(BUILDER, ...)                                    \
    ({                                                                     \
        SCOPED_TRACE("");                                                  \
        updateAndVerify((BUILDER), /*displayChanges=*/false, __VA_ARGS__); \
    })

#define UPDATE_AND_VERIFY_WITH_DISPLAY_CHANGES(BUILDER, ...)              \
    ({                                                                    \
        SCOPED_TRACE("");                                                 \
        updateAndVerify((BUILDER), /*displayChanges=*/true, __VA_ARGS__); \
    })

namespace android::surfaceflinger::frontend {

using ftl::Flags;
using namespace ftl::flag_operators;
using namespace com::android::graphics::surfaceflinger;

// To run test:
/**
 mp :libsurfaceflinger_unittest && adb sync; adb shell \
    /data/nativetest/libsurfaceflinger_unittest/libsurfaceflinger_unittest \
    --gtest_filter="LayerSnapshotTest.*" --gtest_brief=1
*/

class LayerSnapshotTest : public LayerSnapshotTestBase {
protected:
    const Layer::FrameRate FRAME_RATE_VOTE1 =
            Layer::FrameRate(67_Hz, scheduler::FrameRateCompatibility::Default);
    const Layer::FrameRate FRAME_RATE_VOTE2 =
            Layer::FrameRate(14_Hz, scheduler::FrameRateCompatibility::Default);
    const Layer::FrameRate FRAME_RATE_VOTE3 =
            Layer::FrameRate(99_Hz, scheduler::FrameRateCompatibility::Default);
    const Layer::FrameRate FRAME_RATE_TREE =
            Layer::FrameRate(Fps(), scheduler::FrameRateCompatibility::NoVote);
    const Layer::FrameRate FRAME_RATE_NO_VOTE =
            Layer::FrameRate(Fps(), scheduler::FrameRateCompatibility::Default);

    LayerSnapshotTest() : LayerSnapshotTestBase() {
        UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    }

    void update(LayerSnapshotBuilder& actualBuilder, LayerSnapshotBuilder::Args& args) {
        if (mLifecycleManager.getGlobalChanges().test(RequestedLayerState::Changes::Hierarchy)) {
            mHierarchyBuilder.update(mLifecycleManager);
        }
        args.root = mHierarchyBuilder.getHierarchy();
        actualBuilder.update(args);
    }

    void update(LayerSnapshotBuilder& actualBuilder) {
        LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                        .layerLifecycleManager = mLifecycleManager,
                                        .includeMetadata = false,
                                        .displays = mFrontEndDisplayInfos,
                                        .globalShadowSettings = globalShadowSettings,
                                        .supportsBlur = true,
                                        .supportedLayerGenericMetadata = {},
                                        .genericLayerMetadataKeyMap = {}};
        update(actualBuilder, args);
    }

    void updateAndVerify(LayerSnapshotBuilder& actualBuilder, bool hasDisplayChanges,
                         const std::vector<uint32_t> expectedVisibleLayerIdsInZOrder) {
        LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                        .layerLifecycleManager = mLifecycleManager,
                                        .includeMetadata = false,
                                        .displays = mFrontEndDisplayInfos,
                                        .displayChanges = hasDisplayChanges,
                                        .globalShadowSettings = globalShadowSettings,
                                        .supportsBlur = true,
                                        .supportedLayerGenericMetadata = {},
                                        .genericLayerMetadataKeyMap = {}};
        update(actualBuilder, args);

        // rebuild layer snapshots from scratch and verify that it matches the updated state.
        LayerSnapshotBuilder expectedBuilder(args);
        mLifecycleManager.commitChanges();
        ASSERT_TRUE(expectedBuilder.getSnapshots().size() > 0);
        ASSERT_TRUE(actualBuilder.getSnapshots().size() > 0);

        std::vector<uint32_t> actualVisibleLayerIdsInZOrder;
        actualBuilder.forEachVisibleSnapshot(
                [&actualVisibleLayerIdsInZOrder](const LayerSnapshot& snapshot) {
                    actualVisibleLayerIdsInZOrder.push_back(snapshot.path.id);
                });
        EXPECT_EQ(expectedVisibleLayerIdsInZOrder, actualVisibleLayerIdsInZOrder);
    }

    LayerSnapshot* getSnapshot(uint32_t layerId) { return mSnapshotBuilder.getSnapshot(layerId); }
    LayerSnapshot* getSnapshot(const LayerHierarchy::TraversalPath path) {
        return mSnapshotBuilder.getSnapshot(path);
    }
    LayerSnapshotBuilder mSnapshotBuilder;
    static const std::vector<uint32_t> STARTING_ZORDER;
};
const std::vector<uint32_t> LayerSnapshotTest::STARTING_ZORDER = {1,   11,   111, 12, 121,
                                                                  122, 1221, 13,  2};

TEST_F(LayerSnapshotTest, buildSnapshot) {
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    LayerSnapshotBuilder builder(args);
}

TEST_F(LayerSnapshotTest, updateSnapshot) {
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}

    };

    LayerSnapshotBuilder builder;
    builder.update(args);
}

// update using parent snapshot data
TEST_F(LayerSnapshotTest, croppedByParent) {
    /// MAKE ALL LAYERS VISIBLE BY DEFAULT
    DisplayInfo info;
    info.info.logicalHeight = 100;
    info.info.logicalWidth = 200;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), info);
    FloatRect layerCrop(0, 0, 10, 20);
    setCrop(11, layerCrop);
    EXPECT_TRUE(mLifecycleManager.getGlobalChanges().test(RequestedLayerState::Changes::Geometry));
    UPDATE_AND_VERIFY_WITH_DISPLAY_CHANGES(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(11)->geomCrop, layerCrop);
    EXPECT_EQ(getSnapshot(111)->geomLayerBounds, layerCrop);
    float maxHeight = static_cast<float>(info.info.logicalHeight * 10);
    float maxWidth = static_cast<float>(info.info.logicalWidth * 10);

    FloatRect maxDisplaySize(-maxWidth, -maxHeight, maxWidth, maxHeight);
    EXPECT_EQ(getSnapshot(1)->geomLayerBounds, maxDisplaySize);
}

// visibility tests
TEST_F(LayerSnapshotTest, newLayerHiddenByPolicy) {
    createLayer(112, 11);
    hideLayer(112);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    showLayer(112);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 111, 112, 12, 121, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, hiddenByParent) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, reparentShowsChild) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    showLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
}

TEST_F(LayerSnapshotTest, reparentHidesChild) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    reparentLayer(121, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, unHidingUpdatesSnapshot) {
    hideLayer(11);
    Rect crop(1, 2, 3, 4);
    setCrop(111, crop);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    showLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(111)->geomLayerBounds, crop.toFloatRect());
}

TEST_F(LayerSnapshotTest, childBehindParentCanBeHiddenByParent) {
    setZ(111, -1);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 111, 11, 12, 121, 122, 1221, 13, 2});

    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});
}

TEST_F(LayerSnapshotTest, offscreenLayerSnapshotIsInvisible) {
    EXPECT_EQ(getSnapshot(111)->isVisible, true);

    reparentLayer(11, UNASSIGNED_LAYER_ID);
    destroyLayerHandle(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});

    EXPECT_EQ(getSnapshot(111)->isVisible, false);
    EXPECT_TRUE(getSnapshot(111)->changes.test(RequestedLayerState::Changes::Visibility));
}

// relative tests
TEST_F(LayerSnapshotTest, RelativeParentCanHideChild) {
    reparentRelativeLayer(13, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 13, 111, 12, 121, 122, 1221, 2});

    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 2});
}

TEST_F(LayerSnapshotTest, ReparentingToHiddenRelativeParentHidesChild) {
    hideLayer(11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 13, 2});
    reparentRelativeLayer(13, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 12, 121, 122, 1221, 2});
}

TEST_F(LayerSnapshotTest, AlphaInheritedByChildren) {
    setAlpha(1, 0.5);
    setAlpha(122, 0.5);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->alpha, 0.5f);
    EXPECT_EQ(getSnapshot(12)->alpha, 0.5f);
    EXPECT_EQ(getSnapshot(1221)->alpha, 0.25f);
}

TEST_F(LayerSnapshotTest, AlphaInheritedByChildWhenParentIsHiddenByInvalidTransform) {
    setMatrix(1, 0, 0, 0, 0);
    update(mSnapshotBuilder);
    mLifecycleManager.commitChanges();

    setAlpha(1, 0.5);
    update(mSnapshotBuilder);
    mLifecycleManager.commitChanges();

    setMatrix(1, 1, 0, 0, 1);
    update(mSnapshotBuilder);
    mLifecycleManager.commitChanges();

    EXPECT_EQ(getSnapshot(1)->alpha, 0.5f);
    EXPECT_EQ(getSnapshot(11)->alpha, 0.5f);
}

TEST_F(LayerSnapshotTest, AlphaInheritedByChildWhenParentIsHidden) {
    hideLayer(1);
    update(mSnapshotBuilder);
    mLifecycleManager.commitChanges();

    setAlpha(1, 0.5);
    update(mSnapshotBuilder);
    mLifecycleManager.commitChanges();

    showLayer(1);
    update(mSnapshotBuilder);
    mLifecycleManager.commitChanges();

    EXPECT_EQ(getSnapshot(1)->alpha, 0.5f);
    EXPECT_EQ(getSnapshot(11)->alpha, 0.5f);
}

// Change states
TEST_F(LayerSnapshotTest, UpdateClearsPreviousChangeStates) {
    setCrop(1, Rect(1, 2, 3, 4));
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->changes.test(RequestedLayerState::Changes::Geometry));
    EXPECT_TRUE(getSnapshot(11)->changes.test(RequestedLayerState::Changes::Geometry));
    setCrop(2, Rect(1, 2, 3, 4));
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(2)->changes.test(RequestedLayerState::Changes::Geometry));
    EXPECT_FALSE(getSnapshot(1)->changes.test(RequestedLayerState::Changes::Geometry));
    EXPECT_FALSE(getSnapshot(11)->changes.test(RequestedLayerState::Changes::Geometry));
}

TEST_F(LayerSnapshotTest, FastPathClearsPreviousChangeStates) {
    setColor(11, {1._hf, 0._hf, 0._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(11)->changes,
              RequestedLayerState::Changes::Content);
    EXPECT_EQ(getSnapshot(11)->clientChanges, layer_state_t::eColorChanged);
    EXPECT_EQ(getSnapshot(1)->changes.get(), 0u);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(11)->changes.get(), 0u);
}

TEST_F(LayerSnapshotTest, FastPathSetsChangeFlagToContent) {
    setColor(1, {1._hf, 0._hf, 0._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->changes,
              RequestedLayerState::Changes::Content);
    EXPECT_EQ(getSnapshot(1)->clientChanges, layer_state_t::eColorChanged);
}

TEST_F(LayerSnapshotTest, ChildrenInheritGameMode) {
    setGameMode(1, gui::GameMode::Performance);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges(),
              RequestedLayerState::Changes::GameMode | RequestedLayerState::Changes::Metadata);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->clientChanges, layer_state_t::eMetadataChanged);
    EXPECT_EQ(getSnapshot(1)->gameMode, gui::GameMode::Performance);
    EXPECT_EQ(getSnapshot(11)->gameMode, gui::GameMode::Performance);
}

TEST_F(LayerSnapshotTest, ChildrenCanOverrideGameMode) {
    setGameMode(1, gui::GameMode::Performance);
    setGameMode(11, gui::GameMode::Battery);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges(),
              RequestedLayerState::Changes::GameMode | RequestedLayerState::Changes::Metadata);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->clientChanges, layer_state_t::eMetadataChanged);
    EXPECT_EQ(getSnapshot(1)->gameMode, gui::GameMode::Performance);
    EXPECT_EQ(getSnapshot(11)->gameMode, gui::GameMode::Battery);
}

TEST_F(LayerSnapshotTest, ReparentingUpdatesGameMode) {
    setGameMode(1, gui::GameMode::Performance);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges(),
              RequestedLayerState::Changes::GameMode | RequestedLayerState::Changes::Metadata);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->clientChanges, layer_state_t::eMetadataChanged);
    EXPECT_EQ(getSnapshot(1)->gameMode, gui::GameMode::Performance);
    EXPECT_EQ(getSnapshot(2)->gameMode, gui::GameMode::Unsupported);

    reparentLayer(2, 1);
    setZ(2, 2);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(2)->gameMode, gui::GameMode::Performance);
}

TEST_F(LayerSnapshotTest, UpdateMetadata) {
    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back();
    transactions.back().states.push_back({});
    transactions.back().states.front().state.what = layer_state_t::eMetadataChanged;
    // This test focuses on metadata used by ARC++ to ensure LayerMetadata is updated correctly,
    // and not using stale data.
    transactions.back().states.front().state.metadata = LayerMetadata();
    transactions.back().states.front().state.metadata.setInt32(METADATA_OWNER_UID, 123);
    transactions.back().states.front().state.metadata.setInt32(METADATA_WINDOW_TYPE, 234);
    transactions.back().states.front().state.metadata.setInt32(METADATA_TASK_ID, 345);
    transactions.back().states.front().state.metadata.setInt32(METADATA_MOUSE_CURSOR, 456);
    transactions.back().states.front().state.metadata.setInt32(METADATA_ACCESSIBILITY_ID, 567);
    transactions.back().states.front().state.metadata.setInt32(METADATA_OWNER_PID, 678);
    transactions.back().states.front().state.metadata.setInt32(METADATA_CALLING_UID, 789);

    transactions.back().states.front().layerId = 1;
    transactions.back().states.front().state.layerId = static_cast<int32_t>(1);

    mLifecycleManager.applyTransactions(transactions);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges(), RequestedLayerState::Changes::Metadata);

    // Setting includeMetadata=true to ensure metadata update is applied to LayerSnapshot
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = true,
                                    .displays = mFrontEndDisplayInfos,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    update(mSnapshotBuilder, args);

    EXPECT_EQ(getSnapshot(1)->clientChanges, layer_state_t::eMetadataChanged);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_OWNER_UID, -1), 123);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_WINDOW_TYPE, -1), 234);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_TASK_ID, -1), 345);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_MOUSE_CURSOR, -1), 456);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_ACCESSIBILITY_ID, -1), 567);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_OWNER_PID, -1), 678);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_CALLING_UID, -1), 789);
}

TEST_F(LayerSnapshotTest, UpdateMetadataOfHiddenLayers) {
    hideLayer(1);

    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back();
    transactions.back().states.push_back({});
    transactions.back().states.front().state.what = layer_state_t::eMetadataChanged;
    // This test focuses on metadata used by ARC++ to ensure LayerMetadata is updated correctly,
    // and not using stale data.
    transactions.back().states.front().state.metadata = LayerMetadata();
    transactions.back().states.front().state.metadata.setInt32(METADATA_OWNER_UID, 123);
    transactions.back().states.front().state.metadata.setInt32(METADATA_WINDOW_TYPE, 234);
    transactions.back().states.front().state.metadata.setInt32(METADATA_TASK_ID, 345);
    transactions.back().states.front().state.metadata.setInt32(METADATA_MOUSE_CURSOR, 456);
    transactions.back().states.front().state.metadata.setInt32(METADATA_ACCESSIBILITY_ID, 567);
    transactions.back().states.front().state.metadata.setInt32(METADATA_OWNER_PID, 678);
    transactions.back().states.front().state.metadata.setInt32(METADATA_CALLING_UID, 789);

    transactions.back().states.front().layerId = 1;
    transactions.back().states.front().state.layerId = static_cast<int32_t>(1);

    mLifecycleManager.applyTransactions(transactions);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges(),
              RequestedLayerState::Changes::Metadata | RequestedLayerState::Changes::Visibility |
                      RequestedLayerState::Changes::VisibleRegion |
                      RequestedLayerState::Changes::AffectsChildren);

    // Setting includeMetadata=true to ensure metadata update is applied to LayerSnapshot
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = true,
                                    .displays = mFrontEndDisplayInfos,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    update(mSnapshotBuilder, args);

    EXPECT_EQ(getSnapshot(1)->clientChanges,
              layer_state_t::eMetadataChanged | layer_state_t::eFlagsChanged);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_OWNER_UID, -1), 123);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_WINDOW_TYPE, -1), 234);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_TASK_ID, -1), 345);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_MOUSE_CURSOR, -1), 456);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_ACCESSIBILITY_ID, -1), 567);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_OWNER_PID, -1), 678);
    EXPECT_EQ(getSnapshot(1)->layerMetadata.getInt32(METADATA_CALLING_UID, -1), 789);
}

TEST_F(LayerSnapshotTest, NoLayerVoteForParentWithChildVotes) {
    // ROOT
    // ├── 1
    // │   ├── 11 (frame rate set)
    // │   │   └── 111
    // │   ├── 12
    // │   │   ├── 121
    // │   │   └── 122
    // │   │       └── 1221
    // │   └── 13
    // └── 2

    setFrameRate(11, 90.0, ANATIVEWINDOW_FRAME_RATE_EXACT, ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot(11)->frameRate.vote.rate.getIntValue(), 90);
    EXPECT_EQ(getSnapshot(11)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);
    EXPECT_EQ(getSnapshot(111)->frameRate.vote.rate.getIntValue(), 90);
    EXPECT_EQ(getSnapshot(111)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);
    EXPECT_EQ(getSnapshot(1)->frameRate.vote.rate.getIntValue(), 0);
    EXPECT_EQ(getSnapshot(1)->frameRate.vote.type, scheduler::FrameRateCompatibility::NoVote);
}

TEST_F(LayerSnapshotTest, NoLayerVoteForParentWithChildVotesDoesNotAffectSiblings) {
    // ROOT
    // ├── 1 (verify layer has no vote)
    // │   ├── 11 (frame rate set)
    // │   │   └── 111
    // │   ├── 12 (frame rate set)
    // │   │   ├── 121
    // │   │   └── 122
    // │   │       └── 1221
    // │   └── 13 (verify layer has default vote)
    // └── 2

    setFrameRate(11, 90.0, ANATIVEWINDOW_FRAME_RATE_EXACT, ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    setFrameRate(12, 45.0, ANATIVEWINDOW_FRAME_RATE_EXACT, ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot(11)->frameRate.vote.rate.getIntValue(), 90);
    EXPECT_EQ(getSnapshot(11)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);
    EXPECT_EQ(getSnapshot(111)->frameRate.vote.rate.getIntValue(), 90);
    EXPECT_EQ(getSnapshot(111)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);
    EXPECT_EQ(getSnapshot(12)->frameRate.vote.rate.getIntValue(), 45);
    EXPECT_EQ(getSnapshot(12)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);
    EXPECT_EQ(getSnapshot(121)->frameRate.vote.rate.getIntValue(), 45);
    EXPECT_EQ(getSnapshot(121)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);
    EXPECT_EQ(getSnapshot(1221)->frameRate.vote.rate.getIntValue(), 45);
    EXPECT_EQ(getSnapshot(1221)->frameRate.vote.type, scheduler::FrameRateCompatibility::Exact);

    EXPECT_EQ(getSnapshot(1)->frameRate.vote.rate.getIntValue(), 0);
    EXPECT_EQ(getSnapshot(1)->frameRate.vote.type, scheduler::FrameRateCompatibility::NoVote);
    EXPECT_EQ(getSnapshot(13)->frameRate.vote.rate.getIntValue(), 0);
    EXPECT_EQ(getSnapshot(13)->frameRate.vote.type, scheduler::FrameRateCompatibility::Default);
    EXPECT_EQ(getSnapshot(2)->frameRate.vote.rate.getIntValue(), 0);
    EXPECT_EQ(getSnapshot(2)->frameRate.vote.type, scheduler::FrameRateCompatibility::Default);
}

TEST_F(LayerSnapshotTest, CanCropTouchableRegion) {
    // ROOT
    // ├── 1
    // │   ├── 11
    // │   │   └── 111 (touchregion set to touch but cropped by layer 13)
    // │   ├── 12
    // │   │   ├── 121
    // │   │   └── 122
    // │   │       └── 1221
    // │   └── 13 (crop set to touchCrop)
    // └── 2

    Rect touchCrop{300, 300, 400, 500};
    setCrop(13, touchCrop);
    Region touch{Rect{0, 0, 1000, 1000}};
    setTouchableRegionCrop(111, touch, /*touchCropId=*/13, /*replaceTouchableRegionWithCrop=*/true);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 111})->inputInfo.touchableRegion.bounds(), touchCrop);

    Rect modifiedTouchCrop{100, 300, 400, 700};
    setCrop(13, modifiedTouchCrop);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 111})->inputInfo.touchableRegion.bounds(), modifiedTouchCrop);
}

TEST_F(LayerSnapshotTest, CanCropTouchableRegionWithDisplayTransform) {
    DisplayInfo displayInfo;
    displayInfo.transform = ui::Transform(ui::Transform::RotationFlags::ROT_90, 1000, 1000);
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), displayInfo);

    Rect touchCrop{300, 300, 400, 500};
    createRootLayer(3);
    setCrop(3, touchCrop);
    setLayerStack(3, 1);
    Region touch{Rect{0, 0, 1000, 1000}};
    setTouchableRegionCrop(3, touch, /*touchCropId=*/3, /*replaceTouchableRegionWithCrop=*/false);

    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3});
    Rect rotatedCrop = {500, 300, 700, 400};
    EXPECT_EQ(getSnapshot({.id = 3})->inputInfo.touchableRegion.bounds(), rotatedCrop);
}

TEST_F(LayerSnapshotTest, blurUpdatesWhenAlphaChanges) {
    int blurRadius = 42;
    setBackgroundBlurRadius(1221, static_cast<uint32_t>(blurRadius));

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->backgroundBlurRadius, blurRadius);

    blurRadius = 21;
    setBackgroundBlurRadius(1221, static_cast<uint32_t>(blurRadius));
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->backgroundBlurRadius, blurRadius);

    static constexpr float alpha = 0.5;
    setAlpha(12, alpha);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->backgroundBlurRadius,
              static_cast<int>(static_cast<float>(blurRadius) * alpha));
}

// Display Mirroring Tests
// tree with 3 levels of children
// ROOT (DISPLAY 0)
// ├── 1
// │   ├── 11
// │   │   └── 111
// │   ├── 12 (has skip screenshot flag)
// │   │   ├── 121
// │   │   └── 122
// │   │       └── 1221
// │   └── 13
// └── 2
// ROOT (DISPLAY 1)
// └── 3 (mirrors display 0)
TEST_F(LayerSnapshotTest, displayMirrorRespectsLayerSkipScreenshotFlag) {
    setFlags(12, layer_state_t::eLayerSkipScreenshot, layer_state_t::eLayerSkipScreenshot);
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 1);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3, 1, 11, 111, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

TEST_F(LayerSnapshotTest, layerMirrorRespectsLayerSkipScreenshotFlag) {
    setFlags(12, layer_state_t::eLayerSkipScreenshot, layer_state_t::eLayerSkipScreenshot);
    createLayerMirrorLayer(3, 1);
    setLayerStack(3, 1);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3, 1, 11, 111, 13};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// ROOT (DISPLAY 0)
// ├── 1
// │   ├── 11
// │   │   └── 111
// │   └── 13
// └── 2
// ROOT (DISPLAY 3)
// └── 3 (mirrors display 0)
TEST_F(LayerSnapshotTest, mirrorLayerGetsCorrectLayerStack) {
    reparentLayer(12, UNASSIGNED_LAYER_ID);
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 3);
    createDisplayMirrorLayer(4, ui::LayerStack::fromValue(0));
    setLayerStack(4, 4);

    std::vector<uint32_t> expected = {1,  11, 111, 13, 2,  3,   1,  11, 111,
                                      13, 2,  4,   1,  11, 111, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    EXPECT_EQ(getSnapshot({.id = 111, .mirrorRootIds = 3u})->outputFilter.layerStack.id, 3u);
    EXPECT_EQ(getSnapshot({.id = 111, .mirrorRootIds = 4u})->outputFilter.layerStack.id, 4u);
}

// ROOT (DISPLAY 0)
// ├── 1 (crop 50x50)
// │   ├── 11
// │   │   └── 111
// │   └── 13
// └── 2
// ROOT (DISPLAY 3)
// └── 3 (mirrors display 0) (crop 100x100)
TEST_F(LayerSnapshotTest, mirrorLayerTouchIsCroppedByMirrorRoot) {
    reparentLayer(12, UNASSIGNED_LAYER_ID);
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 3);
    setCrop(1, Rect{50, 50});
    setCrop(3, Rect{100, 100});
    setCrop(111, Rect{200, 200});
    Region touch{Rect{0, 0, 1000, 1000}};
    setTouchableRegion(111, touch);
    std::vector<uint32_t> expected = {1, 11, 111, 13, 2, 3, 1, 11, 111, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    EXPECT_TRUE(getSnapshot({.id = 111})->inputInfo.touchableRegion.hasSameRects(touch));
    Region touchCroppedByMirrorRoot{Rect{0, 0, 50, 50}};
    EXPECT_TRUE(getSnapshot({.id = 111, .mirrorRootIds = 3u})
                        ->inputInfo.touchableRegion.hasSameRects(touchCroppedByMirrorRoot));
}

TEST_F(LayerSnapshotTest, inputCloneLineageUsesFullSfTraversal) {
    reparentLayer(12, UNASSIGNED_LAYER_ID);
    mirrorLayer(/*layer*/ 14, /*parent*/ 1, /*layerToMirror*/ 11);
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 3);
    setInputInfo(111, [](auto& info) {
        info.cloneLineage =
                gui::WindowInfo::CloneLineage{.sourceLayerId = 999, .mirrorRootIds = {888}};
        info.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
    });
    std::vector<uint32_t> expected = {1, 11, 111, 13, 14, 11, 111, 2, 3,
                                      1, 11, 111, 13, 14, 11, 111, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    const auto& original = getSnapshot({.id = 111})->inputInfo;
    EXPECT_FALSE(original.cloneLineage);
    for (const std::vector<uint32_t>& roots :
         {std::vector<uint32_t>{14}, std::vector<uint32_t>{3}, std::vector<uint32_t>{3, 14}}) {
        LayerHierarchy::TraversalPath path{.id = 111};
        for (uint32_t rootId : roots) path.mirrorRootIds.emplace_back(rootId);
        const LayerSnapshot* snapshot = getSnapshot(path);
        ASSERT_NE(nullptr, snapshot);
        ASSERT_TRUE(snapshot->inputInfo.cloneLineage);
        EXPECT_EQ(111u, snapshot->inputInfo.cloneLineage->sourceLayerId);
        EXPECT_EQ(roots, snapshot->inputInfo.cloneLineage->mirrorRootIds);
        EXPECT_EQ(original.token, snapshot->inputInfo.token);
        EXPECT_TRUE(snapshot->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    }
}

TEST_F(LayerSnapshotTest, inputCloneLineageConstructorMergeAndRecreation) {
    setInputInfo(111, [](auto& info) {
        info.cloneLineage =
                gui::WindowInfo::CloneLineage{.sourceLayerId = 999, .mirrorRootIds = {888}};
    });
    const LayerLifecycleManager& lifecycle = mLifecycleManager;
    const RequestedLayerState* requested = lifecycle.getLayerFromId(111);
    ASSERT_NE(nullptr, requested);
    const LayerHierarchy::TraversalPath path{.id = 111, .mirrorRootIds = {3u, 14u}};
    LayerSnapshot first(*requested, path, LayerSnapshot{});
    LayerSnapshot recreated(*requested, path, LayerSnapshot{});
    ASSERT_TRUE(first.inputInfo.cloneLineage);
    EXPECT_EQ(111u, first.inputInfo.cloneLineage->sourceLayerId);
    EXPECT_EQ((std::vector<uint32_t>{3, 14}), first.inputInfo.cloneLineage->mirrorRootIds);
    EXPECT_EQ(first.inputInfo.cloneLineage, recreated.inputInfo.cloneLineage);
    // Compare observed IDs, never predict or reset SF's internal sequence counter.
    EXPECT_NE(first.inputInfo.id, recreated.inputInfo.id);
    first.merge(*requested, /*forceUpdate=*/true, /*displayChanges=*/false,
                /*forceFullDamage=*/false, /*displayRotationFlags=*/0);
    EXPECT_EQ(recreated.inputInfo.cloneLineage, first.inputInfo.cloneLineage);

    LayerSnapshot original(*requested, {.id = 111}, LayerSnapshot{});
    original.inputInfo.cloneLineage = first.inputInfo.cloneLineage;
    original.merge(*requested, /*forceUpdate=*/true, /*displayChanges=*/false,
                   /*forceFullDamage=*/false, /*displayRotationFlags=*/0);
    EXPECT_FALSE(original.inputInfo.cloneLineage);
}

TEST_F(LayerSnapshotTest, inputCloneLineageSurvivesRequestedInputRefresh) {
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 3);
    setInputInfo(111, [](auto&) {});
    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3,
                                      1, 11, 111, 12, 121, 122, 1221, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    const auto before = getSnapshot({.id = 111, .mirrorRootIds = 3u})->inputInfo.cloneLineage;
    ASSERT_TRUE(before);
    const int32_t beforeId = getSnapshot({.id = 111, .mirrorRootIds = 3u})->inputInfo.id;
    setInputInfo(111, [](auto& info) {
        info.cloneLineage =
                gui::WindowInfo::CloneLineage{.sourceLayerId = 777, .mirrorRootIds = {666, 555}};
        info.inputConfig |= gui::WindowInfo::InputConfig::NOT_FOCUSABLE;
    });
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    EXPECT_FALSE(getSnapshot({.id = 111})->inputInfo.cloneLineage);
    const auto& refreshed = getSnapshot({.id = 111, .mirrorRootIds = 3u})->inputInfo;
    EXPECT_EQ(before, refreshed.cloneLineage);
    EXPECT_EQ(beforeId, refreshed.id);
    EXPECT_TRUE(refreshed.inputConfig.test(gui::WindowInfo::InputConfig::NOT_FOCUSABLE));
}

TEST_F(LayerSnapshotTest, canRemoveDisplayMirror) {
    setFlags(12, layer_state_t::eLayerSkipScreenshot, layer_state_t::eLayerSkipScreenshot);
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 1);
    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3, 1, 11, 111, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    destroyLayerHandle(3);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
}

TEST_F(LayerSnapshotTest, cleanUpUnreachableSnapshotsAfterMirroring) {
    size_t startingNumSnapshots = mSnapshotBuilder.getSnapshots().size();
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 1);
    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3,
                                      1, 11, 111, 12, 121, 122, 1221, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    destroyLayerHandle(3);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(startingNumSnapshots, mSnapshotBuilder.getSnapshots().size());
}

TEST_F(LayerSnapshotTest, canMirrorDisplayWithMirrors) {
    reparentLayer(12, UNASSIGNED_LAYER_ID);
    mirrorLayer(/*layer*/ 14, /*parent*/ 1, /*layerToMirror*/ 11);
    std::vector<uint32_t> expected = {1, 11, 111, 13, 14, 11, 111, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);

    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0));
    setLayerStack(3, 3);
    expected = {1, 11, 111, 13, 14, 11, 111, 2, 3, 1, 11, 111, 13, 14, 11, 111, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    EXPECT_EQ(getSnapshot({.id = 11, .mirrorRootIds = 14u})->outputFilter.layerStack.id, 0u);
    EXPECT_EQ(getSnapshot({.id = 11, .mirrorRootIds = 3u})->outputFilter.layerStack.id, 3u);
    EXPECT_EQ(getSnapshot({.id = 11, .mirrorRootIds = 3u, 14u})->outputFilter.layerStack.id, 3u);
}

// Rel z doesn't create duplicate snapshots but this is for completeness
TEST_F(LayerSnapshotTest, cleanUpUnreachableSnapshotsAfterRelZ) {
    size_t startingNumSnapshots = mSnapshotBuilder.getSnapshots().size();
    reparentRelativeLayer(13, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 13, 111, 12, 121, 122, 1221, 2});
    setZ(13, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(startingNumSnapshots, mSnapshotBuilder.getSnapshots().size());
}

TEST_F(LayerSnapshotTest, cleanUpUnreachableSnapshotsAfterLayerDestruction) {
    size_t startingNumSnapshots = mSnapshotBuilder.getSnapshots().size();
    destroyLayerHandle(2);
    destroyLayerHandle(122);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);

    EXPECT_LE(startingNumSnapshots - 2, mSnapshotBuilder.getSnapshots().size());
}

TEST_F(LayerSnapshotTest, snashotContainsMetadataFromLayerCreationArgs) {
    LayerCreationArgs args(std::make_optional<uint32_t>(200));
    args.name = "testlayer";
    args.addToRoot = true;
    args.metadata.setInt32(42, 24);

    std::vector<std::unique_ptr<RequestedLayerState>> layers;
    layers.emplace_back(std::make_unique<RequestedLayerState>(args));
    EXPECT_TRUE(layers.back()->metadata.has(42));
    EXPECT_EQ(layers.back()->metadata.getInt32(42, 0), 24);
    mLifecycleManager.addLayers(std::move(layers));

    std::vector<uint32_t> expected = STARTING_ZORDER;
    expected.push_back(200);
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);

    EXPECT_TRUE(mSnapshotBuilder.getSnapshot(200)->layerMetadata.has(42));
    EXPECT_EQ(mSnapshotBuilder.getSnapshot(200)->layerMetadata.getInt32(42, 0), 24);
}

TEST_F(LayerSnapshotTest, frameRateSelectionPriorityPassedToChildLayers) {
    setFrameRateSelectionPriority(11, 1);

    setFrameRateSelectionPriority(12, 2);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRateSelectionPriority, Layer::PRIORITY_UNSET);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRateSelectionPriority, 1);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionPriority, 2);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionPriority, 2);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionPriority, 2);

    // reparent and verify the child gets the new parent's framerate selection priority
    reparentLayer(122, 11);

    std::vector<uint32_t> expected = {1, 11, 111, 122, 1221, 12, 121, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRateSelectionPriority, Layer::PRIORITY_UNSET);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRateSelectionPriority, 1);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionPriority, 2);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionPriority, 1);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionPriority, 1);
}

TEST_F(LayerSnapshotTest, framerate) {
    setFrameRate(11, 244.f, 0, 0);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent is gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer and children get the requested votes
    EXPECT_TRUE(getSnapshot({.id = 11})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 11})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_TRUE(getSnapshot({.id = 111})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 111})->changes.test(RequestedLayerState::Changes::FrameRate));

    // reparent and verify the child gets the new parent's framerate
    reparentLayer(122, 11);

    std::vector<uint32_t> expected = {1, 11, 111, 122, 1221, 12, 121, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    // verify parent is gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);

    // verify layer and children get the requested votes
    EXPECT_TRUE(getSnapshot({.id = 11})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);

    EXPECT_TRUE(getSnapshot({.id = 111})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);

    EXPECT_TRUE(getSnapshot({.id = 122})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    // reparent and verify the new parent gets no vote
    reparentLayer(11, 2);
    expected = {1, 12, 121, 13, 2, 11, 111, 122, 1221};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);

    // verify old parent has invalid framerate (default)
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify new parent get no vote
    EXPECT_FALSE(getSnapshot({.id = 2})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 2})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 2})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer and children get the requested votes (unchanged)
    EXPECT_TRUE(getSnapshot({.id = 11})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);

    EXPECT_TRUE(getSnapshot({.id = 111})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);

    EXPECT_TRUE(getSnapshot({.id = 122})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGet) {
    setFrameRate(1, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent is gets no vote
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE1);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGetParent) {
    setFrameRate(111, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_TREE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_TREE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(111, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGetParentAllVote) {
    setFrameRate(1, FRAME_RATE_VOTE3.vote.rate.getValue(), 0, 0);
    setFrameRate(11, FRAME_RATE_VOTE2.vote.rate.getValue(), 0, 0);
    setFrameRate(111, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE3);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE2);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(111, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE3);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE2);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE2);

    setFrameRate(11, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE3);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE3);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE3);

    setFrameRate(1, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGetChild) {
    setFrameRate(1, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(1, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGetChildAllVote) {
    setFrameRate(1, FRAME_RATE_VOTE3.vote.rate.getValue(), 0, 0);
    setFrameRate(11, FRAME_RATE_VOTE2.vote.rate.getValue(), 0, 0);
    setFrameRate(111, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE3);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE2);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(1, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_TREE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE2);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(11, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_TREE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_TREE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(111, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGetChildAddAfterVote) {
    setFrameRate(1, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    reparentLayer(111, 2);
    std::vector<uint32_t> traversalOrder = {1, 11, 12, 121, 122, 1221, 13, 2, 111};
    UPDATE_AND_VERIFY(mSnapshotBuilder, traversalOrder);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);

    reparentLayer(111, 11);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    setFrameRate(1, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, frameRateSetAndGetChildRemoveAfterVote) {
    setFrameRate(1, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_VOTE1);

    reparentLayer(111, 2);
    std::vector<uint32_t> traversalOrder = {1, 11, 12, 121, 122, 1221, 13, 2, 111};
    UPDATE_AND_VERIFY(mSnapshotBuilder, traversalOrder);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);

    setFrameRate(1, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, traversalOrder);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, frameRateAddChildForParentWithTreeVote) {
    setFrameRate(11, FRAME_RATE_VOTE1.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_TREE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_VOTE1);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate, FRAME_RATE_NO_VOTE);

    setFrameRate(11, FRAME_RATE_NO_VOTE.vote.rate.getValue(), 0, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate, FRAME_RATE_NO_VOTE);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate, FRAME_RATE_NO_VOTE);
}

TEST_F(LayerSnapshotTest, translateDataspace) {
    setDataspace(1, ui::Dataspace::UNKNOWN);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->dataspace, ui::Dataspace::V0_SRGB);
}

// This test is similar to "frameRate" test case but checks that the setFrameRateCategory API
// interaction also works correctly with the setFrameRate API within SF frontend.
TEST_F(LayerSnapshotTest, frameRateWithCategory) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, true);

    // ROOT
    // ├── 1
    // │   ├── 11 (frame rate set to 244.f)
    // │   │   └── 111
    // │   ├── 12
    // │   │   ├── 121
    // │   │   └── 122 (frame rate category set to Normal)
    // │   │       └── 1221
    // │   └── 13
    // └── 2
    setFrameRate(11, 244.f, 0, 0);
    setFrameRateCategory(122, ANATIVEWINDOW_FRAME_RATE_CATEGORY_NORMAL);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent 1 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 11 and children 111 get the requested votes
    EXPECT_TRUE(getSnapshot({.id = 11})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 11})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_TRUE(getSnapshot({.id = 111})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 111})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify parent 12 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 12})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 12})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 122 and children 1221 get the requested votes
    EXPECT_FALSE(getSnapshot({.id = 122})->frameRate.vote.rate.isValid());
    EXPECT_TRUE(getSnapshot({.id = 122})->frameRate.isValid());
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.category, FrameRateCategory::Normal);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));
    EXPECT_TRUE(
            getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::AffectsChildren));

    EXPECT_FALSE(getSnapshot({.id = 1221})->frameRate.vote.rate.isValid());
    EXPECT_TRUE(getSnapshot({.id = 1221})->frameRate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.category, FrameRateCategory::Normal);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));
    EXPECT_TRUE(
            getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::AffectsChildren));

    // reparent and verify the child does NOT get the new parent's framerate because it already has
    // the frame rate category specified.
    // ROOT
    //  ├─1
    //  │  ├─11 (frame rate set to 244.f)
    //  │  │  ├─111
    //  │  │  └─122 (frame rate category set to Normal)
    //  │  │     └─1221
    //  │  ├─12
    //  │  │  └─121
    //  │  └─13
    //  └─2
    reparentLayer(122, 11);

    std::vector<uint32_t> expected = {1, 11, 111, 122, 1221, 12, 121, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
    // verify parent is gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);

    // verify layer 11 and children 111 get the requested votes
    EXPECT_TRUE(getSnapshot({.id = 11})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);

    EXPECT_TRUE(getSnapshot({.id = 111})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);

    // verify layer 122 and children 1221 get the requested category vote (unchanged from
    // reparenting)
    EXPECT_FALSE(getSnapshot({.id = 122})->frameRate.vote.rate.isValid());
    EXPECT_TRUE(getSnapshot({.id = 122})->frameRate.isValid());
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.category, FrameRateCategory::Normal);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_FALSE(getSnapshot({.id = 1221})->frameRate.vote.rate.isValid());
    EXPECT_TRUE(getSnapshot({.id = 1221})->frameRate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.category, FrameRateCategory::Normal);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));
}

TEST_F(LayerSnapshotTest, frameRateSelectionStrategy) {
    // ROOT
    // ├── 1
    // │   ├── 11
    // │   │   └── 111
    // │   ├── 12 (frame rate set to 244.f with strategy OverrideChildren)
    // │   │   ├── 121
    // │   │   └── 122 (frame rate set to 123.f but should be overridden by layer 12)
    // │   │       └── 1221
    // │   └── 13
    // └── 2
    setFrameRate(12, 244.f, 0, 0);
    setFrameRate(122, 123.f, 0, 0);
    setFrameRateSelectionStrategy(12, 1 /* OverrideChildren */);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent 1 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 12 and all descendants (121, 122, 1221) get the requested vote
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 12})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 121})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 121})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 121})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));

    // ROOT
    // ├── 1
    // │   ├── 11
    // │   │   └── 111
    // │   ├── 12 (frame rate set to default with strategy default)
    // │   │   ├── 121
    // │   │   └── 122 (frame rate set to 123.f)
    // │   │       └── 1221
    // │   └── 13
    // └── 2
    setFrameRate(12, -1.f, 0, 0);
    setFrameRateSelectionStrategy(12, 0 /* Default */);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent 1 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 12 and all descendants (121, 122, 1221) get the requested vote
    EXPECT_FALSE(getSnapshot({.id = 12})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 12})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_FALSE(getSnapshot({.id = 121})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 121})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_EQ(getSnapshot({.id = 121})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 121})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.rate.getValue(), 123.f);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.vote.rate.getValue(), 123.f);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));
}

TEST_F(LayerSnapshotTest, frameRateSelectionStrategyWithCategory) {
    SET_FLAG_FOR_TEST(flags::frame_rate_category_mrr, true);

    // ROOT
    // ├── 1
    // │   ├── 11
    // │   │   └── 111
    // │   ├── 12 (frame rate category set to high with strategy OverrideChildren)
    // │   │   ├── 121
    // │   │   └── 122 (frame rate set to 123.f but should be overridden by layer 12)
    // │   │       └── 1221
    // │   └── 13
    // └── 2
    setFrameRateCategory(12, ANATIVEWINDOW_FRAME_RATE_CATEGORY_HIGH);
    setFrameRate(122, 123.f, 0, 0);
    setFrameRateSelectionStrategy(12, 1 /* OverrideChildren */);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent 1 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 12 and all descendants (121, 122, 1221) get the requested vote
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.category, FrameRateCategory::High);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 12})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 121})->frameRate.category, FrameRateCategory::High);
    EXPECT_EQ(getSnapshot({.id = 121})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 121})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.category, FrameRateCategory::High);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.category, FrameRateCategory::High);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));

    // ROOT
    // ├── 1
    // │   ├── 11
    // │   │   └── 111
    // │   ├── 12 (frame rate category to default with strategy default)
    // │   │   ├── 121
    // │   │   └── 122 (frame rate set to 123.f)
    // │   │       └── 1221
    // │   └── 13
    // └── 2
    setFrameRateCategory(12, ANATIVEWINDOW_FRAME_RATE_CATEGORY_DEFAULT);
    setFrameRateSelectionStrategy(12, 0 /* Default */);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent 1 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.category, FrameRateCategory::Default);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 12 and all descendants (121, 122, 1221) get the requested vote
    EXPECT_FALSE(getSnapshot({.id = 12})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.category, FrameRateCategory::Default);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 12})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_FALSE(getSnapshot({.id = 12})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 121})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_EQ(getSnapshot({.id = 121})->frameRate.category, FrameRateCategory::Default);
    EXPECT_EQ(getSnapshot({.id = 121})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_TRUE(getSnapshot({.id = 121})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.rate.getValue(), 123.f);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.category, FrameRateCategory::Default);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.vote.rate.getValue(), 123.f);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.category, FrameRateCategory::Default);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));
}

TEST_F(LayerSnapshotTest, frameRateSelectionStrategyWithOverrideChildrenAndSelf) {
    // ROOT
    // ├── 1
    // │   ├── 11 (frame rate set to 11.f with strategy Self)
    // │   │   └── 111 (frame rate is not inherited)
    // │   ├── 12 (frame rate set to 244.f)
    // │   │   ├── 121
    // │   │   └── 122 (strategy OverrideChildren and inherits frame rate 244.f)
    // │   │       └── 1221 (frame rate set to 123.f but should be overridden by layer 122)
    // │   └── 13
    // └── 2
    setFrameRate(11, 11.f, 0, 0);
    setFrameRateSelectionStrategy(11, 2 /* Self */);
    setFrameRate(12, 244.f, 0, 0);
    setFrameRateSelectionStrategy(122, 1 /* OverrideChildren */);
    setFrameRate(1221, 123.f, 0, 0);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // verify parent 1 gets no vote
    EXPECT_FALSE(getSnapshot({.id = 1})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::NoVote);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 11.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Self);
    EXPECT_TRUE(getSnapshot({.id = 11})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 11 does does not propagate its framerate to 111.
    EXPECT_FALSE(getSnapshot({.id = 111})->frameRate.vote.rate.isValid());
    EXPECT_EQ(getSnapshot({.id = 111})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 111})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 12 and all descendants (121, 122, 1221) get the requested vote
    EXPECT_EQ(getSnapshot({.id = 12})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 12})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 12})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 121})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 121})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::Propagate);
    EXPECT_TRUE(getSnapshot({.id = 121})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 122})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 122})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 122})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 1221})->frameRate.vote.rate.getValue(), 244.f);
    EXPECT_EQ(getSnapshot({.id = 1221})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 1221})->changes.test(RequestedLayerState::Changes::FrameRate));

    // ROOT
    // ├── 1 (frame rate set to 1.f with strategy OverrideChildren)
    // │   ├── 11 (frame rate set to 11.f with strategy Self, but overridden by 1)
    // │   │   └── 111 (frame rate inherited from 11 due to override from 1)
    // ⋮   ⋮
    setFrameRate(1, 1.f, 0, 0);
    setFrameRateSelectionStrategy(1, 1 /* OverrideChildren */);
    setFrameRate(11, 11.f, 0, 0);
    setFrameRateSelectionStrategy(11, 2 /* Self */);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.rate.getValue(), 1.f);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRate.vote.type,
              scheduler::FrameRateCompatibility::Default);
    EXPECT_EQ(getSnapshot({.id = 1})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 1})->changes.test(RequestedLayerState::Changes::FrameRate));

    EXPECT_EQ(getSnapshot({.id = 11})->frameRate.vote.rate.getValue(), 1.f);
    EXPECT_EQ(getSnapshot({.id = 11})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 11})->changes.test(RequestedLayerState::Changes::FrameRate));

    // verify layer 11 does does not propagate its framerate to 111.
    EXPECT_EQ(getSnapshot({.id = 111})->frameRate.vote.rate.getValue(), 1.f);
    EXPECT_EQ(getSnapshot({.id = 111})->frameRateSelectionStrategy,
              scheduler::LayerInfo::FrameRateSelectionStrategy::OverrideChildren);
    EXPECT_TRUE(getSnapshot({.id = 111})->changes.test(RequestedLayerState::Changes::FrameRate));
}

TEST_F(LayerSnapshotTest, skipRoundCornersWhenProtected) {
    setRoundedCorners(1, 42.f);
    setRoundedCorners(2, 42.f);
    static gui::CornerRadii RADII = gui::CornerRadii(42.f);
    setCrop(1, Rect{1000, 1000});
    setCrop(2, Rect{1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot({.id = 1})->roundedCorner.hasEffectiveRadii());
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, RADII);
    EXPECT_TRUE(getSnapshot({.id = 2})->roundedCorner.hasEffectiveRadii());

    // add a buffer with the protected bit, check rounded corners are not set when
    // skipRoundCornersWhenProtected == true
    setBuffer(1,
              std::make_shared<
                      renderengine::mock::FakeExternalTexture>(1U /*width*/, 1U /*height*/,
                                                               1ULL /* bufferId */,
                                                               HAL_PIXEL_FORMAT_RGBA_8888,
                                                               GRALLOC_USAGE_PROTECTED /*usage*/));

    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .displayChanges = false,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {},
                                    .skipRoundCornersWhenProtected = true};
    update(mSnapshotBuilder, args);
    EXPECT_FALSE(getSnapshot({.id = 1})->roundedCorner.hasEffectiveRadii());
    // layer 2 doesn't have a buffer and should be unaffected
    EXPECT_TRUE(getSnapshot({.id = 2})->roundedCorner.hasEffectiveRadii());

    // remove protected bit, check rounded corners are set
    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1U /*width*/, 1U /*height*/,
                                                                        2ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    update(mSnapshotBuilder, args);
    EXPECT_TRUE(getSnapshot({.id = 1})->roundedCorner.hasEffectiveRadii());
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, RADII);
}

TEST_F(LayerSnapshotTest, setRefreshRateIndicatorCompositionType) {
    setFlags(1, layer_state_t::eLayerIsRefreshRateIndicator,
             layer_state_t::eLayerIsRefreshRateIndicator);
    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1U /*width*/, 1U /*height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->compositionType,
              aidl::android::hardware::graphics::composer3::Composition::REFRESH_RATE_INDICATOR);
}

TEST_F(LayerSnapshotTest, setBufferCrop) {
    // validate no buffer but has crop
    Rect crop = Rect(0, 0, 50, 50);
    setBufferCrop(1, crop);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->geomContentCrop, crop);

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(100U /*width*/,
                                                                        100U /*height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    // validate a buffer crop within the buffer bounds
    setBufferCrop(1, crop);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->geomContentCrop, crop);

    // validate a buffer crop outside the buffer bounds
    crop = Rect(0, 0, 150, 150);
    setBufferCrop(1, crop);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->geomContentCrop, Rect(0, 0, 100, 100));

    // validate no buffer crop
    setBufferCrop(1, Rect());
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->geomContentCrop, Rect(0, 0, 100, 100));
}

TEST_F(LayerSnapshotTest, setCornerRadius) {
    static constexpr float RADIUS = 123.f;
    setRoundedCorners(1, RADIUS);
    setCrop(1, Rect{1000, 1000});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.topLeft.x, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.topLeft.y, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.topRight.x, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.topRight.y, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.bottomLeft.x, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.bottomLeft.y, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.bottomRight.x, RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii.bottomRight.y, RADIUS);
}

TEST_F(LayerSnapshotTest, setCornerRadiusFourDistinctRadii) {
    static gui::CornerRadii RADIUS = gui::CornerRadii(111.f, 222.f, 333.f, 444.f);
    setRoundedCorners(1, 111.f, 222.f, 333.f, 444.f);
    setCrop(1, Rect{1000, 1000});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, RADIUS);
}

TEST_F(LayerSnapshotTest, setClientDrawnCornerRadius) {
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii EXPECTED_CLIENT_DRAWN_RADIUS = gui::CornerRadii(RADIUS);
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);
    setClientDrawnCornerRadius(1, RADIUS, FloatRect{0, 0, 1000, 1000});
    setRoundedCorners(1, RADIUS);
    setCrop(1, Rect{1000, 1000});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot({.id = 1})->roundedCorner.hasClientDrawnRadius());
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, EXPECTED_CLIENT_DRAWN_RADIUS);
}

TEST_F(LayerSnapshotTest, setClientDrawnCornerRadiusFourCorners) {
    static gui::CornerRadii RADIUS = gui::CornerRadii(111.f, 222.f, 333.f, 444.f);
    static gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);
    setClientDrawnCornerRadius(1, 111.f, 222.f, 333.f, 444.f, FloatRect{0, 0, 1000, 1000});
    setRoundedCorners(1, 111.f, 222.f, 333.f, 444.f);
    setCrop(1, Rect{1000, 1000});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot({.id = 1})->roundedCorner.hasClientDrawnRadius());
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, RADIUS);
}

TEST_F(LayerSnapshotTest, childInheritsParentScaledSettings) {
    // ROOT
    // ├── 1 (crop rect set to contain child layer)
    // │   ├── 11
    static constexpr float RADIUS = 123.f;
    static gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        1ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setBuffer(11,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        2ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));

    setRoundedCorners(1, RADIUS);

    FloatRect parentCropRect(10, 10, 990, 990);
    setCrop(1, parentCropRect);

    // Rotate surface by 90
    setMatrix(11, 0.f, -1.f, 1.f, 0.f);
    // ensure there is overlap
    setPosition(11, 500, 500);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    ui::Transform t = getSnapshot({.id = 11})->localTransform.inverse();

    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.cropRect, t.transform(parentCropRect));
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.topLeft.x,
              RADIUS * t.getScaleX());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.topLeft.y,
              RADIUS * t.getScaleY());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.topRight.x,
              RADIUS * t.getScaleX());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.topRight.y,
              RADIUS * t.getScaleY());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.bottomLeft.x,
              RADIUS * t.getScaleX());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.bottomLeft.y,
              RADIUS * t.getScaleY());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.bottomRight.x,
              RADIUS * t.getScaleX());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii.bottomRight.y,
              RADIUS * t.getScaleY());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.requestedRadii, ZERO_RADIUS);
}

TEST_F(LayerSnapshotTest, childDoesNotInheritParentSettingsWhenNoCornerOverlap) {
    // ROOT
    // ├── 1 (crop rect set to contain child layer)
    // │   ├── 11
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        1ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setBuffer(11,
              std::make_shared<renderengine::mock::FakeExternalTexture>(200 /*width*/,
                                                                        200 /*height*/,
                                                                        2ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));

    setRoundedCorners(1, RADIUS);

    FloatRect parentCropRect(0, 0, 1000, 1000);
    setCrop(1, parentCropRect);

    setPosition(11, 300, 300);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, ZERO_RADIUS);
}

TEST_F(LayerSnapshotTest, childInheritsParentSettingsWhenCropIsEmpty) {
    // ROOT
    // ├── 1 (crop rect set to contain child layer)
    // │   ├── 11
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        1ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    // No buffer is set on child layer

    setRoundedCorners(1, RADIUS);

    FloatRect parentCropRect(0, 0, 1000, 1000);
    setCrop(1, parentCropRect);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, gui::CornerRadii(RADIUS));
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.cropRect, parentCropRect);
}

TEST_F(LayerSnapshotTest, childScaledInheritsParentSettings) {
    // ROOT
    // ├── 1 (crop rect set to contain child layer)
    // │   ├── 11
    static constexpr float RADIUS = 250.f;

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        1ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setBuffer(11,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        2ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));

    setRoundedCorners(1, RADIUS);

    FloatRect parentCropRect(0, 0, 1000, 1000);
    setCrop(1, parentCropRect);

    // Scale child layer up
    setMatrix(11, 2.f, 0.f, 0.f, 2.f);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    ui::Transform t = getSnapshot({.id = 11})->localTransform.inverse();

    EXPECT_TRUE(getSnapshot({.id = 11})->roundedCorner.hasEffectiveRadii());
}

TEST_F(LayerSnapshotTest, SetClientDrawnClippedRadii) {
    static const gui::CornerRadii RADIUS = gui::CornerRadii(111.f, 222.f, 333.f, 444.f);
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);
    static const gui::CornerRadii CLIPPED_RADIUS = gui::CornerRadii(111.f, 222.f, 0.f, 0.f);

    // set parent(1) crop to clip the bottom half of child(11)
    setCrop(1, Rect{1000, 500});

    setRoundedCorners(11, 111.f, 222.f, 333.f, 444.f);
    setCrop(11, Rect{1000, 1000});
    setClientDrawnCornerRadius(11, 111.f, 222.f, 0.f, 0.f, FloatRect{0, 0, 1000, 500});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 11})->roundedCorner.hasClientDrawnRadius());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.clientDrawnRadii, CLIPPED_RADIUS);
}

TEST_F(LayerSnapshotTest, shouldDisableCornerRounding_EmptyClientDrawnRadii) {
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);
    static const gui::CornerRadii ACTUAL_RADIUS = gui::CornerRadii(RADIUS);

    setRoundedCorners(1, RADIUS);
    setCrop(1, Rect{1000, 1000});
    setClientDrawnCornerRadius(1, RADIUS, FloatRect{0, 0, 1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, ACTUAL_RADIUS);

    // ClientDrawnRadii is empty -> SF draws corners
    setClientDrawnCornerRadius(1, 0.f, FloatRect{0, 0, 1000, 1000});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ACTUAL_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, ZERO_RADIUS);
}

TEST_F(LayerSnapshotTest, shouldDisableCornerRounding_BoundsMismatch) {
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);
    static const gui::CornerRadii ACTUAL_RADIUS = gui::CornerRadii(RADIUS);

    setRoundedCorners(1, RADIUS);
    setCrop(1, Rect{1000, 1000});
    setClientDrawnCornerRadius(1, RADIUS, FloatRect{0, 0, 1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, ACTUAL_RADIUS);

    // Bounds don't match -> SF draws corners
    setClientDrawnCornerRadius(1, RADIUS, FloatRect{0, 0, 999, 999});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ACTUAL_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, ACTUAL_RADIUS);
}

TEST_F(LayerSnapshotTest, clientDrawnCornerRadiiOnlyUpdatesOnFlag) {
    static constexpr float RADIUS_A = 123.f;
    static constexpr float RADIUS_B = 456.f;
    static const gui::CornerRadii CORNER_A = gui::CornerRadii(RADIUS_A);
    static const gui::CornerRadii CORNER_B = gui::CornerRadii(RADIUS_B);

    // Set the client drawn corner radius to A
    setRoundedCorners(1, RADIUS_A);
    setCrop(1, Rect{1000, 1000});
    setClientDrawnCornerRadius(1, RADIUS_A, FloatRect{0, 0, 1000, 1000});

    // Should be A
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, CORNER_A);

    // Trigger an update that does NOT include eClientDrawnCornerRadiusChanged
    setAlpha(1, 0.5f);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    // Should still be A
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, CORNER_A);

    // Now trigger an update includes eClientDrawnCornerRadiusChanged
    setClientDrawnCornerRadius(1, RADIUS_B, FloatRect{0, 0, 1000, 1000});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    // Should now be B
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.clientDrawnRadii, CORNER_B);
}

TEST_F(LayerSnapshotTest, reportedRadiiWithCornerRegionOverlap) {
    static const gui::CornerRadii RADIUS = gui::CornerRadii(111.f, 222.f, 333.f, 444.f);

    // set parent(1) crop to clip the bottom half of child(11) with some corner region overlap
    setCrop(1, Rect{1000, 900});

    setRoundedCorners(11, 111.f, 222.f, 333.f, 444.f);
    setCrop(11, Rect{1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.reportedRadii, RADIUS);
}

TEST_F(LayerSnapshotTest, reportedRadiiWithoutCornerRegionOverlap) {
    static const gui::CornerRadii RADIUS = gui::CornerRadii(111.f, 222.f, 333.f, 444.f);
    static const gui::CornerRadii CLIPPED_RADIUS = gui::CornerRadii(111.f, 222.f, 0.f, 0.f);

    // set parent(1) crop to clip the bottom half of child(11) with no corner region overlap for
    // bottom right and bottom left corners
    setCrop(1, Rect{1000, 500});

    setRoundedCorners(11, 111.f, 222.f, 333.f, 444.f);
    setCrop(11, Rect{1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.reportedRadii, CLIPPED_RADIUS);
}

TEST_F(LayerSnapshotTest, childInheritsParentClientDrawnCornerRadius) {
    // ROOT
    // ├── 1 (crop rect set to contain child layers )
    // │   ├── 11
    // │   │   └── 111

    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii RADII = gui::CornerRadii(RADIUS);

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        1ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setBuffer(11,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        2ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));

    setRoundedCorners(1, RADIUS);
    setClientDrawnCornerRadius(1, RADIUS, FloatRect{0, 0, 1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot({.id = 1})->roundedCorner.hasClientDrawnRadius());
    EXPECT_TRUE(getSnapshot({.id = 11})->roundedCorner.hasEffectiveRadii());
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, RADII);
}

TEST_F(LayerSnapshotTest, childIgnoreCornerRadiusOverridesParent) {
    // ROOT
    // ├── 1 (crop rect set to contain child layers )
    // │   ├── 11
    // │   │   └── 111

    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii RADII = gui::CornerRadii(RADIUS);

    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        1ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setBuffer(11,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        2ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setBuffer(111,
              std::make_shared<renderengine::mock::FakeExternalTexture>(1000 /*width*/,
                                                                        1000 /*height*/,
                                                                        3ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));

    setRoundedCorners(1, RADIUS);

    setRoundedCorners(11, RADIUS);
    setClientDrawnCornerRadius(11, RADIUS, FloatRect{0, 0, 1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, RADII);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, gui::CornerRadii(0.f));
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.clientDrawnRadii, RADII);
    EXPECT_EQ(getSnapshot({.id = 111})->roundedCorner.sfDrawnRadii, RADII);
}

TEST_F(LayerSnapshotTest, childInheritsParentDisableClientDrawnRadiusOpt) {
    // ROOT
    // ├── 1 (crop rect set to contain child layers)
    // │   ├── 11
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);

    setRoundedCorners(1, RADIUS);
    setCrop(1, Rect{1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.reportedRadii, gui::CornerRadii(RADIUS));

    // Disable the optimization on the parent (Task)
    setFlags(1, layer_state_t::eRoundedCornerOptDisabled, layer_state_t::eRoundedCornerOptDisabled);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 11})->roundedCorner.disableClientDrawnRadii);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.reportedRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, gui::CornerRadii(RADIUS));
}

TEST_F(LayerSnapshotTest, childInheritsParentDisableClientDrawnRadiusOptEvenIfParentNotRounded) {
    // ROOT
    // ├── 1 (Task - No rounded corners)
    // │   ├── 11 (Activity - Has rounded corners)
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ACTUAL_RADIUS = gui::CornerRadii(RADIUS);
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);

    setRoundedCorners(11, RADIUS);
    setCrop(11, Rect{1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    // Initially optimization is enabled for child
    EXPECT_FALSE(getSnapshot({.id = 11})->roundedCorner.disableClientDrawnRadii);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.reportedRadii, ACTUAL_RADIUS);

    // Disable the optimization on the parent (Task) which has NO rounded corners
    setFlags(1, layer_state_t::eRoundedCornerOptDisabled, layer_state_t::eRoundedCornerOptDisabled);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    // Child should inherit the disabled flag even though parent is not rounded
    EXPECT_TRUE(getSnapshot({.id = 11})->roundedCorner.disableClientDrawnRadii);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.reportedRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 11})->roundedCorner.sfDrawnRadii, ACTUAL_RADIUS);
}

TEST_F(LayerSnapshotTest, disableClientDrawnRadiusOpt) {
    static constexpr float RADIUS = 123.f;
    static const gui::CornerRadii ZERO_RADIUS = gui::CornerRadii(0.f);
    static const gui::CornerRadii ACTUAL_RADIUS = gui::CornerRadii(RADIUS);

    setRoundedCorners(1, RADIUS);
    setCrop(1, Rect{1000, 1000});

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot({.id = 1})->roundedCorner.disableClientDrawnRadii);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.reportedRadii, ACTUAL_RADIUS);

    // Disable optimization (Transition Start)
    setFlags(1, layer_state_t::eRoundedCornerOptDisabled, layer_state_t::eRoundedCornerOptDisabled);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 1})->roundedCorner.disableClientDrawnRadii);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.reportedRadii, ZERO_RADIUS);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.sfDrawnRadii, ACTUAL_RADIUS);

    // Re-enable optimization (Transition Finish/Cleanup)
    setFlags(1, layer_state_t::eRoundedCornerOptDisabled, 0);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_FALSE(getSnapshot({.id = 1})->roundedCorner.disableClientDrawnRadii);
    EXPECT_EQ(getSnapshot({.id = 1})->roundedCorner.reportedRadii, ACTUAL_RADIUS);
}

TEST_F(LayerSnapshotTest, setShadowRadius) {
    static constexpr float SHADOW_RADIUS = 123.f;
    setShadowRadius(1, SHADOW_RADIUS);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->shadowSettings.length, SHADOW_RADIUS);
}

TEST_F(LayerSnapshotTest, setBorderSettings) {
    gui::BorderSettings settings;
    settings.strokeWidth = 5;
    setBorderSettings(1, settings);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->borderSettings.strokeWidth, settings.strokeWidth);
}

TEST_F(LayerSnapshotTest, setTrustedOverlayForNonVisibleInput) {
    hideLayer(1);
    setTrustedOverlay(1, gui::TrustedOverlay::ENABLED);
    Region touch{Rect{0, 0, 1000, 1000}};
    setTouchableRegion(1, touch);

    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
}

TEST_F(LayerSnapshotTest, alphaChangesPropagateToInput) {
    Region touch{Rect{0, 0, 1000, 1000}};
    setTouchableRegion(1, touch);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    setAlpha(1, 0.5f);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot(1)->inputInfo.alpha, 0.5f);
}

TEST_F(LayerSnapshotTest, isFrontBuffered) {
    setBuffer(1,
              std::make_shared<renderengine::mock::FakeExternalTexture>(
                      1U /*width*/, 1U /*height*/, 1ULL /* bufferId */, HAL_PIXEL_FORMAT_RGBA_8888,
                      GRALLOC_USAGE_HW_TEXTURE | AHARDWAREBUFFER_USAGE_FRONT_BUFFER /*usage*/));

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->isFrontBuffered());

    setBuffer(1,
              std::make_shared<
                      renderengine::mock::FakeExternalTexture>(1U /*width*/, 1U /*height*/,
                                                               1ULL /* bufferId */,
                                                               HAL_PIXEL_FORMAT_RGBA_8888,
                                                               GRALLOC_USAGE_HW_TEXTURE /*usage*/));

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot(1)->isFrontBuffered());
}

TEST_F(LayerSnapshotTest, setSecureRootSnapshot) {
    setFlags(1, layer_state_t::eLayerSecure, layer_state_t::eLayerSecure);
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .displayChanges = false,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    args.rootSnapshot.isSecure = true;
    update(mSnapshotBuilder, args);

    EXPECT_TRUE(getSnapshot(1)->isSecure);
    // Ensure child is also marked as secure
    EXPECT_TRUE(getSnapshot(11)->isSecure);
}

TEST_F(LayerSnapshotTest, setSensitiveForTracingConfigForSecureLayers) {
    setFlags(11, layer_state_t::eLayerSecure, layer_state_t::eLayerSecure);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot(11)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_FALSE(getSnapshot(1)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_FALSE(getSnapshot(12)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_FALSE(getSnapshot(2)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
}

TEST_F(LayerSnapshotTest, setSensitiveForTracingFromInputWindowHandle) {
    setInputInfo(11, [](auto& inputInfo) {
        inputInfo.inputConfig |= gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY;
    });

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot(11)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_FALSE(getSnapshot(1)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_FALSE(getSnapshot(12)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
    EXPECT_FALSE(getSnapshot(2)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::SENSITIVE_FOR_PRIVACY));
}

// b/314350323
TEST_F(LayerSnapshotTest, propagateDropInputMode) {
    setDropInputMode(1, gui::DropInputMode::ALL);
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .displayChanges = false,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    args.rootSnapshot.isSecure = true;
    update(mSnapshotBuilder, args);

    EXPECT_EQ(getSnapshot(1)->dropInputMode, gui::DropInputMode::ALL);
    // Ensure child also has the correct drop input mode regardless of whether either layer has
    // an input channel
    EXPECT_EQ(getSnapshot(11)->dropInputMode, gui::DropInputMode::ALL);
}

TEST_F(LayerSnapshotTest, NonVisibleLayerWithInput) {
    SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::
                              skip_invisible_windows_in_input,
                      false);
    LayerHierarchyTestBase::createRootLayer(3);
    setColor(3, {-1._hf, -1._hf, -1._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back();
    transactions.back().states.push_back({});
    transactions.back().states.front().state.what = layer_state_t::eInputInfoChanged;
    transactions.back().states.front().layerId = 3;
    auto inputInfo = transactions.back().states.front().state.editWindowInfo();
    *inputInfo = {};
    inputInfo->token = sp<BBinder>::make();
    mLifecycleManager.applyTransactions(transactions);

    update(mSnapshotBuilder);

    bool foundInputLayer = false;
    mSnapshotBuilder.forEachInputSnapshot([&](const frontend::LayerSnapshot& snapshot) {
        if (snapshot.uniqueSequence == 3) {
            foundInputLayer = true;
        }
    });
    EXPECT_TRUE(foundInputLayer);
}

TEST_F(LayerSnapshotTest, NonVisibleLayerWithInputShouldNotBeIncluded) {
    SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::
                              skip_invisible_windows_in_input,
                      true);
    LayerHierarchyTestBase::createRootLayer(3);
    setColor(3, {-1._hf, -1._hf, -1._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back();
    transactions.back().states.push_back({});
    transactions.back().states.front().state.what = layer_state_t::eInputInfoChanged;
    transactions.back().states.front().layerId = 3;
    auto inputInfo = transactions.back().states.front().state.editWindowInfo();
    *inputInfo = {};
    inputInfo->token = sp<BBinder>::make();
    hideLayer(3);
    mLifecycleManager.applyTransactions(transactions);

    update(mSnapshotBuilder);

    bool foundInputLayer = false;
    mSnapshotBuilder.forEachInputSnapshot([&](const frontend::LayerSnapshot& snapshot) {
        if (snapshot.uniqueSequence == 3) {
            EXPECT_TRUE(
                    snapshot.inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NOT_VISIBLE));
            EXPECT_FALSE(snapshot.isVisible);
            foundInputLayer = true;
        }
    });
    EXPECT_FALSE(foundInputLayer);
}

TEST_F(LayerSnapshotTest, ForEachNullableSnapshotsWithPredicate) {
    std::vector<uint32_t> visitedUniqueSequences;
    mSnapshotBuilder.forEachNonNullSnapshot(
            [&](const std::unique_ptr<frontend::LayerSnapshot>& snapshot) {
                visitedUniqueSequences.push_back(snapshot->uniqueSequence);
            },
            [](const frontend::LayerSnapshot& snapshot) { return snapshot.uniqueSequence == 111; });
    EXPECT_EQ(visitedUniqueSequences.size(), 1u);
    EXPECT_EQ(visitedUniqueSequences[0], 111u);
}

TEST_F(LayerSnapshotTest, canOccludePresentation) {
    setFlags(12, layer_state_t::eCanOccludePresentation, layer_state_t::eCanOccludePresentation);
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .displayChanges = false,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_EQ(getSnapshot(1)->inputInfo.canOccludePresentation, false);

    // ensure we can set the property on the window info for layer and all its children
    EXPECT_EQ(getSnapshot(12)->inputInfo.canOccludePresentation, true);
    EXPECT_EQ(getSnapshot(121)->inputInfo.canOccludePresentation, true);
    EXPECT_EQ(getSnapshot(1221)->inputInfo.canOccludePresentation, true);
}

TEST_F(LayerSnapshotTest, mirroredHierarchyIgnoresLocalTransform) {
    reparentLayer(12, UNASSIGNED_LAYER_ID);
    setPosition(11, 2, 20);
    setPosition(111, 20, 200);
    mirrorLayer(/*layer*/ 14, /*parent*/ 1, /*layerToMirror*/ 11);
    std::vector<uint32_t> expected = {1, 11, 111, 13, 14, 11, 111, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);

    // mirror root has no position set
    EXPECT_EQ(getSnapshot({.id = 11, .mirrorRootIds = 14u})->localTransform.tx(), 0);
    EXPECT_EQ(getSnapshot({.id = 11, .mirrorRootIds = 14u})->localTransform.ty(), 0);
    // original root still has a position
    EXPECT_EQ(getSnapshot({.id = 11})->localTransform.tx(), 2);
    EXPECT_EQ(getSnapshot({.id = 11})->localTransform.ty(), 20);

    // mirror child still has the correct position
    EXPECT_EQ(getSnapshot({.id = 111, .mirrorRootIds = 14u})->localTransform.tx(), 20);
    EXPECT_EQ(getSnapshot({.id = 111, .mirrorRootIds = 14u})->localTransform.ty(), 200);
    EXPECT_EQ(getSnapshot({.id = 111, .mirrorRootIds = 14u})->geomLayerTransform.tx(), 20);
    EXPECT_EQ(getSnapshot({.id = 111, .mirrorRootIds = 14u})->geomLayerTransform.ty(), 200);

    // original child still has the correct position including its parent's position
    EXPECT_EQ(getSnapshot({.id = 111})->localTransform.tx(), 20);
    EXPECT_EQ(getSnapshot({.id = 111})->localTransform.ty(), 200);
    EXPECT_EQ(getSnapshot({.id = 111})->geomLayerTransform.tx(), 22);
    EXPECT_EQ(getSnapshot({.id = 111})->geomLayerTransform.ty(), 220);
}

TEST_F(LayerSnapshotTest, overrideParentTrustedOverlayState) {
    hideLayer(1);
    setTrustedOverlay(1, gui::TrustedOverlay::ENABLED);

    Region touch{Rect{0, 0, 1000, 1000}};
    setTouchableRegion(1, touch);
    setTouchableRegion(11, touch);
    setTouchableRegion(111, touch);

    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
    EXPECT_TRUE(getSnapshot(11)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));

    // disable trusted overlay and override parent state
    setTrustedOverlay(11, gui::TrustedOverlay::DISABLED);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
    EXPECT_FALSE(getSnapshot(11)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
    EXPECT_FALSE(getSnapshot(111)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));

    // unset state and go back to default behavior of inheriting
    // state
    setTrustedOverlay(11, gui::TrustedOverlay::UNSET);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
    EXPECT_TRUE(getSnapshot(11)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::TRUSTED_OVERLAY));
}

static constexpr const FloatRect LARGE_FLOAT_RECT{std::numeric_limits<float>::min(),
                                                  std::numeric_limits<float>::min(),
                                                  std::numeric_limits<float>::max(),
                                                  std::numeric_limits<float>::max()};
TEST_F(LayerSnapshotTest, layerVisibleByDefault) {
    DisplayInfo info;
    info.info.logicalHeight = 1000000;
    info.info.logicalWidth = 1000000;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), info);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot(1)->isHiddenByPolicy());
}

TEST_F(LayerSnapshotTest, hideLayerWithZeroMatrix) {
    DisplayInfo info;
    info.info.logicalHeight = 1000000;
    info.info.logicalWidth = 1000000;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), info);
    setMatrix(1, 0.f, 0.f, 0.f, 0.f);
    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->isHiddenByPolicy());
}

TEST_F(LayerSnapshotTest, hideLayerWithInfMatrix) {
    DisplayInfo info;
    info.info.logicalHeight = 1000000;
    info.info.logicalWidth = 1000000;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), info);
    setMatrix(1, std::numeric_limits<float>::infinity(), 0.f, 0.f,
              std::numeric_limits<float>::infinity());
    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->isHiddenByPolicy());
}

TEST_F(LayerSnapshotTest, hideLayerWithNanMatrix) {
    DisplayInfo info;
    info.info.logicalHeight = 1000000;
    info.info.logicalWidth = 1000000;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(1), info);
    setMatrix(1, std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f,
              std::numeric_limits<float>::quiet_NaN());
    UPDATE_AND_VERIFY(mSnapshotBuilder, {2});
    EXPECT_TRUE(getSnapshot(1)->isHiddenByPolicy());
}

TEST_F(LayerSnapshotTest, edgeExtensionPropagatesInHierarchy) {
    setCrop(1, Rect(0, 0, 20, 20));
    setBuffer(1221,
              std::make_shared<renderengine::mock::FakeExternalTexture>(20 /* width */,
                                                                        20 /* height */,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    setEdgeExtensionEffect(12, LEFT);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 12})->edgeExtensionEffect.extendsEdge(LEFT));
    EXPECT_TRUE(getSnapshot({.id = 121})->edgeExtensionEffect.extendsEdge(LEFT));
    EXPECT_TRUE(getSnapshot({.id = 1221})->edgeExtensionEffect.extendsEdge(LEFT));

    setEdgeExtensionEffect(12, RIGHT);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 12})->edgeExtensionEffect.extendsEdge(RIGHT));
    EXPECT_TRUE(getSnapshot({.id = 121})->edgeExtensionEffect.extendsEdge(RIGHT));
    EXPECT_TRUE(getSnapshot({.id = 1221})->edgeExtensionEffect.extendsEdge(RIGHT));

    setEdgeExtensionEffect(12, TOP);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 12})->edgeExtensionEffect.extendsEdge(TOP));
    EXPECT_TRUE(getSnapshot({.id = 121})->edgeExtensionEffect.extendsEdge(TOP));
    EXPECT_TRUE(getSnapshot({.id = 1221})->edgeExtensionEffect.extendsEdge(TOP));

    setEdgeExtensionEffect(12, BOTTOM);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot({.id = 12})->edgeExtensionEffect.extendsEdge(BOTTOM));
    EXPECT_TRUE(getSnapshot({.id = 121})->edgeExtensionEffect.extendsEdge(BOTTOM));
    EXPECT_TRUE(getSnapshot({.id = 1221})->edgeExtensionEffect.extendsEdge(BOTTOM));
}

TEST_F(LayerSnapshotTest, leftEdgeExtensionIncreaseBoundSizeWithinCrop) {
    // The left bound is extended when shifting to the right
    setCrop(1, Rect(0, 0, 20, 20));
    const int texSize = 10;
    setBuffer(1221,
              std::make_shared<renderengine::mock::FakeExternalTexture>(texSize /* width */,
                                                                        texSize /* height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    const float translation = 5.0;
    setPosition(12, translation, 0);
    setEdgeExtensionEffect(12, LEFT);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->transformedBounds.right, texSize + translation);
    EXPECT_LT(getSnapshot({.id = 1221})->transformedBounds.left, translation);
    EXPECT_GE(getSnapshot({.id = 1221})->transformedBounds.left, 0.0);
}

TEST_F(LayerSnapshotTest, rightEdgeExtensionIncreaseBoundSizeWithinCrop) {
    // The right bound is extended when shifting to the left
    const int crop = 20;
    setCrop(1, Rect(0, 0, crop, crop));
    const int texSize = 10;
    setBuffer(1221,
              std::make_shared<renderengine::mock::FakeExternalTexture>(texSize /* width */,
                                                                        texSize /* height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    const float translation = -5.0;
    setPosition(12, translation, 0);
    setEdgeExtensionEffect(12, RIGHT);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->transformedBounds.left, 0);
    EXPECT_GT(getSnapshot({.id = 1221})->transformedBounds.right, texSize + translation);
    EXPECT_LE(getSnapshot({.id = 1221})->transformedBounds.right, (float)crop);
}

TEST_F(LayerSnapshotTest, topEdgeExtensionIncreaseBoundSizeWithinCrop) {
    // The top bound is extended when shifting to the bottom
    setCrop(1, Rect(0, 0, 20, 20));
    const int texSize = 10;
    setBuffer(1221,
              std::make_shared<renderengine::mock::FakeExternalTexture>(texSize /* width */,
                                                                        texSize /* height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    const float translation = 5.0;
    setPosition(12, 0, translation);
    setEdgeExtensionEffect(12, TOP);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->transformedBounds.bottom, texSize + translation);
    EXPECT_LT(getSnapshot({.id = 1221})->transformedBounds.top, translation);
    EXPECT_GE(getSnapshot({.id = 1221})->transformedBounds.top, 0.0);
}

TEST_F(LayerSnapshotTest, bottomEdgeExtensionIncreaseBoundSizeWithinCrop) {
    // The bottom bound is extended when shifting to the top
    const int crop = 20;
    setCrop(1, Rect(0, 0, crop, crop));
    const int texSize = 10;
    setBuffer(1221,
              std::make_shared<renderengine::mock::FakeExternalTexture>(texSize /* width */,
                                                                        texSize /* height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    const float translation = -5.0;
    setPosition(12, 0, translation);
    setEdgeExtensionEffect(12, BOTTOM);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1221})->transformedBounds.top, 0);
    EXPECT_GT(getSnapshot({.id = 1221})->transformedBounds.bottom, texSize - translation);
    EXPECT_LE(getSnapshot({.id = 1221})->transformedBounds.bottom, (float)crop);
}

TEST_F(LayerSnapshotTest, multipleEdgeExtensionIncreaseBoundSizeWithinCrop) {
    // The left bound is extended when shifting to the right
    const int crop = 20;
    setCrop(1, Rect(0, 0, crop, crop));
    const int texSize = 10;
    setBuffer(1221,
              std::make_shared<renderengine::mock::FakeExternalTexture>(texSize /* width */,
                                                                        texSize /* height*/,
                                                                        42ULL /* bufferId */,
                                                                        HAL_PIXEL_FORMAT_RGBA_8888,
                                                                        0 /*usage*/));
    const float translation = 5.0;
    setPosition(12, translation, translation);
    setEdgeExtensionEffect(12, LEFT | RIGHT | TOP | BOTTOM);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_GT(getSnapshot({.id = 1221})->transformedBounds.right, texSize + translation);
    EXPECT_LE(getSnapshot({.id = 1221})->transformedBounds.right, (float)crop);
    EXPECT_LT(getSnapshot({.id = 1221})->transformedBounds.left, translation);
    EXPECT_GE(getSnapshot({.id = 1221})->transformedBounds.left, 0.0);
    EXPECT_GT(getSnapshot({.id = 1221})->transformedBounds.bottom, texSize + translation);
    EXPECT_LE(getSnapshot({.id = 1221})->transformedBounds.bottom, (float)crop);
    EXPECT_LT(getSnapshot({.id = 1221})->transformedBounds.top, translation);
    EXPECT_GE(getSnapshot({.id = 1221})->transformedBounds.top, 0);
}

TEST_F(LayerSnapshotTest, shouldUpdateInputWhenNoInputInfo) {
    // If a layer has no buffer or no color, it doesn't have an input info
    setColor(111, {-1._hf, -1._hf, -1._hf});
    UPDATE_AND_VERIFY(mSnapshotBuilder, {1, 11, 12, 121, 122, 1221, 13, 2});
    EXPECT_FALSE(getSnapshot(111)->hasInputInfo());

    setBuffer(111);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);

    EXPECT_TRUE(getSnapshot(111)->hasInputInfo());
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
}

// content dirty test
TEST_F(LayerSnapshotTest, contentDirtyWhenParentAlphaChanges) {
    setAlpha(1, 0.5);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->contentDirty);
    EXPECT_TRUE(getSnapshot(11)->contentDirty);
    EXPECT_TRUE(getSnapshot(111)->contentDirty);

    // subsequent updates clear the dirty bit
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot(1)->contentDirty);
    EXPECT_FALSE(getSnapshot(11)->contentDirty);
    EXPECT_FALSE(getSnapshot(111)->contentDirty);
}

TEST_F(LayerSnapshotTest, contentDirtyWhenAutoRefresh) {
    setAutoRefresh(1, true);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->contentDirty);

    // subsequent updates don't clear the dirty bit
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->contentDirty);

    // second update after removing auto refresh will clear content dirty
    setAutoRefresh(1, false);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot(1)->contentDirty);
}

TEST_F(LayerSnapshotTest, contentDirtyWhenColorChanges) {
    setColor(1, {1, 2, 3});
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->contentDirty);

    // subsequent updates clear the dirty bit
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot(1)->contentDirty);
}

TEST_F(LayerSnapshotTest, contentDirtyWhenParentGeometryChanges) {
    setPosition(1, 2, 3);
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_TRUE(getSnapshot(1)->contentDirty);

    // subsequent updates clear the dirty bit
    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_FALSE(getSnapshot(1)->contentDirty);
}
TEST_F(LayerSnapshotTest, shouldUpdatePictureProfileHandle) {
    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back();
    transactions.back().states.push_back({});
    transactions.back().states.back().layerId = 1;
    transactions.back().states.back().state.layerId = 1;
    transactions.back().states.back().state.what = layer_state_t::ePictureProfileHandleChanged;
    transactions.back().states.back().state.pictureProfileHandle = PictureProfileHandle(3);

    mLifecycleManager.applyTransactions(transactions);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges(), RequestedLayerState::Changes::Content);

    update(mSnapshotBuilder);

    EXPECT_EQ(getSnapshot(1)->clientChanges, layer_state_t::ePictureProfileHandleChanged);
    EXPECT_EQ(getSnapshot(1)->pictureProfileHandle, PictureProfileHandle(3));
}

TEST_F(LayerSnapshotTest, shouldUpdatePictureProfilePriorityFromAppContentPriority) {
    {
        std::vector<QueuedTransactionState> transactions;
        transactions.emplace_back();
        transactions.back().states.push_back({});
        transactions.back().states.back().layerId = 1;
        transactions.back().states.back().state.layerId = 1;
        transactions.back().states.back().state.what = layer_state_t::eAppContentPriorityChanged;
        transactions.back().states.back().state.appContentPriority = 1;
        transactions.back().states.push_back({});
        transactions.back().states.back().layerId = 2;
        transactions.back().states.back().state.layerId = 2;
        transactions.back().states.back().state.what = layer_state_t::eAppContentPriorityChanged;
        transactions.back().states.back().state.appContentPriority = -1;

        mLifecycleManager.applyTransactions(transactions);
        EXPECT_EQ(mLifecycleManager.getGlobalChanges(), RequestedLayerState::Changes::Content);

        update(mSnapshotBuilder);

        EXPECT_GT(getSnapshot(1)->pictureProfilePriority, getSnapshot(2)->pictureProfilePriority);
        EXPECT_EQ(getSnapshot(1)->pictureProfilePriority - getSnapshot(2)->pictureProfilePriority,
                  2);
    }
    {
        std::vector<QueuedTransactionState> transactions;
        transactions.emplace_back();
        transactions.back().states.push_back({});
        transactions.back().states.back().layerId = 1;
        transactions.back().states.back().state.layerId = 1;
        transactions.back().states.back().state.what = layer_state_t::eAppContentPriorityChanged;
        transactions.back().states.back().state.appContentPriority = INT_MIN;
        transactions.back().states.push_back({});
        transactions.back().states.back().layerId = 2;
        transactions.back().states.back().state.layerId = 2;
        transactions.back().states.back().state.what = layer_state_t::eAppContentPriorityChanged;
        transactions.back().states.back().state.appContentPriority = INT_MAX;

        mLifecycleManager.applyTransactions(transactions);
        EXPECT_EQ(mLifecycleManager.getGlobalChanges(), RequestedLayerState::Changes::Content);

        update(mSnapshotBuilder);

        EXPECT_GT(getSnapshot(2)->pictureProfilePriority, getSnapshot(1)->pictureProfilePriority);
    }
}

// Test that child layers of the stop layer are hidden.
TEST_F(LayerSnapshotTest, stopLayer_hidesChildren) {
    setStopLayer(1, 122);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that if a layer specifies itself as a stop layer, then it is hidden.
TEST_F(LayerSnapshotTest, stopLayer_hidesSelf) {
    setStopLayer(122, 122);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 13, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that siblings z-ordered above a stop layer are hidden.
TEST_F(LayerSnapshotTest, stopLayer_hidesSiblings) {
    setStopLayer(1, 121);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that children z-ordered below the stop layer aren't hidden.
TEST_F(LayerSnapshotTest, stopLayer_doesntHideZOrderedBelowChildren) {
    setZ(121, -1);
    setStopLayer(1, 12);

    std::vector<uint32_t> expected = {1, 11, 111, 121, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that relative children are hidden by the stop layer.
TEST_F(LayerSnapshotTest, stopLayer_hidesRelativeChild) {
    reparentRelativeLayer(111, 12);
    setStopLayer(1, 12);

    std::vector<uint32_t> expected = {1, 11, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that detached children aren't hidden by the stop layer.
TEST_F(LayerSnapshotTest, stopLayer_doesntHideDetachedChildren) {
    reparentRelativeLayer(121, 11);
    setStopLayer(1, 12);

    std::vector<uint32_t> expected = {1, 11, 111, 121, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that stop layers work on hierarchies with a single root layer.
TEST_F(LayerSnapshotTest, stopLayer_singleRoot) {
    setStopLayer(1, 11);

    LayerHierarchy root = mHierarchyBuilder.getPartialHierarchy(1, /*childrenOnly=*/false);
    LayerSnapshotBuilder::Args args{.root = root,
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .displayChanges = false,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {}};
    mSnapshotBuilder.update(args);

    std::vector<uint32_t> expectedVisibleLayers = {1};
    std::vector<uint32_t> actualVisibleLayers;
    mSnapshotBuilder.forEachVisibleSnapshot([&actualVisibleLayers](const LayerSnapshot& snapshot) {
        actualVisibleLayers.push_back(snapshot.path.id);
    });
    EXPECT_EQ(expectedVisibleLayers, actualVisibleLayers);
}

// Test two stop layers where there's no interaction between the two stop layers.
TEST_F(LayerSnapshotTest, stopLayer_multipleStopLayers_parentAfterChild) {
    setStopLayer(1, 13);
    setStopLayer(11, 111);

    std::vector<uint32_t> expected = {1, 11, 12, 121, 122, 1221, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test two stop layers where the hierarchy containing the second stop layer is hidden.
TEST_F(LayerSnapshotTest, stopLayer_multipleStopLayers_childHidden) {
    setStopLayer(1, 12);
    setStopLayer(12, 122);

    std::vector<uint32_t> expected = {1, 11, 111, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test two stop layers where the stop layer specified lower in the hierarchy overrides
// the stop layer specified higher in the hierarchy.
TEST_F(LayerSnapshotTest, stopLayer_multipleStopLayers_childStopLayerOverridden) {
    setStopLayer(1, 121);
    setStopLayer(12, 122);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test two stop layers where the stop layer specified higher in the hierarchy applies because
// it appears before the stop layer applied lower in the hierarchy.
TEST_F(LayerSnapshotTest, stopLayer_multipleStopLayers_childApplied) {
    setStopLayer(1, 13);
    setStopLayer(11, 111);

    std::vector<uint32_t> expected = {1, 11, 12, 121, 122, 1221, 2};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

// Test that the stop layer works on mirrored hierarchies.
TEST_F(LayerSnapshotTest, stopLayer_mirrorHierarchy) {
    createDisplayMirrorLayer(3, ui::LayerStack::fromValue(0), 121);
    setLayerStack(3, 1);

    std::vector<uint32_t> expected = {1, 11, 111, 12, 121, 122, 1221, 13, 2, 3, 1, 11, 111, 12};
    UPDATE_AND_VERIFY(mSnapshotBuilder, expected);
}

TEST_F(LayerSnapshotTest, systemContentPriorityPassedToChildLayers) {
    setSystemContentPriority(11, 1);
    setSystemContentPriority(12, 2);

    UPDATE_AND_VERIFY(mSnapshotBuilder, STARTING_ZORDER);
    EXPECT_EQ(getSnapshot({.id = 1})->systemContentPriority,
              gui::ISystemContentPriorityConstants::Unset);
    EXPECT_EQ(getSnapshot({.id = 11})->systemContentPriority, 1);
    EXPECT_EQ(getSnapshot({.id = 12})->systemContentPriority, 2);
    EXPECT_EQ(getSnapshot({.id = 122})->systemContentPriority, 2);
    EXPECT_EQ(getSnapshot({.id = 1221})->systemContentPriority, 2);
}

TEST_F(LayerSnapshotTest, ExclusionMaskFiltersLayers) {
    // Set composition filter flag for layer 11
    std::vector<QueuedTransactionState> transactions;
    transactions.emplace_back();
    transactions.back().states.push_back({});
    transactions.back().states.front().state.what = layer_state_t::eCompositionFilterFlagChanged;
    transactions.back().states.front().state.compositionFilterFlag = 1u << 2;
    transactions.back().states.front().layerId = 11;
    transactions.back().states.front().state.layerId = 11;
    mLifecycleManager.applyTransactions(transactions);

    // Update with exclusion mask that matches the flag
    LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                    .layerLifecycleManager = mLifecycleManager,
                                    .includeMetadata = false,
                                    .displays = mFrontEndDisplayInfos,
                                    .globalShadowSettings = globalShadowSettings,
                                    .supportsBlur = true,
                                    .supportedLayerGenericMetadata = {},
                                    .genericLayerMetadataKeyMap = {},
                                    .exclusionMask = 1u << 2};
    update(mSnapshotBuilder, args);

    // Layer 11 should be hidden by policy
    EXPECT_TRUE(getSnapshot(11)->isHiddenByPolicyFromParent);
    // Children should also be hidden
    EXPECT_TRUE(getSnapshot(111)->isHiddenByPolicyFromParent);

    // Update with exclusion mask that DOES NOT match
    args.exclusionMask = 1u << 3;
    update(mSnapshotBuilder, args);

    // Layer 11 should NOT be hidden by policy
    EXPECT_FALSE(getSnapshot(11)->isHiddenByPolicyFromParent);
}

// Real lifecycle/snapshot owners, not a parallel region or authority model.
class LayerSnapshotStagedAtlasTest : public LayerSnapshotTest {
protected:
    RequestedLayerState* requestedForDiagnostic(uint32_t id) {
        // Existing public owning traversal, not a production accessor/friend
        // expansion or const_cast. Used only for the scoped NaN diagnostic.
        for (const auto& layer : mLifecycleManager.getLayers()) {
            if (layer->id == id) return layer.get();
        }
        return nullptr;
    }
    sp<IBinder> TEST_DOUBLE_ownedMirror(uint32_t id, uint32_t parent, uint32_t source) {
        auto args = createArgs(id, false, parent, source);
        // Explicit creation-origin DOUBLE. This executes the real lifecycle /
        // snapshot producer, NOT the kernel Binder authentication/factory path.
        const auto origin = sp<BBinder>::make();
        args.externalDesktopOwnedInputBlockOrigin = origin;
        std::vector<std::unique_ptr<RequestedLayerState>> layers;
        layers.emplace_back(std::make_unique<RequestedLayerState>(args));
        mLifecycleManager.addLayers(std::move(layers));
        return origin;
    }
    // Lifecycle-only fixture uses explicit test witnesses, NOT physical SF
    // handle proof. Production resolves Layer::getSourceLifetimeForFacts.
    std::map<uint32_t, gui::LayerSourceLifetime> mTestSourceLifetimes;
    std::map<uint32_t, sp<IBinder>> mTestPublishedHandles;
    std::optional<gui::LayerSourceInfo> sourceFactsForTest(const LayerSnapshot& snapshot,
            uint32_t index, const LayerLifecycleManager& lifecycle,
            const LayerSnapshotBuilder& builder, gui::LayerSourceBudget* budget = nullptr) {
        const LayerSourceLifetimeResolver TEST_DOUBLE = [this](uint32_t id)
                -> std::optional<gui::LayerSourceLifetime> {
            auto& value = mTestSourceLifetimes[id];
            if (!value.identity) {
                value.identity = sp<BBinder>::make();
                mTestPublishedHandles[id] = sp<BBinder>::make();
                value.publishedHandle = mTestPublishedHandles[id];
            }
            return value;
        };
        return budget ? buildLayerSourceInfo(snapshot, index, lifecycle, builder, TEST_DOUBLE, *budget)
                      : buildLayerSourceInfo(snapshot, index, lifecycle, builder, TEST_DOUBLE);
    }
    void candidateDisplay(int32_t stack, int32_t width, int32_t height,
                          ui::Transform transform = {}) {
        DisplayInfo display{};
        display.info.displayId = ui::LogicalDisplayId{stack};
        display.info.logicalWidth = width;
        display.info.logicalHeight = height;
        display.info.transform = transform.inverse();
        display.transform = transform;
        display.receivesInput = true;
        display.isSecure = true;
        mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(stack), display);
    }

    void candidateInput(uint32_t id, const Region& ordinary, std::optional<Region> candidate,
                        uint32_t crop = UNASSIGNED_LAYER_ID, bool replace = false,
                        bool spoofTag = false, bool channel = true) {
        std::vector<QueuedTransactionState> transactions(1);
        transactions.front().states.push_back({});
        auto& resolved = transactions.front().states.front();
        resolved.layerId = id;
        resolved.state.what = layer_state_t::eInputInfoChanged;
        resolved.touchCropId = crop;
        auto info = resolved.state.editWindowInfo();
        *info = {};
        info->name = "StagedCandidateOwner";
        if (channel) info->token = sp<BBinder>::make();
        else info->inputConfig |= gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL;
        info->touchableRegion = ordinary;
        info->preViewportTouchableRegion = std::move(candidate);
        info->replaceTouchableRegionWithCrop = replace;
        if (spoofTag) info->stageTag = gui::WindowInfo::StageTag{91, 92, 93, 94, 95};
        mLifecycleManager.applyTransactions(transactions);
    }

    void candidateUpdate(bool displayChanges = false) {
        LayerSnapshotBuilder::Args args{.root = mHierarchyBuilder.getHierarchy(),
                                        .layerLifecycleManager = mLifecycleManager,
                                        .includeMetadata = false,
                                        .displays = mFrontEndDisplayInfos,
                                        .displayChanges = displayChanges,
                                        .globalShadowSettings = globalShadowSettings,
                                        .supportsBlur = true,
                                        .supportedLayerGenericMetadata = {},
                                        .genericLayerMetadataKeyMap = {}};
        update(mSnapshotBuilder, args);
        mLifecycleManager.commitChanges();
    }
};

TEST_F(LayerSnapshotStagedAtlasTest, requestedTagsClearedAtConstructorMergeAndRefresh) {
    candidateDisplay(0, 400, 300);
    setCrop(111, Rect{0, 0, 200, 100});
    candidateInput(111, Region(Rect{0, 0, 40, 100}), Region(Rect{0, 0, 200, 100}),
                   UNASSIGNED_LAYER_ID, false, true);
    const LayerLifecycleManager& lifecycle = mLifecycleManager;
    const RequestedLayerState* requested = lifecycle.getLayerFromId(111);
    ASSERT_NE(nullptr, requested);
    LayerSnapshot direct(*requested, {.id = 111, .mirrorRootIds = {3u, 14u}}, LayerSnapshot{});
    EXPECT_FALSE(direct.inputInfo.stageTag);
    direct.inputInfo.stageTag = gui::WindowInfo::StageTag{1, 2, 3, 4, 5};
    direct.preViewportTouchableRegionBeforeCrop = Region(Rect{1, 1, 2, 2});
    direct.merge(*requested, true, false, false, 0);
    EXPECT_FALSE(direct.inputInfo.stageTag);
    EXPECT_FALSE(direct.preViewportTouchableRegionBeforeCrop);
    ASSERT_TRUE(direct.inputInfo.cloneLineage);
    EXPECT_EQ((std::vector<uint32_t>{3, 14}), direct.inputInfo.cloneLineage->mirrorRootIds);
    candidateUpdate();
    auto* snapshot = getSnapshot(111);
    ASSERT_TRUE(snapshot->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(snapshot->inputInfo.preViewportTouchableRegion->hasSameRects(
            Region(Rect{0, 0, 200, 100})));
    EXPECT_TRUE(snapshot->inputInfo.touchableRegion.hasSameRects(Region(Rect{0, 0, 40, 100})));
    EXPECT_FALSE(snapshot->inputInfo.stageTag);
    candidateInput(111, Region(Rect{0, 0, 10, 100}), std::nullopt,
                   UNASSIGNED_LAYER_ID, false, true);
    candidateUpdate();
    EXPECT_FALSE(snapshot->inputInfo.preViewportTouchableRegion);
    EXPECT_FALSE(snapshot->preViewportTouchableRegionBeforeCrop);
    EXPECT_FALSE(snapshot->inputInfo.stageTag);
}

TEST_F(LayerSnapshotStagedAtlasTest, hiddenStripUsesTranslatedTaskCounterpartAndDestinationCrop) {
    candidateDisplay(0, 100, 100);
    candidateDisplay(3, 400, 100);
    setCrop(11, Rect{0, 0, 200, 100});
    setCrop(111, Rect{0, 0, 200, 100});
    createRootLayer(3);
    setLayerStack(3, 3);
    setCrop(3, Rect{200, 0, 300, 100});
    mirrorLayer(14, 3, 11);
    setPosition(14, 100, 0);
    candidateInput(111, Region(Rect{0, 0, 100, 100}), Region(Rect{0, 0, 200, 100}), 11);
    candidateUpdate();
    const auto& original = getSnapshot(111)->inputInfo;
    const auto* clone = getSnapshot({.id = 111, .mirrorRootIds = {14u}});
    ASSERT_NE(nullptr, clone);
    ASSERT_TRUE(clone->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(clone->inputInfo.preViewportTouchableRegion->hasSameRects(
            Region(Rect{200, 0, 300, 100})));
    EXPECT_TRUE(clone->inputInfo.touchableRegion.isEmpty());
    EXPECT_TRUE(original.touchableRegion.hasSameRects(Region(Rect{0, 0, 100, 100})));
    ASSERT_TRUE(original.preViewportTouchableRegion);
    EXPECT_TRUE(original.preViewportTouchableRegion->hasSameRects(original.touchableRegion));
    EXPECT_FALSE(clone->inputInfo.stageTag);
}

TEST_F(LayerSnapshotStagedAtlasTest, cropOnlyGrowthRecomputesCandidateWithoutChangingStockRegion) {
    candidateDisplay(0, 300, 100);
    setCrop(111, Rect{0, 0, 200, 100});
    setCrop(13, Rect{20, 0, 50, 100});
    candidateInput(111, Region(Rect{0, 0, 40, 100}), Region(Rect{0, 0, 200, 100}), 13);
    candidateUpdate();
    const auto* snapshot = getSnapshot(111);
    ASSERT_TRUE(snapshot->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(snapshot->inputInfo.preViewportTouchableRegion->hasSameRects(
            Region(Rect{20, 0, 50, 100})));
    const Region stockBefore = snapshot->inputInfo.touchableRegion;
    setCrop(13, Rect{20, 0, 80, 100});
    candidateUpdate();
    ASSERT_TRUE(snapshot->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(snapshot->inputInfo.preViewportTouchableRegion->hasSameRects(
            Region(Rect{20, 0, 80, 100})));
    EXPECT_TRUE(snapshot->inputInfo.touchableRegion.hasSameRects(stockBefore));
}

TEST_F(LayerSnapshotStagedAtlasTest, candidateUsesExactInputToDisplayRotationAndScale) {
    candidateDisplay(0, 200, 100,
                     ui::Transform(ui::Transform::RotationFlags::ROT_90, 200, 100));
    setCrop(111, Rect{0, 0, 50, 50});
    setMatrix(111, 2, 0, 0, 2);
    candidateInput(111, Region(Rect{5, 6, 15, 16}), Region(Rect{5, 6, 15, 16}));
    candidateUpdate();
    const auto& info = getSnapshot(111)->inputInfo;
    ASSERT_TRUE(info.preViewportTouchableRegion);
    const Rect candidate = info.preViewportTouchableRegion->getBounds();
    const Rect ordinary = info.touchableRegion.getBounds();
    const ui::Transform inputToDisplay = info.transform.inverse();
    SCOPED_TRACE(testing::Message()
                 << "candidate=[" << candidate.left << "," << candidate.top << ","
                 << candidate.right << "," << candidate.bottom << "] stock=["
                 << ordinary.left << "," << ordinary.top << "," << ordinary.right << ","
                 << ordinary.bottom << "] inputToDisplay=[" << inputToDisplay.dsdx() << ","
                 << inputToDisplay.dtdx() << "," << inputToDisplay.tx() << ";"
                 << inputToDisplay.dtdy() << "," << inputToDisplay.dsdy() << ","
                 << inputToDisplay.ty() << "]");
    // ROT_90(200,100) translates x by the ORIGINAL width (200), not height.
    // The zero-inset, bufferless input uses the layer's 2x scale, so
    // (x,y) -> (200-2*y,2*x): Rect(5,6,15,16) -> Rect(168,10,188,30).
    EXPECT_TRUE(info.preViewportTouchableRegion->hasSameRects(Region(Rect{168, 10, 188, 30})));
    EXPECT_TRUE(info.preViewportTouchableRegion->hasSameRects(info.touchableRegion));
}

TEST_F(LayerSnapshotStagedAtlasTest, nestedCropRequiresEntireOrderedCounterpartPath) {
    candidateDisplay(0, 400, 200);
    candidateDisplay(3, 400, 200);
    candidateDisplay(4, 400, 200);
    setCrop(11, Rect{0, 0, 200, 100});
    setCrop(111, Rect{0, 0, 200, 100});
    setCrop(13, Rect{0, 0, 200, 100});
    createRootLayer(3);
    setLayerStack(3, 3);
    setCrop(3, Rect{0, 0, 400, 200});
    mirrorLayer(14, 3, 11);
    mirrorLayer(15, 3, 13);
    createDisplayMirrorLayer(4, ui::LayerStack::fromValue(3));
    setLayerStack(4, 4);
    setCrop(4, Rect{0, 0, 400, 200});
    candidateInput(111, Region(Rect{0, 0, 100, 100}), Region(Rect{0, 0, 200, 100}), 11);
    candidateUpdate();
    const auto* nested = getSnapshot({.id = 111, .mirrorRootIds = {4u, 14u}});
    ASSERT_NE(nullptr, nested);
    ASSERT_TRUE(nested->inputInfo.preViewportTouchableRegion);
    ASSERT_TRUE(nested->inputInfo.cloneLineage);
    EXPECT_EQ((std::vector<uint32_t>{4, 14}), nested->inputInfo.cloneLineage->mirrorRootIds);
    ASSERT_NE(nullptr, getSnapshot({.id = 13, .mirrorRootIds = {4u, 15u}}));
    EXPECT_EQ(nullptr, getSnapshot({.id = 13, .mirrorRootIds = {4u, 14u}}));
    candidateInput(111, Region(Rect{0, 0, 100, 100}), Region(Rect{0, 0, 200, 100}), 13);
    candidateUpdate();
    EXPECT_FALSE(nested->inputInfo.preViewportTouchableRegion);
    EXPECT_FALSE(getSnapshot({.id = 111, .mirrorRootIds = {14u}})->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(getSnapshot(111)->inputInfo.preViewportTouchableRegion.has_value());
}

TEST_F(LayerSnapshotStagedAtlasTest, invalidRegionTransformOrUnavailableInputStaysAbsent) {
    candidateDisplay(0, 3000, 100);
    setCrop(111, Rect{0, 0, 3000, 100});
    Region complex;
    for (size_t i = 0; i <= gui::WindowInfo::kMaxPreViewportRegionRects; ++i) {
        const int32_t x = static_cast<int32_t>(i) * 2;
        complex.orSelf(Rect{x, 0, x + 1, 1});
    }
    for (const Region& bad : {Region(Rect{-32769, 0, 0, 1}), complex}) {
        candidateInput(111, Region(Rect{0, 0, 1, 1}), bad);
        candidateUpdate();
        EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
        EXPECT_TRUE(getSnapshot(111)->inputInfo.touchableRegion.hasSameRects(Region(Rect{0, 0, 1, 1})));
    }
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(Rect{0, 0, 100, 100}));
    setMatrix(111, 1, .25f, 0, 1); // Region bounding-box substitution is not exact.
    candidateUpdate();
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
    setMatrix(111, 1, 0, 0, 1);
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(Rect{0, 0, 100, 100}),
                   UNASSIGNED_LAYER_ID, false, false, false);
    candidateUpdate();
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
    setCrop(111, Rect{0, 0, 1, 1});
    setMatrix(111, 10000000.f, 0, 0, 1);
    setPosition(111, 1200000000.f, 0);
    // Ordinary x=0..1 maps to 1.20e9..1.21e9, still int32-safe. Candidate
    // x=100..101 has independently safe linear endpoints and translation,
    // but their sum 2.2e9..2.21e9 overflows before the final coordinate fence.
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(Rect{100, 0, 101, 1}));
    candidateUpdate();
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
    EXPECT_FALSE(getSnapshot(111)->preViewportTouchableRegionBeforeCrop);
}

TEST_F(LayerSnapshotStagedAtlasTest, absentDisplayInvalidInputBoundsAndFinalCoordinateLimitRefuse) {
    candidateDisplay(0, 30000, 100);
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(Rect{0, 0, 100, 100}));
    candidateUpdate(); // No crop/buffer gives no valid input bounds.
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
    setCrop(111, Rect{0, 0, 20, 20});
    setInputInfo(111, [](auto& requested) {
        requested.touchableRegion = Region(Rect{0, 0, 1, 1});
        requested.preViewportTouchableRegion = Region(Rect{0, 0, 20, 20});
        requested.surfaceInset = 100; // Stock clamping collapses both input dimensions.
    });
    candidateUpdate();
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
    setCrop(111, Rect{0, 0, 20000, 100});
    setMatrix(111, 2, 0, 0, 1);
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(Rect{16000, 0, 20000, 100}));
    candidateUpdate();
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion); // Final right=40000.
    setMatrix(111, 1, 0, 0, 1);
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(Rect{0, 0, 100, 100}));
    candidateUpdate();
    ASSERT_TRUE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
    mFrontEndDisplayInfos.clear();
    candidateUpdate(true);
    EXPECT_FALSE(getSnapshot(111)->inputInfo.preViewportTouchableRegion);
}

TEST_F(LayerSnapshotStagedAtlasTest, presentEmptyNeverBecomesCropAndStockDropStaysOwnedByStock) {
    candidateDisplay(0, 400, 100);
    setCrop(111, Rect{0, 0, 200, 100});
    setCrop(13, Rect{0, 0, 200, 100});
    candidateInput(111, Region(Rect{0, 0, 1, 1}), Region(), 13, true);
    candidateUpdate();
    const auto& info = getSnapshot(111)->inputInfo;
    ASSERT_TRUE(info.preViewportTouchableRegion);
    EXPECT_TRUE(info.preViewportTouchableRegion->isEmpty());
    EXPECT_TRUE(info.touchableRegion.hasSameRects(Region(Rect{0, 0, 200, 100})));
    setInputInfo(111, [](auto& requested) {
        requested.touchableRegion = Region(Rect{0, 0, 10, 10});
        requested.preViewportTouchableRegion = Region(Rect{0, 0, 20, 20});
        requested.inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
    });
    candidateUpdate();
    EXPECT_TRUE(getSnapshot(111)->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_TRUE(getSnapshot(111)->inputInfo.touchableRegion.hasSameRects(Region(Rect{0, 0, 10, 10})));
}

TEST_F(LayerSnapshotStagedAtlasTest, fullPublishedBatchPreservesCandidateLineageAndStockPolicy) {
    // Stock filtering stays enabled: a NOT_VISIBLE row is never forced in.
    SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::
                              skip_invisible_windows_in_input,
                      true);
    candidateDisplay(0, 100, 100);
    candidateDisplay(3, 400, 100);
    candidateDisplay(4, 400, 100);
    setCrop(11, Rect{0, 0, 200, 100});
    setCrop(111, Rect{0, 0, 200, 100});
    setCrop(13, Rect{0, 0, 200, 100});
    setCrop(2, Rect{0, 0, 80, 80});
    setCrop(121, Rect{0, 0, 20, 20});
    createRootLayer(3);
    setLayerStack(3, 3);
    setCrop(3, Rect{200, 0, 300, 100});
    mirrorLayer(14, 3, 11);
    setPosition(14, 100, 0);
    mirrorLayer(15, 3, 13);
    createDisplayMirrorLayer(4, ui::LayerStack::fromValue(3));
    setLayerStack(4, 4);
    setCrop(4, Rect{0, 0, 400, 100});
    candidateInput(111, Region(Rect{0, 0, 100, 100}), Region(Rect{0, 0, 200, 100}),
                   11, false, true);
    setDropInputMode(11, gui::DropInputMode::ALL);
    // An unrelated ordinary sibling remains in the COMPLETE batch despite
    // having no candidate, lineage or stage tag. A hidden sibling stays out.
    candidateInput(2, Region(Rect{5, 6, 15, 16}), std::nullopt);
    candidateInput(121, Region(Rect{0, 0, 10, 10}), std::nullopt);
    hideLayer(121);
    candidateUpdate();

    using Path = LayerHierarchy::TraversalPath;
    // Independent complete fixture topology, not a channel/candidate subset:
    // eight original rows (hidden121 omitted), six on display3, seven on4.
    // makeChild records the immediate edge variant: display mirror3 is Mirror,
    // task mirror roots11/13 are Detached_Mirror, their children are Attached.
    const std::vector<Path> expectedPaths{
            {.id = 1, .variant = LayerHierarchy::Attached},
            {.id = 11, .variant = LayerHierarchy::Attached},
            {.id = 111, .variant = LayerHierarchy::Attached},
            {.id = 12, .variant = LayerHierarchy::Attached},
            {.id = 122, .variant = LayerHierarchy::Attached},
            {.id = 1221, .variant = LayerHierarchy::Attached},
            {.id = 13, .variant = LayerHierarchy::Attached},
            {.id = 2, .variant = LayerHierarchy::Attached},
            {.id = 3, .variant = LayerHierarchy::Attached},
            {.id = 14, .variant = LayerHierarchy::Attached},
            {.id = 11, .variant = LayerHierarchy::Detached_Mirror, .mirrorRootIds = {14u}},
            {.id = 111, .variant = LayerHierarchy::Attached, .mirrorRootIds = {14u}},
            {.id = 15, .variant = LayerHierarchy::Attached},
            {.id = 13, .variant = LayerHierarchy::Detached_Mirror, .mirrorRootIds = {15u}},
            {.id = 4, .variant = LayerHierarchy::Attached},
            {.id = 3, .variant = LayerHierarchy::Mirror, .mirrorRootIds = {4u}},
            {.id = 14, .variant = LayerHierarchy::Attached, .mirrorRootIds = {4u}},
            {.id = 11, .variant = LayerHierarchy::Detached_Mirror, .mirrorRootIds = {4u, 14u}},
            {.id = 111, .variant = LayerHierarchy::Attached, .mirrorRootIds = {4u, 14u}},
            {.id = 15, .variant = LayerHierarchy::Attached, .mirrorRootIds = {4u}},
            {.id = 13, .variant = LayerHierarchy::Detached_Mirror, .mirrorRootIds = {4u, 15u}}};
    const auto pathKey = [](const Path& path) {
        std::string key = "id=" + std::to_string(path.id) + ",variant=" +
                std::to_string(static_cast<uint32_t>(path.variant)) + ",mirrors=[";
        for (uint32_t root : path.mirrorRootIds) key += std::to_string(root) + ",";
        key += "],relatives=[";
        for (uint32_t root : path.relativeRootIds) key += std::to_string(root) + ",";
        return key + "],invalidRelative=" + std::to_string(path.invalidRelativeRootId) +
                ",detached=" + std::to_string(path.detached);
    };
    const auto pathKeys = [&](const std::vector<Path>& paths) {
        std::vector<std::string> keys;
        for (const auto& path : paths) keys.push_back(pathKey(path));
        return keys;
    };
    const auto expectedKeys = pathKeys(expectedPaths);
    const auto fullBatchRoundTrip = [&](std::vector<Path>& paths,
                                       std::vector<gui::WindowInfo>& rows) {
        gui::WindowInfosUpdate emitted;
        // Same full-row copy as SurfaceFlinger::buildWindowInfos: no tag,
        // display, caller-window or candidate filter is introduced here.
        mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
            paths.push_back(snapshot.path);
            emitted.windowInfos.push_back(snapshot.inputInfo);
        });
        for (const auto& entry : mFrontEndDisplayInfos) {
            emitted.displayInfos.push_back(entry.second.info);
        }
        emitted.vsyncId = 17;
        emitted.timestamp = 123456;
        // Scalar diagnostics include EVERY row, ordered roots and edge variant;
        // TraversalPath::operator== alone intentionally does not compare variant.
        ASSERT_THAT(pathKeys(paths), testing::UnorderedElementsAreArray(expectedKeys));
        ASSERT_EQ(paths.size(), emitted.windowInfos.size());
        Parcel parcel;
        ASSERT_EQ(OK, emitted.writeToParcel(&parcel));
        parcel.setDataPosition(0);
        gui::WindowInfosUpdate decoded;
        ASSERT_EQ(OK, decoded.readFromParcel(&parcel));
        EXPECT_EQ(parcel.dataSize(), parcel.dataPosition());
        ASSERT_EQ(emitted.windowInfos.size(), decoded.windowInfos.size());
        EXPECT_EQ(emitted.vsyncId, decoded.vsyncId);
        EXPECT_EQ(emitted.timestamp, decoded.timestamp);
        ASSERT_EQ(emitted.displayInfos.size(), decoded.displayInfos.size());
        for (size_t i = 0; i < emitted.displayInfos.size(); ++i) {
            const auto& before = emitted.displayInfos[i];
            const auto& after = decoded.displayInfos[i];
            EXPECT_EQ(before.displayId, after.displayId);
            EXPECT_EQ(before.logicalWidth, after.logicalWidth);
            EXPECT_EQ(before.logicalHeight, after.logicalHeight);
            EXPECT_TRUE(before.transform == after.transform);
        }
        for (size_t i = 0; i < paths.size(); ++i) {
            SCOPED_TRACE(testing::Message() << "row=" << i << " " << pathKey(paths[i]));
            const auto& before = emitted.windowInfos[i];
            const auto& after = decoded.windowInfos[i];
            // Full stock fields (region, geometry, channel, drop/visibility)
            // and new metadata must survive the real multi-record codec.
            EXPECT_TRUE(before == after);
            EXPECT_TRUE(before.cloneLayerStackTransform == after.cloneLayerStackTransform);
            EXPECT_FALSE(after.stageTag);
            EXPECT_FALSE(after.inputConfig.test(gui::WindowInfo::InputConfig::NOT_VISIBLE));
            if (paths[i].isClone()) {
                ASSERT_TRUE(after.cloneLineage);
                EXPECT_EQ(paths[i].id, after.cloneLineage->sourceLayerId);
                const std::vector<uint32_t> roots(paths[i].mirrorRootIds.begin(),
                                                  paths[i].mirrorRootIds.end());
                EXPECT_EQ(roots, after.cloneLineage->mirrorRootIds);
                EXPECT_TRUE(after.inputConfig.test(gui::WindowInfo::InputConfig::CLONE));
            } else {
                EXPECT_FALSE(after.cloneLineage);
                EXPECT_FALSE(after.inputConfig.test(gui::WindowInfo::InputConfig::CLONE));
            }
        }
        rows = std::move(decoded.windowInfos);
    };

    ASSERT_TRUE(getSnapshot(121)->inputInfo.inputConfig.test(
            gui::WindowInfo::InputConfig::NOT_VISIBLE));
    std::vector<Path> firstPaths;
    std::vector<gui::WindowInfo> firstRows;
    ASSERT_NO_FATAL_FAILURE(fullBatchRoundTrip(firstPaths, firstRows));
    size_t sourceRows = 0;
    size_t ordinarySiblingRows = 0;
    for (size_t i = 0; i < firstPaths.size(); ++i) {
        const auto& path = firstPaths[i];
        const auto& row = firstRows[i];
        SCOPED_TRACE(testing::Message() << "required subset: " << pathKey(path));
        if (path.id == 2) {
            ++ordinarySiblingRows;
            EXPECT_FALSE(row.preViewportTouchableRegion);
            EXPECT_FALSE(row.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
            EXPECT_TRUE(row.touchableRegion.hasSameRects(Region(Rect{5, 6, 15, 16})));
            continue;
        }
        if (path.id != 111) {
            // Already emitted, encoded and fully stock-field checked above;
            // no-channel/occlusion rows stay in the packet, not filtered away.
            EXPECT_FALSE(row.preViewportTouchableRegion);
            continue;
        }
        ++sourceRows;
        ASSERT_TRUE(row.preViewportTouchableRegion);
        EXPECT_TRUE(row.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
        if (path.isClone()) {
            EXPECT_TRUE(row.preViewportTouchableRegion->hasSameRects(Region(Rect{200, 0, 300, 100})));
            EXPECT_TRUE(row.touchableRegion.isEmpty());
        } else {
            EXPECT_TRUE(row.preViewportTouchableRegion->hasSameRects(Region(Rect{0, 0, 100, 100})));
            EXPECT_TRUE(row.touchableRegion.hasSameRects(Region(Rect{0, 0, 100, 100})));
        }
        EXPECT_TRUE(row.token == getSnapshot(111)->inputInfo.token);
    }
    EXPECT_EQ(3u, sourceRows);
    EXPECT_EQ(1u, ordinarySiblingRows);

    // Retain the actual channel and raw values; change ONLY requested crop.
    // Source13 exists along15 but not along required [14] or [4,14] paths.
    ASSERT_NE(nullptr, getSnapshot({.id = 13, .mirrorRootIds = {4u, 15u}}));
    ASSERT_EQ(nullptr, getSnapshot({.id = 13, .mirrorRootIds = {4u, 14u}}));
    const LayerLifecycleManager& lifecycle = mLifecycleManager;
    const RequestedLayerState* requested = lifecycle.getLayerFromId(111);
    ASSERT_NE(nullptr, requested);
    std::vector<QueuedTransactionState> transactions(1);
    transactions.front().states.push_back({});
    auto& resolved = transactions.front().states.front();
    resolved.layerId = 111;
    resolved.touchCropId = 13;
    resolved.state.what = layer_state_t::eInputInfoChanged;
    *resolved.state.editWindowInfo() = requested->getWindowInfo();
    mLifecycleManager.applyTransactions(transactions);
    candidateUpdate();
    std::vector<Path> secondPaths;
    std::vector<gui::WindowInfo> secondRows;
    ASSERT_NO_FATAL_FAILURE(fullBatchRoundTrip(secondPaths, secondRows));
    ASSERT_EQ(pathKeys(firstPaths), pathKeys(secondPaths));
    ASSERT_EQ(firstRows.size(), secondRows.size());
    for (size_t i = 0; i < secondPaths.size(); ++i) {
        const auto& before = firstRows[i];
        const auto& after = secondRows[i];
        SCOPED_TRACE(testing::Message() << "crop-only update: " << pathKey(secondPaths[i]));
        EXPECT_TRUE(before.token == after.token);
        EXPECT_TRUE(before.touchableRegion.hasSameRects(after.touchableRegion));
        EXPECT_EQ(before.inputConfig, after.inputConfig);
        EXPECT_TRUE(before.cloneLineage == after.cloneLineage);
        if (secondPaths[i].id == 111 && secondPaths[i].isClone()) {
            EXPECT_FALSE(after.preViewportTouchableRegion);
        } else if (secondPaths[i].id == 111) {
            ASSERT_TRUE(after.preViewportTouchableRegion);
            EXPECT_TRUE(after.preViewportTouchableRegion->hasSameRects(Region(Rect{0, 0, 100, 100})));
        } else {
            EXPECT_FALSE(after.preViewportTouchableRegion);
        }
    }
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSourceFactsPreserveRealFull21AndSeparateStockRootDrops) {
    SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::skip_invisible_windows_in_input, true);
    candidateDisplay(0, 100, 100); candidateDisplay(3, 400, 100); candidateDisplay(4, 400, 100);
    setCrop(11, Rect{0, 0, 200, 100}); setCrop(111, Rect{0, 0, 200, 100});
    setCrop(13, Rect{0, 0, 200, 100}); setCrop(2, Rect{0, 0, 80, 80}); setCrop(121, Rect{0, 0, 20, 20});
    createRootLayer(3); setLayerStack(3, 3); setCrop(3, Rect{200, 0, 300, 100});
    mirrorLayer(14, 3, 11); setPosition(14, 100, 0); mirrorLayer(15, 3, 13);
    createDisplayMirrorLayer(4, ui::LayerStack::fromValue(3)); setLayerStack(4, 4); setCrop(4, Rect{0, 0, 400, 100});
    candidateInput(111, Region(Rect{0, 0, 100, 100}), Region(Rect{0, 0, 200, 100}), 11);
    candidateInput(2, Region(Rect{5, 6, 15, 16}), std::nullopt);
    candidateInput(121, Region(Rect{0, 0, 10, 10}), std::nullopt); hideLayer(121);
    // A real NO_INPUT_CHANNEL row may STILL carry a token. Neither token
    // absence nor that flag alone is structural authority/RootClaim evidence.
    setInputInfo(11, [](auto& info) { info.inputConfig |= gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL; });
    setDropInputMode(11, gui::DropInputMode::ALL); // app/stock source drop
    setDropInputMode(14, gui::DropInputMode::ALL); // local mirror-root candidate, not yet owned
    candidateUpdate();
    gui::WindowInfosUpdate packet;
    packet.layerSourcesComplete = true;
    mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
        packet.windowInfos.push_back(snapshot.inputInfo);
        auto facts = sourceFactsForTest(snapshot, static_cast<uint32_t>(packet.windowInfos.size() - 1),
                mLifecycleManager, mSnapshotBuilder);
        ASSERT_TRUE(facts); packet.layerSources.push_back(std::move(*facts));
    });
    ASSERT_EQ(packet.windowInfos.size(), 21u); ASSERT_EQ(packet.layerSources.size(), 21u);
    bool tokenOnNoChannel = false, nested = false, bothDrops = false;
    for (size_t i = 0; i < packet.layerSources.size(); ++i) {
        const auto& fact = packet.layerSources[i]; const auto& window = packet.windowInfos[i];
        EXPECT_EQ(fact.rowIndex, i); EXPECT_EQ(fact.applied.windowId, window.id);
        EXPECT_EQ(fact.applied.inputConfig, window.inputConfig.get());
        EXPECT_EQ(fact.applied.inputTransform, layerSourceMatrix(window.transform));
        ASSERT_FALSE(fact.ancestors.empty()); ASSERT_FALSE(fact.traversal.empty());
        EXPECT_EQ(fact.ancestors.front().id, fact.sourceId); EXPECT_EQ(fact.traversal.front().sourceId, fact.sourceId);
        EXPECT_EQ(fact.applied.hasInputToken, window.token != nullptr);
        EXPECT_FALSE(window.stageTag); // no issuer/enable sneaks into facts production
        if (fact.sourceId == 11 && fact.mirrorRoots.empty()) {
            EXPECT_NE(window.token, nullptr);
            EXPECT_TRUE(window.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
            tokenOnNoChannel = true;
        }
        if (fact.sourceId == 111 && fact.mirrorRoots == std::vector<uint32_t>{14}) {
            ASSERT_EQ(fact.roots.size(), 1u);
            EXPECT_EQ(fact.roots[0].requested.id, 14u); EXPECT_EQ(fact.roots[0].requested.parent, 3u);
            EXPECT_EQ(fact.roots[0].requested.mirrorSource, 11u);
            EXPECT_EQ(fact.roots[0].requested.requestedDropMode, static_cast<uint32_t>(gui::DropInputMode::ALL));
            const auto source = std::find_if(fact.ancestors.begin(), fact.ancestors.end(), [](const auto& node) { return node.id == 11; });
            ASSERT_NE(source, fact.ancestors.end());
            EXPECT_EQ(source->requestedDropMode, static_cast<uint32_t>(gui::DropInputMode::ALL));
            EXPECT_EQ(fact.applied.composedDropMode, static_cast<uint32_t>(gui::DropInputMode::ALL));
            bothDrops = true;
        }
        if (fact.sourceId == 111 && fact.mirrorRoots == std::vector<uint32_t>{4, 14}) {
            ASSERT_EQ(fact.roots.size(), 2u); EXPECT_EQ(fact.roots[0].requested.id, 4u);
            EXPECT_EQ(fact.roots[1].requested.id, 14u); nested = true;
        }
    }
    EXPECT_TRUE(tokenOnNoChannel); EXPECT_TRUE(nested); EXPECT_TRUE(bothDrops);
    Parcel parcel; ASSERT_EQ(packet.writeToParcel(&parcel), OK); parcel.setDataPosition(0);
    gui::WindowInfosUpdate decoded; ASSERT_EQ(decoded.readFromParcel(&parcel), OK);
    EXPECT_EQ(parcel.dataPosition(), parcel.dataSize()); ASSERT_TRUE(decoded.layerSourcesComplete);
    ASSERT_EQ(decoded.layerSources.size(), 21u);
    for (size_t i = 0; i < packet.windowInfos.size(); ++i) EXPECT_TRUE(packet.windowInfos[i] == decoded.windowInfos[i]);
    for (size_t i = 0; i < packet.layerSources.size(); ++i) {
        EXPECT_EQ(packet.layerSources[i].mirrorRoots, decoded.layerSources[i].mirrorRoots);
        EXPECT_EQ(packet.layerSources[i].applied.inputTransform, decoded.layerSources[i].applied.inputTransform);
        EXPECT_EQ(packet.layerSources[i].ancestors.size(), decoded.layerSources[i].ancestors.size());
        EXPECT_EQ(packet.layerSources[i].traversal.size(), decoded.layerSources[i].traversal.size());
    }
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSourceFactsMissingAndNonfiniteNativeStateCannotBecomeComplete) {
    candidateDisplay(0, 100, 100); setCrop(111, Rect{0, 0, 10, 10});
    candidateInput(111, Region(Rect{0, 0, 10, 10}), std::nullopt); candidateUpdate();
    auto* snapshot = getSnapshot(111); ASSERT_NE(snapshot, nullptr);
    const auto ordinary = snapshot->inputInfo;
    LayerLifecycleManager unavailable;
    EXPECT_FALSE(sourceFactsForTest(*snapshot, 0, unavailable, mSnapshotBuilder));
    auto* requested = requestedForDiagnostic(11); ASSERT_NE(requested, nullptr);
    const auto old = requested->requestedTransform;
    requested->requestedTransform.set(std::numeric_limits<float>::quiet_NaN(), 0);
    EXPECT_FALSE(sourceFactsForTest(*snapshot, 0, mLifecycleManager, mSnapshotBuilder));
    requested->requestedTransform = old; // never feed NaN into stock geometry code
    EXPECT_TRUE(snapshot->inputInfo == ordinary);
    auto valid = sourceFactsForTest(*snapshot, 0, mLifecycleManager, mSnapshotBuilder); ASSERT_TRUE(valid);
    gui::WindowInfosUpdate packet; packet.windowInfos.push_back(ordinary);
    packet.layerSourcesComplete = true; packet.layerSources.push_back(*valid);
    packet.layerSources[0].rowIndex = 1;
    Parcel invalid; EXPECT_EQ(packet.writeToParcel(&invalid), BAD_VALUE);
    packet.layerSources[0].rowIndex = 0; packet.layerSources[0].ancestors.clear();
    Parcel missing; EXPECT_EQ(packet.writeToParcel(&missing), BAD_VALUE);
    gui::WindowInfosUpdate absent;
    Parcel encoded; ASSERT_EQ(absent.writeToParcel(&encoded), OK);
    encoded.setDataPosition(encoded.dataSize() - sizeof(uint32_t));
    ASSERT_EQ(encoded.writeUint32(gui::LayerSourceInfo::kMaxRows + 1), OK);
    encoded.setDataPosition(0);
    gui::WindowInfosUpdate reused = packet; reused.layerSourcesComplete = true;
    EXPECT_EQ(reused.readFromParcel(&encoded), BAD_VALUE);
    EXPECT_FALSE(reused.layerSourcesComplete); EXPECT_TRUE(reused.layerSources.empty());
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSourceFactsPreserveFractionalCropAndRejectNonfiniteV6) {
    candidateDisplay(0, 100, 100);
    setCrop(111, FloatRect{0.25f, 0.5f, 10.75f, 11.5f});
    candidateInput(111, Region(Rect{0, 0, 20, 20}), std::nullopt); candidateUpdate();
    const auto* snapshot = getSnapshot(111); ASSERT_NE(snapshot, nullptr);
    const auto ordinary = snapshot->inputInfo;
    auto facts = sourceFactsForTest(*snapshot, 0, mLifecycleManager, mSnapshotBuilder);
    ASSERT_TRUE(facts); ASSERT_FALSE(facts->ancestors.empty());
    ASSERT_EQ(facts->ancestors.front().id, 111u);
    const std::array<float, 4> expected{0.25f, 0.5f, 10.75f, 11.5f};
    EXPECT_EQ(facts->ancestors.front().requestedCrop, expected);
    gui::WindowInfosUpdate packet;
    packet.windowInfos = {ordinary}; packet.layerSources = {*facts}; packet.layerSourcesComplete = true;
    Parcel encoded; ASSERT_EQ(packet.writeToParcel(&encoded), OK); encoded.setDataPosition(0);
    gui::WindowInfosUpdate decoded; ASSERT_EQ(decoded.readFromParcel(&encoded), OK);
    ASSERT_EQ(decoded.layerSources.size(), 1u); ASSERT_FALSE(decoded.layerSources[0].ancestors.empty());
    EXPECT_EQ(decoded.layerSources[0].ancestors[0].requestedCrop, expected);
    EXPECT_TRUE(decoded.windowInfos[0] == ordinary);
    EXPECT_TRUE(snapshot->inputInfo == ordinary); // fact production never changes stock input

    gui::WindowInfosUpdate plain; plain.windowInfos = {ordinary};
    Parcel prefix; ASSERT_EQ(plain.writeToParcel(&prefix), OK);
    // The unchanged v6 header is already included in prefix. Five row words,
    // two path counts, ancestor count, nine node words and three booleans follow.
    const size_t firstCrop = prefix.dataSize() +
            (20 + facts->mirrorRoots.size() + facts->relativeRoots.size()) * sizeof(uint32_t);
    ASSERT_LE(firstCrop + sizeof(float), encoded.dataSize());
    encoded.setDataPosition(firstCrop);
    float observed; ASSERT_EQ(encoded.readFloat(&observed), OK); EXPECT_EQ(observed, expected[0]);
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        auto bad = packet; bad.layerSources[0].ancestors[0].requestedCrop[0] = invalid;
        EXPECT_FALSE(gui::layerSourceFinite(bad.layerSources[0].ancestors[0]));
        Parcel rejected; EXPECT_EQ(bad.writeToParcel(&rejected), BAD_VALUE);
        Parcel corrupt; ASSERT_EQ(corrupt.appendFrom(&encoded, 0, encoded.dataSize()), OK);
        corrupt.setDataPosition(firstCrop); ASSERT_EQ(corrupt.writeFloat(invalid), OK);
        corrupt.setDataPosition(0);
        auto reused = packet; EXPECT_EQ(reused.readFromParcel(&corrupt), BAD_VALUE);
        EXPECT_FALSE(reused.layerSourcesComplete); EXPECT_TRUE(reused.layerSources.empty());
    }
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSourceFactsByteBudgetAndFinalWindowBindingRefuse) {
    candidateDisplay(0, 100, 100); setCrop(111, Rect{0, 0, 10, 10});
    candidateInput(111, Region(Rect{0, 0, 10, 10}), std::nullopt); candidateUpdate();
    const auto* snapshot = getSnapshot(111); ASSERT_NE(snapshot, nullptr);
    gui::LayerSourceBudget noBytes; noBytes.bytes = 0;
    EXPECT_FALSE(sourceFactsForTest(*snapshot, 0, mLifecycleManager, mSnapshotBuilder, &noBytes));
    gui::LayerSourceBudget noNodes; noNodes.nodes = 0;
    EXPECT_FALSE(sourceFactsForTest(*snapshot, 0, mLifecycleManager, mSnapshotBuilder, &noNodes));
    auto facts = sourceFactsForTest(*snapshot, 0, mLifecycleManager, mSnapshotBuilder); ASSERT_TRUE(facts);
    gui::WindowInfosUpdate packet; packet.windowInfos.push_back(snapshot->inputInfo);
    packet.layerSourcesComplete = true; packet.layerSources.push_back(*facts);
    EXPECT_TRUE(gui::layerSourcesValid(packet.layerSources, packet.windowInfos, true));
    packet.windowInfos[0].frame.left += 1;
    EXPECT_FALSE(gui::layerSourcesValid(packet.layerSources, packet.windowInfos, true));
    packet.windowInfos[0] = snapshot->inputInfo;
    packet.windowInfos[0].inputConfig |= gui::WindowInfo::InputConfig::DROP_INPUT;
    EXPECT_FALSE(gui::layerSourcesValid(packet.layerSources, packet.windowInfos, true));
    packet.windowInfos[0] = snapshot->inputInfo;
    packet.layerSources[0].applied.localTransform[0] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(gui::layerSourcesValid(packet.layerSources, packet.windowInfos, true));
    packet.layerSources[0] = *facts;
    Parcel encoded; ASSERT_EQ(packet.writeToParcel(&encoded), OK);
    gui::WindowInfosUpdate plain; plain.windowInfos = packet.windowInfos;
    Parcel ordinary; ASSERT_EQ(plain.writeToParcel(&ordinary), OK);
    const size_t expectedBytes = gui::LayerSourceInfo::kRowBytes +
            facts->ancestors.size() * gui::LayerSourceInfo::kNodeBytes +
            facts->traversal.size() * gui::LayerSourceInfo::kTraversalBytes;
    EXPECT_EQ(encoded.dataSize() - ordinary.dataSize(), expectedBytes);
    packet.windowInfos.assign(1024, snapshot->inputInfo);
    packet.layerSources.assign(1024, *facts);
    ASSERT_LE(packet.layerSources.size(), std::numeric_limits<uint32_t>::max());
    for (size_t i = 0; i < packet.layerSources.size(); ++i) {
        packet.layerSources[i].rowIndex = static_cast<uint32_t>(i);
    }
    EXPECT_FALSE(gui::layerSourcesValid(packet.layerSources, packet.windowInfos, true));
    Parcel oversized; EXPECT_EQ(packet.writeToParcel(&oversized), BAD_VALUE);
    // Unavailable facts do NOT suppress or change ordinary rows.
    packet.layerSourcesComplete = false; packet.layerSources.clear();
    Parcel unavailable; EXPECT_EQ(packet.writeToParcel(&unavailable), OK);
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSourceFactsUseActualRelativeAndDetachedMirrorParentPaths) {
    candidateDisplay(0, 100, 100); candidateDisplay(3, 100, 100);
    setCrop(1, Rect{0,0,100,100}); setCrop(13, Rect{0,0,10,10});
    reparentRelativeLayer(13, 11); // actual stock relative hierarchy path
    createRootLayer(3); setLayerStack(3, 3); setCrop(3, Rect{0,0,100,100});
    mirrorLayer(14, 3, 1);
    candidateInput(13, Region(Rect{0,0,10,10}), std::nullopt); candidateUpdate();
    bool relativeSource = false, detachedMirror = false;
    mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
        if (snapshot.path.mirrorRootIds.empty()) return;
        const auto ordinary = snapshot.inputInfo;
        const auto facts = sourceFactsForTest(snapshot, 0, mLifecycleManager, mSnapshotBuilder);
        ASSERT_TRUE(facts);
        EXPECT_TRUE(snapshot.inputInfo == ordinary);
        ASSERT_EQ(facts->roots.size(), 1u);
        const auto actual = std::find_if(facts->traversal.begin(), facts->traversal.end(),
                [](const auto& edge) { return edge.sourceId == 14 && edge.mirrorRoots.empty(); });
        ASSERT_NE(actual, facts->traversal.end());
        EXPECT_EQ(facts->roots[0].requested.lifetime.identity, actual->requested.lifetime.identity);
        EXPECT_TRUE(facts->roots[0].applied == actual->applied);
        if (snapshot.path.id == 13) {
            EXPECT_EQ(facts->ancestors.front().relativeParent, 11u);
            relativeSource = true;
        }
        if (snapshot.path.variant == LayerHierarchy::Detached_Mirror) detachedMirror = true;
    });
    EXPECT_TRUE(relativeSource); EXPECT_TRUE(detachedMirror);
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSnapshotIdentitySurvivesGeometryButReconstructionIsDistinct) {
    candidateDisplay(0,100,100);setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);candidateUpdate();
    const auto* stored=getSnapshot(111);ASSERT_NE(stored,nullptr);
    const auto identity=stored->nativeSnapshotIdentity;ASSERT_NE(identity,nullptr);
    const auto path=stored->path;
    setPosition(111,2,3);candidateUpdate();
    const auto* moved=getSnapshot(path);ASSERT_NE(moved,nullptr);
    EXPECT_EQ(moved->nativeSnapshotIdentity,identity);
    auto facts=sourceFactsForTest(*moved,0,mLifecycleManager,mSnapshotBuilder);ASSERT_TRUE(facts);
    EXPECT_EQ(facts->applied.snapshotIdentity,identity);
    const auto* requested=static_cast<const LayerLifecycleManager&>(mLifecycleManager).getLayerFromId(111);ASSERT_NE(requested,nullptr);
    LayerSnapshot rebuilt(*requested,path,LayerSnapshot{}); // Explicit resource-free fixture root.
    EXPECT_NE(rebuilt.nativeSnapshotIdentity,identity);
    LayerSnapshot observation=*moved; // A value observation is not a newly owned traversal instance.
    EXPECT_EQ(observation.nativeSnapshotIdentity,identity);
    gui::WindowInfosUpdate packet;packet.windowInfos.push_back(moved->inputInfo);
    packet.layerSourcesComplete=true;packet.layerSources.push_back(*facts);
    Parcel encoded;ASSERT_EQ(packet.writeToParcel(&encoded),OK);encoded.setDataPosition(0);
    gui::WindowInfosUpdate decoded;ASSERT_EQ(decoded.readFromParcel(&encoded),OK);
    ASSERT_EQ(decoded.layerSources.size(),1u);
    EXPECT_EQ(decoded.layerSources[0].applied.snapshotIdentity,identity);
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeSnapshotIdentitySeparatesOriginalAndTwoOrderedMirrorPaths) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);candidateDisplay(4,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});mirrorLayer(14,3,1);
    createRootLayer(4);setLayerStack(4,4);setCrop(4,Rect{0,0,100,100});mirrorLayer(15,4,1);
    candidateUpdate();
    const auto* original=getSnapshot(111);
    const auto* left=getSnapshot({.id=111,.mirrorRootIds={14u}});
    const auto* right=getSnapshot({.id=111,.mirrorRootIds={15u}});
    ASSERT_NE(original,nullptr);ASSERT_NE(left,nullptr);ASSERT_NE(right,nullptr);
    EXPECT_NE(original->nativeSnapshotIdentity,left->nativeSnapshotIdentity);
    EXPECT_NE(left->nativeSnapshotIdentity,right->nativeSnapshotIdentity);
    EXPECT_NE(original->inputInfo.id,left->inputInfo.id);
    EXPECT_NE(left->inputInfo.id,right->inputInfo.id);
    for (const auto* snapshot:{original,left,right}) {
        auto facts=sourceFactsForTest(*snapshot,0,mLifecycleManager,mSnapshotBuilder);ASSERT_TRUE(facts);
        EXPECT_EQ(facts->applied.snapshotIdentity,snapshot->nativeSnapshotIdentity);
        EXPECT_EQ(facts->mirrorRoots,(std::vector<uint32_t>(snapshot->path.mirrorRootIds.begin(),
                                                          snapshot->path.mirrorRootIds.end())));
        EXPECT_EQ(facts->variant,static_cast<uint32_t>(snapshot->path.variant));
        EXPECT_EQ(facts->relativeRoots,(std::vector<uint32_t>(snapshot->path.relativeRootIds.begin(),
                                                            snapshot->path.relativeRootIds.end())));
    }
}

TEST_F(LayerSnapshotStagedAtlasTest, nativeStructuralOwnedDropDoesNotForgeReceiverDropInputBit) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});
    setCrop(111,Rect{0,0,10,10});createLayer(112,11);setCrop(112,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt,
                   UNASSIGNED_LAYER_ID,false,false,false);
    candidateInput(112,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    mirrorLayer(14,3,11);setDropInputMode(14,gui::DropInputMode::ALL);candidateUpdate();
    const auto* requested=static_cast<const LayerLifecycleManager&>(mLifecycleManager).getLayerFromId(111);ASSERT_NE(requested,nullptr);
    EXPECT_TRUE(requested->hasInputInfo());EXPECT_TRUE(requested->needsInputInfo());
    const auto* structural=getSnapshot({.id=111,.mirrorRootIds={14u}});
    const auto* receiver=getSnapshot({.id=112,.mirrorRootIds={14u}});
    ASSERT_NE(structural,nullptr);ASSERT_NE(receiver,nullptr);
    EXPECT_TRUE(structural->hasInputInfo());
    EXPECT_FALSE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NOT_VISIBLE));
    EXPECT_TRUE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::CLONE));
    EXPECT_EQ(structural->inputInfo.token,nullptr);EXPECT_EQ(structural->inputInfo.windowToken,nullptr);
    EXPECT_TRUE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
    EXPECT_EQ(structural->dropInputMode,gui::DropInputMode::ALL);
    EXPECT_EQ(structural->nativeParentDropInputMode,gui::DropInputMode::ALL);
    EXPECT_FALSE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    const auto actual=layerSourceApplied(*structural); // actual SF snapshot, no lifetime resolver DOUBLE
    EXPECT_FALSE(actual.hasInputToken);EXPECT_FALSE(actual.hasClientToken);
    EXPECT_EQ(actual.composedDropMode,static_cast<uint32_t>(gui::DropInputMode::ALL));
    ASSERT_TRUE(structural->inputInfo.cloneLineage);
    EXPECT_EQ(structural->inputInfo.cloneLineage->sourceLayerId,111u);
    EXPECT_EQ(structural->inputInfo.cloneLineage->mirrorRootIds,std::vector<uint32_t>{14u});
    EXPECT_NE(receiver->inputInfo.token,nullptr);
    EXPECT_FALSE(receiver->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
    EXPECT_TRUE(receiver->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    ASSERT_NE(getSnapshot(111),nullptr);ASSERT_NE(getSnapshot(112),nullptr);
    EXPECT_EQ(getSnapshot(111)->dropInputMode,gui::DropInputMode::NONE);
    EXPECT_FALSE(getSnapshot(112)->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));

    gui::WindowInfosUpdate emitted;
    size_t structuralRows=0,receiverRows=0;
    // Same unfiltered ordinary-row projection as SF::buildWindowInfos. This
    // lifecycle fixture has no actual Layer/getHandle companion resolver;
    // deliberately do not fabricate a complete source-facts companion here.
    mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
        emitted.windowInfos.push_back(snapshot.inputInfo);
        if (&snapshot==structural) ++structuralRows;
        if (&snapshot==receiver) ++receiverRows;
    });
    ASSERT_EQ(structuralRows,1u);ASSERT_EQ(receiverRows,1u);
    for (const auto& entry:mFrontEndDisplayInfos) emitted.displayInfos.push_back(entry.second.info);
    emitted.vsyncId=17;emitted.timestamp=123456;
    EXPECT_FALSE(emitted.layerSourcesComplete);
    Parcel encoded;ASSERT_EQ(emitted.writeToParcel(&encoded),OK);encoded.setDataPosition(0);
    gui::WindowInfosUpdate decoded;ASSERT_EQ(decoded.readFromParcel(&encoded),OK);
    EXPECT_EQ(encoded.dataPosition(),encoded.dataSize());
    ASSERT_EQ(decoded.windowInfos.size(),emitted.windowInfos.size());
    EXPECT_EQ(decoded.vsyncId,emitted.vsyncId);EXPECT_EQ(decoded.timestamp,emitted.timestamp);
    EXPECT_FALSE(decoded.layerSourcesComplete);
    ASSERT_EQ(decoded.displayInfos.size(),emitted.displayInfos.size());
    for (size_t i=0;i<decoded.displayInfos.size();++i) {
        EXPECT_EQ(decoded.displayInfos[i].displayId,emitted.displayInfos[i].displayId);
        EXPECT_EQ(decoded.displayInfos[i].logicalWidth,emitted.displayInfos[i].logicalWidth);
        EXPECT_EQ(decoded.displayInfos[i].logicalHeight,emitted.displayInfos[i].logicalHeight);
        EXPECT_TRUE(decoded.displayInfos[i].transform==emitted.displayInfos[i].transform);
    }
    size_t decodedStructuralRows=0,decodedReceiverRows=0;
    for (size_t i=0;i<decoded.windowInfos.size();++i) {
        const auto& row=decoded.windowInfos[i];EXPECT_TRUE(row==emitted.windowInfos[i]);
        EXPECT_TRUE(row.cloneLayerStackTransform==emitted.windowInfos[i].cloneLayerStackTransform);
        if (row.id==structural->inputInfo.id) {
            ++decodedStructuralRows;EXPECT_FALSE(row.token);EXPECT_FALSE(row.windowToken);
            EXPECT_TRUE(row.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
            EXPECT_TRUE(row.inputConfig.test(gui::WindowInfo::InputConfig::CLONE));
            EXPECT_FALSE(row.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
            ASSERT_TRUE(row.cloneLineage);EXPECT_TRUE(row.cloneLineage==structural->inputInfo.cloneLineage);
        }
        if (row.id==receiver->inputInfo.id) {
            ++decodedReceiverRows;EXPECT_NE(row.token,nullptr);
            EXPECT_TRUE(row.inputConfig.test(gui::WindowInfo::InputConfig::CLONE));
            EXPECT_TRUE(row.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
        }
    }
    EXPECT_EQ(decodedStructuralRows,1u);EXPECT_EQ(decodedReceiverRows,1u);
}

TEST(ExternalDesktopOwnedBlockCreatorTest, KernelSystemUidAndExactSystemServerSidAreBothRequired) {
    EXPECT_TRUE(externalDesktopOwnedBlockCreator(AID_SYSTEM, "u:r:system_server:s0"));
    for (uid_t uid : {uid_t{0}, uid_t{2000}, uid_t{10004}, uid_t{10152}}) {
        EXPECT_FALSE(externalDesktopOwnedBlockCreator(uid, "u:r:system_server:s0"));
    }
    for (const char* sid : std::array<const char*,6>{nullptr, "", "u:r:system_app:s0", "u:r:su:s0",
                           "u:r:system_server:s0:c1", "u:r:system_server:s0-extra"}) {
        EXPECT_FALSE(externalDesktopOwnedBlockCreator(AID_SYSTEM, sid));
    }
    // Missing / forged metadata is not a replacement for a kernel calling SID.
}

TEST(ExternalDesktopOwnedBlockStateTest, MultipleOriginsKeepAllButRefuseCompleteEvidence) {
    const auto first=sp<BBinder>::make(),second=sp<BBinder>::make();
    ExternalDesktopOwnedInputBlockState empty,one,two,child;
    one.inherit(empty,first);
    EXPECT_TRUE(one.all);EXPECT_TRUE(one.covered);EXPECT_EQ(one.origin,first);
    two.inherit(one,second);
    EXPECT_TRUE(two.all);EXPECT_FALSE(two.covered);EXPECT_FALSE(two.origin);
    child.inherit(two,{});
    EXPECT_TRUE(child.all);EXPECT_FALSE(child.covered);EXPECT_FALSE(child.origin);
    // No origin is selected by overwriting another blocker. No enable / clear.
}

TEST(LayerSourceStockProjectionTest, UnknownAndIndependentStockAreNotMergedDropSubtraction) {
    gui::LayerSourceApplied value{};
    EXPECT_TRUE(gui::layerSourceStockProjectionValid(value)); // explicitly unavailable
    value.stockProjectionComplete=true;
    value.stockInputConfig=static_cast<uint32_t>(gui::WindowInfo::InputConfig::CLONE);
    value.inputConfig=value.stockInputConfig |
            static_cast<uint32_t>(gui::WindowInfo::InputConfig::DROP_INPUT);
    value.stockDropMode=static_cast<uint32_t>(gui::DropInputMode::NONE);
    value.composedDropMode=static_cast<uint32_t>(gui::DropInputMode::ALL);
    value.ownedDropAll=true;value.ownedInputBlockOrigin=sp<BBinder>::make();
    value.ownedInputBlockRevision=1; // actual creation revision in companion v5
    EXPECT_TRUE(gui::layerSourceStockProjectionValid(value));
    auto malformed=value;malformed.inputConfig=malformed.stockInputConfig;
    EXPECT_FALSE(gui::layerSourceStockProjectionValid(malformed));
    malformed=value;malformed.ownedInputBlockOrigin.clear();
    EXPECT_FALSE(gui::layerSourceStockProjectionValid(malformed));
    malformed=value;malformed.stockProjectionComplete=false;
    EXPECT_FALSE(gui::layerSourceStockProjectionValid(malformed));
    malformed=value;malformed.stockDropMode=99;
    EXPECT_FALSE(gui::layerSourceStockProjectionValid(malformed));
    // An actual STOCK DROP survives independently; it is never subtracted.
    value.stockInputConfig=value.inputConfig;
    value.stockDropMode=static_cast<uint32_t>(gui::DropInputMode::ALL);
    EXPECT_TRUE(gui::layerSourceStockProjectionValid(value));
    value.stockInputConfig|=static_cast<uint32_t>(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL);
    value.inputConfig=value.stockInputConfig;
    EXPECT_TRUE(gui::layerSourceStockProjectionValid(value));
}

TEST_F(LayerSnapshotStagedAtlasTest, ownedComponentAndOrdinaryMirrorHaveIndependentStockProjection) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);
    mirrorLayer(15,3,11);candidateUpdate();
    const auto* blocked=getSnapshot({.id=111,.mirrorRootIds={14u}});
    const auto* ordinary=getSnapshot({.id=111,.mirrorRootIds={15u}});
    ASSERT_NE(blocked,nullptr);ASSERT_NE(ordinary,nullptr);
    EXPECT_TRUE(blocked->nativeOwnedInputBlock.all);
    EXPECT_EQ(blocked->nativeOwnedInputBlock.origin,origin);
    EXPECT_FALSE(ordinary->nativeOwnedInputBlock.all);
    EXPECT_TRUE(blocked->nativeStockProjectionComplete);
    EXPECT_EQ(blocked->nativeStockInputConfig,ordinary->inputInfo.inputConfig.get());
    EXPECT_EQ(blocked->dropInputMode,gui::DropInputMode::NONE);
    EXPECT_TRUE(blocked->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_FALSE(ordinary->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_TRUE(blocked->inputInfo.touchableRegion.hasSameRects(ordinary->inputInfo.touchableRegion));
    EXPECT_FALSE(getSnapshot(111)->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    const auto facts=sourceFactsForTest(*blocked,0,mLifecycleManager,mSnapshotBuilder);ASSERT_TRUE(facts);
    ASSERT_EQ(facts->roots.size(),1u);
    EXPECT_EQ(facts->roots[0].requested.ownedInputBlockOrigin,origin);
    EXPECT_EQ(facts->roots[0].requested.requestedDropMode,static_cast<uint32_t>(gui::DropInputMode::NONE));
    EXPECT_EQ(facts->applied.stockDropMode,static_cast<uint32_t>(gui::DropInputMode::NONE));
    EXPECT_EQ(facts->applied.composedDropMode,static_cast<uint32_t>(gui::DropInputMode::ALL));
    EXPECT_TRUE(facts->applied.ownedDropAll);EXPECT_EQ(facts->applied.ownedInputBlockOrigin,origin);
    EXPECT_TRUE(gui::layerSourceMatchesWindow(*facts,blocked->inputInfo,0));
    setPosition(14,2,3);candidateUpdate();
    EXPECT_EQ(getSnapshot({.id=111,.mirrorRootIds={14u}})->nativeOwnedInputBlock.origin,origin);
}

TEST_F(LayerSnapshotStagedAtlasTest, ownedProjectionCannotEraseAppAncestorOrSecureDisplayDrop) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    TEST_DOUBLE_ownedMirror(14,3,11);setDropInputMode(3,gui::DropInputMode::ALL);candidateUpdate();
    auto* blocked=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(blocked,nullptr);
    const auto drop=static_cast<uint32_t>(gui::WindowInfo::InputConfig::DROP_INPUT);
    EXPECT_NE(blocked->nativeStockInputConfig & drop,0u);
    EXPECT_EQ(blocked->dropInputMode,gui::DropInputMode::ALL);
    setDropInputMode(3,gui::DropInputMode::NONE);
    setInputInfo(111,[](auto& info){info.inputConfig|=gui::WindowInfo::InputConfig::DROP_INPUT;});
    candidateUpdate();
    blocked=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(blocked,nullptr);
    EXPECT_NE(blocked->nativeStockInputConfig & drop,0u); // app-provided bit, NOT owned
    EXPECT_EQ(blocked->dropInputMode,gui::DropInputMode::NONE);
    setInputInfo(111,[](auto& info){info.inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);});
    setFlags(111,layer_state_t::eLayerSecure,layer_state_t::eLayerSecure);
    DisplayInfo insecure{};insecure.info.displayId=ui::LogicalDisplayId{3};
    insecure.info.logicalWidth=100;insecure.info.logicalHeight=100;
    insecure.receivesInput=true;insecure.isSecure=false;
    mFrontEndDisplayInfos.emplace_or_replace(ui::LayerStack::fromValue(3),insecure);candidateUpdate(true);
    blocked=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(blocked,nullptr);
    EXPECT_NE(blocked->nativeStockInputConfig & drop,0u); // real secure-on-insecure stock rule
    EXPECT_TRUE(blocked->nativeOwnedInputBlock.all);
    const auto actual=layerSourceApplied(*blocked);
    EXPECT_TRUE(actual.stockProjectionComplete);EXPECT_TRUE(actual.ownedDropAll);
    EXPECT_TRUE(gui::layerSourceStockProjectionValid(actual));
}

TEST_F(LayerSnapshotStagedAtlasTest, ownedStructuralAndReceiverRowsExportWholePacketWithoutForgingReceiverBits) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    createLayer(112,11);setCrop(112,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt,UNASSIGNED_LAYER_ID,false,false,false);
    candidateInput(112,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);candidateUpdate();
    const auto* structural=getSnapshot({.id=111,.mirrorRootIds={14u}});
    const auto* receiver=getSnapshot({.id=112,.mirrorRootIds={14u}});
    ASSERT_NE(structural,nullptr);ASSERT_NE(receiver,nullptr);
    EXPECT_FALSE(structural->inputInfo.token);EXPECT_FALSE(structural->inputInfo.windowToken);
    EXPECT_TRUE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
    EXPECT_FALSE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_TRUE(receiver->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    for (const auto* value:{structural,receiver}) {
        EXPECT_TRUE(value->nativeOwnedInputBlock.all);EXPECT_EQ(value->nativeOwnedInputBlock.origin,origin);
        EXPECT_EQ(value->dropInputMode,gui::DropInputMode::NONE);EXPECT_TRUE(value->nativeStockProjectionComplete);
    }
    gui::WindowInfosUpdate full;full.layerSourcesComplete=true;
    gui::LayerSourceBudget budget;
    mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
        full.windowInfos.push_back(snapshot.inputInfo);
        ASSERT_LE(full.windowInfos.size()-1,std::numeric_limits<uint32_t>::max());
        auto facts=sourceFactsForTest(snapshot,static_cast<uint32_t>(full.windowInfos.size()-1),
                mLifecycleManager,mSnapshotBuilder,&budget);
        ASSERT_TRUE(facts);full.layerSources.push_back(std::move(*facts));
    });
    ASSERT_FALSE(full.windowInfos.empty());ASSERT_EQ(full.layerSources.size(),full.windowInfos.size());
    ASSERT_TRUE(gui::layerSourcesValid(full.layerSources,full.windowInfos,true));
    Parcel encoded;ASSERT_EQ(full.writeToParcel(&encoded),OK);encoded.setDataPosition(0);
    gui::WindowInfosUpdate decoded;ASSERT_EQ(decoded.readFromParcel(&encoded),OK);
    EXPECT_EQ(encoded.dataPosition(),encoded.dataSize());ASSERT_EQ(decoded.windowInfos.size(),full.windowInfos.size());
    ASSERT_EQ(decoded.layerSources.size(),full.layerSources.size());
    size_t structuralRows=0,receiverRows=0;
    for (size_t i=0;i<full.windowInfos.size();++i) {
        EXPECT_TRUE(decoded.windowInfos[i]==full.windowInfos[i]);
        EXPECT_EQ(decoded.layerSources[i].applied,full.layerSources[i].applied);
        EXPECT_FALSE(decoded.windowInfos[i].stageTag);
        if (decoded.windowInfos[i].id==structural->inputInfo.id) {
            ++structuralRows;EXPECT_TRUE(decoded.layerSources[i].applied.ownedDropAll);
            EXPECT_FALSE(decoded.windowInfos[i].inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
        }
        if (decoded.windowInfos[i].id==receiver->inputInfo.id) ++receiverRows;
    }
    EXPECT_EQ(structuralRows,1u);EXPECT_EQ(receiverRows,1u);
}

TEST_F(LayerSnapshotStagedAtlasTest, stockOwnedCodecRejectsVersionMalformedBooleanAndProspectiveBounds) {
    candidateDisplay(0,100,100);setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);candidateUpdate();
    const auto* value=getSnapshot(111);ASSERT_NE(value,nullptr);
    auto facts=sourceFactsForTest(*value,0,mLifecycleManager,mSnapshotBuilder);ASSERT_TRUE(facts);
    gui::WindowInfosUpdate full;full.windowInfos={value->inputInfo};full.layerSources={*facts};full.layerSourcesComplete=true;
    Parcel encoded;ASSERT_EQ(full.writeToParcel(&encoded),OK);
    gui::WindowInfosUpdate ordinary;ordinary.windowInfos=full.windowInfos;
    Parcel prefix;ASSERT_EQ(ordinary.writeToParcel(&prefix),OK);
    // kHeaderBytes is a budget charge including Binder object-table offsets,
    // NOT the number of bytes in Parcel::data(). Measure the v6 header's
    // actual primitive layout, including its publication witness.
    Parcel headerLayout;ASSERT_EQ(headerLayout.writeUint32(6),OK);
    ASSERT_EQ(gui::writeNativePublication(&headerLayout,{}),OK);
    const size_t availabilityOffset=headerLayout.dataPosition();
    ASSERT_EQ(headerLayout.writeUint32(0),OK);
    const size_t countOffset=headerLayout.dataPosition();
    ASSERT_EQ(headerLayout.writeUint32(0),OK);
    ASSERT_LE(headerLayout.dataSize(),prefix.dataSize());
    const size_t header=prefix.dataSize()-headerLayout.dataSize();
    encoded.setDataPosition(header);
    uint32_t observed;ASSERT_EQ(encoded.readUint32(&observed),OK);ASSERT_EQ(observed,6u);
    gui::NativePublication publication;
    ASSERT_EQ(gui::readNativePublication(&encoded,publication),OK);
    EXPECT_FALSE(publication.available());
    ASSERT_EQ(encoded.dataPosition(),header+availabilityOffset);
    ASSERT_EQ(encoded.readUint32(&observed),OK);ASSERT_EQ(observed,1u);
    ASSERT_EQ(encoded.dataPosition(),header+countOffset);
    ASSERT_EQ(encoded.readUint32(&observed),OK);ASSERT_EQ(observed,1u);

    // The final applied row ends in the stock/owned projection, including v6
    // revision+epoch. Locate it without confusing budget bytes with data bytes.
    const auto& applied=full.layerSources.back().applied;
    Parcel projectionLayout;ASSERT_EQ(projectionLayout.writeUint32(applied.stockProjectionComplete),OK);
    ASSERT_EQ(projectionLayout.writeUint32(applied.stockInputConfig),OK);
    ASSERT_EQ(projectionLayout.writeUint32(applied.stockDropMode),OK);
    const size_t ownedAllOffset=projectionLayout.dataPosition();
    ASSERT_EQ(projectionLayout.writeUint32(applied.ownedDropAll),OK);
    ASSERT_EQ(projectionLayout.writeStrongBinder(applied.ownedInputBlockOrigin),OK);
    ASSERT_EQ(projectionLayout.writeUint64(applied.ownedInputBlockRevision),OK);
    ASSERT_EQ(projectionLayout.writeInt64(applied.ownedInputBlockEpoch),OK);
    ASSERT_LE(projectionLayout.dataSize(),encoded.dataSize());
    const size_t projection=encoded.dataSize()-projectionLayout.dataSize();
    encoded.setDataPosition(projection);
    ASSERT_EQ(encoded.readUint32(&observed),OK);ASSERT_EQ(observed,1u);
    encoded.setDataPosition(projection+ownedAllOffset);
    ASSERT_EQ(encoded.readUint32(&observed),OK);ASSERT_EQ(observed,0u);
    const auto refused=[&](size_t position,uint32_t word) {
        Parcel bad;ASSERT_EQ(bad.appendFrom(&encoded,0,encoded.dataSize()),OK);
        bad.setDataPosition(position);ASSERT_EQ(bad.writeUint32(word),OK);bad.setDataPosition(0);
        gui::WindowInfosUpdate decoded;EXPECT_EQ(decoded.readFromParcel(&bad),BAD_VALUE);
        EXPECT_FALSE(decoded.layerSourcesComplete);EXPECT_TRUE(decoded.layerSources.empty());
    };
    // Private v3/v4/v5 and the distinct normal-source v7 are not this v6 wire.
    for (uint32_t version:{3u,4u,5u,7u}) refused(header,version);
    refused(header+availabilityOffset,2);
    refused(header+countOffset,gui::LayerSourceInfo::kMaxRows+1); // before row allocation
    refused(projection,2); // malformed stockProjectionComplete boolean
    refused(projection+ownedAllOffset,2); // malformed ownedDropAll boolean
    auto invalid=full;invalid.layerSources[0].applied.stockInputConfig^=1u;
    EXPECT_FALSE(gui::layerSourcesValid(invalid.layerSources,invalid.windowInfos,true));
    gui::LayerSourceBudget budget;budget.bytes=gui::LayerSourceInfo::kRowBytes-1;
    EXPECT_FALSE(sourceFactsForTest(*value,0,mLifecycleManager,mSnapshotBuilder,&budget));
    full.layerSources.assign(1024,*facts);full.windowInfos.assign(1024,value->inputInfo);
    ASSERT_LE(full.layerSources.size(),std::numeric_limits<uint32_t>::max());
    for (size_t i=0;i<full.layerSources.size();++i) {
        full.layerSources[i].rowIndex=static_cast<uint32_t>(i);
    }
    EXPECT_FALSE(gui::layerSourcesValid(full.layerSources,full.windowInfos,true));
}

TEST_F(LayerSnapshotStagedAtlasTest, nestedOwnedOriginsRemainActuallyBlockedButCompanionIsUnavailable) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto first=TEST_DOUBLE_ownedMirror(14,3,11);
    const auto second=TEST_DOUBLE_ownedMirror(15,3,14);ASSERT_NE(first,second);candidateUpdate();
    const auto* nested=getSnapshot({.id=111,.mirrorRootIds={15u,14u}});ASSERT_NE(nested,nullptr);
    EXPECT_TRUE(nested->nativeOwnedInputBlock.all);
    EXPECT_FALSE(nested->nativeOwnedInputBlock.covered);EXPECT_FALSE(nested->nativeOwnedInputBlock.origin);
    EXPECT_TRUE(nested->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_FALSE(nested->nativeStockProjectionComplete);
    const auto actual=layerSourceApplied(*nested);
    EXPECT_TRUE(actual.ownedDropAll);EXPECT_FALSE(gui::layerSourceStockProjectionValid(actual));
    EXPECT_FALSE(sourceFactsForTest(*nested,0,mLifecycleManager,mSnapshotBuilder));
    // Publication of ordinary rows still proceeds; unavailable evidence is
    // NEVER an enable, an eviction of either blocker or a stage acceptance.
    size_t occurrences=0;
    mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
        if (&snapshot==nested) ++occurrences;
    });
    EXPECT_EQ(occurrences,1u);
}

TEST(ExternalDesktopOwnedBlockControlTest, ExactRevisionArmedEpochAndCheckedExhaustion) {
    ExternalDesktopOwnedInputBlockControl value;
    EXPECT_FALSE(value.accepts(1,7,false)); // creation is blocked and unarmed
    EXPECT_FALSE(value.accepts(0,7,true));EXPECT_FALSE(value.accepts(1,0,true));
    ASSERT_TRUE(value.accepts(1,7,true));value.advance(7,true);
    EXPECT_TRUE(value.blocked);EXPECT_EQ(value.revision,2u);EXPECT_EQ(value.epoch,7);
    EXPECT_FALSE(value.accepts(1,7,false));EXPECT_FALSE(value.accepts(2,6,false));
    ASSERT_TRUE(value.accepts(2,7,false));value.advance(7,false);
    EXPECT_FALSE(value.blocked);EXPECT_EQ(value.revision,3u);
    ASSERT_TRUE(value.accepts(3,8,true));value.advance(8,true);
    EXPECT_FALSE(value.accepts(4,7,false));EXPECT_FALSE(value.accepts(3,8,false));
    value.revision=uint64_t(INT64_MAX);EXPECT_FALSE(value.accepts(value.revision,9,true));
}

TEST(ExternalDesktopOwnedBlockStateTest, DistinctEnabledOriginsRemainBlockedAndUncoveredInEveryDescendant) {
    const auto first=sp<BBinder>::make(),second=sp<BBinder>::make();
    ExternalDesktopOwnedInputBlockControl enabled;
    ASSERT_TRUE(enabled.accepts(1,7,true));enabled.advance(7,true);
    ASSERT_TRUE(enabled.accepts(2,7,false));enabled.advance(7,false);
    ExternalDesktopOwnedInputBlockState empty,one,two,descendant;
    one.inherit(empty,first,enabled);
    EXPECT_FALSE(one.all);EXPECT_TRUE(one.covered);EXPECT_EQ(one.origin,first);
    EXPECT_EQ(one.revision,3u);EXPECT_EQ(one.epoch,7);
    two.inherit(one,second,enabled);
    EXPECT_TRUE(two.all);EXPECT_FALSE(two.covered);EXPECT_FALSE(two.origin);
    EXPECT_EQ(two.revision,0u);EXPECT_EQ(two.epoch,0);
    descendant.inherit(two,nullptr,enabled);
    EXPECT_TRUE(descendant.all);EXPECT_FALSE(descendant.covered);EXPECT_FALSE(descendant.origin);
    EXPECT_EQ(descendant.revision,0u);EXPECT_EQ(descendant.epoch,0);
    descendant.inherit(two,first,enabled);
    EXPECT_TRUE(descendant.all);EXPECT_FALSE(descendant.covered);EXPECT_FALSE(descendant.origin);
}

TEST_F(LayerSnapshotStagedAtlasTest, scopedOwnedArmAndEnablePublishActualInputStateWithoutStockMutation) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),Region(Rect{0,0,10,10}));
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);candidateUpdate();
    auto* row=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(row,nullptr);
    const auto originalStock=row->nativeStockInputConfig;const auto region=row->inputInfo.touchableRegion;
    const auto identity=row->nativeSnapshotIdentity;
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{1},7,true));
    EXPECT_TRUE(mLifecycleManager.getGlobalChanges().test(RequestedLayerState::Changes::Input));
    candidateUpdate();row=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(row,nullptr);
    EXPECT_TRUE(row->nativeOwnedInputBlock.all);EXPECT_EQ(row->nativeOwnedInputBlock.revision,2u);
    EXPECT_EQ(row->nativeOwnedInputBlock.epoch,7);EXPECT_EQ(row->nativeStockInputConfig,originalStock);
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{2},7,false));
    candidateUpdate();row=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(row,nullptr);
    EXPECT_FALSE(row->nativeOwnedInputBlock.all);EXPECT_EQ(row->nativeOwnedInputBlock.origin,origin);
    EXPECT_EQ(row->nativeOwnedInputBlock.revision,3u);EXPECT_EQ(row->nativeOwnedInputBlock.epoch,7);
    EXPECT_EQ(row->nativeSnapshotIdentity,identity);EXPECT_EQ(row->nativeStockInputConfig,originalStock);
    EXPECT_EQ(row->inputInfo.inputConfig.get(),originalStock);
    EXPECT_TRUE(row->inputInfo.touchableRegion.hasSameRects(region));
    EXPECT_FALSE(getSnapshot(111)->nativeOwnedInputBlock.origin);
    const auto facts=sourceFactsForTest(*row,0,mLifecycleManager,mSnapshotBuilder);ASSERT_TRUE(facts);
    EXPECT_TRUE(gui::layerSourceMatchesWindow(*facts,row->inputInfo,0));
    EXPECT_EQ(facts->applied.ownedInputBlockRevision,3u);EXPECT_EQ(facts->applied.ownedInputBlockEpoch,7);
}

TEST_F(LayerSnapshotStagedAtlasTest, scopedOwnedCohortRejectsLateBadRootDuplicatesForeignAndStaleWithoutPrefix) {
    const auto first=TEST_DOUBLE_ownedMirror(14,1,11),second=TEST_DOUBLE_ownedMirror(15,1,11);
    candidateUpdate();
    const auto control=[&](uint32_t id)->const auto& {
        const LayerLifecycleManager& lifecycle=mLifecycleManager;
        return lifecycle.getLayerFromId(id)->externalDesktopOwnedInputBlockControl;
    };
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,99},{first,second},{1,1},7,true));
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,first},{1,1},7,true));
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,14},{first,first},{1,1},7,true));
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,second},{1,0},7,true));
    EXPECT_TRUE(control(14).blocked);EXPECT_EQ(control(14).revision,1u);
    EXPECT_TRUE(control(15).blocked);EXPECT_EQ(control(15).revision,1u);
    EXPECT_EQ(mLifecycleManager.getGlobalChanges().get(),0u);
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,second},{1,1},7,true));
    candidateUpdate();
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,second},{2,1},7,false));
    EXPECT_EQ(control(14).revision,2u);EXPECT_TRUE(control(14).blocked);
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,second},{2,2},7,false));
    EXPECT_FALSE(control(14).blocked);EXPECT_FALSE(control(15).blocked);
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,second},{3,3},8,true));
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14,15},{first,second},{4,4},7,false));
    EXPECT_TRUE(control(14).blocked);EXPECT_TRUE(control(15).blocked);
}

TEST_F(LayerSnapshotStagedAtlasTest, scopedOwnedEnableCannotEraseIndependentStockAncestorOrAppDrop) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);
    setDropInputMode(3,gui::DropInputMode::ALL);candidateUpdate();
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{1},7,true));
    candidateUpdate();
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{2},7,false));
    candidateUpdate();
    const auto* row=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(row,nullptr);
    EXPECT_FALSE(row->nativeOwnedInputBlock.all);EXPECT_EQ(row->dropInputMode,gui::DropInputMode::ALL);
    EXPECT_TRUE(row->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_TRUE(gui::layerSourceStockProjectionValid(layerSourceApplied(*row)));
    setDropInputMode(3,gui::DropInputMode::NONE);
    setInputInfo(111,[](auto& info){info.inputConfig|=gui::WindowInfo::InputConfig::DROP_INPUT;});
    candidateUpdate();row=getSnapshot({.id=111,.mirrorRootIds={14u}});ASSERT_NE(row,nullptr);
    EXPECT_FALSE(row->nativeOwnedInputBlock.all);EXPECT_EQ(row->dropInputMode,gui::DropInputMode::NONE);
    EXPECT_TRUE(row->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_EQ(row->nativeStockInputConfig,row->inputInfo.inputConfig.get());
}

TEST_F(LayerSnapshotStagedAtlasTest, scopedOwnedEnabledEpochRevisionRoundTripWithCompleteActualRows) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),Region(Rect{0,0,10,10}));
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);candidateUpdate();
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{1},7,true));candidateUpdate();
    ASSERT_TRUE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{2},7,false));candidateUpdate();
    gui::WindowInfosUpdate full;gui::LayerSourceBudget budget;bool available=true;
    mSnapshotBuilder.forEachInputSnapshot([&](const LayerSnapshot& snapshot) {
        const auto index=static_cast<uint32_t>(full.windowInfos.size());
        full.windowInfos.push_back(snapshot.inputInfo);
        const auto facts=sourceFactsForTest(snapshot,index,mLifecycleManager,mSnapshotBuilder,&budget);
        if (!facts) available=false;else full.layerSources.push_back(*facts);
    });
    ASSERT_TRUE(available);full.layerSourcesComplete=true;
    for (const auto& display:mFrontEndDisplayInfos) full.displayInfos.push_back(display.second.info);
    ASSERT_TRUE(gui::layerSourcesValid(full.layerSources,full.windowInfos,true));
    Parcel encoded;ASSERT_EQ(full.writeToParcel(&encoded),OK);encoded.setDataPosition(0);
    gui::WindowInfosUpdate restored;ASSERT_EQ(restored.readFromParcel(&encoded),OK);
    EXPECT_EQ(encoded.dataPosition(),encoded.dataSize());ASSERT_TRUE(restored.layerSourcesComplete);
    ASSERT_EQ(restored.layerSources.size(),full.layerSources.size());
    bool sawEnabled=false;
    for (size_t i=0;i<restored.layerSources.size();++i) {
        EXPECT_TRUE(restored.layerSources[i].applied==full.layerSources[i].applied);
        ASSERT_EQ(restored.layerSources[i].roots.size(),full.layerSources[i].roots.size());
        ASSERT_EQ(restored.layerSources[i].traversal.size(),full.layerSources[i].traversal.size());
        for (size_t r=0;r<restored.layerSources[i].roots.size();++r) {
            EXPECT_TRUE(restored.layerSources[i].roots[r].applied==full.layerSources[i].roots[r].applied);
            EXPECT_EQ(restored.layerSources[i].roots[r].requested.ownedInputBlockOrigin,
                      full.layerSources[i].roots[r].requested.ownedInputBlockOrigin);
        }
        for (size_t r=0;r<restored.layerSources[i].traversal.size();++r) {
            EXPECT_TRUE(restored.layerSources[i].traversal[r].applied==full.layerSources[i].traversal[r].applied);
        }
        const auto& applied=restored.layerSources[i].applied;
        if (applied.ownedInputBlockOrigin==origin) {
            sawEnabled=true;EXPECT_FALSE(applied.ownedDropAll);
            EXPECT_EQ(applied.ownedInputBlockRevision,3u);EXPECT_EQ(applied.ownedInputBlockEpoch,7);
        }
    }
    EXPECT_TRUE(sawEnabled);
}

TEST_F(LayerSnapshotStagedAtlasTest, preOwnerViewportCandidateReachesReplicaWithoutBroadeningOrdinaryRegion) {
    candidateDisplay(0,10,10);candidateDisplay(3,10,10);
    setCrop(1,Rect{0,0,20,10});setCrop(11,Rect{0,0,20,10});setCrop(111,Rect{0,0,20,10});
    // Exactly the WMS distinction: ordinary owner-viewport region is already
    // clipped; the independent candidate retains the stock pre-owner cut.
    candidateInput(111,Region(Rect{0,0,10,10}),Region(Rect{0,0,20,10}));
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,10,10});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);
    setCrop(14,Rect{10,0,20,10});setPosition(14,-10,0);candidateUpdate();
    const auto* original=getSnapshot(111);const auto* replica=getSnapshot({.id=111,.mirrorRootIds={14u}});
    ASSERT_NE(original,nullptr);ASSERT_NE(replica,nullptr);
    EXPECT_TRUE(original->inputInfo.touchableRegion.hasSameRects(Region(Rect{0,0,10,10})));
    EXPECT_TRUE(replica->inputInfo.touchableRegion.isEmpty());
    ASSERT_TRUE(replica->inputInfo.preViewportTouchableRegion);
    EXPECT_TRUE(replica->inputInfo.preViewportTouchableRegion->hasSameRects(Region(Rect{0,0,10,10})));
    EXPECT_TRUE(replica->nativeOwnedInputBlock.all);EXPECT_EQ(replica->nativeOwnedInputBlock.origin,origin);
}

TEST(ExternalDesktopProtectedRootStateTest, ExactStoredKeyRevisionAndPhaseAreRequired) {
    using Control=ExternalDesktopOwnedInputBlockControl;using Op=Control::Operation;
    const gui::WindowInfo::StageTag tag{5,6,7,8,9};
    Control value;
    EXPECT_FALSE(value.acceptsState(1,8,Op::ENABLE_SAME_TAG,tag));
    EXPECT_FALSE(value.acceptsState(1,8,Op::ARM_TAGGED_DROP,std::nullopt));
    ASSERT_TRUE(value.acceptsState(1,8,Op::ARM_TAGGED_DROP,tag));value.advanceState(8,Op::ARM_TAGGED_DROP,tag);
    EXPECT_TRUE(value.blocked);EXPECT_EQ(value.revision,2u);EXPECT_EQ(value.stageTag,tag);
    EXPECT_FALSE(value.accepts(2,8,false));EXPECT_FALSE(value.accepts(2,8,true)); // legacy cannot bypass either way
    EXPECT_FALSE(value.acceptsState(2,8,Op::ARM_TAGGED_DROP,tag));
    for (int field=0;field<5;++field) {
        auto changed=tag;
        if(field==0)++changed.authorityGeneration;
        if(field==1)++changed.atlasRevision;
        if(field==2)++changed.geometryRevision;
        if(field==3)++changed.epoch;
        if(field==4)++changed.presentationKey;
        EXPECT_FALSE(value.acceptsState(2,8,Op::ENABLE_SAME_TAG,changed));
    }
    ASSERT_TRUE(value.acceptsState(2,8,Op::ENABLE_SAME_TAG,tag));value.advanceState(8,Op::ENABLE_SAME_TAG,tag);
    EXPECT_FALSE(value.blocked);EXPECT_EQ(value.stageTag,tag);EXPECT_EQ(value.revision,3u);
    EXPECT_FALSE(value.acceptsState(2,8,Op::ENABLE_SAME_TAG,tag));
    EXPECT_FALSE(value.acceptsState(3,8,Op::REPARK_CLEAR_TAG,tag));
    ASSERT_TRUE(value.acceptsState(3,9,Op::REPARK_CLEAR_TAG,std::nullopt));value.advanceState(9,Op::REPARK_CLEAR_TAG,std::nullopt);
    EXPECT_TRUE(value.blocked);EXPECT_FALSE(value.stageTag);EXPECT_EQ(value.revision,4u);
    EXPECT_FALSE(value.acceptsState(4,8,Op::ARM_TAGGED_DROP,tag));
    value.revision=uint64_t(INT64_MAX);
    EXPECT_FALSE(value.acceptsState(value.revision,9,Op::REPARK_CLEAR_TAG,std::nullopt));
    EXPECT_FALSE(value.acceptsState(value.revision,9,static_cast<Op>(99),std::nullopt));
}

TEST(ExternalDesktopProtectedRootStateTest, ConflictingActualOriginsNeverExportATagEvenIfEnabled) {
    using Control=ExternalDesktopOwnedInputBlockControl;using Op=Control::Operation;
    const auto first=sp<BBinder>::make(),second=sp<BBinder>::make();
    Control control;const gui::WindowInfo::StageTag tag{5,6,7,8,9};
    control.advanceState(8,Op::ARM_TAGGED_DROP,tag);control.advanceState(8,Op::ENABLE_SAME_TAG,tag);
    ExternalDesktopOwnedInputBlockState empty,one,two,child;
    one.inherit(empty,first,control,14);ASSERT_TRUE(one.stageTag);EXPECT_FALSE(one.all);
    two.inherit(one,second,control,15);EXPECT_FALSE(two.covered);EXPECT_TRUE(two.all);EXPECT_FALSE(two.stageTag);
    child.inherit(two,nullptr);EXPECT_FALSE(child.covered);EXPECT_TRUE(child.all);EXPECT_FALSE(child.stageTag);
}

TEST_F(LayerSnapshotStagedAtlasTest, protectedRootTagComesFromActualRootAndSurvivesOnlyMatchingEnable) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    candidateInput(111,Region(Rect{0,0,10,10}),Region(Rect{0,0,10,10}),UNASSIGNED_LAYER_ID,false,true);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});
    const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);candidateUpdate();
    const auto path=LayerHierarchy::TraversalPath{.id=111,.mirrorRootIds={14u}};
    const auto* before=getSnapshot(path);ASSERT_NE(before,nullptr);
    const auto identity=before->nativeSnapshotIdentity;const auto stock=before->nativeStockInputConfig;
    EXPECT_FALSE(before->inputInfo.stageTag);EXPECT_TRUE(before->nativeOwnedInputBlock.all);
    const gui::WindowInfo::StageTag tag{5,6,7,8,9};
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14},{origin},{1},1,5,6,7,8,{9}));candidateUpdate();
    const auto* armed=getSnapshot(path);ASSERT_NE(armed,nullptr);
    EXPECT_EQ(armed->inputInfo.stageTag,tag);EXPECT_TRUE(armed->nativeOwnedInputBlock.all);
    EXPECT_EQ(armed->nativeStockInputConfig,stock);EXPECT_EQ(armed->nativeSnapshotIdentity,identity);
    EXPECT_TRUE(armed->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    ASSERT_NE(getSnapshot(111),nullptr);EXPECT_FALSE(getSnapshot(111)->inputInfo.stageTag);
    EXPECT_FALSE(mLifecycleManager.setExternalDesktopOwnedInputBlocks({14},{origin},{2},8,false));
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14},{origin},{2},2,5,6,7,8,{10}));
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14},{origin},{2},2,5,6,7,8,{9}));candidateUpdate();
    const auto* enabled=getSnapshot(path);ASSERT_NE(enabled,nullptr);
    EXPECT_EQ(enabled->inputInfo.stageTag,tag);EXPECT_FALSE(enabled->nativeOwnedInputBlock.all);
    EXPECT_EQ(enabled->inputInfo.inputConfig.get(),stock);EXPECT_EQ(enabled->nativeSnapshotIdentity,identity);
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14},{origin},{3},0,0,0,0,9,{0}));candidateUpdate();
    const auto* parked=getSnapshot(path);ASSERT_NE(parked,nullptr);
    EXPECT_FALSE(parked->inputInfo.stageTag);EXPECT_TRUE(parked->nativeOwnedInputBlock.all);
    EXPECT_EQ(parked->nativeStockInputConfig,stock);
}

TEST_F(LayerSnapshotStagedAtlasTest, protectedRootStateValidatesWholeCohortBeforeMutation) {
    createRootLayer(3);const auto first=TEST_DOUBLE_ownedMirror(14,3,11),second=TEST_DOUBLE_ownedMirror(15,3,11);
    auto control=[&](uint32_t id)->const ExternalDesktopOwnedInputBlockControl& {
        return static_cast<const LayerLifecycleManager&>(mLifecycleManager).getLayerFromId(id)->externalDesktopOwnedInputBlockControl;
    };
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{1,0},1,5,6,7,8,{9,10}));
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,first},{1,1},1,5,6,7,8,{9,10}));
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,14},{first,first},{1,1},1,5,6,7,8,{9,10}));
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{1,1},1,5,6,7,8,{9}));
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{1,1},99,5,6,7,8,{9,10}));
    EXPECT_EQ(control(14).revision,1u);EXPECT_FALSE(control(14).stageTag);
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{1,1},1,5,6,7,8,{9,10}));
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{2,2},2,5,6,7,8,{9,99}));
    EXPECT_TRUE(control(14).blocked);EXPECT_EQ(control(14).revision,2u);EXPECT_EQ(control(15).revision,2u);
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{2,2},2,5,6,7,8,{9,10}));
    EXPECT_FALSE(control(14).blocked);EXPECT_FALSE(control(15).blocked);
    mLifecycleManager.onHandlesDestroyed({{15,"actual destroyed fixture handle"}});
    EXPECT_FALSE(mLifecycleManager.transitionExternalDesktopRootStates({14,15},{first,second},{3,3},0,0,0,0,9,{0,0}));
    EXPECT_FALSE(control(14).blocked);EXPECT_EQ(control(14).revision,3u);
}

TEST_F(LayerSnapshotStagedAtlasTest, protectedTagsPreserveStockDropAndStructuralNoChannelProjection) {
    candidateDisplay(0,100,100);candidateDisplay(3,100,100);
    setCrop(1,Rect{0,0,100,100});setCrop(11,Rect{0,0,100,100});setCrop(111,Rect{0,0,10,10});
    createLayer(112,11);setCrop(112,Rect{0,0,10,10});setDropInputMode(11,gui::DropInputMode::ALL);
    candidateInput(111,Region(Rect{0,0,10,10}),std::nullopt);
    candidateInput(112,Region(Rect{0,0,10,10}),std::nullopt,UNASSIGNED_LAYER_ID,false,false,false);
    createRootLayer(3);setLayerStack(3,3);setCrop(3,Rect{0,0,100,100});const auto origin=TEST_DOUBLE_ownedMirror(14,3,11);
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14},{origin},{1},1,5,6,7,8,{9}));candidateUpdate();
    ASSERT_TRUE(mLifecycleManager.transitionExternalDesktopRootStates({14},{origin},{2},2,5,6,7,8,{9}));candidateUpdate();
    const auto* receiver=getSnapshot({.id=111,.mirrorRootIds={14u}});
    const auto* structural=getSnapshot({.id=112,.mirrorRootIds={14u}});
    ASSERT_NE(receiver,nullptr);ASSERT_NE(structural,nullptr);
    EXPECT_TRUE(receiver->inputInfo.stageTag);EXPECT_TRUE(structural->inputInfo.stageTag);
    EXPECT_FALSE(receiver->nativeOwnedInputBlock.all);EXPECT_FALSE(structural->nativeOwnedInputBlock.all);
    EXPECT_TRUE(receiver->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_TRUE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL));
    EXPECT_FALSE(structural->inputInfo.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_EQ(receiver->nativeStockInputConfig,receiver->inputInfo.inputConfig.get());
    EXPECT_EQ(structural->nativeStockInputConfig,structural->inputInfo.inputConfig.get());
}

} // namespace android::surfaceflinger::frontend
