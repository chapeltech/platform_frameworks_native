/* Private default-off native owner. Not a lease issuer or a routing policy. */
#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <gui/SurfaceComposerClient.h>
#include <gui/WindowInfosUpdate.h>
#include <external_desktop_native_stage.h>
#include <external_desktop_input_stage_authority.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include "ExternalDesktopStageRoster.h"
#include "ExternalDesktopSourceLifetimes.h"
#include "ExternalDesktopTaskRootObservation.h"
#include "ExternalDesktopNativeClaims.h"
#include "ExternalDesktopSubmissionLedger.h"

namespace android::inputdispatcher {

// Serialized by the owning InputDispatcher's mLock, never by a second mutex.
// One Context survives all callbacks, geometry changes and future revocations;
// destroying the actual native owner is the only destruction path here.
class ExternalDesktopStageOwner final {
public:
    enum class ReceiptState { NO_PUBLICATION, CONTEXT_UNAVAILABLE, UNSUPPORTED_SIZE, EXHAUSTED,
                              CLOCK_UNAVAILABLE };
    struct Receipt {
        // Diagnostics of real full callbacks ONLY. Not a stage epoch, lease,
        // geometry revision, completion acknowledgement or activation proof.
        uint64_t observations = 0;
        size_t rawWindows = 0;
        size_t inspectedWindows = 0;
        size_t installedWindows = 0;
        size_t installedDisplayMaps = 0;
        size_t displayInfos = 0;
        size_t informationalTagRows = 0;
        size_t lineageRows = 0;
        int64_t vsyncId = -1;
        int64_t timestamp = 0;
        // Native elapsedRealtime milliseconds captured ONCE at callback entry.
        // Separate from SF's timestamp/vsync and never refreshed by a lender.
        int64_t nativeObservedAt = -1;
        // Whole callback retained, never just the dispatchable/channel-unique rows.
        bool hasCompleteBatch = false;
        ReceiptState state = ReceiptState::NO_PUBLICATION;
    };

    ExternalDesktopStageOwner();
    ~ExternalDesktopStageOwner() = default;
    ExternalDesktopStageOwner(const ExternalDesktopStageOwner&) = delete;
    ExternalDesktopStageOwner& operator=(const ExternalDesktopStageOwner&) = delete;
    ExternalDesktopStageOwner(ExternalDesktopStageOwner&&) = delete;
    ExternalDesktopStageOwner& operator=(ExternalDesktopStageOwner&&) = delete;

