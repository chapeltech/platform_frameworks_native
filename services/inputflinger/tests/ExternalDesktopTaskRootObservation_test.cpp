// SOURCE ONLY/uncompiled: real join/cohort helper and native SC/Binder objects.
// Identity witnesses below are explicit public doubles, NOT WMS authority,
// RootClaim/Live issuance, stage activation or physical retirement proof.
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <gtest/gtest.h>
#include <binder/Binder.h>
#include <gui/SurfaceComposerClient.h>
#include <limits>
#include <type_traits>
#include "../dispatcher/ExternalDesktopTaskRootObservation.h"
#include "../dispatcher/ExternalDesktopNativeRootFacts.h"
#include "../dispatcher/ExternalDesktopNativeClaims.h"
#include "../dispatcher/ExternalDesktopStageOwner.h"

namespace android::inputdispatcher {
class ExternalDesktopTaskRootObservationTest : public ::testing::Test {
protected:
    void SetUp() override {
        client=sp<SurfaceComposerClient>::make();ASSERT_EQ(client->initCheck(),NO_ERROR);
        source=client->createSurface(String8("root transport source"),0,0,PIXEL_FORMAT_UNKNOWN,ISurfaceComposerClient::eFXSurfaceContainer);
        first=client->mirrorSurface(source.get());second=client->mirrorSurface(source.get());
        p1=client->createSurface(String8("destination one"),0,0,PIXEL_FORMAT_UNKNOWN,ISurfaceComposerClient::eFXSurfaceContainer);
        p2=client->createSurface(String8("destination two"),0,0,PIXEL_FORMAT_UNKNOWN,ISurfaceComposerClient::eFXSurfaceContainer);
        ASSERT_TRUE(source && first && second && p1 && p2);
        frame=std::make_shared<ExternalDesktopStageObjectIdentity>();task=std::make_shared<ExternalDesktopStageObjectIdentity>();
    }
    std::shared_ptr<ExternalDesktopTaskRootObservation> observed(const sp<SurfaceControl>& root,
            const sp<SurfaceControl>& parent,int64_t destination) {
        auto value=std::make_shared<ExternalDesktopTaskRootObservation>();
        value->packet=std::make_shared<ExternalDesktopStageObjectIdentity>();
        value->producer=std::make_shared<ExternalDesktopStageObjectIdentity>();value->frame=frame;
        value->submission=std::make_shared<ExternalDesktopStageObjectIdentity>();value->task=task;
        value->source=source;value->root=root;value->destinationParent=parent;
        value->serial=1;
        value->endpoints={100,100,destination,destination};value->geometry={0,0,10,10,0,0,10,10};
        return value;
    }
    gui::WindowInfo clone(const sp<SurfaceControl>& root,int32_t display,const ExternalDesktopRootOriginal& original) {
        EXPECT_GE(original.uid,0);
        gui::WindowInfo info;info.id=original.windowId;info.ownerUid=gui::Uid{static_cast<uint32_t>(original.uid)};
        info.token=original.channel;info.windowToken=original.client;info.displayId=ui::LogicalDisplayId{display};
        info.cloneLineage=gui::WindowInfo::CloneLineage{static_cast<uint32_t>(original.sourceLayer),{static_cast<uint32_t>(root->getLayerId())}};
        info.inputConfig|=gui::WindowInfo::InputConfig::CLONE;info.inputConfig|=gui::WindowInfo::InputConfig::DROP_INPUT;
        return info;
    }
    sp<SurfaceComposerClient> client;sp<SurfaceControl> source,first,second,p1,p2;
    ExternalDesktopStageObject frame,task;
    const ExternalDesktopStageEpochTicket ticket{17,19,42}; // Public test fixture, NOT a reserved authority.
};

TEST_F(ExternalDesktopTaskRootObservationTest, TwoRealRootsSameTaskChildChannelDifferentDestinationsCoexist) {
    auto a=observed(first,p1,200),b=observed(second,p2,300);
    auto two=prepareExternalDesktopRootBatch(nullptr,{{ticket,a,3,100000,1},{ticket,b,4,100000,1}});ASSERT_TRUE(two);
    EXPECT_EQ(two->current.records.size(),2u);EXPECT_EQ(two->history.records.size(),2u);
    EXPECT_EQ(two->current.records[0].observed->task,two->current.records[1].observed->task);
    const auto channel=sp<BBinder>::make(),windowClient=sp<BBinder>::make();
    ExternalDesktopRootOriginal child{1,2,3,4,source->getLayerId(),77,10152,channel,windowClient};
    gui::WindowInfosUpdate full;full.windowInfos={clone(first,3,child),clone(second,4,child)};
    auto ordinary=full.windowInfos[0];ordinary.cloneLineage.reset();ordinary.displayId=ui::LogicalDisplayId{0};
    full.windowInfos.push_back(ordinary);const auto before=full.windowInfos;
    const auto left=joinExternalDesktopTaskRoot(full,9,first->getLayerId(),3,{child});
    const auto right=joinExternalDesktopTaskRoot(full,9,second->getLayerId(),4,{child});
    EXPECT_EQ(left.state,ExternalDesktopRootJoin::CLONES_OBSERVED);EXPECT_EQ(right.state,ExternalDesktopRootJoin::CLONES_OBSERVED);
    EXPECT_FALSE(left.coherentSourceFacts);EXPECT_FALSE(right.coherentSourceFacts);
    EXPECT_EQ(left.inspected,3u);EXPECT_EQ(right.inspected,3u);EXPECT_EQ(full.windowInfos,before);
    EXPECT_EQ(left.originalLifetimes,right.originalLifetimes); // Shared original, NOT collapsed roots.
    EXPECT_NE(left.cloneRows,right.cloneRows); // Exact distinct SF rows retained for each root.
}

TEST_F(ExternalDesktopTaskRootObservationTest, InvalidLateMemberNeverPublishesPartialCohortOrEvictsHistory) {
    auto a=observed(first,p1,200),b=observed(second,p2,300);
    auto two=prepareExternalDesktopRootBatch(nullptr,{{ticket,a,3,100000,1},{ticket,b,4,100000,1}});ASSERT_TRUE(two);
    auto substituted=observed(first,p1,200); // Same native root, wrong producer witness.
    auto fresh=observed(client->mirrorSurface(source.get()),p2,300);
    EXPECT_FALSE(prepareExternalDesktopRootBatch(&two->history,{{ticket,fresh,4,100000,1},{ticket,substituted,3,100000,1}}));
    // A valid NEW first root plus invalid last root publishes neither prefix.
    EXPECT_EQ(two->current.records.size(),2u);EXPECT_EQ(two->history.records.size(),2u);
    EXPECT_EQ(two->current.records[0].observed,a);EXPECT_EQ(two->current.records[1].observed,b);
    auto wrongFrame=std::make_shared<ExternalDesktopTaskRootObservation>(*a);wrongFrame->frame=std::make_shared<ExternalDesktopStageObjectIdentity>();
    EXPECT_FALSE(prepareExternalDesktopRootBatch(&two->history,{{ticket,b,4,100000,1},{ticket,wrongFrame,3,100000,1}}));
    EXPECT_FALSE(prepareExternalDesktopRootBatch(&two->history,{{ticket,a,3,100000,1},{ticket,a,3,100000,1}}));
    auto duplicate=prepareExternalDesktopRootBatch(&two->history,{{ticket,a,3,200000,1},{ticket,b,4,200000,1}});ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->current.records.size(),2u);EXPECT_EQ(duplicate->current.records[0].expires,100000); // No implicit renewal.
}

TEST_F(ExternalDesktopTaskRootObservationTest, OrderedLineageWrongBindingMissingAndUnknownAncestorFailClosed) {
    const auto channel=sp<BBinder>::make(),windowClient=sp<BBinder>::make();
    ExternalDesktopRootOriginal child{1,2,3,4,source->getLayerId(),77,10152,channel,windowClient};
    gui::WindowInfosUpdate full;full.windowInfos={clone(first,3,child)};
    auto bad=full;bad.windowInfos[0].cloneLineage->mirrorRootIds.push_back(second->getLayerId());
    EXPECT_EQ(joinExternalDesktopTaskRoot(bad,9,first->getLayerId(),3,{child}).state,ExternalDesktopRootJoin::REJECTED);
    bad=full;bad.windowInfos[0].token=sp<BBinder>::make();
    EXPECT_EQ(joinExternalDesktopTaskRoot(bad,9,first->getLayerId(),3,{child}).state,ExternalDesktopRootJoin::REJECTED);
    bad=full;bad.windowInfos.push_back(bad.windowInfos[0]);
    EXPECT_EQ(joinExternalDesktopTaskRoot(bad,9,first->getLayerId(),3,{child}).state,ExternalDesktopRootJoin::REJECTED);
    bad=full;bad.windowInfos[0].cloneLineage->sourceLayerId=p1->getLayerId();
    EXPECT_EQ(joinExternalDesktopTaskRoot(bad,9,first->getLayerId(),3,{child}).state,ExternalDesktopRootJoin::UNRESOLVED_ANCESTOR);
    EXPECT_EQ(joinExternalDesktopTaskRoot({},9,first->getLayerId(),3,{child}).state,ExternalDesktopRootJoin::INCOMPLETE);
    auto negativeUid=child;negativeUid.uid=-1;
    EXPECT_EQ(joinExternalDesktopTaskRoot(full,9,first->getLayerId(),3,{negativeUid}).state,ExternalDesktopRootJoin::REJECTED);
    EXPECT_EQ(full.windowInfos.size(),1u); // No stock-window mutation or retirement.
}

