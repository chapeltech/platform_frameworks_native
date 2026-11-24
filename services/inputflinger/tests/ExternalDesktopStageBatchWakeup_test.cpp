// UNCOMPILED: real dispatcher/owner path, explicit inert observation doubles.
// These tests do not manufacture a lease, RootClaim, Live or retirement proof.
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <gtest/gtest.h>
#include <functional>
#include <thread>
#include "FakeInputDispatcherPolicy.h"
#include "../dispatcher/InputDispatcher.h"

namespace android::inputdispatcher {
class ExternalDesktopStageBatchWakeupTest : public ::testing::Test {
protected:
    class Policy : public FakeInputDispatcherPolicy {
    public:
        std::function<void(int64_t,int64_t,int64_t,int64_t)> observed;
        void notifyExternalDesktopStageBatchObserved(int64_t owner,int64_t epoch,
                int64_t generation,int64_t batch) override {
            if (observed) observed(owner,epoch,generation,batch);
        }
    };
    void SetUp() override {
        dispatcher=std::make_unique<InputDispatcher>(policy,nullptr);
    }
    void TEST_DOUBLE_registration(ExternalDesktopStageEpochTicket ticket={17,19,42}) {
        std::scoped_lock lock(dispatcher->mLock);
        // A wakeup selector needs no authority. This is intentionally NOT an
        // observeRoster/lease success; refresh still fails without real evidence.
        dispatcher->mExternalDesktopStageOwner.mWmsRoster=std::make_shared<ExternalDesktopStageRoster>();
        dispatcher->mExternalDesktopStageOwner.mWmsRosterTicket=ticket;
    }
    bool lockAvailableOnOtherThread() {
        bool available=false;
        std::thread probe([&] {
            std::unique_lock lock(dispatcher->mLock,std::try_to_lock);
            available=lock.owns_lock();
        });
        probe.join(); return available;
    }
    void clearRegistration() {
        std::shared_ptr<const ExternalDesktopStageRoster> displaced;
        {std::scoped_lock lock(dispatcher->mLock);
         displaced=dispatcher->mExternalDesktopStageOwner.takeRosterObservation({17,19,42});}
        // Actual observation disposal, outside mLock; not claim retirement.
    }
    void receiveIncompleteBatchAndExpectNoWakeup() {
        std::scoped_lock lock(dispatcher->mLock);
        gui::WindowInfosUpdate oversized;oversized.windowInfos.resize(8193);
        dispatcher->mExternalDesktopStageOwner.receiveFullBatch(oversized,0,0);
        EXPECT_FALSE(dispatcher->mExternalDesktopStageOwner.batchWakeup());
    }
    void exhaustObservationCounter() {
        std::scoped_lock lock(dispatcher->mLock);
        dispatcher->mExternalDesktopStageOwner.mReceipt.observations=UINT64_MAX;
    }
    void expectOneCompleteObservation() {
        std::scoped_lock lock(dispatcher->mLock);
        EXPECT_TRUE(dispatcher->mExternalDesktopStageOwner.receipt().hasCompleteBatch);
        EXPECT_EQ(dispatcher->mExternalDesktopStageOwner.receipt().observations,1u);
    }
    Policy policy;
    std::unique_ptr<InputDispatcher> dispatcher;
};

TEST_F(ExternalDesktopStageBatchWakeupTest, FullBatchHintRunsOnlyAfterDispatcherUnlock) {
    TEST_DOUBLE_registration();int calls=0;
    policy.observed=[&](int64_t owner,int64_t epoch,int64_t generation,int64_t batch) {
        EXPECT_TRUE(lockAvailableOnOtherThread());
        EXPECT_EQ(owner,17);EXPECT_EQ(epoch,19);EXPECT_EQ(generation,42);EXPECT_EQ(batch,++calls);
    };
    dispatcher->onWindowInfosChanged({});dispatcher->onWindowInfosChanged({});
    EXPECT_EQ(calls,2); // New actual callbacks, not a refresh feedback loop.
}

TEST_F(ExternalDesktopStageBatchWakeupTest, TeardownAndUnrepresentableOwnerNeverHint) {
    int calls=0;policy.observed=[&](int64_t,int64_t,int64_t,int64_t){++calls;};
    dispatcher->onWindowInfosChanged({});EXPECT_EQ(calls,0);
    TEST_DOUBLE_registration();dispatcher->onWindowInfosChanged({});EXPECT_EQ(calls,1);
    clearRegistration();dispatcher->onWindowInfosChanged({});EXPECT_EQ(calls,1);
    TEST_DOUBLE_registration({UINT64_MAX,19,42});
    dispatcher->onWindowInfosChanged({});EXPECT_EQ(calls,1);
}

TEST_F(ExternalDesktopStageBatchWakeupTest, IncompleteLatestAndExhaustionNeverReplayOlderHint) {
    TEST_DOUBLE_registration();int calls=0;
    policy.observed=[&](int64_t,int64_t,int64_t,int64_t){++calls;};
    dispatcher->onWindowInfosChanged({});ASSERT_EQ(calls,1);
    // Exercise the private owning-copy availability bound directly; do not
    // feed malformed duplicate stock WindowInfos through its fatal validator.
    receiveIncompleteBatchAndExpectNoWakeup();
    EXPECT_EQ(calls,1);
    exhaustObservationCounter();
    dispatcher->onWindowInfosChanged({});EXPECT_EQ(calls,1);
}

TEST_F(ExternalDesktopStageBatchWakeupTest, HintCannotReplaceFreshTicketRosterAndAuthority) {
    TEST_DOUBLE_registration();dispatcher->onWindowInfosChanged({});
    ExternalDesktopStageLeaseView noAuthority{};
    EXPECT_FALSE(dispatcher->refreshExternalDesktopStageClaims(noAuthority,{17,19,42},{},1));
    EXPECT_FALSE(dispatcher->refreshExternalDesktopStageClaims(noAuthority,{18,19,42},{},1));
    expectOneCompleteObservation();
}
} // namespace android::inputdispatcher
#endif