    void receiveFullBatch(const gui::WindowInfosUpdate&, size_t installedWindows,
                          size_t installedDisplayMaps);
    Receipt receipt() const { return mReceipt; }
    // Wakeup selector only. It carries no handle, borrowed authority or acceptance.
    struct BatchWakeup { ExternalDesktopStageEpochTicket ticket; uint64_t batch; };
    std::optional<BatchWakeup> batchWakeup() const;
    bool refreshNativeClaims(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket,
            const std::shared_ptr<const ExternalDesktopStageRoster>&, uint64_t batch,
            const std::vector<ExternalDesktopOwnedBlockExpectation>* expected = nullptr);
    bool borrowAuthority(const ExternalDesktopStageLeaseView&);
    bool observeRoots(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket,
            const std::vector<std::shared_ptr<const ExternalDesktopTaskRootObservation>>&,
            std::shared_ptr<const ExternalDesktopTaskRootCohort>& displaced,
            std::shared_ptr<const ExternalDesktopTaskRootCohort>& displacedHistory,
            const std::shared_ptr<const ExternalDesktopStageRoster>& expectedRoster = {});
    void invalidateRootObservations(ExternalDesktopStageEpochTicket);
    bool observeRoster(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket,
                       int64_t displayToken, int64_t serial,
                       const std::shared_ptr<const ExternalDesktopStageRoster>&, int32_t operation,
                       std::shared_ptr<const ExternalDesktopStageRoster>& displaced,
                       std::shared_ptr<const ExternalDesktopTaskRootCohort>* displacedRoots = nullptr);
    // Caller drops returned observations only AFTER releasing dispatcher mLock.
    std::shared_ptr<const ExternalDesktopStageRoster> takeRosterObservation(ExternalDesktopStageEpochTicket);
    std::shared_ptr<const ExternalDesktopTaskRootCohort> takeRootObservations(ExternalDesktopStageEpochTicket);
    bool reserveSubmission(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket*,
            std::shared_ptr<const ExternalDesktopStageRoster>* displacedRoster = nullptr,
            std::shared_ptr<const ExternalDesktopTaskRootCohort>* displacedRoots = nullptr);
    bool beginSubmission(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket,
                         int64_t displayToken, int64_t serial);
    bool noteSubmission(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket,
                        int64_t displayToken, int64_t serial, bool inputReported);
    bool closeSubmission(ExternalDesktopStageEpochTicket);
    bool invalidateSubmission(ExternalDesktopStageEpochTicket, int64_t displayToken, int64_t serial);

private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    friend class ExternalDesktopStageBatchWakeupTest;
    friend class ExternalDesktopNativeClaimFactoryTest;
    bool validRosterObservations(const ExternalDesktopStageLeaseView&, const ExternalDesktopStageRoster&) const;
    bool issueNativeClaims(const ExternalDesktopStageLeaseView&,
            const std::vector<ExternalDesktopOwnedBlockExpectation>* expected = nullptr);
    // Native-only future StageLive caller. No JNI/result/key setter exists.
    // These invoke the existing real Rust protocol under its borrowed lender;
    // no SC operation/IPC is performed while dispatcher mLock is held.
    bool submitNativeStage(const ExternalDesktopStageLeaseView&, const WsLive&, const WsStage&);
    bool admitNativeIntent(const ExternalDesktopStageLeaseView&, const WsIntent&);
    // Explicit SAME intent renewal, not reserveSubmission/admit_intent. The
    // result is a real newer EpochOwner reservation, never a caller epoch.
    bool renewNativeIntent(const ExternalDesktopStageLeaseView&, ExternalDesktopStageEpochTicket,
            const WsLive& independentlyObserved, ExternalDesktopStageEpochTicket*);
    bool observeNativeRenewalBinding(const ExternalDesktopStageLeaseView&,ExternalDesktopStageEpochTicket,
            const std::shared_ptr<const ExternalDesktopStageRoster>&,
            const std::vector<std::shared_ptr<const ExternalDesktopTaskRootObservation>>&,
            const std::vector<ExternalDesktopStageSubmissionObservation>&,
            std::shared_ptr<const ExternalDesktopStageRoster>& displacedRoster,
            std::shared_ptr<const ExternalDesktopTaskRootCohort>& displacedRoots,
            std::shared_ptr<const ExternalDesktopTaskRootCohort>& displacedHistory);
    static bool sameNativeRenewalRoster(const ExternalDesktopStageRoster&,const ExternalDesktopStageRoster&);
    bool sealNativeInputEpoch(const ExternalDesktopStageLeaseView&, const WsLive&,
            const WsStage&, const WsBatch& bootstrap);
    bool retainNativePending(const ExternalDesktopStageLeaseView&, const WsLive&, const WsStage&,
            uint64_t intent, const WsBatch* bootstrap);
    bool observeNativeStage(const ExternalDesktopStageLeaseView&, const WsLive&, const WsBatch&);
    bool failNativeStage();
    bool revokeNativeAuthority();
    void supersedeNativeScope(ExternalDesktopStageEpochTicket);
    bool closeNativeFrame(ExternalDesktopStageEpochTicket);
    bool reconcileNativeStage(const ExternalDesktopStageLeaseView&, const WsLive&);
    bool matchesCurrentNativeBatch(const WsBatch&) const;
    bool matchesNativeBatch(const WsBatch&, const ExternalDesktopNativeClaims::Current&) const;
    struct NativeBatch {
        std::vector<WsWindow> windows;
        std::vector<WsStructural> structurals;
        std::vector<std::vector<WsRect>> regions;
        std::vector<std::vector<uint32_t>> paths;
        // Preserve the actual full raw table/order across the core's separate
        // receiver/structural arrays. Never a filtered dispatchable inventory.
        std::vector<std::pair<bool,size_t>> rawOrder;
        WsBatch view{};
    };
    bool projectNativeBatch(const ExternalDesktopNativeClaims::Current&, NativeBatch&) const;
    struct NativeEpoch {
        NativeBatch batch;
        // Deep ownership of the genuine JNI/WMS sample. observed_at is NOT
        // refreshed at route time; only Live.now and the lender clock advance.
        std::vector<uint8_t> package;
        std::vector<int64_t> current,environment;
        std::vector<WsOutput> outputs;
        std::vector<WsClaim> claims;
        std::vector<std::vector<uint32_t>> claimPaths;
        std::vector<WsChild> children;
        std::vector<WsReplica> replicas;
        std::vector<WsBinding> bindings;
        std::vector<WsStructuralBinding> structurals;
        WsLive live{};
        WsStage stage{};
    };
    bool buildNativeEpoch(const ExternalDesktopStageLeaseView&,const WsLive& independentlyObserved,
        const ExternalDesktopNativeClaims::Current&,NativeEpoch&) const;
    static bool ownNativeLive(const WsLive&,NativeEpoch&);
    struct NativeRouteSelection {
        uint32_t route=WS_BLOCKED;
        uint64_t domain=0,windowLifetime=0,presentationLifetime=0;
        int64_t generation=0;
        std::shared_ptr<const ExternalDesktopSubmissionIdentity> window,presentation,producer;
    };
    struct NativeRouteWindow {
        uint64_t claim=0,lifetime=0,root=0;
        gui::WindowInfo stock; // independently published STOCK, never merged-drop subtraction
    };
    struct NativeRoutePublication {
        ExternalDesktopStageLeaseView descriptor{};
        std::vector<int64_t> topology,expires;
        std::unique_ptr<NativeEpoch> epoch;
        ExternalDesktopStageEpochTicket ticket{};
        uint64_t batch=0,sourceRevision=0,registryRevision=0;
        uint64_t intent=0;
        std::vector<NativeRouteWindow> windows;
        std::shared_ptr<ExternalDesktopSubmissionIdentity> producer;
    };
    struct NativeRouteLifetime {
        uint32_t kind=0;uint64_t key=0,lifetime=0;
        std::shared_ptr<const ExternalDesktopSubmissionIdentity> identity;
    };
    std::vector<NativeRouteLifetime> mRouteLifetimes; // bounded, non-evicting NONRESOURCE identities
    uint64_t mRetirementCursor=0;
    bool hasRetirementHistory() const { return !mNativeClaims.mRows.empty(); }
    using NativeRetirement = ExternalDesktopNativeClaims::RetirementPlan;
    std::optional<ExternalDesktopNativeClaims::RetirementPlan> prepareNativeRetirement();
    static std::vector<gui::NativeLifetimeFact> queryNativeRetirement(
            const ExternalDesktopNativeClaims::RetirementPlan&);
    bool finishNativeRetirement(const ExternalDesktopNativeClaims::RetirementPlan&,
            const std::vector<gui::NativeLifetimeFact>&,const std::vector<uint64_t>&,
            std::vector<NativeRouteLifetime>&);
    std::unique_ptr<NativeRoutePublication> mRoutePublication;
    bool publishNativeRoutes(const ExternalDesktopStageLeaseView&,std::unique_ptr<NativeEpoch>);
    NativeRouteSelection routeNativeWindow(const gui::WindowInfo&);
    NativeRouteSelection routeNativeStream(uint64_t window,uint64_t presentation,
            const std::shared_ptr<const ExternalDesktopSubmissionIdentity>& producer);
    bool ownsNativeWindow(const gui::WindowInfo&) const;
    std::optional<gui::WindowInfo> stockNativeWindow(const gui::WindowInfo&) const;
    static bool sameNativeRoutingWindow(const gui::WindowInfo&,const gui::WindowInfo&);
    static bool sameNativeRouteContinuity(const NativeRoutePublication&,const NativeRoutePublication&);
    bool sameNativeRenewalContinuity(const NativeRoutePublication&,const NativeRoutePublication&) const;
    // Nonterminal: blocks every route/publication while retaining the original
    // producer and raw held-stream referents. Only a genuine matching B2 resumes.
    void suspendNativeRoutes();
    std::optional<gui::WindowInfo> suspendedStockWindow(const gui::WindowInfo&,
            const std::shared_ptr<const ExternalDesktopSubmissionIdentity>&);
    void invalidateNativeRoutes(); // no release/ACK/FD/physical teardown inference
    struct RootOperation {
        ExternalDesktopStageEpochTicket ticket{};
        uint64_t serial=0,batch=0;
        int32_t operation=0;
        WsKey key{};
        std::shared_ptr<const ExternalDesktopTaskRootCohort> cohort;
        sp<SurfaceComposerClient> client;
        std::vector<sp<SurfaceControl>> roots;
        std::vector<sp<IBinder>> origins;
        std::vector<int64_t> revisions,presentations;
        std::vector<ExternalDesktopOwnedBlockExpectation> expected;
    }; // owning resources disposed by dispatcher AFTER mLock is released
    bool prepareRootOperation(int32_t,WsKey,RootOperation&);
    bool finishRootOperation(const ExternalDesktopStageLeaseView&,const RootOperation&,bool succeeded);
    // 0 unavailable,1 waiting,2 actual Rust ACTIVE (never an APK APPLIED ACK).
    int32_t progressNativeEpoch(const ExternalDesktopStageLeaseView&,ExternalDesktopStageEpochTicket,
        const std::shared_ptr<const ExternalDesktopStageRoster>&,uint64_t,const WsLive&,std::optional<RootOperation>&);
    uint64_t mRootOperationHigh=0,mRootOperationInFlight=0,mRootOperationBatchBarrier=0;
    std::vector<ExternalDesktopOwnedBlockExpectation> mRouteBlockExpectations;
    uint64_t mRouteBlockBatch=0;
    bool refreshTaggedClaims(const ExternalDesktopStageLeaseView&, uint64_t batch);
    struct DestroyContext {
        void operator()(ExternalDesktopStageContext* context) const { external_desktop_stage_destroy(context); }
    };
    std::unique_ptr<ExternalDesktopStageContext, DestroyContext> mContext;
    struct DestroyEpochOwner {
        void operator()(ExternalDesktopStageEpochOwner* value) const { external_desktop_input_epoch_destroy(value); }
    };
    std::unique_ptr<ExternalDesktopStageEpochOwner, DestroyEpochOwner> mEpochOwner;
    Receipt mReceipt;
    int64_t mNativeArrivalHighWater = -1;
    // Owned evidence only, not active state or an object-retirement proof.
    // Read ONLY with the owning dispatcher's mLock held. Replaced atomically
    // after validating ALL rows; any rejected callback clears this evidence.
    // WindowInfo's owning copy preserves Binder references and COW Regions.
    // Do not publish it to Choreographer or infer authority from tags/vsyncId.
    std::optional<gui::WindowInfosUpdate> mCompleteBatch;
    // Bounded latest WMS observations ONLY, separately from the latest SF batch.
    // Replacing/clearing these references releases NO claims, queues or tails.
    std::shared_ptr<const ExternalDesktopStageRoster> mWmsRoster;
    ExternalDesktopStageEpochTicket mWmsRosterTicket{};
    // Receipt count only. Not a lease, stage epoch or activation acknowledgement.
    uint64_t mAuthorityBorrows = 0;
    // Actual source-object/window identities feed the private native issuer.
    // Neither observation storage nor claim issuance submits/activates input.
    ExternalDesktopSourceLifetimes mSourceLifetimes;
    ExternalDesktopNativeClaims mNativeClaims; // default-off issuer/storage only; NO submit/drop/route
    std::optional<ExternalDesktopNativeClaims::TaggedPlan> mPendingTagged;
    std::optional<ExternalDesktopNativeClaims::TaggedPlan> mActiveTagged; // SAME successful B2 table, not a new intent
    uint64_t mNativeIntent=0; // actual Context output only, never Java/APK scalar
    struct NativeIntentFrame {
        int64_t atlas=0,expiresAt=0;
        int32_t uid=0,user=0;
        std::vector<int64_t> current,desired;
        std::vector<WsOrigin> origins;
        std::vector<WsSeam> seams;
        std::vector<WsOutput> outputs; // ORIGINAL admitted outputs; fresh observations stay separate
        std::vector<WsPresentation> presentations; // actual admitted Rust proof, not C++ intersections
    };
    std::optional<NativeIntentFrame> mNativeIntentFrame; // ORIGINAL admitted values, never Live.snapshot
    // Reservation is issued by this real EpochOwner. Context ticket owns all
    // real Rust intent/pending/active output, including after B2 clears plan.
    std::optional<ExternalDesktopStageEpochTicket> mNativeReservation,mNativeContextTicket;
    std::optional<ExternalDesktopStageEpochTicket> mNativeClosedFrame;
    bool mNativeSuperseding=false; // native-inert until real admit_intent accepts
    // Bounded private receipt of a REAL postchecked renew_intent + EpochOwner
    // reservation. No SC/Java packet/FD ownership, borrowed spans or authority.
    // It bridges exactly ONE old publication to ONE new binding, not arbitrary
    // epoch/ticket/expiry differences. Clearing it never emits a release.
    struct NativeRenewal {
        struct Root {
            wp<IBinder> root,source,parent;
            int32_t rootId=0,destination=0,user=0,clientUid=0;
            ExternalDesktopStageObject producer,frame,submission,task;
            std::array<int64_t,4> endpoints{};
            std::array<int32_t,8> geometry{};
            int64_t serial=0;
        };
        ExternalDesktopStageEpochTicket oldTicket{},ticket{};
        WsKey oldKey{};
        uint64_t intent=0,batch=0;
        int64_t oldExpires=0,expiresAt=0;
        ExternalDesktopStageLeaseView descriptor{};
        std::vector<int64_t> topology,expires;
        std::vector<ExternalDesktopNativeClaims::Row> claims; // complete nonresource lifetime history
        std::vector<ExternalDesktopOwnedBlockExpectation> originalBlocks; // exact last genuinely accepted old B2
        std::vector<Root> roots;
        std::shared_ptr<ExternalDesktopSubmissionIdentity> producer;
        bool rosterObserved=false,rootsObserved=false;
    };
    std::unique_ptr<NativeRenewal> mNativeRenewal;
    bool isNativeRenewalTicket(ExternalDesktopStageEpochTicket) const;
    bool sameNativeRenewalClaims() const;
    bool sameNativeRenewalRoots(const ExternalDesktopTaskRootCohort&,bool fresh) const;
    bool suspendedNativeRenewalStock(const std::vector<ExternalDesktopOwnedBlockExpectation>&,int64_t now) const;
    // Bounded current + historical real-object observations. No same-frame
    // root is overwritten by another destination/shared channel. NO eviction,
    // physical retirement; history can backpressure. Issuer storage is separate.
    std::shared_ptr<const ExternalDesktopTaskRootCohort> mWmsRoots, mWmsRootHistory;
    std::vector<ExternalDesktopRootJoinReceipt> mRootJoins;
    std::vector<ExternalDesktopRootJoinReceipt> prepareRootJoins(const ExternalDesktopTaskRootCohort*, int64_t now,
            const std::vector<ExternalDesktopOwnedBlockExpectation>* expected = nullptr) const;
    void joinRootObservation(int64_t now);
    void observeCurrentBatch(bool joinRoots);
};

} // namespace android::inputdispatcher
#endif