class ExternalDesktopNativeRootFactsTest : public ExternalDesktopTaskRootObservationTest {
protected:
    static auto TEST_DOUBLE_stockScope(ExternalDesktopStageEpochTicket binding,WsKey before,WsKey after) {
        // DATA witness only: these geometry tests do NOT call/qualify the real
        // renewal issuer. Real Context/lender/EpochOwner tests remain separate.
        return ExternalDesktopSuspendedRootStockScope(binding,before,after);
    }
    // Public typed fixtures exercise the real native join. These tokens are
    // NOT evidence that SF created a source lifetime; separate Layer-owner
    // tests exercise that issuer, and full wire integration remains pending.
    std::map<int32_t,gui::LayerSourceLifetime> testLifetimes;
    std::map<int32_t,sp<IBinder>> TEST_DOUBLE_snapshots;
    gui::LayerSourceNode node(const sp<SurfaceControl>& surface, uint32_t parent=UINT32_MAX,
                              uint32_t mirror=UINT32_MAX, bool dropped=false) {
        gui::LayerSourceNode n{}; n.id=static_cast<uint32_t>(surface->getLayerId());
        n.parent=parent; n.relativeParent=n.croppedBy=n.stopAt=UINT32_MAX; n.mirrorSource=mirror;
        n.ownerUid=10152; n.requestedCrop={0,0,10,10}; n.requestedTransform={1,0,0,1,0,0};
        n.requestedDropMode=static_cast<uint32_t>(dropped ? gui::DropInputMode::ALL : gui::DropInputMode::NONE);
        n.handleAlive=true;
        auto& lifetime=testLifetimes[surface->getLayerId()];
        if (!lifetime.identity) { lifetime.identity=sp<BBinder>::make(); lifetime.publishedHandle=surface->getHandle(); }
        n.lifetime=lifetime; return n;
    }
    gui::LayerSourceApplied applied(const gui::WindowInfo& info, bool dropped=false) {
        gui::LayerSourceApplied a{}; a.windowId=info.id; a.displayId=info.displayId.val();
        a.inputConfig=info.inputConfig.get(); a.hasInputToken=info.token!=nullptr;
        a.hasClientToken=info.windowToken!=nullptr; a.hasInputInfo=true; a.snapshotCanReceiveInput=true;
        a.inputFrame={info.frame.left,info.frame.top,info.frame.right,info.frame.bottom};
        a.layerBounds={0,0,10,10}; a.inputTransform=gui::layerSourceInputMatrix(info.transform);
        a.localTransform=a.layerTransform={1,0,0,1,0,0};
        a.composedDropMode=static_cast<uint32_t>(dropped ? gui::DropInputMode::ALL : gui::DropInputMode::NONE);
        auto& snapshot=TEST_DOUBLE_snapshots[info.id];
        if (!snapshot) snapshot=sp<BBinder>::make();
        a.snapshotIdentity=snapshot; // Explicit observation DOUBLE, not an SF-owned snapshot issuer.
        return a;
    }
    gui::WindowInfo structural(const sp<SurfaceControl>& surface, int32_t id) {
        gui::WindowInfo info{}; info.id=id; info.alpha=1;
        info.displayId=ui::LogicalDisplayId{3}; info.frame={0,0,10,10};
        info.inputConfig|=gui::WindowInfo::InputConfig::CLONE;
        info.inputConfig|=gui::WindowInfo::InputConfig::DROP_INPUT;
        info.inputConfig|=gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL;
        info.cloneLineage=gui::WindowInfo::CloneLineage{static_cast<uint32_t>(surface->getLayerId()),
                {static_cast<uint32_t>(first->getLayerId())}};
        return info;
    }
    gui::LayerSourceTraversal edge(const gui::LayerSourceNode& n, const gui::LayerSourceApplied& a,
                                   std::vector<uint32_t> roots={}) {
        gui::LayerSourceTraversal e{}; e.sourceId=n.id; e.mirrorRoots=std::move(roots);
        e.requested=n; e.applied=a; return e;
    }
    struct Packet {
        gui::WindowInfosUpdate full;
        std::vector<ExternalDesktopRootOriginal> originals;
        ExternalDesktopTaskRootRecord record;
        ExternalDesktopTaskRootCohort cohort;
        sp<SurfaceControl> child;
    };
    Packet packet() {
        Packet p; p.child=client->createSurface(String8("source child"),0,0,PIXEL_FORMAT_UNKNOWN,
                ISurfaceComposerClient::eFXSurfaceContainer,source->getHandle());
        EXPECT_TRUE(p.child);
        p.record={ticket,observed(first,p1,200),3,100000,1}; p.cohort.records={p.record};
        p.originals.push_back({1,2,3,4,p.child->getLayerId(),77,10152,sp<BBinder>::make(),sp<BBinder>::make()});
        auto input=clone(first,3,p.originals.front()); input.alpha=1; input.frame={0,0,10,10};
        const auto taskRow=structural(source,78); p.full.windowInfos={input,taskRow};
        gui::DisplayInfo display{}; display.displayId=ui::LogicalDisplayId{3}; p.full.displayInfos={display};
        const auto sourceNode=node(source), childNode=node(p.child,sourceNode.id);
        const auto parentNode=node(p1), rootNode=node(first,parentNode.id,sourceNode.id,true);
        auto rootInfo=structural(first,first->getLayerId()); const auto rootApplied=applied(rootInfo,true);
        auto parentInfo=structural(p1,p1->getLayerId()); const auto parentApplied=applied(parentInfo);
        const auto sourceApplied=applied(taskRow,true); const std::vector<uint32_t> roots{rootNode.id};
        for (size_t i=0;i<p.full.windowInfos.size();++i) {
            gui::LayerSourceInfo f{}; f.rowIndex=i; f.sourceId=i==0 ? childNode.id : sourceNode.id;
            f.mirrorRoots=roots; f.ancestors=i==0 ? std::vector{childNode,sourceNode} : std::vector{sourceNode};
            if (i==0) f.traversal.push_back(edge(childNode,applied(input,true),roots));
            f.traversal.push_back(edge(sourceNode,sourceApplied,roots));
            f.traversal.push_back(edge(rootNode,rootApplied)); f.traversal.push_back(edge(parentNode,parentApplied));
            f.roots.push_back({rootNode,rootApplied}); f.applied=applied(p.full.windowInfos[i],true);
            p.full.layerSources.push_back(std::move(f));
        }
        p.full.layerSourcesComplete=true; return p;
    }
    Packet TEST_DOUBLE_ownedBlockPacket(bool blocked=true,uint64_t revision=1,
            int64_t epoch=0,bool stockAll=false) {
        auto p=packet();const auto origin=sp<BBinder>::make();
        const auto rootId=static_cast<uint32_t>(first->getLayerId());
        const uint32_t clone=static_cast<uint32_t>(gui::WindowInfo::InputConfig::CLONE);
        const uint32_t noChannel=static_cast<uint32_t>(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL);
        const uint32_t drop=static_cast<uint32_t>(gui::WindowInfo::InputConfig::DROP_INPUT);
        const auto projection=[&](gui::LayerSourceApplied& a,uint32_t stock) {
            a.stockProjectionComplete=true;a.stockInputConfig=stock|(stockAll ? drop:0u);
            a.stockDropMode=static_cast<uint32_t>(stockAll ? gui::DropInputMode::ALL:gui::DropInputMode::NONE);
            a.ownedDropAll=blocked;a.ownedInputBlockOrigin=origin;
            a.ownedInputBlockRevision=revision;a.ownedInputBlockEpoch=epoch;
            a.composedDropMode=blocked ? static_cast<uint32_t>(gui::DropInputMode::ALL):a.stockDropMode;
            a.inputConfig=a.stockInputConfig|((blocked && !(stock & noChannel)) ? drop:0u);
        };
        const auto requested=[&](gui::LayerSourceNode& node) {
            if (node.id==rootId) {
                node.ownedInputBlockOrigin=origin;
                node.requestedDropMode=static_cast<uint32_t>(gui::DropInputMode::NONE);
            }
        };
        for (size_t i=0;i<p.full.layerSources.size();++i) {
            auto& f=p.full.layerSources[i];
            projection(f.applied,clone|(i==1 ? noChannel:0u));
            p.full.windowInfos[i].inputConfig=ftl::Flags<gui::WindowInfo::InputConfig>(f.applied.inputConfig);
            for (auto& node:f.ancestors) requested(node);
            for (auto& root:f.roots) {requested(root.requested);projection(root.applied,0);}
            for (auto& e:f.traversal) {
                requested(e.requested);
                if (e.sourceId==rootId) projection(e.applied,0);
                else if (e.sourceId==static_cast<uint32_t>(source->getLayerId())) projection(e.applied,clone|noChannel);
                else if (e.sourceId==f.sourceId) e.applied=f.applied;
            }
        }
        // Origin / metadata are explicit doubles; real SC/handle/path checking
        // below does NOT qualify the private kernel-authenticated SF factory.
        return p;
    }
};

TEST_F(ExternalDesktopNativeRootFactsTest, RenewalStockDataProofNeverBecomesGenericCurrentOrClaim) {
    static_assert(!std::is_convertible_v<ExternalDesktopSuspendedRootStockResult,ExternalDesktopRootJoinReceipt>);
    const auto next=ticket.epoch+1;
    const auto p=TEST_DOUBLE_ownedBlockPacket(true,2,next);
    ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    const auto& root=p.full.layerSources[0].roots[0];
    const std::vector<ExternalDesktopOwnedBlockExpectation> expected{{root.requested.id,
        root.requested.ownedInputBlockOrigin,2,next,true}};
    EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(p.cohort,expected));
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&expected).coherentSourceFacts);
    const auto scope=TEST_DOUBLE_stockScope(ticket,{ticket.generation,7,10,ticket.epoch},
            {ticket.generation,7,10,next});
    EXPECT_TRUE(joinExternalDesktopSuspendedRootStockFacts(p.full,9,p.record,p.cohort,p.originals,expected,scope).exact);
    EXPECT_EQ(p.record.ticket.epoch,ticket.epoch); // neither record nor firstExpires is rewritten
}

TEST_F(ExternalDesktopNativeRootFactsTest, RenewalStockDataProofUsesExactFloatCropAndAllRealGraphChecks) {
    const auto next=ticket.epoch+1;
    const auto p=TEST_DOUBLE_ownedBlockPacket(true,2,next);
    const auto rootId=static_cast<uint32_t>(first->getLayerId());
    const auto& root=p.full.layerSources[0].roots[0];
    const std::vector<ExternalDesktopOwnedBlockExpectation> expected{{root.requested.id,
        root.requested.ownedInputBlockOrigin,2,next,true}};
    const auto scope=TEST_DOUBLE_stockScope(ticket,{ticket.generation,7,10,ticket.epoch},
            {ticket.generation,7,10,next});
    ASSERT_TRUE(joinExternalDesktopSuspendedRootStockFacts(p.full,9,p.record,p.cohort,p.originals,expected,scope).exact);
    for (int defect=0;defect<7;++defect) {
        auto changed=p.full;
        if (defect<3) {
            const float crop=defect==0?0.25f:defect==1?std::numeric_limits<float>::quiet_NaN():
                    std::numeric_limits<float>::infinity();
            const auto alter=[&](gui::LayerSourceNode& node) {if(node.id==rootId)node.requestedCrop[0]=crop;};
            for(auto& facts:changed.layerSources) {
                for(auto& node:facts.ancestors)alter(node);
                for(auto& entry:facts.roots)alter(entry.requested);
                for(auto& edge:facts.traversal)alter(edge.requested);
            }
            if(defect==0)ASSERT_TRUE(gui::layerSourcesValid(changed.layerSources,changed.windowInfos,true));
        }
        if(defect==3)changed.layerSources[0].roots[0].requested.parent++;
        if(defect==4)changed.layerSources[0].roots[0].applied.localTransform[0]=-1;
        if(defect==5)changed.windowInfos[0].windowToken=sp<BBinder>::make();
        if(defect==6)changed.layerSources[0].applied.stockInputConfig^=1u;
        EXPECT_FALSE(joinExternalDesktopSuspendedRootStockFacts(changed,9,p.record,p.cohort,p.originals,expected,scope).exact)
                << "defect=" << defect;
    }
    auto foreign=expected;foreign[0].origin=sp<BBinder>::make();
    EXPECT_FALSE(joinExternalDesktopSuspendedRootStockFacts(p.full,9,p.record,p.cohort,p.originals,foreign,scope).exact);
}

TEST_F(ExternalDesktopNativeRootFactsTest, RenewalStockDataScopeRejectsForeignKeysAndReticketedBindings) {
    const auto next=ticket.epoch+1;
    const auto p=TEST_DOUBLE_ownedBlockPacket(true,2,next);
    const auto& root=p.full.layerSources[0].roots[0];
    const std::vector<ExternalDesktopOwnedBlockExpectation> expected{{root.requested.id,
        root.requested.ownedInputBlockOrigin,2,next,true}};
    const auto scope=TEST_DOUBLE_stockScope(ticket,{ticket.generation,7,10,ticket.epoch},
            {ticket.generation,7,10,next});
    auto reticketed=p.cohort;reticketed.records[0].ticket.epoch=next;
    EXPECT_FALSE(joinExternalDesktopSuspendedRootStockFacts(p.full,9,reticketed.records[0],reticketed,p.originals,expected,scope).exact);
    for(int defect=0;defect<4;++defect) {
        WsKey key{ticket.generation,7,10,next};
        if(defect==0)key.generation++;
        if(defect==1)key.atlas++;
        if(defect==2)key.geometry++;
        if(defect==3)key.epoch=ticket.epoch;
        const auto wrong=TEST_DOUBLE_stockScope(ticket,{ticket.generation,7,10,ticket.epoch},key);
        EXPECT_FALSE(joinExternalDesktopSuspendedRootStockFacts(p.full,9,p.record,p.cohort,p.originals,expected,wrong).exact);
    }
}

TEST_F(ExternalDesktopNativeRootFactsTest, SeparateOwnedOriginRequiresSameActualRootSourceAndAllRowProjections) {
    const auto p=TEST_DOUBLE_ownedBlockPacket();
    ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    const auto joined=joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals);
    EXPECT_EQ(joined.state,ExternalDesktopRootJoin::CLONES_OBSERVED);EXPECT_TRUE(joined.coherentSourceFacts);
    ASSERT_EQ(joined.cloneSnapshots.size(),2u);
    EXPECT_FALSE(p.full.windowInfos[1].inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    auto foreign=p.full;
    foreign.layerSources[0].applied.ownedInputBlockOrigin=sp<BBinder>::make();
    foreign.layerSources[0].traversal.front().applied=foreign.layerSources[0].applied;
    ASSERT_TRUE(gui::layerSourcesValid(foreign.layerSources,foreign.windowInfos,true));
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(foreign,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto wrongHandle=p.full;
    for (auto& f:wrongHandle.layerSources) {
        f.roots[0].requested.lifetime.publishedHandle=second->getHandle();
        for (auto& e:f.traversal) if (e.sourceId==f.roots[0].requested.id)
            e.requested.lifetime.publishedHandle=second->getHandle();
    }
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(wrongHandle,9,p.record,p.cohort,p.originals).coherentSourceFacts);
}

TEST_F(ExternalDesktopNativeRootFactsTest, EnabledOwnedRowsNeedExactFullCohortEpochRevisionAndNeverInitialAdmission) {
    const auto p=TEST_DOUBLE_ownedBlockPacket(false,3,ticket.epoch);
    ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    const auto& root=p.full.layerSources[0].roots[0];
    const std::vector<ExternalDesktopOwnedBlockExpectation> expected{{root.requested.id,
        root.requested.ownedInputBlockOrigin,3,ticket.epoch,false}};
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    const auto joined=joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&expected);
    EXPECT_TRUE(joined.coherentSourceFacts);EXPECT_EQ(joined.cloneSnapshots.size(),2u);
    auto bad=expected;bad[0].revision=2;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&bad).coherentSourceFacts);
    bad=expected;bad[0].epoch++;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&bad).coherentSourceFacts);
    bad=expected;bad[0].origin=sp<BBinder>::make();
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&bad).coherentSourceFacts);
    bad=expected;bad.push_back(bad[0]);
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&bad).coherentSourceFacts);
    bad.clear();
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&bad).coherentSourceFacts);
}

TEST_F(ExternalDesktopNativeRootFactsTest, EnabledOwnedProjectionStillPreservesStockAllAndCannotHideRowRevisionMismatch) {
    const auto p=TEST_DOUBLE_ownedBlockPacket(false,3,ticket.epoch,true);
    ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    const auto& root=p.full.layerSources[0].roots[0];
    const std::vector<ExternalDesktopOwnedBlockExpectation> expected{{root.requested.id,
        root.requested.ownedInputBlockOrigin,3,ticket.epoch,false}};
    EXPECT_TRUE(joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals,&expected).coherentSourceFacts);
    EXPECT_TRUE(p.full.windowInfos[0].inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
    EXPECT_FALSE(p.full.layerSources[0].applied.ownedDropAll);
    auto changed=p.full;changed.layerSources[1].applied.ownedInputBlockRevision++;
    changed.layerSources[1].traversal.front().applied=changed.layerSources[1].applied;
    ASSERT_TRUE(gui::layerSourcesValid(changed.layerSources,changed.windowInfos,true));
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(changed,9,p.record,p.cohort,p.originals,&expected).coherentSourceFacts);
}

TEST_F(ExternalDesktopNativeRootFactsTest, SeparateOwnedOriginDoesNotMakeMissingStockOrForgedGraphAvailable) {
    const auto p=TEST_DOUBLE_ownedBlockPacket();auto missing=p.full;
    missing.layerSources[0].applied.stockProjectionComplete=false;
    missing.layerSources[0].traversal.front().applied=missing.layerSources[0].applied;
    EXPECT_FALSE(gui::layerSourcesValid(missing.layerSources,missing.windowInfos,true));
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(missing,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto reused=p.full;
    const auto origin=reused.layerSources[0].roots[0].requested.ownedInputBlockOrigin;
    const auto child=static_cast<uint32_t>(p.child->getLayerId());
    for (auto& f:reused.layerSources) {
        for (auto& node:f.ancestors) if (node.id==child) node.ownedInputBlockOrigin=origin;
        for (auto& edge:f.traversal) if (edge.sourceId==child) edge.requested.ownedInputBlockOrigin=origin;
    }
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(reused,9,p.record,p.cohort,p.originals).coherentSourceFacts);
}

TEST_F(ExternalDesktopNativeRootFactsTest, OwnedTaskStructuralRowsJoinWithoutInventedChannelsOrResourceRetention) {
    const auto p=packet(); ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    const auto joined=joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals);
    EXPECT_EQ(joined.state,ExternalDesktopRootJoin::CLONES_OBSERVED); EXPECT_EQ(joined.inspected,2u);
    EXPECT_TRUE(joined.coherentSourceFacts);
    EXPECT_EQ(joined.matched,1u); ASSERT_EQ(joined.structuralSources.size(),1u);
    EXPECT_EQ(joined.structuralSources.front().row,1u);
    EXPECT_EQ(joined.structuralSources.front().lifetime,p.full.layerSources[1].ancestors.front().lifetime.identity);
    EXPECT_EQ(joined.structuralSources.front().existingHandle.promote(),source->getHandle());
    EXPECT_EQ(joined.structuralSources.front().snapshotIdentity,p.full.layerSources[1].applied.snapshotIdentity);
    ASSERT_EQ(joined.cloneSnapshots.size(),2u);
    EXPECT_EQ(joined.cloneSnapshots[1].snapshot,p.full.layerSources[1].applied.snapshotIdentity);
    EXPECT_EQ(joined.cloneSnapshots[1].actualParents.size(),p.full.layerSources[1].traversal.size());
    EXPECT_EQ(p.full.windowInfos[1].token,nullptr); EXPECT_EQ(p.full.windowInfos[1].windowToken,nullptr);
}

TEST_F(ExternalDesktopNativeRootFactsTest, WrongHandleGeometryStockAndChannelBearingAncestorsRefuse) {
    const auto p=packet();
    auto bad=p.full;
    for (auto& f:bad.layerSources) {
        f.roots[0].applied.layerTransform[4]=1;
        for (auto& e:f.traversal) if (e.sourceId==f.roots[0].requested.id) e.applied=f.roots[0].applied;
    }
    EXPECT_EQ(joinExternalDesktopTaskRootFacts(bad,9,p.record,p.cohort,p.originals).state,ExternalDesktopRootJoin::REJECTED);
    bad=p.full; bad.windowInfos[1].token=sp<BBinder>::make(); bad.layerSources[1].applied.hasInputToken=true;
    EXPECT_EQ(joinExternalDesktopTaskRootFacts(bad,9,p.record,p.cohort,p.originals).state,ExternalDesktopRootJoin::UNRESOLVED_ANCESTOR);
    bad=p.full; bad.windowInfos[1].inputConfig|=gui::WindowInfo::InputConfig::TRUSTED_OVERLAY;
    bad.layerSources[1].applied.inputConfig=bad.windowInfos[1].inputConfig.get();
    EXPECT_EQ(joinExternalDesktopTaskRootFacts(bad,9,p.record,p.cohort,p.originals).state,ExternalDesktopRootJoin::REJECTED);
    bad=p.full;
    for (auto& f:bad.layerSources) {
        f.roots[0].requested.lifetime.publishedHandle=second->getHandle();
        for (auto& e:f.traversal) if (e.sourceId==f.roots[0].requested.id)
            e.requested.lifetime.publishedHandle=second->getHandle();
    }
    EXPECT_EQ(joinExternalDesktopTaskRootFacts(bad,9,p.record,p.cohort,p.originals).state,ExternalDesktopRootJoin::REJECTED);
    EXPECT_EQ(p.full.windowInfos.size(),2u); // No stock/drop/region or history mutation.
}

TEST_F(ExternalDesktopNativeRootFactsTest, EqualBoundingBoxesDoNotAuthorizeReflectedOrRotatedRootPixels) {
    const auto p=packet();const auto& original=p.full.layerSources[0].roots[0];
    ASSERT_TRUE(externalDesktopRootAppliedTarget(original,p.record,p.full.displayInfos));
    auto reflected=original;reflected.applied.layerTransform={-1,0,0,1,10,0};
    EXPECT_FALSE(externalDesktopRootAppliedTarget(reflected,p.record,p.full.displayInfos));
    auto rotated=original;rotated.applied.layerTransform={0,-1,1,0,10,0};
    EXPECT_FALSE(externalDesktopRootAppliedTarget(rotated,p.record,p.full.displayInfos));
    EXPECT_TRUE(externalDesktopRootAppliedTarget(original,p.record,p.full.displayInfos));
}

TEST_F(ExternalDesktopTaskRootObservationTest, StablePhysicalRootAcceptsNewBindingWithoutGrowingHistory) {
    auto a=observed(first,p1,200);
    auto original=prepareExternalDesktopRootBatch(nullptr,{{ticket,a,3,100000,1,0,10152}}); ASSERT_TRUE(original);
    ASSERT_EQ(original->history.records.size(),1u);
    EXPECT_FALSE(original->history.records[0].observed);
    ASSERT_TRUE(original->history.records[0].physical);
    auto next=std::make_shared<ExternalDesktopTaskRootObservation>(*a);
    next->frame=std::make_shared<ExternalDesktopStageObjectIdentity>();
    next->submission=std::make_shared<ExternalDesktopStageObjectIdentity>();next->serial=2;
    next->geometry={0,0,10,10,20,30,30,40};
    const ExternalDesktopStageEpochTicket newer{ticket.owner,ticket.epoch+1,ticket.generation};
    auto moved=prepareExternalDesktopRootBatch(&original->history,{{newer,next,3,200000,2,0,10152}});
    ASSERT_TRUE(moved);EXPECT_EQ(moved->history.records.size(),1u);
    EXPECT_EQ(moved->current.records[0].observed,next);
    EXPECT_EQ(moved->current.records[0].expires,200000); // Existing lease postcheck remains required.
    EXPECT_EQ(moved->history.records[0].physical,original->history.records[0].physical);
    EXPECT_EQ(original->current.records[0].observed,a); // Stale/current witnesses are independent.
    auto wrongScope=ExternalDesktopTaskRootRecord{newer,next,3,200000,2,0,10153};
    EXPECT_FALSE(prepareExternalDesktopRootBatch(&original->history,{wrongScope}));
    wrongScope.clientUid=10152;wrongScope.ticket.generation++;
    EXPECT_FALSE(prepareExternalDesktopRootBatch(&original->history,{wrongScope}));
    EXPECT_EQ(original->history.records.size(),1u);
}

TEST_F(ExternalDesktopTaskRootObservationTest, SourceReplacementPreservesLogicalProducerButHistoryNeverPinsResourcePacket) {
    auto a=observed(first,p1,200);
    auto prior=prepareExternalDesktopRootBatch(nullptr,{{ticket,a,3,100000,1}});ASSERT_TRUE(prior);
    auto replacementSource=client->createSurface(String8("replacement source"),0,0,PIXEL_FORMAT_UNKNOWN,
            ISurfaceComposerClient::eFXSurfaceContainer);
    ASSERT_TRUE(replacementSource);
    auto replacement=std::make_shared<ExternalDesktopTaskRootObservation>(*a);
    replacement->source=replacementSource;replacement->root=client->mirrorSurface(replacementSource.get());
    replacement->frame=std::make_shared<ExternalDesktopStageObjectIdentity>();
    replacement->submission=std::make_shared<ExternalDesktopStageObjectIdentity>();replacement->serial=2;
    const ExternalDesktopStageEpochTicket newer{ticket.owner,ticket.epoch+1,ticket.generation};
    auto next=prepareExternalDesktopRootBatch(&prior->history,{{newer,replacement,3,200000,2}});ASSERT_TRUE(next);
    EXPECT_EQ(next->history.records.size(),2u);
    EXPECT_EQ(next->history.records[0].physical->producer,next->history.records[1].physical->producer);
    EXPECT_EQ(next->history.records[0].physical->task,next->history.records[1].physical->task);
    std::weak_ptr<const ExternalDesktopTaskRootObservation> oldPacket=a;
    prior->current.records.clear();a.reset();EXPECT_TRUE(oldPacket.expired());
    for (const auto& record:next->history.records) { EXPECT_FALSE(record.observed);EXPECT_TRUE(record.physical); }
    auto empty=prepareExternalDesktopRootBatch(&next->history,{});ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->current.records.empty());EXPECT_EQ(empty->history.records.size(),2u);
    // Clearing CURRENT is resource disposal, NOT physical retirement or a claim/history release.
}

TEST_F(ExternalDesktopNativeRootFactsTest, RecreatedSnapshotAndDuplicateSnapshotHaveDistinctBoundedObservationSemantics) {
    const auto p=packet();
    const auto firstJoin=joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals);
    ASSERT_TRUE(firstJoin.coherentSourceFacts);
    auto mismatched=p.full;
    mismatched.layerSources[1].applied.snapshotIdentity=sp<BBinder>::make();
    EXPECT_FALSE(gui::layerSourcesValid(mismatched.layerSources,mismatched.windowInfos,true));
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(mismatched,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto recreated=p.full;auto& fact=recreated.layerSources[1];
    fact.applied.snapshotIdentity=sp<BBinder>::make();
    fact.traversal.front().applied.snapshotIdentity=fact.applied.snapshotIdentity;
    const auto nextJoin=joinExternalDesktopTaskRootFacts(recreated,10,p.record,p.cohort,p.originals);
    ASSERT_TRUE(nextJoin.coherentSourceFacts);
    EXPECT_EQ(nextJoin.structuralSources[0].lifetime,firstJoin.structuralSources[0].lifetime);
    EXPECT_NE(nextJoin.structuralSources[0].snapshotIdentity,firstJoin.structuralSources[0].snapshotIdentity);
    // Native lifetime allocation must refuse recycled WindowInfo IDs until
    // explicit destruction/producer/tail joins exist; this observation is no release proof.
    auto duplicate=p.full;duplicate.layerSources[1].applied.snapshotIdentity=
            duplicate.layerSources[0].applied.snapshotIdentity;
    duplicate.layerSources[1].traversal.front().applied.snapshotIdentity=
            duplicate.layerSources[1].applied.snapshotIdentity;
    const auto rejected=joinExternalDesktopTaskRootFacts(duplicate,9,p.record,p.cohort,p.originals);
    EXPECT_EQ(rejected.state,ExternalDesktopRootJoin::REJECTED);EXPECT_FALSE(rejected.coherentSourceFacts);
}
// Actual native issuer/storage and same Rust codec/predicate. SF snapshot and
// WMS lease objects are EXPLICIT test doubles; no real authority/activation,
// compositor producer qualification, FD death or retirement is inferred.
class ExternalDesktopNativeClaimFactoryTest : public ExternalDesktopNativeRootFactsTest {
protected:
    using TaggedPhase=ExternalDesktopNativeClaims::TaggedPlan::Phase;
    class References final : public ExternalDesktopStageRosterReferences {
        bool same(const ExternalDesktopStageRosterReferences& other) const override {return this==&other;}
        const void* implementation() const override {return this;}
    };
    void SetUp() override {
        ExternalDesktopNativeRootFactsTest::SetUp();
        scene=packet();scene.record.clientUid=10152;
        scene.cohort.records={scene.record};
        // Use real signed clone WindowInfo IDs, distinct from actual source
        // layer IDs; the source original MUST NOT supply the clone lifetime.
        for (size_t i=0;i<scene.full.windowInfos.size();++i) {
            scene.full.windowInfos[i].id=-700-static_cast<int32_t>(i);
            scene.full.layerSources[i].applied.windowId=scene.full.windowInfos[i].id;
            scene.full.layerSources[i].traversal.front().applied.windowId=scene.full.windowInfos[i].id;
        }
        auto current=std::make_shared<ExternalDesktopStageRoster>();
        current->references=std::make_shared<References>();
        current->nativeOwnerIdentity=std::make_shared<ExternalDesktopStageObjectIdentity>();
        current->tasks.push_back({100,10152,0,1,source,source->getLayerId(),100,task});
        current->windows.push_back({scene.child,scene.originals[0].channel,scene.originals[0].client,
                scene.child->getLayerId(),1,1,0,0,63,task,std::make_shared<ExternalDesktopStageObjectIdentity>()});
        auto observed=std::make_shared<ExternalDesktopTaskRootObservation>(*scene.record.observed);
        observed->roster=current;scene.record.observed=observed;scene.cohort.records={scene.record};
        gui::WindowInfo original=scene.full.windowInfos[0];
        original.id=scene.child->getLayerId();original.cloneLineage.reset();
        original.displayId=ui::LogicalDisplayId{2};original.inputConfig.clear(gui::WindowInfo::InputConfig::CLONE);
        original.inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);
        gui::LayerSourceInfo facts{};facts.rowIndex=2;facts.sourceId=static_cast<uint32_t>(original.id);
        facts.applied=applied(original);
        facts.ancestors={node(scene.child,static_cast<uint32_t>(source->getLayerId())),node(source)};
        facts.traversal={edge(facts.ancestors[0],facts.applied)};
        scene.full.windowInfos.push_back(original);scene.full.layerSources.push_back(facts);
        view.user=0;view.expected_client_uid=10152;view.generation=42;view.observed_at=1;
        view.topology_snapshot={topology.data(),topology.size()};view.expires_by_display={expiry.data(),expiry.size()};
        auto prepared=storage.prepare(view,ticket,*current);ASSERT_TRUE(prepared);
        ASSERT_TRUE(storage.commit(std::move(*prepared)));
        storage.observeForTest(scene.full,1,1);
        ASSERT_TRUE(storage.current());ASSERT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
        joinStructural();
    }
    void joinStructural() {
        const auto originals=storage.originalsForRoot(ticket,task,source,storage.current()->batchSequence);
        ASSERT_TRUE(originals);
        std::vector joins{joinExternalDesktopTaskRootFacts(scene.full,storage.current()->batchSequence,
                scene.record,scene.cohort,*originals)};
        ASSERT_TRUE(joins[0].coherentSourceFacts);
        auto prepared=storage.prepareStructural(joins);ASSERT_TRUE(prepared);
        ASSERT_TRUE(storage.commitStructural(std::move(*prepared)));
    }
    auto prepare() {return registry.prepare(view,scene.cohort,scene.full,storage.current()->batchSequence,storage);}
    // Explicit metadata DOUBLE over the SAME real source/root/roster objects.
    // These calls do not invoke the SF primitive or qualify an enabled device.
    void TEST_DOUBLE_ownedState(bool blocked,uint64_t revision,int64_t epoch) {
        if (!TEST_DOUBLE_origin) TEST_DOUBLE_origin=sp<BBinder>::make();
        const auto rootId=static_cast<uint32_t>(first->getLayerId());
        const uint32_t clone=static_cast<uint32_t>(gui::WindowInfo::InputConfig::CLONE);
        const uint32_t noChannel=static_cast<uint32_t>(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL);
        const uint32_t drop=static_cast<uint32_t>(gui::WindowInfo::InputConfig::DROP_INPUT);
        const auto project=[&](gui::LayerSourceApplied& a,uint32_t stock) {
            a.stockProjectionComplete=true;a.stockInputConfig=stock;
            a.stockDropMode=static_cast<uint32_t>(gui::DropInputMode::NONE);
            a.ownedDropAll=blocked;a.ownedInputBlockOrigin=TEST_DOUBLE_origin;
            a.ownedInputBlockRevision=revision;a.ownedInputBlockEpoch=epoch;
            a.composedDropMode=static_cast<uint32_t>(blocked ? gui::DropInputMode::ALL:gui::DropInputMode::NONE);
            a.inputConfig=stock|((blocked && !(stock&noChannel)) ? drop:0u);
        };
        const auto requested=[&](gui::LayerSourceNode& node) {
            if (node.id==rootId) {node.ownedInputBlockOrigin=TEST_DOUBLE_origin;
                node.requestedDropMode=static_cast<uint32_t>(gui::DropInputMode::NONE);}
        };
        for (size_t i=0;i<scene.full.layerSources.size();++i) {
            auto& f=scene.full.layerSources[i];if (f.mirrorRoots.empty()) continue;
            const uint32_t stock=clone|(!scene.full.windowInfos[i].token ? noChannel:0u);
            project(f.applied,stock);
            scene.full.windowInfos[i].inputConfig=ftl::Flags<gui::WindowInfo::InputConfig>(f.applied.inputConfig);
            for (auto& node:f.ancestors) requested(node);
            for (auto& root:f.roots) {requested(root.requested);project(root.applied,0);}
            for (auto& e:f.traversal) {
                requested(e.requested);
                if (e.sourceId==rootId) project(e.applied,0);
                else if (e.sourceId==static_cast<uint32_t>(source->getLayerId())) project(e.applied,clone|noChannel);
                else if (e.sourceId==f.sourceId) e.applied=f.applied;
            }
        }
    }
    std::vector<ExternalDesktopOwnedBlockExpectation> TEST_DOUBLE_expected(bool blocked,uint64_t revision) const {
        return {{static_cast<uint32_t>(first->getLayerId()),TEST_DOUBLE_origin,revision,ticket.epoch,blocked}};
    }
    bool TEST_DOUBLE_observeExpected(uint64_t sequence,
            const std::vector<ExternalDesktopOwnedBlockExpectation>* expected=nullptr) {
        storage.observe(scene.full,sequence,1,view.observed_at); // Equal-ms distinct actual fixture callbacks.
        if (!storage.current() || storage.current()->join!=ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED) return false;
        const auto originals=storage.originalsForRoot(ticket,task,source,sequence);if (!originals) return false;
        std::vector joins{joinExternalDesktopTaskRootFacts(scene.full,sequence,scene.record,scene.cohort,*originals,expected)};
        if (!joins[0].coherentSourceFacts) return false;
        auto candidate=storage.prepareStructural(joins,!expected);
        return candidate && storage.commitStructural(std::move(*candidate));
    }
    auto TEST_DOUBLE_prepareExpected(const std::vector<ExternalDesktopOwnedBlockExpectation>& expected) {
        return registry.prepare(view,scene.cohort,scene.full,storage.current()->batchSequence,storage,&expected);
    }
    // ONLY fixture code bypasses the real Rust pending-result owner. This
    // exercises registry/real-SC joins, not the authority or SF tag issuer.
    auto TEST_DOUBLE_pendingPlan(int64_t atlas=7,int64_t geometry=10) {
        std::vector<WsReplica> replicas;
        std::vector<WsBinding> bindings;
        std::vector<WsStructuralBinding> structurals;
        const auto values=rows();
        std::vector<std::vector<WsRect>> regions(current()->rows.size());
        const auto root=kind(values,WS_ROOT_CLAIM);
        const auto& g=scene.record.observed->geometry;
        replicas.push_back({77,100,100,200,{g[0],g[1],g[2],g[3]},{g[4],g[5],g[6],g[7]},
                0,0,root.source,root.destination,root.roots,root.key});
        size_t index=0;
        for (const auto& row:current()->rows) {
            auto& region=regions[index++];
            if (!row.touchable.isEmpty()) for (auto r:row.touchable)
                region.push_back({r.left,r.top,r.right,r.bottom});
            const auto claim=std::find_if(values.begin(),values.end(),[&](auto c){return c.key==row.claim;});
            const auto& a=row.facts.applied;
            const WsStock stock{a.stockInputConfig,a.stockDropMode,0};
            const WsRect crop{a.inputFrame[0],a.inputFrame[1],a.inputFrame[2],a.inputFrame[3]};
            if (claim->kind==WS_WINDOW_CLAIM) {
                const auto originals=storage.originalsForRoot(ticket,task,source,current()->batch);
                const auto o=std::find_if(originals->begin(),originals->end(),[&](auto original){return original.channelKey==claim->channel;});
                WsBinding b{};b.child=o->child;b.presentation=77;b.crop=crop;b.stock=stock;
                std::copy(a.inputTransform.begin(),a.inputTransform.end(),b.transform);
                b.region={region.data(),region.size()};bindings.push_back(b);
            } else {
                WsStructuralBinding b{};b.claim=row.claim;b.presentation=77;b.crop=crop;b.stock=stock;
                b.snapshot_can_receive_input=a.snapshotCanReceiveInput;
                std::copy(a.inputTransform.begin(),a.inputTransform.end(),b.transform);
                b.region={region.data(),region.size()};structurals.push_back(b);
            }
        }
        WsStage stage{};stage.key={ticket.generation,atlas,geometry,ticket.epoch};
        stage.expires_at=current()->expiresAt;
        stage.replicas={replicas.data(),replicas.size()};stage.bindings={bindings.data(),bindings.size()};
        stage.structural_bindings={structurals.data(),structurals.size()};
        WsLive live{};live.now=view.observed_at;live.registry_revision=registry.mRevision;
        return registry.copyTaggedPlan(stage,live); // All local spans die here.
    }
    void TEST_DOUBLE_taggedState(const ExternalDesktopNativeClaims::TaggedPlan& plan) {
        TEST_DOUBLE_ownedState(plan.expected[0].blocked,plan.expected[0].revision,ticket.epoch);
        for (auto& info:scene.full.windowInfos) if (info.cloneLineage) info.stageTag=plan.expected[0].tag;
    }
    auto TEST_DOUBLE_prepareTagged(const ExternalDesktopNativeClaims::TaggedPlan& plan) {
        return registry.prepareTagged(view,scene.cohort,scene.full,storage.current()->batchSequence,storage,plan);
    }
    auto TEST_DOUBLE_prepareStillDropped(const ExternalDesktopNativeClaims::TaggedPlan& plan) {
        return registry.prepareStillDropped(view,scene.cohort,scene.full,storage.current()->batchSequence,storage,plan);
    }
    void TEST_DOUBLE_forgetScope() {registry.invalidateScope();}
    auto TEST_DOUBLE_parked() const {return registry.mParked;}
    bool publish(ExternalDesktopNativeClaims::Prepared&& value) {return registry.publish(std::move(value),storage,view.observed_at);}
    bool publishExpired(ExternalDesktopNativeClaims::Prepared&& value) {return registry.publish(std::move(value),storage,100000);}
    std::vector<WsClaim> rows() const {return registry.views();}
    bool available() const {return registry.mCurrent.has_value();}
    std::optional<ExternalDesktopNativeClaims::Current> current() const {return registry.mCurrent;}
    void invalidate() {registry.invalidate();}
    void invalidate(ExternalDesktopStageEpochTicket value) {registry.invalidate(value);}
    size_t historyBytes() const {return registry.mHistoryBytes;}
    void TEST_DOUBLE_historyBudgetFull() {registry.mHistoryBytes=ExternalDesktopNativeClaims::kMaxHistoryBytes;}
    void TEST_DOUBLE_restoreBudget(size_t value) {registry.mHistoryBytes=value;}
    bool stopped() const {return registry.mIssuanceStopped;}
    uint64_t highClaim() const {return registry.mHigh;}
    void TEST_DOUBLE_completeOrdinaryStock() {
        // Explicit same-pass SF metadata DOUBLE, no stock/owned subtraction.
        for (auto& facts:scene.full.layerSources) if (!facts.applied.ownedInputBlockOrigin) {
            facts.applied.stockProjectionComplete=true;
            facts.applied.stockInputConfig=facts.applied.inputConfig;
            facts.applied.stockDropMode=facts.applied.composedDropMode;
            facts.traversal.front().applied=facts.applied;
        }
    }
    void TEST_DOUBLE_addOrdinary(int32_t display) {
        auto surface=client->createSurface(String8("ordinary projection source"),0,0,
            PIXEL_FORMAT_UNKNOWN,ISurfaceComposerClient::eFXSurfaceContainer);
        ASSERT_TRUE(surface);TEST_DOUBLE_ordinarySurfaces.push_back(surface);
        auto info=scene.full.windowInfos.back();info.id=surface->getLayerId();
        info.displayId=ui::LogicalDisplayId{display};info.cloneLineage.reset();info.stageTag.reset();
        info.token=sp<BBinder>::make();info.windowToken=sp<BBinder>::make();
        info.inputConfig.clear(gui::WindowInfo::InputConfig::CLONE);
        info.inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);
        gui::LayerSourceInfo facts{};facts.rowIndex=scene.full.windowInfos.size();facts.sourceId=info.id;
        facts.applied=applied(info);facts.ancestors={node(surface)};
        facts.traversal={edge(facts.ancestors[0],facts.applied)};
        scene.full.windowInfos.push_back(info);scene.full.layerSources.push_back(facts);
    }
    auto TEST_DOUBLE_project() {
        owner.mCompleteBatch=scene.full;owner.mReceipt.hasCompleteBatch=true;
        owner.mReceipt.observations=storage.current()->batchSequence;
        owner.mReceipt.nativeObservedAt=storage.current()->observedAt;
        ExternalDesktopStageOwner::NativeBatch result;
        if (!registry.mCurrent || !owner.projectNativeBatch(*registry.mCurrent,result))
            return std::optional<ExternalDesktopStageOwner::NativeBatch>{};
        return std::optional<ExternalDesktopStageOwner::NativeBatch>{std::move(result)};
    }
    bool TEST_DOUBLE_matches(const WsBatch& batch) {
        return registry.mCurrent && owner.matchesNativeBatch(batch,*registry.mCurrent);
    }
    struct AssemblyReadback {
        WsKey key{};
        std::vector<int64_t> original,desired,actual;
        std::vector<uint64_t> children;
        size_t raw=0,windows=0,structurals=0;
    };
    // Explicit Context-issued-output/intent-value DOUBLE for assembly ONLY.
    // The genuine lender/handle/geometry admission is tested separately by the
    // actual C++->Rust gate. This does NOT submit/authorize this Android fixture.
    auto TEST_DOUBLE_buildAssembly(bool superseding) {
        owner.mWmsRosterTicket=ticket;owner.mNativeReservation=ticket;
        owner.mNativeContextTicket=ExternalDesktopStageEpochTicket{ticket.owner,ticket.epoch-1,ticket.generation};
        owner.mNativeIntent=1;owner.mNativeIntentFrame.emplace();
        auto& intent=*owner.mNativeIntentFrame;
        intent.atlas=7;intent.expiresAt=100000;intent.uid=10152;intent.user=0;
        intent.current.assign(topology.begin(),topology.end());intent.current[4]=1;
        const std::array<int64_t,12> taskRow{100,100,0,0,10,10,1,1,1,1,0,0};
        intent.current.insert(intent.current.end(),taskRow.begin(),taskRow.end());
        intent.desired={42,10,2,100,0,1,0,100,100,0,0,10,10,1,0,0};
        intent.origins={{100,0,0},{200,0,0}};
        const auto& g=scene.record.observed->geometry;
        intent.presentations={{100,100,200,{g[0],g[1],g[2],g[3]},{g[4],g[5],g[6],g[7]},0,0}};
        auto active=TEST_DOUBLE_pendingPlan();
        if (!active) return std::optional<AssemblyReadback>{};
        active->key.epoch=ticket.epoch-1;owner.mActiveTagged=std::move(*active);
        owner.mNativeSuperseding=superseding;
        owner.mWmsRoots=std::make_shared<const ExternalDesktopTaskRootCohort>(scene.cohort);
        owner.mCompleteBatch=scene.full;owner.mReceipt.hasCompleteBatch=true;
        owner.mReceipt.observations=storage.current()->batchSequence;
        owner.mReceipt.nativeObservedAt=storage.current()->observedAt;
        auto observed=intent.current;observed[1]=11;
        WsLive live{};live.uid=view.expected_client_uid;live.user=view.user;live.now=view.observed_at;
        std::copy(std::begin(view.signer),std::end(view.signer),live.signer);
        live.environment_snapshot=view.topology_snapshot;live.current={observed.data(),observed.size()};
        ExternalDesktopStageOwner::NativeEpoch epoch;
        if (!registry.mCurrent || !owner.buildNativeEpoch(view,live,*registry.mCurrent,epoch))
            return std::optional<AssemblyReadback>{};
        AssemblyReadback result;result.key=epoch.stage.key;
        result.original.assign(epoch.stage.current.data,epoch.stage.current.data+epoch.stage.current.len);
        result.desired.assign(epoch.stage.desired.data,epoch.stage.desired.data+epoch.stage.desired.len);
        result.actual.assign(epoch.live.current.data,epoch.live.current.data+epoch.live.current.len);
        for (const auto& child:epoch.children) result.children.push_back(child.child);
        result.raw=epoch.batch.rawOrder.size();result.windows=epoch.batch.windows.size();
        result.structurals=epoch.batch.structurals.size();
        return std::optional<AssemblyReadback>{std::move(result)};
    }
    WsClaim kind(const std::vector<WsClaim>& values,uint32_t wanted) {
        const auto found=std::find_if(values.begin(),values.end(),[&](const auto& row){return row.kind==wanted;});
        EXPECT_NE(found,values.end());return found==values.end()?WsClaim{}:*found;
    }
    Packet scene;
    ExternalDesktopStageOwner owner; // actual production owner/projection, not a parallel model
    ExternalDesktopSourceLifetimes& storage=owner.mSourceLifetimes;
    ExternalDesktopNativeClaims& registry=owner.mNativeClaims;
    std::vector<sp<SurfaceControl>> TEST_DOUBLE_ordinarySurfaces;
    ExternalDesktopStageLeaseView view{};
    sp<IBinder> TEST_DOUBLE_origin;
    const std::array<int64_t,23> topology{42,10,2,2,0,100,2,0,0,1024,768,160,1,42,
                                                 200,3,0,0,1024,768,160,1,42};
    const std::array<int64_t,2> expiry{100000,100000};
};

TEST_F(ExternalDesktopNativeClaimFactoryTest, FullProjectionKeepsOrdinaryPhoneSystemAndIssuedOriginalIds) {
    TEST_DOUBLE_addOrdinary(0);TEST_DOUBLE_addOrdinary(3);TEST_DOUBLE_completeOrdinaryStock();
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto projected=TEST_DOUBLE_project();ASSERT_TRUE(projected);
    ASSERT_EQ(projected->rawOrder.size(),scene.full.windowInfos.size());
    ASSERT_EQ(projected->windows.size()+projected->structurals.size(),scene.full.windowInfos.size());
    EXPECT_TRUE(TEST_DOUBLE_matches(projected->view));
    for (size_t i=0;i<projected->rawOrder.size();++i) {
        const auto [structural,index]=projected->rawOrder[i];
        const auto id=structural?projected->structurals[index].identity.id:projected->windows[index].identity.id;
        EXPECT_EQ(id,scene.full.windowInfos[i].id);
    }
    const auto original=std::find_if(projected->windows.begin(),projected->windows.end(),[&](const auto& r) {
        return r.identity.id==scene.child->getLayerId();
    });
    ASSERT_NE(original,projected->windows.end());
    EXPECT_GT(original->identity.lifetime,0u);EXPECT_GT(original->identity.channel,0u);EXPECT_GT(original->identity.client,0u);
    for (const auto& surface:TEST_DOUBLE_ordinarySurfaces) {
        const auto row=std::find_if(projected->windows.begin(),projected->windows.end(),[&](const auto& r) {
            return r.identity.id==surface->getLayerId();
        });
        ASSERT_NE(row,projected->windows.end());EXPECT_EQ(row->identity.lifetime,0u);
        EXPECT_EQ(row->identity.channel,0u);EXPECT_EQ(row->identity.client,0u);
    }
    auto omitted=projected->view;--omitted.windows.len;EXPECT_FALSE(TEST_DOUBLE_matches(omitted));
    projected->windows.back().transform[4]+=1;EXPECT_FALSE(TEST_DOUBLE_matches(projected->view));
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, FullProjectionRejectsForeignTagAndUnknownOwnedRawRowWithoutFiltering) {
    TEST_DOUBLE_addOrdinary(0);TEST_DOUBLE_completeOrdinaryStock();
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    const auto high=storage.highKey(),claims=highClaim();
    ASSERT_TRUE(TEST_DOUBLE_project());
    scene.full.windowInfos.back().stageTag=gui::WindowInfo::StageTag{999,1,1,1,1};
    ASSERT_TRUE(gui::layerSourcesValid(scene.full.layerSources,scene.full.windowInfos,true));
    EXPECT_FALSE(TEST_DOUBLE_project());scene.full.windowInfos.back().stageTag.reset();
    auto info=scene.full.windowInfos[0];info.id=-999;
    auto facts=scene.full.layerSources[0];facts.rowIndex=scene.full.windowInfos.size();
    facts.applied.windowId=info.id;facts.applied.snapshotIdentity=sp<BBinder>::make();
    facts.traversal.front().applied=facts.applied;
    scene.full.windowInfos.push_back(info);scene.full.layerSources.push_back(facts);
    ASSERT_TRUE(gui::layerSourcesValid(scene.full.layerSources,scene.full.windowInfos,true));
    EXPECT_FALSE(TEST_DOUBLE_project()); // real full batch, no silently filtered owned row
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claims);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, ZeroAtlasAndGeometryUseSameCoreNonnegativeContractWithoutChangingEpoch) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan(0,0);ASSERT_TRUE(plan);
    EXPECT_EQ(plan->key.atlas,0);EXPECT_EQ(plan->key.geometry,0);EXPECT_EQ(plan->key.epoch,ticket.epoch);
    EXPECT_TRUE(externalDesktopOwnedBlockCohortValid(scene.cohort,plan->expected));
    auto bad=plan->expected;bad[0].tag->geometryRevision=-1;
    EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(scene.cohort,bad));
    bad=plan->expected;bad[0].tag->atlasRevision=-1;
    EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(scene.cohort,bad));
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, SupersedingAssemblyUsesReservedEpochAndActualRevisionNotPriorActiveOrBase) {
    TEST_DOUBLE_completeOrdinaryStock();TEST_DOUBLE_ownedState(true,1,ticket.epoch);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    const auto high=storage.highKey(),claims=highClaim();
    auto built=TEST_DOUBLE_buildAssembly(true);ASSERT_TRUE(built);
    EXPECT_EQ(built->key.generation,ticket.generation);EXPECT_EQ(built->key.epoch,ticket.epoch);
    EXPECT_EQ(built->key.atlas,7);EXPECT_EQ(built->key.geometry,11);
    EXPECT_EQ(built->original[1],10);EXPECT_EQ(built->desired[1],10);EXPECT_EQ(built->actual[1],11);
    EXPECT_EQ(built->raw,scene.full.windowInfos.size());EXPECT_EQ(built->raw,built->windows+built->structurals);
    EXPECT_EQ(built->children.size(),storage.current()->children.size());
    EXPECT_TRUE(std::is_sorted(built->children.begin(),built->children.end()));
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claims);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, EnabledRefreshRequiresPublishedArmedParkedB1AndPreservesEveryKey) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto initial=prepare();ASSERT_TRUE(initial);ASSERT_TRUE(publish(std::move(*initial)));
    ASSERT_TRUE(TEST_DOUBLE_parked());EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u);
    const auto old=rows();const auto high=storage.highKey();
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);
    TEST_DOUBLE_taggedState(*plan);ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&plan->expected));
    EXPECT_FALSE(prepare());EXPECT_FALSE(TEST_DOUBLE_prepareExpected(plan->expected));
    auto b1=TEST_DOUBLE_prepareTagged(*plan);ASSERT_TRUE(b1);ASSERT_TRUE(publish(std::move(*b1)));
    plan->droppedRows=current()->rows;
    EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u); // B1 NEVER replaces the B0 issuer witness.
    // Explicit protocol-result DOUBLE: production changes this only after its
    // own external_desktop_stage_observe returned ENABLE_OWNED_DROP through the lender.
    plan->phase=TaggedPhase::ENABLED;plan->lastSequence=3;
    plan->lastObservedAt=1;plan->expected[0].blocked=false;++plan->expected[0].revision;
    TEST_DOUBLE_taggedState(*plan);ASSERT_TRUE(TEST_DOUBLE_observeExpected(4,&plan->expected));
    auto b2=TEST_DOUBLE_prepareTagged(*plan);ASSERT_TRUE(b2);ASSERT_TRUE(publish(std::move(*b2)));
    ASSERT_EQ(current()->ownedBlocks.size(),1u);EXPECT_EQ(current()->ownedBlocks[0].revision,3u);
    EXPECT_EQ(rows().size(),old.size());EXPECT_EQ(storage.highKey(),high);
    for (size_t i=0;i<old.size();++i) {EXPECT_EQ(rows()[i].key,old[i].key);EXPECT_EQ(rows()[i].lifetime,old[i].lifetime);}
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, EnabledInitialOrUnpublishedParkedKeysNeverGainAdmission) {
    TEST_DOUBLE_ownedState(true,2,ticket.epoch);const auto armed=TEST_DOUBLE_expected(true,2);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(2,&armed));
    const auto high=storage.highKey();EXPECT_FALSE(TEST_DOUBLE_prepareExpected(armed));
    EXPECT_FALSE(TEST_DOUBLE_pendingPlan());EXPECT_TRUE(rows().empty());EXPECT_EQ(storage.highKey(),high);
    auto unpublished=prepare();ASSERT_TRUE(unpublished);unpublished.reset();
    EXPECT_FALSE(TEST_DOUBLE_parked());EXPECT_FALSE(TEST_DOUBLE_pendingPlan());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, B0CannotReplaceRealNewSequenceArmedParkedPublication) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&plan->expected));
    EXPECT_FALSE(prepare());EXPECT_FALSE(available());EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u);
    auto fresh=TEST_DOUBLE_prepareTagged(*plan);ASSERT_TRUE(fresh);
    EXPECT_FALSE(publishExpired(std::move(*fresh)));EXPECT_FALSE(available());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, FailedArmedParkedPostcheckCannotPromoteOlderB0Receipt) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&plan->expected));
    auto b1=TEST_DOUBLE_prepareTagged(*plan);ASSERT_TRUE(b1);b1.reset();
    plan->phase=TaggedPhase::DROP_PUBLISHED;
    EXPECT_FALSE(TEST_DOUBLE_prepareTagged(*plan)); // This phase permits only real Rust observe, not B2.
    EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u);EXPECT_FALSE(available());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, EnabledRefreshRefusesStaleMissingExtraAndForeignExpectationsWithoutAllocation) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&plan->expected));const auto high=storage.highKey(),claim=highClaim();
    auto bad=*plan;bad.expected[0].epoch--;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.expected[0].revision--;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.expected[0].origin=sp<BBinder>::make();EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.expected[0].root=static_cast<uint32_t>(second->getLayerId());EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.expected.push_back(bad.expected[0]);EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.expected.clear();EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.lastSequence=3;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.stagedAt=2;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claim);EXPECT_FALSE(available());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, UnknownEnabledStructuralSnapshotCannotAllocateOrReplaceParkedReceipt) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    const auto old=scene.full.layerSources[1].applied.snapshotIdentity;
    const auto next=sp<BBinder>::make();scene.full.windowInfos[1].id=-711;
    for (auto& facts:scene.full.layerSources) {
        if (facts.applied.snapshotIdentity==old) {facts.applied.snapshotIdentity=next;facts.applied.windowId=-711;}
        for (auto& edge:facts.traversal) if (edge.applied.snapshotIdentity==old) {
            edge.applied.snapshotIdentity=next;edge.applied.windowId=-711;
        }
    }
    const auto high=storage.highKey(),claim=highClaim();
    EXPECT_FALSE(TEST_DOUBLE_observeExpected(3,&plan->expected));
    EXPECT_FALSE(TEST_DOUBLE_prepareTagged(*plan));
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claim);
    ASSERT_TRUE(TEST_DOUBLE_parked());EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, ScopeCloseClearsOnlyMatchingParkedPublicationNotRetainedHistory) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    const auto before=rows();invalidate();ASSERT_TRUE(TEST_DOUBLE_parked()); // Batch availability only.
    invalidate({ticket.owner,ticket.epoch-1,ticket.generation});ASSERT_TRUE(TEST_DOUBLE_parked());
    invalidate(ticket);EXPECT_FALSE(TEST_DOUBLE_parked());EXPECT_EQ(rows().size(),before.size());
    auto parked=prepare();ASSERT_TRUE(parked);ASSERT_TRUE(publish(std::move(*parked)));
    TEST_DOUBLE_forgetScope();EXPECT_FALSE(TEST_DOUBLE_parked());EXPECT_FALSE(available());
    EXPECT_EQ(rows().size(),before.size());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, EveryTagFieldAndEveryParticipantMustMatchCopiedPendingPlan) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    const auto good=scene.full;const auto high=storage.highKey(),claim=highClaim();
    using Tag=gui::WindowInfo::StageTag;
    for (auto member:{&Tag::authorityGeneration,&Tag::atlasRevision,&Tag::geometryRevision,&Tag::epoch}) {
        auto bad=good;++(bad.windowInfos[0].stageTag.value().*member);
        EXPECT_FALSE(joinExternalDesktopTaskRootFacts(bad,3,scene.record,scene.cohort,scene.originals,&plan->expected).coherentSourceFacts);
    }
    auto bad=good;++bad.windowInfos[1].stageTag->presentationKey;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(bad,3,scene.record,scene.cohort,scene.originals,&plan->expected).coherentSourceFacts);
    bad=good;bad.windowInfos[1].stageTag.reset();
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(bad,3,scene.record,scene.cohort,scene.originals,&plan->expected).coherentSourceFacts);
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(good,3,scene.record,scene.cohort,scene.originals).coherentSourceFacts);
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claim);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, CopiedBindingGeometryAndCompleteTablesCannotBeReplacedByObservedReplay) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);ASSERT_EQ(plan->bindings.size(),1u);
    ASSERT_EQ(plan->structurals.size(),1u);TEST_DOUBLE_taggedState(*plan);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&plan->expected));
    const auto high=storage.highKey(),claim=highClaim();
    auto bad=*plan;bad.bindings[0].value.transform[4]+=1;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;++bad.bindings[0].value.crop.right;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.bindings[0].region.push_back({1,1,2,2});EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.bindings.clear();EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.bindings.push_back(bad.bindings[0]);EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;bad.structurals.clear();EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    bad=*plan;++bad.replicas[0].value.destination_rect.right;EXPECT_FALSE(TEST_DOUBLE_prepareTagged(bad));
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claim);EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u);
    auto valid=TEST_DOUBLE_prepareTagged(*plan);ASSERT_TRUE(valid);EXPECT_TRUE(publish(std::move(*valid)));
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, TaggedRefreshMustNotMintEvenWhenCurrentSourceHasKnownNewSnapshot) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&plan->expected));
    const auto high=storage.highKey(),claim=highClaim();
    const auto old=scene.full.layerSources[0].applied.snapshotIdentity;
    const auto next=sp<BBinder>::make();
    for (auto& facts:scene.full.layerSources) {
        if (facts.applied.snapshotIdentity==old) facts.applied.snapshotIdentity=next;
        for (auto& e:facts.traversal) if (e.applied.snapshotIdentity==old) e.applied.snapshotIdentity=next;
    }
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(4,&plan->expected));
    EXPECT_FALSE(TEST_DOUBLE_prepareTagged(*plan));EXPECT_FALSE(TEST_DOUBLE_prepareExpected(plan->expected));
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claim);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, StillDroppedCandidateNeedsExactPublishedB1FactsWithoutPublicationOrMinting) {
    TEST_DOUBLE_ownedState(true,1,ticket.epoch);ASSERT_TRUE(TEST_DOUBLE_observeExpected(2));
    auto b0=prepare();ASSERT_TRUE(b0);ASSERT_TRUE(publish(std::move(*b0)));
    auto plan=TEST_DOUBLE_pendingPlan();ASSERT_TRUE(plan);TEST_DOUBLE_taggedState(*plan);
    const auto dropped=plan->expected;
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(3,&dropped));
    auto b1=TEST_DOUBLE_prepareTagged(*plan);ASSERT_TRUE(b1);ASSERT_TRUE(publish(std::move(*b1)));
    plan->droppedRows=current()->rows;plan->phase=TaggedPhase::ENABLED;
    plan->lastSequence=3;plan->lastObservedAt=1;plan->expected[0].blocked=false;++plan->expected[0].revision;
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(4,&dropped));
    const auto high=storage.highKey(),claim=highClaim();
    auto wait=TEST_DOUBLE_prepareStillDropped(*plan);
    ASSERT_TRUE(wait);EXPECT_FALSE(available());EXPECT_EQ(TEST_DOUBLE_parked()->batch,2u);
    wait.reset();
    // Same final input affine alone is insufficient: retain the actual full
    // composed/requested facts of B1, including non-input local geometry.
    scene.full.layerSources[0].applied.localTransform[4]+=1;
    scene.full.layerSources[0].traversal.front().applied=scene.full.layerSources[0].applied;
    ASSERT_TRUE(TEST_DOUBLE_observeExpected(5,&dropped));
    EXPECT_FALSE(TEST_DOUBLE_prepareStillDropped(*plan));
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(highClaim(),claim);EXPECT_FALSE(available());
}

TEST_F(ExternalDesktopNativeRootFactsTest, WholeOwnedExpectationCohortRejectsMixedStateTicketAndDuplicateOrigins) {
    const auto a=observed(first,p1,200),b=observed(second,p2,300);
    ExternalDesktopTaskRootCohort cohort;cohort.records={{ticket,a,3,100000,1},{ticket,b,4,100000,1}};
    const auto originA=sp<BBinder>::make(),originB=sp<BBinder>::make();
    const std::vector<ExternalDesktopOwnedBlockExpectation> expected{
        {static_cast<uint32_t>(first->getLayerId()),originA,2,ticket.epoch,true},
        {static_cast<uint32_t>(second->getLayerId()),originB,7,ticket.epoch,true}};
    EXPECT_TRUE(externalDesktopOwnedBlockCohortValid(cohort,expected));
    auto bad=expected;bad[1].blocked=false;EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(cohort,bad));
    bad=expected;bad[1].origin=originA;EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(cohort,bad));
    bad=expected;bad[1].revision=0;EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(cohort,bad));
    bad=expected;bad.pop_back();EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(cohort,bad));
    auto foreign=cohort;foreign.records[1].ticket.owner++;
    EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(foreign,expected));
    foreign=cohort;foreign.records[1].ticket.generation++;
    EXPECT_FALSE(externalDesktopOwnedBlockCohortValid(foreign,expected));
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, IssuesDistinctRootReceiverStructuralRowsFromFullFactsOnly) {
    auto prepared=prepare();ASSERT_TRUE(prepared);
    const auto values=rows();ASSERT_EQ(values.size(),3u);
    EXPECT_EQ(external_desktop_stage_validate_claim_registry(1,{values.data(),values.size()}),1);
    const auto& root=kind(values,WS_ROOT_CLAIM);const auto& receiver=kind(values,WS_WINDOW_CLAIM);
    const auto& structural=kind(values,WS_STRUCTURAL_CLAIM);
    EXPECT_EQ(root.root_id,static_cast<uint32_t>(first->getLayerId()));
    EXPECT_EQ(receiver.window_id,-700);EXPECT_EQ(structural.window_id,-701);
    EXPECT_EQ(receiver.root_claim,root.key);EXPECT_EQ(structural.root_claim,root.key);
    EXPECT_NE(receiver.lifetime,structural.lifetime);EXPECT_EQ(structural.channel,0u);EXPECT_EQ(structural.client,0u);
    const auto originals=storage.originalsForRoot(ticket,task,source,1);ASSERT_TRUE(originals);
    EXPECT_NE(receiver.lifetime,originals->front().window);
    ASSERT_TRUE(publish(std::move(*prepared)));ASSERT_TRUE(available());
    EXPECT_EQ(current()->rows.size(),2u); // Original/ordinary row untouched, not cloned authority.
    EXPECT_FALSE(publish(std::move(*prepared)));EXPECT_EQ(current()->rows.size(),2u);
    auto next=prepare();ASSERT_TRUE(next);auto moved=std::move(*next);
    EXPECT_FALSE(publish(std::move(*next)));EXPECT_FALSE(available());
    ASSERT_TRUE(publish(std::move(moved)));EXPECT_EQ(current()->rows.size(),2u);
    EXPECT_FALSE(publish(std::move(moved)));EXPECT_EQ(current()->rows.size(),2u);
    invalidate({ticket.owner,ticket.epoch-1,ticket.generation});EXPECT_TRUE(available());
    invalidate(ticket);EXPECT_FALSE(available());EXPECT_EQ(rows().size(),3u);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, BindingGeometryReusesClaimsAndKeepsPriorEvidenceImmutable) {
    auto prepared=prepare();ASSERT_TRUE(prepared);ASSERT_TRUE(publish(std::move(*prepared)));
    const auto before=rows();const auto previous=current();
    auto moved=std::make_shared<ExternalDesktopTaskRootObservation>(*scene.record.observed);
    moved->geometry={0,0,10,10,20,30,30,40};scene.record.observed=moved;scene.cohort.records={scene.record};
    for (auto& fact:scene.full.layerSources) if (!fact.mirrorRoots.empty()) {
        fact.roots[0].applied.layerTransform={1,0,0,1,20,30};
        for (auto& parent:fact.traversal) if (parent.sourceId==fact.roots[0].requested.id && parent.mirrorRoots.empty())
            parent.applied=fact.roots[0].applied;
    }
    prepared=prepare();ASSERT_TRUE(prepared);
    EXPECT_EQ(rows().size(),before.size());
    for (size_t i=0;i<before.size();++i) {EXPECT_EQ(rows()[i].key,before[i].key);EXPECT_EQ(rows()[i].lifetime,before[i].lifetime);}
    ASSERT_TRUE(publish(std::move(*prepared)));
    EXPECT_EQ(previous->rows[0].rootCropTarget[4],0);EXPECT_EQ(current()->rows[0].rootCropTarget[4],20);
    // Same physical root/snapshot, but actual destination parent SC replaced.
    // The trusted WMS/JNI binding must supply that new parent; current complete
    // graph proves it again, without treating ancestry geometry as lifetime.
    auto rebound=std::make_shared<ExternalDesktopTaskRootObservation>(*moved);
    rebound->destinationParent=p2;scene.record.observed=rebound;scene.cohort.records={scene.record};
    for (auto& fact:scene.full.layerSources) if (!fact.mirrorRoots.empty()) {
        fact.roots[0].requested.parent=static_cast<uint32_t>(p2->getLayerId());
        for (auto& parent:fact.traversal) {
            if (parent.sourceId==fact.roots[0].requested.id && parent.mirrorRoots.empty())
                parent.requested=fact.roots[0].requested;
            if (parent.sourceId==static_cast<uint32_t>(p1->getLayerId()))
                parent=edge(node(p2),applied(structural(p2,p2->getLayerId())));
        }
    }
    prepared=prepare();ASSERT_TRUE(prepared);ASSERT_TRUE(publish(std::move(*prepared)));
    EXPECT_EQ(rows().size(),before.size());
    for (size_t i=0;i<before.size();++i) EXPECT_EQ(rows()[i].lifetime,before[i].lifetime);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, DiagnosticFallbackUnknownRootAndMissingRowCannotIssueClaims) {
    const auto original=scene.full;
    scene.full.layerSourcesComplete=false;scene.full.layerSources.clear();EXPECT_FALSE(prepare());
    EXPECT_TRUE(rows().empty());EXPECT_FALSE(available());
    scene.full=original;scene.full.layerSources[0].roots[0].requested.lifetime.publishedHandle=second->getHandle();
    EXPECT_FALSE(prepare());EXPECT_TRUE(rows().empty());
    scene.full=original;scene.full.windowInfos.erase(scene.full.windowInfos.begin());
    scene.full.layerSources.erase(scene.full.layerSources.begin());
    for (size_t i=0;i<scene.full.layerSources.size();++i) scene.full.layerSources[i].rowIndex=i;
    EXPECT_FALSE(prepare());EXPECT_TRUE(rows().empty());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, FailedPostcheckDoesNotRecycleOrDropExposedHistory) {
    auto prepared=prepare();ASSERT_TRUE(prepared);const auto before=rows();
    EXPECT_FALSE(available()); // Explicit fixture models fresh postcheck REFUSAL by not publishing.
    prepared.reset();EXPECT_EQ(rows().size(),3u);
    auto replay=prepare();ASSERT_TRUE(replay);EXPECT_EQ(rows().size(),before.size());
    for (size_t i=0;i<before.size();++i) EXPECT_EQ(rows()[i].key,before[i].key);
    EXPECT_FALSE(publishExpired(std::move(*replay)));EXPECT_FALSE(available());
    EXPECT_EQ(rows().size(),before.size()); // Actual final clock refusal also preserves exposure history.
    auto stale=prepare();ASSERT_TRUE(stale);
    invalidate();EXPECT_FALSE(publish(std::move(*stale))); // source revision unchanged: reservation itself fenced
    auto changed=prepare();ASSERT_TRUE(changed);
    storage.rejectBatch();EXPECT_FALSE(publish(std::move(*changed)));
    EXPECT_FALSE(available());EXPECT_EQ(rows().size(),3u);
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, SnapshotRecreationAndChannelPresenceCannotRetagExistingLifetimes) {
    auto prepared=prepare();ASSERT_TRUE(prepared);ASSERT_TRUE(publish(std::move(*prepared)));
    const auto before=rows();
    scene.full.layerSources[0].applied.snapshotIdentity=sp<BBinder>::make();
    scene.full.layerSources[0].traversal.front().applied.snapshotIdentity=scene.full.layerSources[0].applied.snapshotIdentity;
    EXPECT_FALSE(prepare());EXPECT_FALSE(available());
    EXPECT_EQ(rows().size(),before.size());
    scene.full.windowInfos[1].token=sp<BBinder>::make();scene.full.layerSources[1].applied.hasInputToken=true;
    scene.full.layerSources[1].traversal.front().applied.hasInputToken=true;
    EXPECT_FALSE(prepare());EXPECT_EQ(rows().size(),before.size());
}

TEST_F(ExternalDesktopNativeClaimFactoryTest, AppendRefusalBurnsAndCannotLaterExposeAnOlderReservedLifetime) {
    auto prepared=prepare();ASSERT_TRUE(prepared);ASSERT_TRUE(publish(std::move(*prepared)));
    const auto bytes=historyBytes(),high=highClaim(),sourceHigh=storage.highKey();
    // Explicit resource fault, NOT a production release/retirement API.
    TEST_DOUBLE_historyBudgetFull();
    scene.full.windowInfos[0].id=-702;
    scene.full.layerSources[0].applied.windowId=-702;
    scene.full.layerSources[0].applied.snapshotIdentity=sp<BBinder>::make();
    scene.full.layerSources[0].traversal.front().applied=scene.full.layerSources[0].applied;
    EXPECT_FALSE(prepare());EXPECT_TRUE(stopped());EXPECT_FALSE(available());
    EXPECT_GT(storage.highKey(),sourceHigh);EXPECT_GT(highClaim(),high);
    EXPECT_EQ(rows().size(),3u); // No Row4 exposure; IDs nevertheless burned.
    TEST_DOUBLE_restoreBudget(bytes);
    const auto burned=storage.highKey();
    EXPECT_FALSE(prepare());EXPECT_TRUE(stopped());EXPECT_EQ(storage.highKey(),burned);
    EXPECT_EQ(rows().size(),3u); // Smaller future data may not revive this issuance context.
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    // Ordinary/source observations still progress; no packet drop/FD/drain claim.
}
TEST_F(ExternalDesktopNativeRootFactsTest, StructuralNoReceiverBitUsesProvenRootAndComposedDropSidecar) {
    auto p=packet();
    p.full.windowInfos[1].inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);
    p.full.layerSources[1].applied.inputConfig=p.full.windowInfos[1].inputConfig.get();
    for (auto& facts:p.full.layerSources) for (auto& parent:facts.traversal)
        if (parent.applied.snapshotIdentity==p.full.layerSources[1].applied.snapshotIdentity)
            parent.applied=p.full.layerSources[1].applied;
    ASSERT_FALSE(p.full.windowInfos[1].token);ASSERT_FALSE(p.full.windowInfos[1].windowToken);
    ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    auto actual=joinExternalDesktopTaskRootFacts(p.full,9,p.record,p.cohort,p.originals);
    EXPECT_EQ(actual.state,ExternalDesktopRootJoin::CLONES_OBSERVED);EXPECT_TRUE(actual.coherentSourceFacts);
    ASSERT_EQ(actual.structuralSources.size(),1u);EXPECT_EQ(actual.matched,1u);
    auto missing=p.full;missing.layerSources[1].applied.composedDropMode=
            static_cast<uint32_t>(gui::DropInputMode::NONE);
    missing.layerSources[1].traversal.front().applied=missing.layerSources[1].applied;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(missing,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto notOwned=p.full;
    for (auto& facts:notOwned.layerSources) {
        facts.roots[0].requested.requestedDropMode=static_cast<uint32_t>(gui::DropInputMode::NONE);
        for (auto& parent:facts.traversal) if (parent.sourceId==facts.roots[0].requested.id)
            parent.requested=facts.roots[0].requested;
    }
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(notOwned,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto foreign=p.full;
    for (auto& facts:foreign.layerSources) {
        facts.roots[0].requested.lifetime.publishedHandle=second->getHandle();
        for (auto& parent:facts.traversal) if (parent.sourceId==facts.roots[0].requested.id)
            parent.requested=facts.roots[0].requested;
    }
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(foreign,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    EXPECT_TRUE(p.full.windowInfos[0].inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT));
}

TEST_F(ExternalDesktopNativeRootFactsTest, NoChannelFlagCannotExemptActualReceiverFromDroppedProof) {
    const auto p=packet();auto receiver=p.full;
    receiver.windowInfos[0].inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);
    receiver.layerSources[0].applied.inputConfig=receiver.windowInfos[0].inputConfig.get();
    receiver.layerSources[0].traversal.front().applied=receiver.layerSources[0].applied;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(receiver,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    // NO_INPUT_CHANNEL plus an ACTUAL token is not a structural row.
    auto flagged=receiver;flagged.windowInfos[0].inputConfig|=gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL;
    flagged.layerSources[0].applied.inputConfig=flagged.windowInfos[0].inputConfig.get();
    flagged.layerSources[0].traversal.front().applied=flagged.layerSources[0].applied;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(flagged,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto wrong=p.full;wrong.windowInfos[1].token=sp<BBinder>::make();
    wrong.windowInfos[1].inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);
    wrong.layerSources[1].applied.hasInputToken=true;
    wrong.layerSources[1].applied.inputConfig=wrong.windowInfos[1].inputConfig.get();
    wrong.layerSources[1].traversal.front().applied=wrong.layerSources[1].applied;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(wrong,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    auto clientOnly=p.full;clientOnly.windowInfos[1].windowToken=sp<BBinder>::make();
    clientOnly.windowInfos[1].inputConfig.clear(gui::WindowInfo::InputConfig::DROP_INPUT);
    clientOnly.layerSources[1].applied.hasClientToken=true;
    clientOnly.layerSources[1].applied.inputConfig=clientOnly.windowInfos[1].inputConfig.get();
    clientOnly.layerSources[1].traversal.front().applied=clientOnly.layerSources[1].applied;
    EXPECT_FALSE(joinExternalDesktopTaskRootFacts(clientOnly,9,p.record,p.cohort,p.originals).coherentSourceFacts);
    EXPECT_FALSE(p.full.windowInfos[1].token); // no producer/stock mutation to green the checks
}
} // namespace android::inputdispatcher
#endif

// Exact unchanged weak-history resource fixture appended as a separate guarded namespace.
// SOURCE ONLY/UNCOMPILED. Real storage class + real SC/Binder owners; the
// public lease/object doubles below issue NO authority/RootClaim/Live/retirement.
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <gtest/gtest.h>
#include <binder/Binder.h>
#include <gui/SurfaceComposerClient.h>
#include <array>
#include "../dispatcher/ExternalDesktopSourceLifetimes.h"

namespace android::inputdispatcher {
class ExternalDesktopSourceLifetimesResourceTest : public ::testing::Test {
protected:
    class References : public ExternalDesktopStageRosterReferences {
        bool same(const ExternalDesktopStageRosterReferences& other) const override { return this==&other; }
        const void* implementation() const override { return this; }
    };
    void SetUp() override {
        composer=sp<SurfaceComposerClient>::make();ASSERT_EQ(composer->initCheck(),NO_ERROR);
        task=surface("history task");child=surface("history child");
        ASSERT_TRUE(task && child && task->isValid() && child->isValid());
        channel=sp<BBinder>::make();client=sp<BBinder>::make();
        auto taskIdentity=std::make_shared<ExternalDesktopStageObjectIdentity>();
        roster.references=std::make_shared<References>();
        roster.nativeOwnerIdentity=std::make_shared<ExternalDesktopStageObjectIdentity>();
        roster.tasks.push_back({100,10152,0,1,task,task->getLayerId(),11,taskIdentity});
        roster.windows.push_back({child,channel,client,child->getLayerId(),1,1,0x2000,0,63,
                                  taskIdentity,std::make_shared<ExternalDesktopStageObjectIdentity>()});
        view.user=0;view.generation=42;view.observed_at=1;
        view.topology_snapshot={topology.data(),topology.size()};
        view.expires_by_display={expiry.data(),expiry.size()};
    }
    sp<SurfaceControl> surface(const char* name) {
        return composer->createSurface(String8(name),0,0,PIXEL_FORMAT_UNKNOWN,
                                       ISurfaceComposerClient::eFXSurfaceContainer);
    }
    bool publish(const ExternalDesktopStageRoster& value) {
        auto candidate=storage.prepare(view,ticket,value);
        return candidate && storage.commit(std::move(*candidate));
    }
    uint64_t childKey() const { return storage.current()->children.front().rustValue.child; }
    sp<SurfaceComposerClient> composer;
    sp<SurfaceControl> task,child;
    sp<IBinder> channel,client;
    ExternalDesktopStageRoster roster;
    ExternalDesktopSourceLifetimes storage;
    ExternalDesktopStageLeaseView view{};
    const ExternalDesktopStageEpochTicket ticket{17,19,42}; // storage-only public fixture
    const std::array<int64_t,14> topology{42,10,2,1,0,100,2,0,0,1024,768,160,1,42};
    const std::array<int64_t,1> expiry{100000};
};

TEST_F(ExternalDesktopSourceLifetimesResourceTest, CurrentOwnsResourcesHistoryDoesNotAfterInvalidation) {
    const auto taskRefs=task->getStrongCount(),childRefs=child->getStrongCount();
    const auto channelRefs=channel->getStrongCount(),clientRefs=client->getStrongCount();
    ASSERT_TRUE(publish(roster));ASSERT_TRUE(storage.current());
    EXPECT_EQ(task->getStrongCount(),taskRefs+1);EXPECT_EQ(child->getStrongCount(),childRefs+1);
    EXPECT_EQ(channel->getStrongCount(),channelRefs+1);EXPECT_EQ(client->getStrongCount(),clientRefs+1);
    const auto high=storage.highKey();const auto retained=storage.retainedChildren();
    storage.invalidate(ticket);EXPECT_FALSE(storage.current());
    EXPECT_EQ(task->getStrongCount(),taskRefs);EXPECT_EQ(child->getStrongCount(),childRefs);
    EXPECT_EQ(channel->getStrongCount(),channelRefs);EXPECT_EQ(client->getStrongCount(),clientRefs);
    EXPECT_EQ(storage.highKey(),high);EXPECT_EQ(storage.retainedChildren(),retained);
    // Observation disposal did not delete identities or prove claim retirement.
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, ExactReobservationReusesKeysAndCurrentMutableFactsOnly) {
    ASSERT_TRUE(publish(roster));const auto high=storage.highKey();
    const auto key=storage.current()->children.front().rustValue;
    auto changed=roster;changed.windows[0].state=2;changed.windows[0].flags|=0x20;
    ASSERT_TRUE(publish(changed));EXPECT_EQ(storage.highKey(),high);
    EXPECT_EQ(storage.current()->children.front().rustValue.child,key.child);
    EXPECT_EQ(storage.current()->children.front().rustValue.channel,key.channel);
    EXPECT_EQ(storage.current()->children.front().observed.state,2u);
    EXPECT_EQ(storage.current()->children.front().observed.flags,changed.windows[0].flags);
    EXPECT_EQ(roster.windows[0].state,63u);EXPECT_EQ(roster.windows[0].flags,0x2000);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, ReplacedSurfaceKeepsOldIdentityWithoutStrongHistoryOwner) {
    const auto oldRefs=child->getStrongCount();ASSERT_TRUE(publish(roster));
    const auto oldKey=storage.current()->children.front().rustValue.child;
    auto replacement=surface("replacement child");ASSERT_TRUE(replacement && replacement->isValid());
    auto next=roster;next.windows[0].surface=replacement;next.windows[0].layerId=replacement->getLayerId();
    const auto nextRefs=replacement->getStrongCount();ASSERT_TRUE(publish(next));
    EXPECT_NE(storage.current()->children.front().rustValue.child,oldKey);
    EXPECT_EQ(storage.retainedChildren(),2u);EXPECT_EQ(child->getStrongCount(),oldRefs);
    EXPECT_EQ(replacement->getStrongCount(),nextRefs+1);
    storage.invalidate(ticket);EXPECT_EQ(replacement->getStrongCount(),nextRefs);
    ASSERT_TRUE(publish(roster));
    EXPECT_EQ(storage.current()->children.front().rustValue.child,oldKey);
    EXPECT_EQ(storage.retainedChildren(),2u); // live old referent is reused, not evicted
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, FailedCandidateDoesNotPinResourcesOrPublishHistory) {
    ASSERT_TRUE(publish(roster));const auto high=storage.highKey();
    const auto key=storage.current()->children.front().rustValue.child;
    auto invalid=roster;invalid.windows[0].type=2011;
    const auto taskRefs=task->getStrongCount(),childRefs=child->getStrongCount();
    const auto channelRefs=channel->getStrongCount(),clientRefs=client->getStrongCount();
    EXPECT_FALSE(publish(invalid));EXPECT_EQ(storage.highKey(),high);
    EXPECT_EQ(storage.current()->children.front().rustValue.child,key);
    EXPECT_EQ(task->getStrongCount(),taskRefs);EXPECT_EQ(child->getStrongCount(),childRefs);
    EXPECT_EQ(channel->getStrongCount(),channelRefs);EXPECT_EQ(client->getStrongCount(),clientRefs);
    EXPECT_EQ(storage.retainedChildren(),1u);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, FullEmptyBatchRetainsEveryCurrentChildAndIdentity) {
    ASSERT_TRUE(publish(roster));
    const auto before=storage.current()->children.front().rustValue;
    const auto high=storage.highKey();
    gui::WindowInfosUpdate full;full.layerSourcesComplete=true;
    ASSERT_TRUE(gui::layerSourcesValid(full.layerSources,full.windowInfos,true));
    storage.observeForTest(full,1,1);
    ASSERT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    ASSERT_EQ(storage.current()->children.size(),1u);
    const auto& actual=storage.current()->children.front().rustValue;
    EXPECT_EQ(actual.participation,WS_CHILD_NO_EXPORTED_ROW);
    EXPECT_EQ(actual.child,before.child);EXPECT_EQ(actual.layer,before.layer);
    EXPECT_EQ(actual.channel,before.channel);EXPECT_EQ(actual.client,before.client);
    EXPECT_EQ(storage.retainedChildren(),1u);EXPECT_EQ(storage.retainedWindows(),0u);
    EXPECT_EQ(storage.highKey(),high);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, IncompleteCompanionCannotEstablishChildAbsence) {
    ASSERT_TRUE(publish(roster));
    gui::WindowInfosUpdate incomplete;
    storage.observeForTest(incomplete,1,1);
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
    EXPECT_EQ(storage.current()->children.front().rustValue.participation,WS_CHILD_RECEIVER);
    EXPECT_EQ(storage.retainedChildren(),1u);EXPECT_EQ(storage.retainedWindows(),0u);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, NoSurfaceOrChannelStillKeepsMandatoryRealClient) {
    auto noSurface=roster;
    noSurface.windows[0].surface.clear();noSurface.windows[0].layerId=0;
    noSurface.windows[0].channel.clear();noSurface.windows[0].state=0;
    ASSERT_TRUE(publish(noSurface));
    gui::WindowInfosUpdate full;full.layerSourcesComplete=true;
    storage.observeForTest(full,1,1);
    ASSERT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    const auto& actual=storage.current()->children.front().rustValue;
    EXPECT_EQ(actual.participation,WS_CHILD_NO_EXPORTED_ROW);
    EXPECT_EQ(actual.layer,0u);EXPECT_EQ(actual.channel,0u);EXPECT_NE(actual.client,0u);
    EXPECT_EQ(storage.current()->children.front().observed.client,client);
    auto missingClient=noSurface;missingClient.windows[0].client.clear();
    EXPECT_FALSE(publish(missingClient));
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, EmittedClientOnlyRowCannotBeDisguisedAsAbsentOrStructural) {
    auto noChannel=roster;noChannel.windows[0].channel.clear();
    ASSERT_TRUE(publish(noChannel));
    gui::WindowInfosUpdate batch;
    gui::WindowInfo row;row.id=child->getLayerId();row.windowToken=client;
    row.inputConfig|=gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL;
    batch.windowInfos.push_back(row);
    storage.observeForTest(batch,1,1);
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::REJECTED);
    EXPECT_EQ(storage.retainedChildren(),1u);EXPECT_EQ(storage.retainedWindows(),0u);
}

TEST_F(ExternalDesktopNativeRootFactsTest, CloneOnlyFullBatchCannotProveNoExportedChildRow) {
    auto p=packet();
    ASSERT_TRUE(gui::layerSourcesValid(p.full.layerSources,p.full.windowInfos,true));
    class References final : public ExternalDesktopStageRosterReferences {
        bool same(const ExternalDesktopStageRosterReferences& other) const override { return this==&other; }
        const void* implementation() const override { return this; }
    };
    const auto taskIdentity=std::make_shared<ExternalDesktopStageObjectIdentity>();
    ExternalDesktopStageRoster roster;roster.references=std::make_shared<References>();
    roster.nativeOwnerIdentity=std::make_shared<ExternalDesktopStageObjectIdentity>();
    roster.tasks.push_back({100,10152,0,1,source,source->getLayerId(),11,taskIdentity});
    const auto& original=p.originals.front();
    roster.windows.push_back({p.child,original.channel,original.client,p.child->getLayerId(),
            1,1,0x2000,0,63,taskIdentity,std::make_shared<ExternalDesktopStageObjectIdentity>()});
    const std::array<int64_t,14> topology{42,10,2,1,0,100,2,0,0,1024,768,160,1,42};
    const std::array<int64_t,1> expiry{100000};
    ExternalDesktopStageLeaseView view{};view.user=0;view.generation=42;view.observed_at=1;
    view.topology_snapshot={topology.data(),topology.size()};
    view.expires_by_display={expiry.data(),expiry.size()};
    ExternalDesktopSourceLifetimes storage;
    auto prepared=storage.prepare(view,ExternalDesktopStageEpochTicket{17,19,42},roster);
    ASSERT_TRUE(prepared);ASSERT_TRUE(storage.commit(std::move(*prepared)));
    storage.observeForTest(p.full,1,1);
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::REJECTED);
    EXPECT_EQ(storage.current()->children.front().rustValue.participation,WS_CHILD_RECEIVER);
    EXPECT_EQ(storage.retainedChildren(),1u);EXPECT_EQ(storage.retainedWindows(),0u);
}
TEST_F(ExternalDesktopSourceLifetimesResourceTest, CachedBatchKeepsActualArrivalAcrossFreshNowAndRosterReplacement) {
    ASSERT_TRUE(publish(roster));
    gui::WindowInfosUpdate batch;batch.layerSourcesComplete=true;
    storage.observe(batch,1,10,20);
    ASSERT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    const auto childKeyBefore=storage.current()->children.front().rustValue.child;
    EXPECT_EQ(storage.current()->observedAt,10);EXPECT_EQ(storage.current()->batchSequence,1u);
    auto refreshed=roster;refreshed.windows[0].state=2;
    ASSERT_TRUE(publish(refreshed));EXPECT_EQ(storage.current()->observedAt,-1);
    storage.observe(batch,1,10,30);
    ASSERT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    EXPECT_EQ(storage.current()->observedAt,10);EXPECT_EQ(storage.current()->batchSequence,1u);
    EXPECT_EQ(storage.current()->children.front().rustValue.child,childKeyBefore);
    // Bootstrap B0 is useful evidence, but it remains older than submit20.
    EXPECT_LT(storage.current()->observedAt,20);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, SameSequenceCannotRelabelCachedArrivalAndNewBatchAllowsEqualMilliseconds) {
    ASSERT_TRUE(publish(roster));gui::WindowInfosUpdate batch;batch.layerSourcesComplete=true;
    storage.observe(batch,1,10,20);
    storage.observe(batch,1,25,30); // relabeling B0 as fresh B1 is refused
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
    EXPECT_EQ(storage.current()->observedAt,-1);EXPECT_EQ(storage.current()->batchSequence,0u);
    EXPECT_EQ(storage.retainedChildren(),1u);
    storage.observe(batch,1,10,30); // honest cached bootstrap reuse is still legal
    ASSERT_EQ(storage.current()->observedAt,10);
    storage.observe(batch,2,10,31); // distinct real callbacks may share one clock millisecond
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    EXPECT_EQ(storage.current()->observedAt,10);EXPECT_EQ(storage.current()->batchSequence,2u);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, ClockSequenceRollbackFutureAndNegativeTimesRefuseWithoutRetiringIdentities) {
    ASSERT_TRUE(publish(roster));gui::WindowInfosUpdate batch;batch.layerSourcesComplete=true;
    const auto childKeyBefore=storage.current()->children.front().rustValue.child;
    const auto high=storage.highKey();
    storage.observe(batch,2,10,20);
    const std::array<std::array<int64_t,3>,5> bad{{{1,10,21},{3,9,21},{2,10,19},{3,-1,21},{3,22,21}}};
    for (const auto& value:bad) {
        storage.observe(batch,static_cast<uint64_t>(value[0]),value[1],value[2]);
        EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
        EXPECT_EQ(storage.current()->observedAt,-1);EXPECT_EQ(storage.current()->batchSequence,0u);
        EXPECT_EQ(storage.current()->children.front().rustValue.child,childKeyBefore);
        EXPECT_EQ(storage.retainedChildren(),1u);EXPECT_EQ(storage.highKey(),high);
    }
    storage.observe(batch,4,11,22); // invalid seq3 is burned; only a genuine newer callback recovers
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
    EXPECT_EQ(storage.current()->observedAt,11);EXPECT_EQ(storage.current()->batchSequence,4u);
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, InvalidNewerClockBurnsOlderAndCannotBeRetimedButNewValidBatchRecovers) {
    const std::array<std::array<int64_t,2>,3> invalid{{{9,21},{11,19},{22,21}}};
    for (const auto& clock:invalid) {
        ExternalDesktopSourceLifetimes actual;
        auto prepared=actual.prepare(view,ticket,roster);ASSERT_TRUE(prepared);
        ASSERT_TRUE(actual.commit(std::move(*prepared)));
        gui::WindowInfosUpdate full;full.layerSourcesComplete=true;
        actual.observe(full,1,10,20);
        ASSERT_EQ(actual.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
        const auto key=actual.current()->children.front().rustValue.child;
        actual.observe(full,2,clock[0],clock[1]);
        EXPECT_EQ(actual.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
        actual.observe(full,1,10,30); // exact older callback cannot resurrect availability
        EXPECT_EQ(actual.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
        actual.observe(full,2,12,30); // same invalid sequence cannot be retimed
        EXPECT_EQ(actual.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
        actual.observe(full,2,clock[0],30); // fresh now cannot repair its invalid original clock
        EXPECT_EQ(actual.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
        EXPECT_EQ(actual.current()->children.front().rustValue.child,key);
        actual.observe(full,3,12,31);
        EXPECT_EQ(actual.current()->join,ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED);
        EXPECT_EQ(actual.current()->observedAt,12);EXPECT_EQ(actual.current()->batchSequence,3u);
    }
}

TEST_F(ExternalDesktopSourceLifetimesResourceTest, CachedArrivalDoesNotBypassFreshExpiryOrRejectedLatestBatch) {
    ASSERT_TRUE(publish(roster));gui::WindowInfosUpdate batch;batch.layerSourcesComplete=true;
    storage.observe(batch,1,1,2);
    storage.observe(batch,1,1,100000); // cached arrival1 does NOT renew the actual deadline
    EXPECT_EQ(storage.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
    EXPECT_EQ(storage.current()->observedAt,-1);EXPECT_EQ(storage.retainedChildren(),1u);

    ExternalDesktopSourceLifetimes other;
    auto prepared=other.prepare(view,ticket,roster);ASSERT_TRUE(prepared);
    ASSERT_TRUE(other.commit(std::move(*prepared)));
    other.observe(batch,1,10,20);
    gui::WindowInfosUpdate incomplete;
    other.observe(incomplete,2,11,21); // rejected NEWEST batch still burns its sequence/arrival
    EXPECT_EQ(other.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
    other.observe(batch,1,10,22);
    EXPECT_EQ(other.current()->join,ExternalDesktopSourceLifetimes::Join::INCOMPLETE);
    EXPECT_EQ(other.current()->observedAt,-1);EXPECT_EQ(other.retainedChildren(),1u);
}
} // namespace android::inputdispatcher
#endif
