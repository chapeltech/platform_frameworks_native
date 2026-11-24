#include "ExternalDesktopStageOwner.h"

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopNativeRootFacts.h"
#include <limits>
#include <unordered_set>
#include <utils/SystemClock.h>

namespace android::inputdispatcher {
namespace {
// Match the frozen native_stage work bounds. These bound private inspection;
// overflow never truncates/changes the ordinary Android window maps or input.
constexpr size_t kMaxRawWindows = 8192;
constexpr size_t kMaxRectsPerWindow = 1024;
constexpr size_t kMaxRects = 32768;
// Names are retained too. Bound all three owned strings without truncation;
// these bounds affect private evidence only, never ordinary dispatch maps.
constexpr size_t kMaxStringBytes = 1024 * 1024;
static_assert(gui::WindowInfo::kMaxPreViewportRegionRects == kMaxRectsPerWindow);
static_assert(gui::WindowInfo::kMaxCloneLineageDepth == 50);

size_t rectCount(const Region& region) {
    return region.isEmpty() ? 0 : static_cast<size_t>(region.end() - region.begin());
}
bool sameRootScope(const ExternalDesktopTaskRootCohort* a,const ExternalDesktopTaskRootCohort& b) {
    if (!a || a->records.size()!=b.records.size()) return false;
    for (const auto& next:b.records) {
        if (!next.observed || !next.observed->root) return false;
        const auto old=std::find_if(a->records.begin(),a->records.end(),[&](const auto& entry) {
            return entry.observed && entry.observed->root &&
                    entry.observed->root->getHandle()==next.observed->root->getHandle();
        });
        if (old==a->records.end()) return false;
        const auto& x=*old->observed;const auto& y=*next.observed;
        if (old->ticket.owner!=next.ticket.owner || old->ticket.epoch!=next.ticket.epoch ||
                old->ticket.generation!=next.ticket.generation || old->expires!=next.expires ||
                old->serial!=next.serial || old->user!=next.user || old->clientUid!=next.clientUid ||
                old->destination!=next.destination || x.producer!=y.producer || x.frame!=y.frame ||
                x.submission!=y.submission || x.task!=y.task || x.geometry!=y.geometry ||
                x.endpoints!=y.endpoints || !x.source || !y.source || !x.destinationParent || !y.destinationParent ||
                x.source->getHandle()!=y.source->getHandle() ||
                x.destinationParent->getHandle()!=y.destinationParent->getHandle()) return false;
    }
    return true;
}
} // namespace

ExternalDesktopStageOwner::ExternalDesktopStageOwner()
      : mContext(external_desktop_stage_create()),
        mEpochOwner(external_desktop_input_epoch_create(android::elapsedRealtimeNano())) {}

std::optional<ExternalDesktopNativeClaims::RetirementPlan> ExternalDesktopStageOwner::prepareNativeRetirement() {
    if(!mContext || !mCompleteBatch || !mReceipt.hasCompleteBatch || mNativeRenewal ||
            mPendingTagged || mRootOperationInFlight)return {};
    auto plan=mNativeClaims.retirementPlan(*mCompleteBatch,mReceipt.observations,mRetirementCursor);
    if(!plan && mRetirementCursor){mRetirementCursor=0;
        plan=mNativeClaims.retirementPlan(*mCompleteBatch,mReceipt.observations,0);}
    if(plan)mRetirementCursor=plan->queries.back().claim;
    return plan;
}

std::vector<gui::NativeLifetimeFact> ExternalDesktopStageOwner::queryNativeRetirement(
        const ExternalDesktopNativeClaims::RetirementPlan& plan) {
    // ONLY the off-lock native I/O entry calls this method. A token owns
    // nonresource state, not the Layer/Snapshot/Connection being queried.
    std::vector<gui::NativeLifetimeFact> results;results.reserve(plan.queries.size());
    for(const auto& query:plan.queries){gui::NativeLifetimeFact fact;
        const auto status=query.kind==WS_ROOT_CLAIM ?
            gui::queryNativeRootResourceGroupFact(query.token,fact):
            gui::queryNativeLifetimeFact(query.token,fact);
        if(status!=OK)fact={};
        results.push_back(std::move(fact));
    }
    return results;
}

bool ExternalDesktopStageOwner::finishNativeRetirement(const ExternalDesktopNativeClaims::RetirementPlan& plan,
        const std::vector<gui::NativeLifetimeFact>& facts,const std::vector<uint64_t>& drained,
        std::vector<NativeRouteLifetime>& retired) {
    if(!retired.empty() || !mContext || !mCompleteBatch || !mReceipt.hasCompleteBatch ||
            mNativeRenewal || mPendingTagged || mRootOperationInFlight)return false;
    // Rejoin the CURRENT whole publication, not a cached query-time callback.
    // A newer batch is allowed only in the SAME actual SF publication domain;
    // identities, registry, absence and every physical barrier are rechecked.
    auto prepared=mNativeClaims.prepareRetirement(plan,facts,*mCompleteBatch,mReceipt.observations,drained);
    if(!prepared)return false;
    for(const auto& release:prepared->releases)if(mActiveTagged &&
            std::binary_search(mActiveTagged->b0.claims.begin(),mActiveTagged->b0.claims.end(),release.claim.key))return false;
    auto source=mSourceLifetimes.prepareRetiredClones(
            {prepared->releases.data(),prepared->releases.size()});
    if(!source)return false;
    auto history=std::make_shared<ExternalDesktopTaskRootCohort>(mWmsRootHistory?*mWmsRootHistory:ExternalDesktopTaskRootCohort{});
    std::vector<NativeRouteLifetime> removed;removed.reserve(prepared->releases.size());
    for(const auto& release:prepared->releases){const auto& claim=release.claim;
        const auto route=std::find_if(mRouteLifetimes.begin(),mRouteLifetimes.end(),[&](const auto& value){
            return value.kind==claim.kind && value.key==claim.key && value.lifetime==claim.lifetime;});
        removed.push_back(route==mRouteLifetimes.end()?NativeRouteLifetime{claim.kind,claim.key,claim.lifetime,{}}:*route);
        if(claim.kind!=WS_ROOT_CLAIM)continue;
        const auto row=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),
                [&](const auto& value){return value.value.key==claim.key;});
        if(row==mNativeClaims.mRows.end() || !row->physical)return false;
        const auto physical=row->physical;
        std::erase_if(history->records,[&](const auto& value){return value.physical &&
            value.physical->root==physical->root && value.physical->source==physical->source &&
            value.physical->rootId==physical->rootId && value.physical->owner==physical->owner &&
            value.physical->generation==physical->generation;});
    }
    // All allocations and potentially refusing native checks precede the
    // shared Rust owner's atomic decision. Afterwards only no-throw swaps,
    // erases and scalar bookkeeping are performed under the SAME mLock.
    if(!mNativeClaims.commitRetirement(std::move(*prepared),mContext.get()))return false;
    mSourceLifetimes.commitRetiredClones(std::move(*source));
    mWmsRootHistory=std::move(history);
    if(mActiveTagged)mActiveTagged->b0.registryRevision=mNativeClaims.mRevision;
    std::erase_if(mRouteLifetimes,[&](const auto& value){return std::any_of(removed.begin(),removed.end(),
            [&](const auto& old){return old.key==value.key && old.kind==value.kind && old.lifetime==value.lifetime;});});
    retired.swap(removed);
    // A global registry change is not a fresh Current/Live/route grant. Keep
    // an unchanged held producer nonterminal until real WMS/full-batch refresh.
    suspendNativeRoutes();return true;
}

bool ExternalDesktopStageOwner::reserveSubmission(const ExternalDesktopStageLeaseView& view,
        ExternalDesktopStageEpochTicket* out, std::shared_ptr<const ExternalDesktopStageRoster>* displacedRoster,
        std::shared_ptr<const ExternalDesktopTaskRootCohort>* displacedRoots) {
    if (!mContext || !mEpochOwner || !out ||
        (mWmsRoster && (!displacedRoster || *displacedRoster)) ||
        (mWmsRoots && (!displacedRoots || *displacedRoots))) return false;
    struct Call { ExternalDesktopStageEpochOwner* owner; const ExternalDesktopStageLeaseView* view;
                  ExternalDesktopStageEpochTicket* out; } call{mEpochOwner.get(), &view, out};
    const auto receive = [](void* data, const ExternalDesktopStageAuthority* authority) -> uint8_t {
        if (!authority) return 0;
        auto& borrowed = *static_cast<Call*>(data);
        // Ordinal only within this owner; equal/backward clock values refuse.
        return external_desktop_input_epoch_reserve(borrowed.owner, android::elapsedRealtimeNano(),
                                         borrowed.view, borrowed.out);
    };
    const auto now = []() -> int64_t { return android::elapsedRealtime(); };
    if (external_desktop_input_with_wms_authority(&view, &call, now, receive) != 1) return false;
    // New current frame invalidates only the previous binding. WMS still
    // owns physical mirrors until their actual producer/removal fences.
    mSourceLifetimes.invalidate(mWmsRosterTicket);
    if (displacedRoster) *displacedRoster=std::move(mWmsRoster);
    if (displacedRoots) *displacedRoots=std::move(mWmsRoots);
    mWmsRosterTicket={}; mRootJoins.clear();
    mNativeClaims.invalidateScope();
    supersedeNativeScope(*out); // ordinary frame replacement is NOT generation retirement
    return true; // caller disposes outside mLock, never a claim/history release
}

bool ExternalDesktopStageOwner::beginSubmission(const ExternalDesktopStageLeaseView& view,
                                     ExternalDesktopStageEpochTicket ticket,
                                     int64_t displayToken, int64_t serial) {
    if (!mContext || !mEpochOwner) return false;
    struct Call { ExternalDesktopStageEpochOwner* owner; const ExternalDesktopStageLeaseView* view;
                  ExternalDesktopStageEpochTicket ticket; int64_t token; int64_t serial;
    } call{mEpochOwner.get(), &view, ticket, displayToken, serial};
    const auto receive = [](void* data, const ExternalDesktopStageAuthority* authority) -> uint8_t {
        if (!authority) return 0;
        auto& borrowed = *static_cast<Call*>(data);
        return external_desktop_input_epoch_begin(borrowed.owner, borrowed.view, borrowed.ticket, borrowed.token, borrowed.serial);
    };
    const auto now = []() -> int64_t { return android::elapsedRealtime(); };
    return external_desktop_input_with_wms_authority(&view, &call, now, receive) == 1;
}

bool ExternalDesktopStageOwner::noteSubmission(const ExternalDesktopStageLeaseView& view,
                                    ExternalDesktopStageEpochTicket ticket, int64_t displayToken,
                                    int64_t serial, bool inputReported) {
    if (!mContext || !mEpochOwner) return false;
    struct Call { ExternalDesktopStageEpochOwner* owner; const ExternalDesktopStageLeaseView* view;
                  ExternalDesktopStageEpochTicket ticket; int64_t token; int64_t serial; bool input;
    } call{mEpochOwner.get(), &view, ticket, displayToken, serial, inputReported};
    const auto receive = [](void* data, const ExternalDesktopStageAuthority* authority) -> uint8_t {
        if (!authority) return 0;
        auto& borrowed = *static_cast<Call*>(data);
        return external_desktop_input_epoch_note(borrowed.owner, borrowed.view, borrowed.ticket, borrowed.token, borrowed.serial,
                                      borrowed.input ? 1 : 0);
    };
    const auto now = []() -> int64_t { return android::elapsedRealtime(); };
    return external_desktop_input_with_wms_authority(&view, &call, now, receive) == 1;
}

bool ExternalDesktopStageOwner::validRosterObservations(const ExternalDesktopStageLeaseView& view,
        const ExternalDesktopStageRoster& roster) const {
    if (!roster.references || roster.tasks.size() > 128 || roster.windows.size() > kMaxRawWindows ||
            view.topology_snapshot.len < 14 || view.topology_snapshot.len > 86 ||
            !view.topology_snapshot.data) return false;
    const auto* topology = view.topology_snapshot.data;
    const int64_t displays = topology[3];
    if (displays < 1 || displays > 9) return false;
    const auto count = static_cast<size_t>(displays);
    if (view.topology_snapshot.len != 5 + count * 9) return false;
    std::unordered_set<int32_t> layers;
    size_t next = 0;
    for (const auto& task : roster.tasks) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) found |= topology[5 + i * 9] == task.displayToken;
        if (!found || task.uid < 0 || task.uid / 100000 != view.user || !task.surface ||
                !task.surface->isValid() || task.layerId <= 0 || task.surface->getLayerId() != task.layerId ||
                !layers.insert(task.layerId).second || task.start != next || task.end < task.start ||
                task.end - task.start > 256 || task.end > roster.windows.size()) return false;
        next = task.end;
    }
    if (next != roster.windows.size()) return false;
    for (const auto& child : roster.windows) {
        if (!child.client || (child.state & ~63u) != 0 ||
                (child.surface ? !child.surface->isValid() || child.layerId <= 0 ||
                    child.surface->getLayerId() != child.layerId || !layers.insert(child.layerId).second :
                    child.layerId != 0)) return false;
    }
    return true;
}

bool ExternalDesktopStageOwner::observeRoster(const ExternalDesktopStageLeaseView& view,
        ExternalDesktopStageEpochTicket ticket, int64_t token, int64_t serial,
        const std::shared_ptr<const ExternalDesktopStageRoster>& candidate, int32_t operation,
        std::shared_ptr<const ExternalDesktopStageRoster>& displaced,
        std::shared_ptr<const ExternalDesktopTaskRootCohort>* displacedRoots) {
    if (!mContext || !mEpochOwner || !candidate || displaced || operation < 0 || operation > 2 ||
            !validRosterObservations(view, *candidate)) return false;
    const auto& roster = *candidate;
    const bool sameTicket = mWmsRosterTicket.owner == ticket.owner &&
            mWmsRosterTicket.epoch == ticket.epoch && mWmsRosterTicket.generation == ticket.generation;
    // Renewal observations have a distinct atomic complete-binding entry.
    // Publishing only its roster would create an unlocked stock-proof gap.
    if (isNativeRenewalTicket(ticket)) return false;
    if (!sameTicket && mWmsRoots && (!displacedRoots || *displacedRoots)) return false;
    if (sameTicket && mWmsRoster && !mWmsRoster->references->same(*roster.references)) return false;
    if (operation != 0 && (!sameTicket || !mWmsRoster)) return false;
    auto identities = mSourceLifetimes.prepare(view, ticket, roster);
    if (!identities) return false; // Complete validation/backpressure BEFORE any publication.
    // JNI already owns this complete immutable candidate before mLock/borrow.
    // A refusal leaves the prior observation and all its references intact.
    struct Call { ExternalDesktopStageEpochOwner* owner; const ExternalDesktopStageLeaseView* view;
        ExternalDesktopStageEpochTicket ticket; int64_t token; int64_t serial; int32_t operation;
    } call{mEpochOwner.get(), &view, ticket, token, serial, operation};
    const auto receive = [](void* data, const ExternalDesktopStageAuthority* authority) -> uint8_t {
        if (!authority) return 0;
        auto& borrowed = *static_cast<Call*>(data);
        return borrowed.operation == 0 ?
                external_desktop_input_epoch_begin(borrowed.owner, borrowed.view, borrowed.ticket,
                                        borrowed.token, borrowed.serial) :
                external_desktop_input_epoch_note(borrowed.owner, borrowed.view, borrowed.ticket,
                    borrowed.token, borrowed.serial, borrowed.operation == 2 ? 1 : 0);
    };
    const auto now = []() -> int64_t { return android::elapsedRealtime(); };
    if (external_desktop_input_with_wms_authority(&view, &call, now, receive) != 1) return false;
    // mLock stayed held and the borrowed callback cannot reenter this owner;
    // the exact prepared revision therefore remains current through postcheck.
    if (!mSourceLifetimes.commit(std::move(*identities))) return false;
    suspendNativeRoutes();
    mNativeClaims.invalidate();
    // Borrow postchecks complete. The dispatcher retains displaced refs until
    // AFTER it unlocks: SC destructors can perform Binder flushes.
    displaced = std::move(mWmsRoster);
    if (!sameTicket) {
        if (!mNativeContextTicket || !ExternalDesktopNativeClaims::sameTicket(*mNativeContextTicket,ticket))
            supersedeNativeScope(ticket);
        mNativeClaims.invalidateScope();
        if (displacedRoots) *displacedRoots=std::move(mWmsRoots);
        mRootJoins.clear();
    }
    mWmsRoster = candidate;
    mWmsRosterTicket = ticket;
    observeCurrentBatch(false);
    return true; // Observed/bookkept, never staged/applied/active/retired.
}

bool ExternalDesktopStageOwner::closeSubmission(ExternalDesktopStageEpochTicket ticket) {
    // Actual JNI NativeImpl/frame identity and mLock still mandatory; missing
    // authority must NOT prevent terminal invalidation. This drains no claim.
    const bool closed = mEpochOwner && external_desktop_input_epoch_close(mEpochOwner.get(), ticket) == 1;
    if (closed && closeNativeFrame(ticket)) {mSourceLifetimes.invalidate(ticket);mNativeClaims.invalidate(ticket);}
    return closed;
}

bool ExternalDesktopStageOwner::observeRoots(const ExternalDesktopStageLeaseView& view, ExternalDesktopStageEpochTicket ticket,
        const std::vector<std::shared_ptr<const ExternalDesktopTaskRootObservation>>& roots,
        std::shared_ptr<const ExternalDesktopTaskRootCohort>& displaced,
        std::shared_ptr<const ExternalDesktopTaskRootCohort>& displacedHistory,
        const std::shared_ptr<const ExternalDesktopStageRoster>& expectedRoster) {
    if (!mContext || !mEpochOwner || !mWmsRoster || roots.size()>128*8 ||
        (!roots.empty() && !roots.front()) ||
        displaced || displacedHistory || mWmsRosterTicket.owner!=ticket.owner ||
        mWmsRosterTicket.epoch!=ticket.epoch || mWmsRosterTicket.generation!=ticket.generation) return false;
    const auto& expected=expectedRoster ? expectedRoster : (roots.empty() ? expectedRoster : roots.front()->roster);
    if (!expected || !expected->references || !mWmsRoster->references->same(*expected->references) ||
        expected->nativeOwnerIdentity!=mWmsRoster->nativeOwnerIdentity || !validRosterObservations(view,*expected)) return false;
    std::vector<ExternalDesktopTaskRootRecord> expectedRecords;expectedRecords.reserve(roots.size());
    for (const auto& root:roots) {
        if (!mContext || !mEpochOwner || !root || !root->roster || !root->roster->references || !root->packet ||
                !root->producer || !root->submission || !root->task || root->serial<=0 || !mWmsRoster ||
                mWmsRosterTicket.owner!=ticket.owner || mWmsRosterTicket.epoch!=ticket.epoch ||
                mWmsRosterTicket.generation!=ticket.generation ||
                !mWmsRoster->references->same(*root->roster->references) ||
                root->roster->nativeOwnerIdentity!=mWmsRoster->nativeOwnerIdentity ||
                !validRosterObservations(view,*root->roster) || !mSourceLifetimes.current()) return false;
        const auto same=[](const sp<SurfaceControl>& a,const sp<SurfaceControl>& b) {
            return a && b && a->getHandle()==b->getHandle();
        };
        if (!root->root || !root->source || !root->destinationParent ||
                !root->root->isValid() || !root->source->isValid() || !root->destinationParent->isValid() ||
                root->root->getLayerId()<=0 || same(root->root,root->source) ||
                same(root->root,root->destinationParent) || same(root->source,root->destinationParent)) return false;
        const auto task=std::find_if(root->roster->tasks.begin(),root->roster->tasks.end(),
            [&](const auto& t) { return t.taskIdentity==root->task && same(t.surface,root->source) && t.displayToken==root->endpoints[0]; });
        const int64_t token=root->endpoints[2];
        if (task==root->roster->tasks.end() ||
                root->endpoints[0]!=root->endpoints[1] || root->endpoints[2]!=root->endpoints[3] ||
                root->endpoints[0]==root->endpoints[2]) return false;
        for (const auto& t:root->roster->tasks) if (same(t.surface,root->root)) return false;
        for (const auto& w:root->roster->windows) if (same(w.surface,root->root)) return false;
        if (!view.topology_snapshot.data || !view.expires_by_display.data || view.topology_snapshot.len<14) return false;
        const int64_t count=view.topology_snapshot.data[3];
        if (count<1 || count>9 || view.topology_snapshot.len!=5+static_cast<size_t>(count)*9 ||
                view.expires_by_display.len!=static_cast<size_t>(count)) return false;
        int32_t destination=-1;bool source=false;int64_t expires=INT64_MAX;
        for (size_t i=0;i<static_cast<size_t>(count);++i) {
            const auto* row=view.topology_snapshot.data+5+i*9;
            expires=std::min(expires,view.expires_by_display.data[i]);
            source|=row[0]==root->endpoints[0];
            if (row[0]==token) {
                if (row[1]<0 || row[1]>INT32_MAX || root->geometry[4]<0 || root->geometry[5]<0 ||
                        root->geometry[6]>row[4] || root->geometry[7]>row[5]) return false;
                destination=static_cast<int32_t>(row[1]);
            }
        }
        if (!source || destination<0 || view.observed_at<0 || view.observed_at>=expires) return false;
        const auto& g=root->geometry;
        for (int32_t v:g) if (v< -32768 || v>32768) return false;
        if (g[0]<0 || g[1]<0 || g[2]<=g[0] || g[3]<=g[1] ||
                int64_t{g[2]}-g[0]!=int64_t{g[6]}-g[4] || int64_t{g[3]}-g[1]!=int64_t{g[7]}-g[5]) return false;
        if (!roots.front() || root->frame!=roots.front()->frame) return false;
        auto sharedRoot=std::make_shared<ExternalDesktopTaskRootObservation>(*root);
        sharedRoot->roster=mWmsRoster;
        expectedRecords.push_back({ticket,std::move(sharedRoot),destination,expires,root->serial,
                view.user,view.expected_client_uid});
    }
    auto prepared=prepareExternalDesktopRootBatch(mWmsRootHistory.get(),expectedRecords);
    if (!prepared) return false; // A valid A + invalid B publishes neither.
    auto candidate=std::make_shared<const ExternalDesktopTaskRootCohort>(std::move(prepared->current));
    auto history=std::make_shared<const ExternalDesktopTaskRootCohort>(std::move(prepared->history));
    const bool renewal=isNativeRenewalTicket(ticket);
    if (renewal && (!mNativeRenewal->rosterObserved || !sameNativeRenewalRoots(*candidate,true) ||
            !sameNativeRenewalClaims())) return failNativeStage();
    auto joins=prepareRootJoins(candidate.get(),android::elapsedRealtime());
    for (size_t i=0;i<joins.size();++i) if (joins[i].state==ExternalDesktopRootJoin::REJECTED ||
            joins[i].state==ExternalDesktopRootJoin::UNRESOLVED_ANCESTOR) {
        // An exact already-issued physical root may still carry its previous
        // B2 tag before the successor can re-PARK it. Accept only its CURRENT
        // authenticated WMS observation, never its rejected SF join/claims.
        // Recycled IDs/foreign roots/lifetimes/source/path/lease differences
        // fail the same existing issuer's allowNew=false lookup below.
        if (!mNativeIntent || !mNativeIntentFrame || !mNativeReservation ||
                !ExternalDesktopNativeClaims::sameTicket(*mNativeReservation,ticket) ||
                !mNativeContextTicket || mNativeContextTicket->owner!=ticket.owner ||
                mNativeContextTicket->generation!=ticket.generation || !mCompleteBatch ||
                !mCompleteBatch->layerSourcesComplete || i>=candidate->records.size()) return false;
        const auto& record=candidate->records[i];bool exact=false;
        for (const auto& facts:mCompleteBatch->layerSources) {
            if (std::find(facts.mirrorRoots.begin(),facts.mirrorRoots.end(),
                    static_cast<uint32_t>(record.observed->root->getLayerId()))==facts.mirrorRoots.end()) continue;
            const auto key=mNativeClaims.rootClaim(record,facts,mSourceLifetimes,false);
            if (!key) return false;
            exact=true;
        }
        if (!exact) return false;
        // Leave joins[i] REJECTED/unavailable. Only an actual later untagged
        // PARKED batch can publish Current, issue new descendants, or seal.
    }
    auto structural=mSourceLifetimes.prepareStructural(joins);
    if (!structural) return false;
    const auto receive=[](void*,const ExternalDesktopStageAuthority* authority)->uint8_t { return authority ? 1:0; };
    const auto now=[]()->int64_t { return android::elapsedRealtime(); };
    if (external_desktop_input_with_wms_authority(&view,this,now,receive)!=1) return false;
    if (!mSourceLifetimes.commitStructural(std::move(*structural))) return false;
    // Every allocation, expected member and join prepared BEFORE the actual
    // authority postcheck. All publication swaps below are no-throw.
    if (!sameRootScope(mWmsRoots.get(),*candidate)) {
        if (!renewal && (mPendingTagged || mActiveTagged || (mNativeContextTicket && !mNativeIntent)))
            supersedeNativeScope(ticket);
        mNativeClaims.invalidateScope();
    }
    displaced=std::move(mWmsRoots);mWmsRoots.swap(candidate);
    displacedHistory=std::move(mWmsRootHistory);mWmsRootHistory.swap(history);
    mRootJoins.swap(joins);
    if (renewal) mNativeRenewal->rootsObserved=true;
    // Actual trusted WMS owner is the caller, never an APK/scalar issuer.
    // Observation acceptance is distinct from claim availability. An incomplete
    // SF batch/second authority refusal leaves CURRENT claims unavailable.
    issueNativeClaims(view);
    if (renewal && !mNativeRenewal) return false; // terminal mismatch in B0 preparation cannot become binding success
    return true; // Complete CURRENT observation set only, NOT SF acceptance.
}

bool ExternalDesktopStageOwner::issueNativeClaims(const ExternalDesktopStageLeaseView& view,
        const std::vector<ExternalDesktopOwnedBlockExpectation>* expected) {
    suspendNativeRoutes();
    mNativeClaims.invalidate();
    if (!mWmsRoots || !mCompleteBatch || !mSourceLifetimes.current() || !borrowAuthority(view)) return false;
    auto prepared=mNativeClaims.prepare(view,*mWmsRoots,*mCompleteBatch,mReceipt.observations,mSourceLifetimes,expected);
    if (!prepared) return false;
    // prepare already burns all keys and runs the same Rust registry predicate.
    // Even this fresh postcheck's refusal MUST NOT rollback/reuse those values.
    const auto receive=[](void*,const ExternalDesktopStageAuthority* authority)->uint8_t {return authority ? 1:0;};
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,this,now,receive)!=1) return false;
    const bool published=mNativeClaims.publish(std::move(*prepared),mSourceLifetimes,android::elapsedRealtime());
    if (mNativeRenewal && !sameNativeRenewalClaims()) return failNativeStage();
    return published;
    // Still NO fake Live, StageMachine submit, owned-drop clear or route call.
}

std::optional<ExternalDesktopStageOwner::BatchWakeup> ExternalDesktopStageOwner::batchWakeup() const {
    // An unsupported latest batch stays unavailable; never replay an older one.
    if (!mContext || !mWmsRoster || !mReceipt.hasCompleteBatch ||
            mReceipt.observations == 0 ||
            mReceipt.observations > static_cast<uint64_t>(INT64_MAX) ||
            mWmsRosterTicket.owner == 0 ||
            mWmsRosterTicket.owner > static_cast<uint64_t>(INT64_MAX) ||
            mWmsRosterTicket.epoch <= 0 || mWmsRosterTicket.generation <= 0) return {};
    return BatchWakeup{mWmsRosterTicket, mReceipt.observations};
}

bool ExternalDesktopStageOwner::refreshNativeClaims(const ExternalDesktopStageLeaseView& view,
        ExternalDesktopStageEpochTicket ticket, const std::shared_ptr<const ExternalDesktopStageRoster>& roster,
        uint64_t batch,const std::vector<ExternalDesktopOwnedBlockExpectation>* expected) {
    // Old hints must not invalidate a successor's CURRENT registry. A hint is
    // never sufficient: the real ticket/roster and fresh WMS lender are required.
    if (!roster || !roster->references || !mWmsRoster || !mWmsRoster->references ||
            !mWmsRoots || batch == 0 || batch != mReceipt.observations ||
            !mReceipt.hasCompleteBatch || !mCompleteBatch ||
            ticket.owner != mWmsRosterTicket.owner || ticket.epoch != mWmsRosterTicket.epoch ||
            ticket.generation != mWmsRosterTicket.generation ||
            !mWmsRoster->references->same(*roster->references) ||
            mWmsRoster->nativeOwnerIdentity != roster->nativeOwnerIdentity ||
            !validRosterObservations(view, *roster)) return false;
    // Public/JNI wakeups cannot supply expected keys or tagged expectations.
    if (expected) return false;
    if (mNativeSuperseding) {
        if (!borrowAuthority(view)) return revokeNativeAuthority();
        // Root publication and native full-batch arrival have no fixed order.
        // Permit ONLY existing strict untagged/PARKED discovery for this exact
        // current ticket/roster. This publishes genuine B0 claims, not a Stage;
        // keep the old Rust scope tracked AND native-inert until real reseal.
        return issueNativeClaims(view);
    }
    // A hint has no independently applied/complete Live witness. The private
    // observe path alone may classify a post-submit tagged candidate.
    if (mPendingTagged || mActiveTagged) {
        const auto& plan=mPendingTagged?*mPendingTagged:*mActiveTagged;
        if (view.observed_at>=plan.expiresAt) return failNativeStage();
        if (!borrowAuthority(view)) return revokeNativeAuthority();
        return false;
    }
    mNativeClaims.invalidate();
    // No SF transaction, callback, timer, StageLive, submit or drop change here.
    return issueNativeClaims(view,expected);
}

bool ExternalDesktopStageOwner::submitNativeStage(const ExternalDesktopStageLeaseView& view,
        const WsLive& live,const WsStage& stage) {
    return retainNativePending(view,live,stage,0,nullptr);
}

bool ExternalDesktopStageOwner::admitNativeIntent(const ExternalDesktopStageLeaseView& view,const WsIntent& intent) {
    const auto target=mNativeReservation.value_or(mWmsRosterTicket);
    if (!mContext || !target.owner || target.epoch<=0 || target.generation!=intent.lease_generation ||
            target.generation!=view.generation || intent.now!=view.observed_at) return false;
    if (mNativeClosedFrame && ExternalDesktopNativeClaims::sameTicket(target,*mNativeClosedFrame)) return false;
    if (mNativeContextTicket && (target.owner!=mNativeContextTicket->owner ||
            ((mNativeSuperseding || !mNativeIntent || mPendingTagged) && target.epoch<=mNativeContextTicket->epoch))) return false;
    if (intent.current.len<14 || intent.current.len>1622 || !intent.current.data ||
            intent.desired.len<7 || intent.desired.len>5767 || !intent.desired.data ||
            intent.origins.len>9 || !intent.origins.len || !intent.origins.data ||
            intent.seams.len>1024 || (intent.seams.len && !intent.seams.data) ||
            !intent.outputs.len || intent.outputs.len>9 || !intent.outputs.data) return false;
    NativeIntentFrame original;
    original.atlas=intent.atlas_revision;original.expiresAt=intent.expires_at;
    original.uid=intent.lease_uid;original.user=intent.lease_user;
    original.current.assign(intent.current.data,intent.current.data+intent.current.len);
    original.desired.assign(intent.desired.data,intent.desired.data+intent.desired.len);
    original.origins.assign(intent.origins.data,intent.origins.data+intent.origins.len);
    original.outputs.assign(intent.outputs.data,intent.outputs.data+intent.outputs.len);
    if (intent.seams.len) original.seams.assign(intent.seams.data,intent.seams.data+intent.seams.len);
    original.presentations.resize(1024); // bounded writable output, owned before lender
    struct Call {ExternalDesktopStageContext* context;const WsIntent* intent;WsResult result{};bool ran=false;
        WsPresentation* rows;size_t count=0;bool admitted=false,readback=false;uint64_t handle=0;
    } call{mContext.get(),&intent,{},false,original.presentations.data()};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);
        if (!authority) return 0;
        c.ran=external_desktop_stage_admit_intent(c.context,authority,c.intent,&c.result)==1;
        c.admitted=c.ran && c.result.refusal==WS_NO_REFUSAL && !c.result.revoked &&
            !c.result.has_pending && c.result.intent_handle;
        if (!c.admitted) return 0;
        c.handle=c.result.intent_handle;
        c.readback=external_desktop_stage_intent_plan(c.context,authority,c.intent,c.handle,c.rows,1024,&c.count,&c.result)==1 &&
            c.result.refusal==WS_NO_REFUSAL && !c.result.revoked && !c.result.has_pending &&
            c.result.intent_handle==c.handle && c.count<=1024;
        return c.readback;
    };
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,&call,now,receive)!=1) {
        // Invalid proposals preserve the still-owned previous Rust scope.
        // A successful call whose lender postcheck failed is terminal instead.
        if (call.readback) {
            mNativeContextTicket=target;return revokeNativeAuthority(); // real lender postcheck loss
        } else if (call.ran && call.result.revoked) {
            return revokeNativeAuthority(); // real core already observed authority loss
        }
        if (call.admitted) {
            // A value/output refusal is not authority loss. Track the actual
            // admitted context but keep this frame inert until fresh recovery.
            mNativeContextTicket=target;mNativeIntent=call.handle;
            original.presentations.clear();mNativeIntentFrame=std::move(original);
            return failNativeStage();
        }
        return false;
    }
    // Existing real admit_intent has now cleared old pending/candidate and
    // blocked prior active. Only this successful checkpoint replaces ownership.
    invalidateNativeRoutes();
    mPendingTagged.reset();mActiveTagged.reset();mNativeContextTicket=target;mNativeSuperseding=false;
    original.presentations.resize(call.count);
    mNativeIntentFrame=std::move(original);mNativeIntent=call.result.intent_handle;return true;
}

bool ExternalDesktopStageOwner::isNativeRenewalTicket(ExternalDesktopStageEpochTicket ticket) const {
    return mNativeRenewal && ExternalDesktopNativeClaims::sameTicket(ticket,mNativeRenewal->ticket) &&
        mNativeReservation && ExternalDesktopNativeClaims::sameTicket(ticket,*mNativeReservation) &&
        mRoutePublication && mRoutePublication->producer==mNativeRenewal->producer &&
        mNativeRenewal->producer && mNativeRenewal->producer->mState==ExternalDesktopSubmissionIdentity::State::SUSPENDED &&
        mNativeRenewal->intent==mNativeIntent;
}

bool ExternalDesktopStageOwner::sameNativeRenewalClaims() const {
    if (!mNativeRenewal || mNativeRenewal->claims.size()!=mNativeClaims.mRows.size()) return false;
    for (size_t i=0;i<mNativeRenewal->claims.size();++i) {
        const auto& old=mNativeRenewal->claims[i];const auto& next=mNativeClaims.mRows[i];
        const auto& a=old.value;const auto& b=next.value;
        if (a.key!=b.key || a.generation!=b.generation || a.kind!=b.kind || a.lifetime!=b.lifetime ||
                a.root_id!=b.root_id || !ExternalDesktopNativeClaims::sameEnd(a.source,b.source) ||
                !ExternalDesktopNativeClaims::sameEnd(a.destination,b.destination) || a.window_id!=b.window_id ||
                a.source_layer!=b.source_layer || a.channel!=b.channel || a.client!=b.client ||
                a.root_claim!=b.root_claim || old.path!=next.path || old.physical!=next.physical ||
                old.rootSource!=next.rootSource || old.rootIdentity!=next.rootIdentity ||
                old.rootOwnedInputBlockOrigin!=next.rootOwnedInputBlockOrigin ||
                !ExternalDesktopSourceLifetimes::sameClone(old.snapshot,next.snapshot) ||
                old.snapshot.actualParents!=next.snapshot.actualParents) return false;
    }
    return true; // no appended/retired/reissued claim or source/path reinterpretation
}

bool ExternalDesktopStageOwner::sameNativeRenewalRoots(const ExternalDesktopTaskRootCohort& cohort,bool fresh) const {
    if (!mNativeRenewal || cohort.records.size()!=mNativeRenewal->roots.size()) return false;
    for (size_t i=0;i<cohort.records.size();++i) {
        const auto& before=mNativeRenewal->roots[i];const auto& record=cohort.records[i];
        if (!record.observed) return false;
        const auto& root=*record.observed;
        if (!root.root || !root.source || !root.destinationParent ||
                !root.root->isValid() || !root.source->isValid() || !root.destinationParent->isValid() ||
                wp<IBinder>(root.root->getHandle())!=before.root || root.root->getLayerId()!=before.rootId ||
                wp<IBinder>(root.source->getHandle())!=before.source ||
                wp<IBinder>(root.destinationParent->getHandle())!=before.parent ||
                root.producer!=before.producer || root.frame!=before.frame || root.task!=before.task ||
                root.endpoints!=before.endpoints || root.geometry!=before.geometry ||
                record.destination!=before.destination || record.user!=before.user || record.clientUid!=before.clientUid)
            return false;
        if (fresh) {
            if (!ExternalDesktopNativeClaims::sameTicket(record.ticket,mNativeRenewal->ticket) ||
                    root.submission==before.submission || record.serial<=before.serial ||
                    record.expires<=mNativeRenewal->descriptor.observed_at) return false;
        } else if (!ExternalDesktopNativeClaims::sameTicket(record.ticket,mNativeRenewal->oldTicket) ||
                root.submission!=before.submission || record.serial!=before.serial) return false;
    }
    return true; // original firstExpires replay clamp is left completely intact
}

bool ExternalDesktopStageOwner::renewNativeIntent(const ExternalDesktopStageLeaseView& view,ExternalDesktopStageEpochTicket ticket,
        const WsLive& observed,ExternalDesktopStageEpochTicket* out) {
    if (!out || out->owner || out->epoch || out->generation || !mContext || !mEpochOwner ||
            mNativeRenewal || !mRoutePublication || !mRoutePublication->epoch || !mRoutePublication->producer ||
            mRoutePublication->producer->mState==ExternalDesktopSubmissionIdentity::State::INERT ||
            !mNativeIntent || !mNativeIntentFrame || !mNativeReservation || !mNativeContextTicket ||
            !mActiveTagged || mPendingTagged || mNativeSuperseding || mRootOperationInFlight ||
            !mWmsRoster || !mWmsRoots || mWmsRoots->records.empty() || mWmsRoots->records.size()>1024 ||
            !mNativeClaims.mCurrent || !mSourceLifetimes.mCurrent || !mCompleteBatch ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,*mNativeReservation) ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,*mNativeContextTicket) ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,mWmsRosterTicket) ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,mRoutePublication->ticket) ||
            !ExternalDesktopNativeClaims::sameKey(mActiveTagged->key,mRoutePublication->epoch->stage.key) ||
            !matchesCurrentNativeBatch(mRoutePublication->epoch->batch.view) ||
            observed.now!=view.observed_at || observed.now<0 || observed.uid!=view.expected_client_uid ||
            observed.user!=view.user || view.generation!=ticket.generation ||
            observed.current.len!=mRoutePublication->epoch->current.size() || !observed.current.data ||
            !std::equal(mRoutePublication->epoch->current.begin(),mRoutePublication->epoch->current.end(),observed.current.data) ||
            !validRosterObservations(view,*mWmsRoster) ||
            view.topology_snapshot.len<14 || view.topology_snapshot.len>86 || !view.topology_snapshot.data ||
            !view.expires_by_display.len || view.expires_by_display.len>9 || !view.expires_by_display.data ||
            mNativeIntentFrame->outputs.empty() || mNativeIntentFrame->outputs.size()>9) return false;
    auto& publication=*mRoutePublication;
    const auto& frame=*mNativeIntentFrame;
    const auto startedAt=android::elapsedRealtime();
    const auto deadline=*std::min_element(view.expires_by_display.data,
            view.expires_by_display.data+view.expires_by_display.len);
    if (startedAt<observed.now || startedAt>=frame.expiresAt ||
            deadline<=frame.expiresAt || observed.now>=frame.expiresAt ||
            (publication.producer->mState==ExternalDesktopSubmissionIdentity::State::SUSPENDED &&
                observed.now>=publication.producer->mSuspendedUntil)) return false;
    auto receipt=std::make_unique<NativeRenewal>();
    receipt->oldTicket=ticket;receipt->oldKey=mActiveTagged->key;receipt->intent=mNativeIntent;
    receipt->oldExpires=frame.expiresAt;receipt->expiresAt=deadline;receipt->batch=mReceipt.observations;
    receipt->producer=publication.producer;receipt->claims=mNativeClaims.mRows;
    for (auto& claim:receipt->claims)
        claim.value.roots={claim.path.empty()?nullptr:claim.path.data(),claim.path.size()};
    receipt->originalBlocks=mActiveTagged->expected;
    receipt->topology.assign(view.topology_snapshot.data,view.topology_snapshot.data+view.topology_snapshot.len);
    receipt->expires.assign(view.expires_by_display.data,view.expires_by_display.data+view.expires_by_display.len);
    receipt->descriptor=view; // genuine new observation owned SEPARATELY; old B2 is never retimed
    receipt->descriptor.topology_snapshot={receipt->topology.data(),receipt->topology.size()};
    receipt->descriptor.expires_by_display={receipt->expires.data(),receipt->expires.size()};
    receipt->roots.reserve(mWmsRoots->records.size());
    for (const auto& record:mWmsRoots->records) {
        if (!record.observed || !record.observed->root || !record.observed->source ||
                !record.observed->destinationParent || !ExternalDesktopNativeClaims::sameTicket(record.ticket,ticket)) return false;
        const auto& root=*record.observed;
        receipt->roots.push_back({root.root->getHandle(),root.source->getHandle(),root.destinationParent->getHandle(),
                root.root->getLayerId(),record.destination,record.user,record.clientUid,
                root.producer,root.frame,root.submission,root.task,root.endpoints,root.geometry,record.serial});
    }
    WsIntent replacement{};
    replacement.uid=observed.uid;replacement.user=observed.user;replacement.package_name=observed.package_name;
    std::copy(std::begin(observed.signer),std::end(observed.signer),replacement.signer);
    replacement.shared_uid=observed.shared_uid;replacement.active_user=observed.active_user;
    replacement.enabled=observed.enabled;replacement.locked=observed.locked;replacement.now=observed.now;
    replacement.atlas_revision=frame.atlas;replacement.lease_generation=ticket.generation;
    replacement.lease_uid=frame.uid;replacement.lease_user=frame.user;replacement.expires_at=deadline;
    replacement.environment_snapshot=observed.environment_snapshot;
    replacement.current={frame.current.data(),frame.current.size()};
    replacement.desired={frame.desired.data(),frame.desired.size()};
    replacement.origins={frame.origins.data(),frame.origins.size()};replacement.seams={frame.seams.data(),frame.seams.size()};
    replacement.outputs={frame.outputs.data(),frame.outputs.size()};replacement.observed_outputs=observed.outputs;
    struct Call {ExternalDesktopStageContext* context;ExternalDesktopStageEpochOwner* owner;const ExternalDesktopStageLeaseView* view;
        const WsIntent* replacement;uint64_t handle;int64_t oldDeadline;ExternalDesktopStageEpochTicket next{};WsResult result{};
        bool borrowed=false,reserved=false,ran=false,accepted=false,missedDeadline=false;int64_t lastClock=-1;
    } call{mContext.get(),mEpochOwner.get(),&view,&replacement,mNativeIntent,frame.expiresAt};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);if (!authority) return 0;
        c.borrowed=true;
        const auto before=android::elapsedRealtime();
        if (before<c.view->observed_at || before>=c.oldDeadline) return 0;
        c.reserved=external_desktop_input_epoch_reserve(c.owner,android::elapsedRealtimeNano(),c.view,&c.next)==1;
        if (!c.reserved) return 0;
        c.ran=external_desktop_stage_renew_intent(c.context,authority,c.replacement,c.handle,&c.result)==1;
        c.accepted=c.ran && !c.result.revoked && c.result.refusal==WS_NO_REFUSAL &&
            c.result.intent_handle==c.handle && !c.result.has_pending && c.result.transition==WS_NO_TRANSITION;
        const auto after=android::elapsedRealtime();
        c.lastClock=after;
        c.missedDeadline=after<before || after>=c.oldDeadline;
        return c.accepted && !c.missedDeadline;
    };
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    const bool lent=external_desktop_input_with_wms_authority(&view,&call,now,receive)==1;
    const auto returnedAt=android::elapsedRealtime();
    if (call.accepted && (call.missedDeadline || returnedAt<startedAt || returnedAt<call.lastClock ||
            returnedAt>=frame.expiresAt)) {
        // Rust may genuinely have accepted before the old cutoff, but no late
        // native return can revive a producer whose original usable interval
        // ended. Track the real same-handle result, close EXACT new reservation,
        // keep it native-inert; this is not proof the authority generation died.
        external_desktop_input_epoch_close(mEpochOwner.get(),call.next);
        mNativeContextTicket=call.next;mNativeReservation=call.next;mNativeClosedFrame=call.next;
        mNativeIntentFrame->expiresAt=deadline;
        return failNativeStage(); // no receipt, route or suspension-deadline advance
    }
    if (!lent) {
        if (call.reserved) external_desktop_input_epoch_close(mEpochOwner.get(),call.next);
        if (call.accepted || (call.ran && call.result.revoked)) return revokeNativeAuthority();
        if (call.ran && call.result.refusal==WS_NO_REFUSAL) return failNativeStage();
        if (!call.borrowed) return failNativeStage(); // fresh current authority unavailable: never leave old route usable
        return false; // refused proposal never becomes a renewal receipt
    }
    if (call.next.owner!=ticket.owner || call.next.generation!=ticket.generation || call.next.epoch<=ticket.epoch) {
        external_desktop_input_epoch_close(mEpochOwner.get(),call.next);return failNativeStage();
    }
    receipt->ticket=call.next;
    mNativeIntentFrame->expiresAt=deadline; // ONLY real core success + lender postcheck can advance this
    mNativeRenewal=std::move(receipt);mNativeReservation=call.next;
    mPendingTagged.reset();mActiveTagged.reset();mNativeSuperseding=true;
    // Keep the original full B2 and owned physical observations unchanged until
    // WMS publishes an independently fenced new binding. No route is permitted.
    publication.producer->mState=ExternalDesktopSubmissionIdentity::State::SUSPENDED;
    publication.producer->mSuspendedUntil=deadline;
    mNativeClaims.invalidate();*out=call.next;return true;
}

bool ExternalDesktopStageOwner::sameNativeRenewalRoster(const ExternalDesktopStageRoster& a,const ExternalDesktopStageRoster& b) {
    if (!a.references || !b.references || !a.references->same(*b.references) ||
            a.nativeOwnerIdentity!=b.nativeOwnerIdentity || a.windows!=b.windows || a.tasks.size()!=b.tasks.size()) return false;
    for (size_t i=0;i<a.tasks.size();++i) {
        const auto& x=a.tasks[i];const auto& y=b.tasks[i];
        if (x.displayToken!=y.displayToken || x.uid!=y.uid || x.start!=y.start || x.end!=y.end ||
                x.surface!=y.surface || x.layerId!=y.layerId || x.taskToken!=y.taskToken ||
                x.taskIdentity!=y.taskIdentity) return false;
    }
    return true; // exact observed object/state equality, not a new layout/eligibility policy
}

bool ExternalDesktopStageOwner::observeNativeRenewalBinding(const ExternalDesktopStageLeaseView& view,ExternalDesktopStageEpochTicket ticket,
        const std::shared_ptr<const ExternalDesktopStageRoster>& roster,
        const std::vector<std::shared_ptr<const ExternalDesktopTaskRootObservation>>& roots,
        const std::vector<ExternalDesktopStageSubmissionObservation>& submissions,
        std::shared_ptr<const ExternalDesktopStageRoster>& displacedRoster,
        std::shared_ptr<const ExternalDesktopTaskRootCohort>& displacedRoots,
        std::shared_ptr<const ExternalDesktopTaskRootCohort>& displacedHistory) {
    if (!isNativeRenewalTicket(ticket) || mNativeRenewal->rosterObserved || !roster ||
            !mWmsRoster || displacedRoster || displacedRoots || displacedHistory) return false;
    if (!sameNativeRenewalRoster(*mWmsRoster,*roster) || !validRosterObservations(view,*roster) ||
            !sameNativeRenewalClaims() || roots.size()!=mNativeRenewal->roots.size() ||
            submissions.empty() || submissions.size()>9 || view.topology_snapshot.len<14 ||
            !view.topology_snapshot.data || view.topology_snapshot.data[3]!=int64_t(submissions.size()) ||
            view.topology_snapshot.len!=5+submissions.size()*9) return failNativeStage();
    std::set<int64_t> tokens;
    for (const auto& submission:submissions) {
        if (!submission.frame || submission.frame!=mNativeRenewal->roots.front().frame ||
                !submission.submission || submission.serial<=0 || !submission.committed || !submission.reported ||
                !tokens.insert(submission.displayToken).second) return failNativeStage();
        bool found=false;
        for (size_t i=0;i<submissions.size();++i)
            found|=view.topology_snapshot.data[5+i*9]==submission.displayToken;
        if (!found) return failNativeStage();
    }
    for (const auto& root:roots) {
        if (!root) return failNativeStage();
        const auto submission=std::find_if(submissions.begin(),submissions.end(),[&](const auto& entry) {
            return entry.displayToken==root->endpoints[2];
        });
        if (submission==submissions.end() || root->submission!=submission->submission ||
                root->frame!=submission->frame || root->serial!=submission->serial) return failNativeStage();
    }
    auto source=mSourceLifetimes.prepare(view,ticket,*roster);
    if (!source) return failNativeStage();
    struct Call {ExternalDesktopStageEpochOwner* owner;const ExternalDesktopStageLeaseView* view;ExternalDesktopStageEpochTicket ticket;
        const std::vector<ExternalDesktopStageSubmissionObservation>* submissions;
    } call{mEpochOwner.get(),&view,ticket,&submissions};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);if (!authority) return 0;
        // Begin and these exact real listener notes already used the same
        // EpochOwner. Repeat-note is idempotent; never invent a listener/serial.
        for (const auto& entry:*c.submissions) if (
                external_desktop_input_epoch_note(c.owner,c.view,c.ticket,entry.displayToken,entry.serial,0)!=1 ||
                external_desktop_input_epoch_note(c.owner,c.view,c.ticket,entry.displayToken,entry.serial,1)!=1) return 0;
        return 1;
    };
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,&call,now,receive)!=1) return revokeNativeAuthority();
    if (!mSourceLifetimes.commit(std::move(*source))) return failNativeStage();
    suspendNativeRoutes();
    // Real time can reach the accepted deadline during the synchronous notes
    // or source commit. Suspension then clears the bridge terminally; never
    // dereference it or publish a fresh roster after that refusal.
    if (!isNativeRenewalTicket(ticket)) return failNativeStage();
    mNativeClaims.invalidateScope();
    displacedRoster=std::move(mWmsRoster);mWmsRoster=roster;mWmsRosterTicket=ticket;
    mNativeRenewal->rosterObserved=true;
    // This call remains under the SAME dispatcher lock. Success publishes the
    // complete independently fenced binding. Failure retains inert metadata
    // (possibly the new roster beside old root history), never Current/routes
    // or a successful prefix; actual lifetime history is not erased.
    if (!observeRoots(view,ticket,roots,displacedRoots,displacedHistory,roster)) return failNativeStage();
    return true; // binding publication only; an empty transaction ACK is NOT B0
}

bool ExternalDesktopStageOwner::sealNativeInputEpoch(const ExternalDesktopStageLeaseView& view,
        const WsLive& live,const WsStage& stage,const WsBatch& bootstrap) {
    if (!mNativeIntent || !mNativeIntentFrame || !matchesCurrentNativeBatch(bootstrap)) return false;
    auto original=stage;
    // Resealing unchanged semantic intent does not invent an atlas counter or
    // rewrite Frame/base to the postapply physical snapshot. Only bindings and
    // the genuine new native epoch change; Rust validates this SAME handle.
    const auto& frame=*mNativeIntentFrame;
    original.current={frame.current.data(),frame.current.size()};
    original.desired={frame.desired.data(),frame.desired.size()};
    original.origins={frame.origins.data(),frame.origins.size()};
    original.seams={frame.seams.data(),frame.seams.size()};
    return retainNativePending(view,live,original,mNativeIntent,&bootstrap);
}

bool ExternalDesktopStageOwner::retainNativePending(const ExternalDesktopStageLeaseView& view,
        const WsLive& live,const WsStage& stage,uint64_t intent,const WsBatch* bootstrap) {
    if (!mContext || !mWmsRoots || !mNativeClaims.mCurrent ||
            !mSourceLifetimes.current() || !mNativeClaims.matchesRegistry(live) || live.now!=view.observed_at ||
            mNativeClaims.mCurrent->batch!=mReceipt.observations ||
            mNativeClaims.mCurrent->ticket.owner!=mWmsRosterTicket.owner) return false;
    // Own ALL replica/root paths and both binding tables/regions BEFORE Rust.
    // Its validator remains the sole Stage policy; this adds real B0 joins.
    auto plan=mNativeClaims.copyTaggedPlan(stage,live);if (!plan) return false;
    const bool reseal=bootstrap && intent && intent==mNativeIntent && mNativeContextTicket &&
        mNativeReservation && ExternalDesktopNativeClaims::sameTicket(*mNativeReservation,plan->b0.ticket) &&
        mNativeContextTicket->owner==plan->b0.ticket.owner &&
        mNativeContextTicket->generation==plan->b0.ticket.generation &&
        plan->b0.ticket.epoch>mNativeContextTicket->epoch;
    if ((mPendingTagged || mNativeSuperseding) && !reseal) return false;
    struct Call { ExternalDesktopStageContext* context;const WsLive* live;const WsStage* stage;
        uint64_t intent;const WsBatch* bootstrap;WsResult result{};bool ran=false;
    } call{mContext.get(),&live,&stage,intent,bootstrap};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);if (!authority) return 0;
        c.ran=(c.bootstrap ? external_desktop_stage_seal_input_epoch(c.context,authority,c.live,c.intent,c.stage,c.bootstrap,&c.result):
                external_desktop_stage_submit(c.context,authority,c.live,c.stage,&c.result))==1;
        return c.ran && c.result.refusal==WS_NO_REFUSAL && !c.result.revoked &&
            c.result.has_pending && c.result.phase==WS_PENDING_DROP &&
            c.result.transition==WS_NO_TRANSITION && ExternalDesktopNativeClaims::sameKey(c.result.pending,c.stage->key);
    };
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,&call,now,receive)!=1) {
        // A failed lender postcheck must not retain a successful Rust pending
        // state as usable. Revoke is terminal/default-deny, never DROP clear.
        if (call.ran && call.result.refusal==WS_NO_REFUSAL) {
            mNativeContextTicket=plan->b0.ticket;return revokeNativeAuthority();
        }
        if (call.ran && call.result.revoked) return revokeNativeAuthority();
        return failNativeStage();
    }
    mNativeContextTicket=plan->b0.ticket;mPendingTagged=std::move(*plan);mActiveTagged.reset();
    mNativeSuperseding=false;return true;
}

bool ExternalDesktopStageOwner::refreshTaggedClaims(const ExternalDesktopStageLeaseView& view,uint64_t batch) {
    suspendNativeRoutes(); // check new facts without terminally retiring an unchanged held stream
#if defined(__cpp_exceptions)
    try {
#endif
    mNativeClaims.invalidate();
    if (!mPendingTagged && !mActiveTagged) return false;
    if (!mWmsRoots || !mCompleteBatch) return failNativeStage();
    if (!borrowAuthority(view)) return revokeNativeAuthority();
    auto& plan=mPendingTagged?*mPendingTagged:*mActiveTagged;
    if (plan.phase!=ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP &&
            plan.phase!=ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED) return failNativeStage();
    auto joins=prepareRootJoins(mWmsRoots.get(),android::elapsedRealtime(),&plan.expected);
    for (const auto& join:joins) if (!join.coherentSourceFacts ||
            join.state!=ExternalDesktopRootJoin::CLONES_OBSERVED || join.batch!=batch) return failNativeStage();
    // Existing snapshots only: never issue structural lifetimes during B1/B2.
    auto structural=mSourceLifetimes.prepareStructural(joins,false);
    if (!structural || !mSourceLifetimes.commitStructural(std::move(*structural))) return failNativeStage();
    auto prepared=mNativeClaims.prepareTagged(view,*mWmsRoots,*mCompleteBatch,batch,mSourceLifetimes,plan);
    if (!prepared) return failNativeStage();
    const auto receive=[](void*,const ExternalDesktopStageAuthority* a)->uint8_t {return a?1:0;};
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,this,now,receive)!=1) return revokeNativeAuthority();
    if (!mNativeClaims.publish(std::move(*prepared),mSourceLifetimes,now())) return failNativeStage();
    plan.lastSequence=batch;plan.lastObservedAt=mReceipt.nativeObservedAt;
    if (plan.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP)
        plan.droppedRows=mNativeClaims.mCurrent->rows;
    plan.phase=plan.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP ?
        ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP_PUBLISHED:ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED_PUBLISHED;
    mRootJoins.swap(joins);return true; // publication, NOT activation/enable
#if defined(__cpp_exceptions)
    } catch (...) {
        return failNativeStage(); // allocation/preparation exceptions cannot revive old-good state
    }
#endif
}

bool ExternalDesktopStageOwner::matchesCurrentNativeBatch(const WsBatch& batch) const {
    return mNativeClaims.mCurrent && matchesNativeBatch(batch,*mNativeClaims.mCurrent);
}

bool ExternalDesktopStageOwner::projectNativeBatch(const ExternalDesktopNativeClaims::Current& current,NativeBatch& out) const {
    const auto& source=mSourceLifetimes;
    if (!mCompleteBatch || !mReceipt.hasCompleteBatch || !source.mCurrent ||
            source.mCurrent->join!=ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED ||
            current.batch!=mReceipt.observations || source.mCurrent->batchSequence!=current.batch ||
            !ExternalDesktopNativeClaims::sameTicket(current.ticket,source.mCurrent->ticket) ||
            !mCompleteBatch->layerSourcesComplete ||
            !gui::layerSourcesValid(mCompleteBatch->layerSources,mCompleteBatch->windowInfos,true) ||
            !out.windows.empty() || !out.structurals.empty() || !out.regions.empty() ||
            !out.paths.empty() || !out.rawOrder.empty()) return false;
    const auto count=mCompleteBatch->windowInfos.size();
    if (count>8192) return false;
    out.regions.reserve(count);out.paths.reserve(count);out.rawOrder.reserve(count);
    out.windows.reserve(count);out.structurals.reserve(count);
    size_t totalRects=0;
    std::set<size_t> known;
    for (size_t index=0;index<count;++index) {
        const auto& info=mCompleteBatch->windowInfos[index];
        const auto& facts=mCompleteBatch->layerSources[index];const auto& applied=facts.applied;
        if (!applied.stockProjectionComplete || !gui::layerSourceStockProjectionValid(applied)) return false;
        const auto geometry=std::find_if(current.rows.begin(),current.rows.end(),[&](const auto& r){return r.row==index;});
        const ExternalDesktopNativeClaims::Row* owned=nullptr;
        if (geometry!=current.rows.end()) {
            if (!known.insert(index).second) return false;
            const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),
                [&](const auto& r){return r.value.key==geometry->claim;});
            if (claim==mNativeClaims.mRows.end() || claim->value.kind==WS_ROOT_CLAIM ||
                    !ExternalDesktopNativeClaims::sameFacts(geometry->facts,facts) ||
                    !geometry->touchable.hasSameRects(info.touchableRegion)) return false;
            owned=&*claim;
        }
        const ExternalDesktopSourceLifetimes::WindowBinding* original=nullptr;
        if (!owned) {
            // Known or historical ownership must NEVER be erased into the
            // ordinary sentinel representation, even if currently inactive.
            if (info.stageTag || std::any_of(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& r) {
                return r.value.kind!=WS_ROOT_CLAIM && r.value.window_id==info.id;
            })) return false;
            if (info.cloneLineage && std::any_of(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& r) {
                return r.value.kind==WS_ROOT_CLAIM && std::find(info.cloneLineage->mirrorRootIds.begin(),
                    info.cloneLineage->mirrorRootIds.end(),r.value.root_id)!=info.cloneLineage->mirrorRootIds.end();
            })) return false;
            for (const auto& entry:source.mCurrent->windows) {
                if (entry.physical>=source.mWindows.size() || entry.child>=source.mCurrent->children.size()) return false;
                if (source.mWindows[entry.physical].id==info.id) {
                    if (original) return false;
                    original=&entry;
                }
            }
            if (!original && (std::any_of(source.mWindows.begin(),source.mWindows.end(),[&](const auto& r){return r.id==info.id;}) ||
                    std::any_of(source.mCloneWindows.begin(),source.mCloneWindows.end(),[&](const auto& r){return r.snapshot.windowId==info.id;}) ||
                    std::any_of(source.mStructural.begin(),source.mStructural.end(),[&](const auto& r){return r.windowId==info.id;}))) return false;
        }
        out.regions.emplace_back();auto& region=out.regions.back();
        const size_t rects=info.touchableRegion.isEmpty()?0:static_cast<size_t>(info.touchableRegion.end()-info.touchableRegion.begin());
        if (rects>1024 || rects>32768-totalRects) return false;
        totalRects+=rects;region.reserve(rects);
        if (rects) for (auto r=info.touchableRegion.begin();r!=info.touchableRegion.end();++r)
            region.push_back({r->left,r->top,r->right,r->bottom});
        out.paths.emplace_back();auto& path=out.paths.back();
        if (info.cloneLineage) path=info.cloneLineage->mirrorRootIds;
        if (path.size()>50) return false;
        WsIdentity identity{};identity.id=info.id;
        identity.has_tag=info.stageTag.has_value();
        if (info.stageTag) {
            const auto& tag=*info.stageTag;
            identity.tag={tag.authorityGeneration,tag.atlasRevision,tag.geometryRevision,tag.epoch};
            identity.presentation=tag.presentationKey;
        }
        identity.has_lineage=info.cloneLineage.has_value();
        if (info.cloneLineage) identity.source_layer=info.cloneLineage->sourceLayerId;
        identity.roots={path.data(),path.size()};
        bool candidate=false;
        if (owned) {
            const auto& claim=owned->value;
            identity.lifetime=claim.lifetime;identity.destination=claim.destination;
            identity.channel=claim.channel;identity.client=claim.client;
            candidate=claim.kind==WS_WINDOW_CLAIM && applied.hasInputToken && applied.hasClientToken;
            if (geometry->stockCandidate) candidate=*geometry->stockCandidate;
        } else if (original) {
            const auto& window=source.mWindows[original->physical];
            const auto& child=source.mCurrent->children[original->child];
            if (child.task>=source.mCurrent->tasks.size() || !child.observed.channel || !child.observed.client ||
                    info.token!=child.observed.channel || info.windowToken!=child.observed.client) return false;
            const auto& task=source.mCurrent->tasks[child.task];
            identity.lifetime=window.key;identity.channel=child.rustValue.channel;identity.client=child.rustValue.client;
            identity.destination={task.incarnation,static_cast<uint64_t>(task.incarnation)};
            candidate=true; // genuine receiver only; ordinary Android stock rules still own dispatch
        } else if (mNativeIntentFrame && mNativeIntentFrame->current.size()>=14) {
            // Unmapped ordinary phone/system IDs remain sentinel0; retain raw
            // id/tag/lineage/stock/geometry. Never turn an unmapped endpoint into
            // an owned target. Unknown owned/tagged rows refused above.
            const auto& table=mNativeIntentFrame->current;
            const auto displays=table[3];
            if (displays<1 || displays>9 || table.size()<5+static_cast<size_t>(displays)*9) return false;
            for (size_t d=0;d<static_cast<size_t>(displays);++d) if (table[5+d*9+1]==info.displayId.val())
                identity.destination={table[5+d*9],static_cast<uint64_t>(table[5+d*9])};
        }
        const WsStock stock{applied.stockInputConfig,applied.stockDropMode,static_cast<uint8_t>(candidate)};
        const auto& frame=applied.inputFrame;
        const WsRect crop{frame[0],frame[1],frame[2],frame[3]};
        if (owned && owned->value.kind==WS_STRUCTURAL_CLAIM) {
            WsStructural row{};
            row.identity={identity.id,identity.lifetime,identity.destination,identity.has_tag,identity.tag,
                identity.presentation,identity.has_lineage,identity.source_layer,identity.roots};
            std::copy(applied.inputTransform.begin(),applied.inputTransform.end(),row.transform);
            row.crop=crop;row.region={region.data(),region.size()};row.stock=stock;
            row.snapshot_can_receive_input=applied.snapshotCanReceiveInput;
            row.has_input_token=applied.hasInputToken;row.has_client_token=applied.hasClientToken;
            row.external_desktop_drop=applied.ownedDropAll;
            out.rawOrder.push_back({true,out.structurals.size()});out.structurals.push_back(row);
        } else {
            WsWindow row{};row.identity=identity;
            std::copy(applied.inputTransform.begin(),applied.inputTransform.end(),row.transform);
            row.crop=crop;row.region={region.data(),region.size()};row.stock=stock;row.external_desktop_drop=applied.ownedDropAll;
            out.rawOrder.push_back({false,out.windows.size()});out.windows.push_back(row);
        }
    }
    if (known.size()!=current.rows.size() || out.rawOrder.size()!=count) return false;
    out.view={static_cast<int64_t>(mReceipt.observations),mReceipt.nativeObservedAt,1,
        {out.windows.data(),out.windows.size()},{out.structurals.data(),out.structurals.size()}};
    return true; // complete ordered projection, not claims/Stage/Live/routing authority
}

bool ExternalDesktopStageOwner::ownNativeLive(const WsLive& observed,NativeEpoch& out) {
    if (!observed.package_name.data || !observed.package_name.len || observed.package_name.len>256 ||
            !observed.outputs.data || !observed.outputs.len || observed.outputs.len>9 ||
            !observed.current.data || observed.current.len<14 || observed.current.len>1622 ||
            !observed.environment_snapshot.data || observed.environment_snapshot.len<14 ||
            observed.environment_snapshot.len>86 || !out.package.empty() || !out.current.empty() ||
            !out.environment.empty() || !out.outputs.empty()) return false;
    out.live=observed; // genuine WMS values; never manufacture/retime a descriptor
    out.package.assign(observed.package_name.data,observed.package_name.data+observed.package_name.len);
    out.current.assign(observed.current.data,observed.current.data+observed.current.len);
    out.environment.assign(observed.environment_snapshot.data,
            observed.environment_snapshot.data+observed.environment_snapshot.len);
    out.outputs.assign(observed.outputs.data,observed.outputs.data+observed.outputs.len);
    out.live.package_name={out.package.data(),out.package.size()};
    out.live.current={out.current.data(),out.current.size()};
    out.live.environment_snapshot={out.environment.data(),out.environment.size()};
    out.live.outputs={out.outputs.data(),out.outputs.size()};
    return true;
}

bool ExternalDesktopStageOwner::buildNativeEpoch(const ExternalDesktopStageLeaseView& view,const WsLive& observed,
        const ExternalDesktopNativeClaims::Current& current,NativeEpoch& out) const {
    if (!mNativeIntent || !mNativeIntentFrame || !mNativeContextTicket || !mNativeReservation ||
            !mWmsRoots || !mSourceLifetimes.mCurrent ||
            !ExternalDesktopNativeClaims::sameTicket(*mNativeReservation,current.ticket) ||
            !ExternalDesktopNativeClaims::sameTicket(current.ticket,mWmsRosterTicket) ||
            (mNativeClosedFrame && ExternalDesktopNativeClaims::sameTicket(*mNativeClosedFrame,current.ticket)) ||
            view.observed_at!=observed.now || view.generation!=current.ticket.generation ||
            observed.uid!=view.expected_client_uid || observed.user!=view.user ||
            !std::equal(std::begin(observed.signer),std::end(observed.signer),std::begin(view.signer)) ||
            observed.current.len<14 || observed.current.len>1622 || !observed.current.data ||
            observed.environment_snapshot.len!=view.topology_snapshot.len ||
            !observed.environment_snapshot.data || !view.topology_snapshot.data ||
            !std::equal(view.topology_snapshot.data,view.topology_snapshot.data+view.topology_snapshot.len,
                observed.environment_snapshot.data) || !out.claims.empty() || !out.children.empty() ||
            !out.replicas.empty() || !out.bindings.empty() || !out.structurals.empty()) return false;
    const auto& original=*mNativeIntentFrame;
    const auto& source=*mSourceLifetimes.mCurrent;
    if (observed.current.data[0]!=current.ticket.generation || observed.current.data[1]<0 ||
            source.batchSequence!=current.batch || source.observedAt!=mReceipt.nativeObservedAt ||
            source.join!=ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED ||
            observed.now<source.observedAt || observed.now>=current.expiresAt ||
            observed.now>=original.expiresAt || source.children.size()>8192 ||
            !projectNativeBatch(current,out.batch)) return false;
    if (!ownNativeLive(observed,out)) return false;
    out.claims=mNativeClaims.views();
    // Epochs can remain suspended across unrelated historical retirement.
    // Their C spans must own their paths, not borrow the mutable native table.
    out.claimPaths.reserve(out.claims.size());
    for(auto& claim:out.claims){
        if(claim.roots.len>50 || (claim.roots.len && !claim.roots.data))return false;
        out.claimPaths.emplace_back();auto& path=out.claimPaths.back();
        if(claim.roots.len)path.assign(claim.roots.data,claim.roots.data+claim.roots.len);
        claim.roots={path.data(),path.size()};
    }
    for (const auto& entry:source.children) out.children.push_back(entry.rustValue); // EVERY current child
    std::sort(out.children.begin(),out.children.end(),[](const auto& a,const auto& b){return a.child<b.child;});
    out.live.children={out.children.data(),out.children.size()};
    out.live.registry_revision=mNativeClaims.mRevision;out.live.claims={out.claims.data(),out.claims.size()};
    out.live.releases={}; // No object/tail retirement API or invented proof.
    const auto key=!mNativeSuperseding && mPendingTagged?mPendingTagged->key:
        !mNativeSuperseding && mActiveTagged?mActiveTagged->key:
        WsKey{current.ticket.generation,original.atlas,observed.current.data[1],current.ticket.epoch};
    out.live.prepared=1;out.live.prepared_key=key;
    out.live.prepared_desired={original.desired.data(),original.desired.size()};
    out.live.prepared_origins={original.origins.data(),original.origins.size()};
    out.live.prepared_seams={original.seams.data(),original.seams.size()};
    // This entry is called ONLY from the actual complete WMS producer-fence
    // path, joined to the independent broker/WMS snapshot and this native batch.
    // No requested tag or desired table supplies observed application fields.
    out.live.applied=1;out.live.applied_key=key;out.live.reported_sequence=out.batch.view.sequence;
    out.live.applied_origins=out.live.prepared_origins;out.live.applied_seams=out.live.prepared_seams;
    out.live.removal_committed_and_reported=0; // nonempty physical root path only
    out.stage.key=key;out.stage.lease_generation=current.ticket.generation;
    out.stage.lease_uid=original.uid;out.stage.lease_user=original.user;out.stage.expires_at=original.expiresAt;
    out.stage.current={original.current.data(),original.current.size()};out.stage.desired=out.live.prepared_desired;
    out.stage.origins=out.live.prepared_origins;out.stage.seams=out.live.prepared_seams;
    for (const auto& record:mWmsRoots->records) {
        if (!record.observed || !record.observed->root || !record.observed->source ||
                !ExternalDesktopNativeClaims::sameTicket(record.ticket,current.ticket)) return false;
        const auto& root=*record.observed;
        const auto task=std::find_if(source.tasks.begin(),source.tasks.end(),[&](const auto& t) {
            return t.physical<mSourceLifetimes.mTasks.size() && t.surface &&
                mSourceLifetimes.mTasks[t.physical].identity==root.task &&
                t.surface->getHandle()==root.source->getHandle();
        });
        if (task==source.tasks.end()) return false;
        const auto geometry=std::find_if(original.presentations.begin(),original.presentations.end(),[&](const auto& p) {
            return p.task==task->token && p.owner_display==root.endpoints[0] &&
                p.destination_display==root.endpoints[2];
        });
        if (geometry==original.presentations.end()) return false;
        const auto& g=root.geometry;const auto& p=*geometry;
        if (g!=std::array<int32_t,8>{p.source_rect.left,p.source_rect.top,p.source_rect.right,p.source_rect.bottom,
                p.destination_rect.left,p.destination_rect.top,p.destination_rect.right,p.destination_rect.bottom}) return false;
        const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& row) {
            return row.value.kind==WS_ROOT_CLAIM && row.value.root_id==static_cast<uint32_t>(root.root->getLayerId());
        });
        if (claim==mNativeClaims.mRows.end()) return false;
        const auto owned=std::find_if(out.claims.begin(),out.claims.end(),
                [&](const auto& value){return value.key==claim->value.key;});
        if(owned==out.claims.end())return false;
        // Reuse the SAME native-issued logical key; geometry never reissues it.
        out.replicas.push_back({claim->value.key,p.task,p.owner_display,p.destination_display,
            p.source_rect,p.destination_rect,p.input,p.z,claim->value.source,claim->value.destination,
            owned->roots,claim->value.key});
    }
    if (out.replicas.size()!=original.presentations.size()) return false;
    for (const auto& row:current.rows) {
        const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& c){return c.value.key==row.claim;});
        const auto replica=std::find_if(out.replicas.begin(),out.replicas.end(),[&](const auto& p){return p.root_claim==row.rootClaim;});
        if (claim==mNativeClaims.mRows.end() || replica==out.replicas.end() || row.row>=out.batch.rawOrder.size()) return false;
        const auto [structural,index]=out.batch.rawOrder[row.row];
        if (structural) {
            const auto& actual=out.batch.structurals[index];WsStructuralBinding value{};
            value.claim=row.claim;value.presentation=replica->key;
            std::copy(std::begin(actual.transform),std::end(actual.transform),value.transform);
            value.crop=actual.crop;value.region=actual.region;value.stock=actual.stock;
            value.snapshot_can_receive_input=actual.snapshot_can_receive_input;out.structurals.push_back(value);
        } else {
            const auto& actual=out.batch.windows[index];
            const auto child=std::find_if(out.children.begin(),out.children.end(),[&](const auto& c) {
                return c.task==replica->task && c.layer==claim->value.source_layer &&
                    c.channel==claim->value.channel && c.client==claim->value.client && c.participation==WS_CHILD_RECEIVER;
            });
            if (child==out.children.end()) return false;
            WsBinding value{};value.child=child->child;value.presentation=replica->key;
            std::copy(std::begin(actual.transform),std::end(actual.transform),value.transform);
            value.crop=actual.crop;value.region=actual.region;value.stock=actual.stock;out.bindings.push_back(value);
        }
    }
    out.stage.replicas={out.replicas.data(),out.replicas.size()};
    out.stage.bindings={out.bindings.data(),out.bindings.size()};
    out.stage.structural_bindings={out.structurals.data(),out.structurals.size()};
    return true; // existing real Rust validates the full child×presentation/history contract
}

void ExternalDesktopStageOwner::invalidateNativeRoutes() {
    if (mRoutePublication && mRoutePublication->producer) {
        mRoutePublication->producer->mState=ExternalDesktopSubmissionIdentity::State::INERT;
        mRoutePublication->producer->mSuspendedUntil=0;
    }
    mRoutePublication.reset(); // NONRESOURCE tombstones in the dispatcher ledger remain retained
    mNativeRenewal.reset(); // no successful bridge survives terminal producer invalidation
    mRouteBlockExpectations.clear();mRouteBlockBatch=0;
}

void ExternalDesktopStageOwner::suspendNativeRoutes() {
    if (!mRoutePublication || !mRoutePublication->producer) return;
    auto& publication=*mRoutePublication;
    if (publication.producer->mState==ExternalDesktopSubmissionIdentity::State::INERT ||
            !publication.epoch || !mNativeIntentFrame || !publication.intent ||
            publication.intent!=mNativeIntent || publication.expires.empty()) {
        invalidateNativeRoutes();return;
    }
    const auto until=mNativeRenewal && mNativeRenewal->producer==publication.producer &&
            mNativeRenewal->intent==publication.intent ? mNativeRenewal->expiresAt :
        std::min({mNativeIntentFrame->expiresAt,publication.epoch->stage.expires_at,
            *std::min_element(publication.expires.begin(),publication.expires.end())});
    const auto now=android::elapsedRealtime();
    if (now<0 || until<=now) {invalidateNativeRoutes();return;}
    publication.producer->mSuspendedUntil=until; // real admitted bounds, never now+duration
    publication.producer->mState=ExternalDesktopSubmissionIdentity::State::SUSPENDED;
}

bool ExternalDesktopStageOwner::sameNativeRoutingWindow(const gui::WindowInfo& a,const gui::WindowInfo& b) {
    // WindowInfo::operator== omits these ordinary receiver/occlusion fields.
    return a==b && a.windowToken==b.windowToken && a.alpha==b.alpha &&
            a.focusTransferTarget==b.focusTransferTarget &&
            a.cloneLayerStackTransform==b.cloneLayerStackTransform &&
            a.touchableRegionCropHandle==b.touchableRegionCropHandle;
}

std::optional<gui::WindowInfo> ExternalDesktopStageOwner::stockNativeWindow(const gui::WindowInfo& actual) const {
    if (!mCompleteBatch || !mReceipt.hasCompleteBatch || !mCompleteBatch->layerSourcesComplete ||
            actual.displayId.val()<=0) return {};
    size_t index=mCompleteBatch->windowInfos.size();
    for (size_t i=0;i<mCompleteBatch->windowInfos.size();++i) if (mCompleteBatch->windowInfos[i].id==actual.id) {
        if (index!=mCompleteBatch->windowInfos.size() ||
                !sameNativeRoutingWindow(mCompleteBatch->windowInfos[i],actual)) return {};
        index=i;
    }
    if (index>=mCompleteBatch->layerSources.size()) return {};
    const auto& facts=mCompleteBatch->layerSources[index];
    if (!gui::layerSourceMatchesWindow(facts,actual,index) || !facts.applied.stockProjectionComplete ||
            !gui::layerSourceStockProjectionValid(facts.applied)) return {};
    auto stock=actual;
    stock.inputConfig=static_cast<gui::WindowInfo::InputConfig>(facts.applied.stockInputConfig);
    stock.stageTag.reset(); // private epoch tag is not a stock eligibility fact
    return stock;
}

bool ExternalDesktopStageOwner::sameNativeRouteContinuity(const NativeRoutePublication& old,
        const NativeRoutePublication& next) {
    // This first path deliberately requires the SAME real active epoch. An
    // independently accepted higher-epoch renewal needs its own original-intent
    // equivalence gate; numeric epoch equality is not a substitute for that.
    if (!old.epoch || !next.epoch || !old.intent || old.intent!=next.intent ||
            !ExternalDesktopNativeClaims::sameTicket(old.ticket,next.ticket) ||
            !ExternalDesktopNativeClaims::sameKey(old.epoch->stage.key,next.epoch->stage.key) ||
            old.descriptor.expected_client_uid!=next.descriptor.expected_client_uid ||
            old.descriptor.user!=next.descriptor.user || old.descriptor.generation!=next.descriptor.generation ||
            !std::equal(std::begin(old.descriptor.signer),std::end(old.descriptor.signer),std::begin(next.descriptor.signer)) ||
            old.topology!=next.topology || old.windows.size()!=next.windows.size()) return false;
    for (size_t i=0;i<old.windows.size();++i) {
        const auto& a=old.windows[i];const auto& b=next.windows[i];
        if (a.claim!=b.claim || a.lifetime!=b.lifetime || a.root!=b.root ||
                !sameNativeRoutingWindow(a.stock,b.stock)) return false;
    }
    return true; // called ONLY after real Rust B2 acceptance + current registry/full-batch joins
}

bool ExternalDesktopStageOwner::sameNativeRenewalContinuity(const NativeRoutePublication& old,
        const NativeRoutePublication& next) const {
    const auto* renewal=mNativeRenewal.get();
    if (!renewal || !isNativeRenewalTicket(next.ticket) || !renewal->rosterObserved || !renewal->rootsObserved ||
            !sameNativeRenewalClaims() || !mWmsRoots || !sameNativeRenewalRoots(*mWmsRoots,true) ||
            old.producer!=renewal->producer || !old.epoch || !next.epoch ||
            !ExternalDesktopNativeClaims::sameTicket(old.ticket,renewal->oldTicket) ||
            !ExternalDesktopNativeClaims::sameKey(old.epoch->stage.key,renewal->oldKey) ||
            old.epoch->stage.expires_at!=renewal->oldExpires || next.epoch->stage.expires_at!=renewal->expiresAt ||
            old.intent!=renewal->intent || next.intent!=renewal->intent ||
            next.epoch->stage.key.generation!=renewal->oldKey.generation ||
            next.epoch->stage.key.atlas!=renewal->oldKey.atlas ||
            next.epoch->stage.key.geometry!=renewal->oldKey.geometry ||
            next.epoch->stage.key.epoch!=renewal->ticket.epoch ||
            next.batch<=renewal->batch || old.epoch->current!=next.epoch->current ||
            old.descriptor.expected_client_uid!=next.descriptor.expected_client_uid ||
            old.descriptor.user!=next.descriptor.user || old.descriptor.generation!=next.descriptor.generation ||
            !std::equal(std::begin(old.descriptor.signer),std::end(old.descriptor.signer),std::begin(next.descriptor.signer)) ||
            old.topology!=next.topology || old.windows.size()!=next.windows.size()) return false;
    for (size_t i=0;i<old.windows.size();++i) {
        const auto& a=old.windows[i];const auto& b=next.windows[i];
        if (a.claim!=b.claim || a.lifetime!=b.lifetime || a.root!=b.root ||
                !sameNativeRoutingWindow(a.stock,b.stock)) return false;
    }
    return true; // called ONLY after the NEW higher-epoch B2 actually passed core/RootFacts
}

std::optional<gui::WindowInfo> ExternalDesktopStageOwner::suspendedStockWindow(const gui::WindowInfo& actual,
        const std::shared_ptr<const ExternalDesktopSubmissionIdentity>& producer) {
    auto* publication=mRoutePublication.get();
    const auto now=android::elapsedRealtime();
    if (!publication || !producer || publication->producer!=producer ||
            producer->mState!=ExternalDesktopSubmissionIdentity::State::SUSPENDED) return {};
    const auto refuse=[&]()->std::optional<gui::WindowInfo> {invalidateNativeRoutes();return {};};
    const bool renewal=mNativeRenewal && mNativeRenewal->producer==producer &&
            isNativeRenewalTicket(mNativeRenewal->ticket);
    if (now<0 || now>=producer->mSuspendedUntil || !mNativeIntentFrame ||
            publication->intent!=mNativeIntent || !mNativeReservation ||
            (!renewal && !ExternalDesktopNativeClaims::sameTicket(publication->ticket,*mNativeReservation)) ||
            !mWmsRoots || !mCompleteBatch || !mReceipt.hasCompleteBatch ||
            mReceipt.observations<publication->batch ||
            !borrowAuthority(renewal?mNativeRenewal->descriptor:publication->descriptor) ||
            (renewal && (!sameNativeRenewalClaims() ||
                !sameNativeRenewalRoots(*mWmsRoots,mNativeRenewal->rootsObserved)))) return refuse();
    auto stock=stockNativeWindow(actual);
    const auto old=std::find_if(publication->windows.begin(),publication->windows.end(),
            [&](const auto& row){return row.stock.id==actual.id;});
    if (!stock || old==publication->windows.end() || !sameNativeRoutingWindow(old->stock,*stock)) return refuse();
    // Full independent root/source/parent/snapshot/channel proof, not a flag
    // waiver or a stale cached WindowInfo. The only non-stock difference may be
    // the exact expected result of our scoped native ARM/re-PARK operation.
    const auto matches=[&](const std::vector<ExternalDesktopOwnedBlockExpectation>& expected) {
        const auto joins=prepareRootJoins(mWmsRoots.get(),now,&expected);
        return !joins.empty() && joins.size()==mWmsRoots->records.size() &&
            std::all_of(joins.begin(),joins.end(),[&](const auto& row) {
                return row.batch==mReceipt.observations && row.coherentSourceFacts &&
                    row.state==ExternalDesktopRootJoin::CLONES_OBSERVED;
            });
    };
    const auto exact=[&](const std::vector<ExternalDesktopOwnedBlockExpectation>& expected) {
        return renewal?suspendedNativeRenewalStock(expected,now):matches(expected);
    };
    const bool action=mReceipt.observations>mRouteBlockBatch && !mRouteBlockExpectations.empty() &&
            exact(mRouteBlockExpectations);
    const bool prior=renewal?exact(mNativeRenewal->originalBlocks):
            (mActiveTagged && matches(mActiveTagged->expected));
    if (!action && !prior) return refuse();
    stock->stageTag=actual.stageTag; // return the actual row; only independently observed inputConfig differs
    return stock; // eligibility checks still execute on this exact STOCK copy; NO route authority
}

bool ExternalDesktopStageOwner::suspendedNativeRenewalStock(const std::vector<ExternalDesktopOwnedBlockExpectation>& expected,
        int64_t now) const {
    if (!mNativeRenewal || !isNativeRenewalTicket(mNativeRenewal->ticket) || now<0 ||
            now>=mNativeRenewal->expiresAt || !mWmsRoots || mWmsRoots->records.empty() ||
            !mCompleteBatch || !mReceipt.hasCompleteBatch || !mCompleteBatch->layerSourcesComplete ||
            !sameNativeRenewalClaims() || !sameNativeRenewalRoots(*mWmsRoots,mNativeRenewal->rootsObserved)) return false;
    const auto& renewal=*mNativeRenewal;
    const WsKey next{renewal.oldKey.generation,renewal.oldKey.atlas,renewal.oldKey.geometry,renewal.ticket.epoch};
    const ExternalDesktopSuspendedRootStockScope scope(mWmsRoots->records.front().ticket,renewal.oldKey,next);
    for (const auto& record:mWmsRoots->records) {
        // Never retime an expired old observation. The new independently fenced
        // WMS binding must arrive while the old one is still usable or the stream
        // becomes terminal; a renewed intent alone is not physical evidence.
        if (!record.observed || now>=record.expires) return false;
        const auto originals=mSourceLifetimes.originalsForRoot(record.ticket,record.observed->task,
                record.observed->source,mReceipt.observations);
        if (!originals || !joinExternalDesktopSuspendedRootStockFacts(*mCompleteBatch,mReceipt.observations,
                record,*mWmsRoots,*originals,expected,scope).exact) return false;
    }
    return true; // NON-Current proof only. Caller still executes stock per-event eligibility.
}

bool ExternalDesktopStageOwner::ownsNativeWindow(const gui::WindowInfo& info) const {
    if (info.stageTag) return true;
    for (const auto& row:mNativeClaims.mRows) {
        if (row.value.kind!=WS_ROOT_CLAIM && row.value.window_id==info.id) return true;
        if (row.value.kind==WS_ROOT_CLAIM && info.cloneLineage &&
                std::find(info.cloneLineage->mirrorRootIds.begin(),info.cloneLineage->mirrorRootIds.end(),
                    row.value.root_id)!=info.cloneLineage->mirrorRootIds.end()) return true;
    }
    return std::any_of(mSourceLifetimes.mCloneWindows.begin(),mSourceLifetimes.mCloneWindows.end(),
                [&](const auto& row){return row.snapshot.windowId==info.id;}) ||
            std::any_of(mSourceLifetimes.mStructural.begin(),mSourceLifetimes.mStructural.end(),
                [&](const auto& row){return row.windowId==info.id;});
}

bool ExternalDesktopStageOwner::publishNativeRoutes(const ExternalDesktopStageLeaseView& view,std::unique_ptr<NativeEpoch> epoch) {
    if (!epoch || !mActiveTagged || mPendingTagged || mNativeSuperseding || !mNativeReservation ||
            !mNativeClaims.mCurrent || !mSourceLifetimes.mCurrent ||
            !ExternalDesktopNativeClaims::sameTicket(*mNativeReservation,mNativeClaims.mCurrent->ticket) ||
            !ExternalDesktopNativeClaims::sameKey(epoch->stage.key,mActiveTagged->key) ||
            !matchesCurrentNativeBatch(epoch->batch.view) ||
            !mNativeClaims.matchesRegistry(epoch->live) || view.topology_snapshot.len>86 ||
            !view.topology_snapshot.data || !view.expires_by_display.data ||
            view.expires_by_display.len<1 || view.expires_by_display.len>9 || !mCompleteBatch) {
        invalidateNativeRoutes();return false;
    }
    auto publication=std::make_unique<NativeRoutePublication>();
    publication->topology.assign(view.topology_snapshot.data,
            view.topology_snapshot.data+view.topology_snapshot.len);
    publication->expires.assign(view.expires_by_display.data,
            view.expires_by_display.data+view.expires_by_display.len);
    publication->descriptor=view; // preserve GENUINE WMS observed_at; never retime its descriptor
    publication->descriptor.topology_snapshot={publication->topology.data(),publication->topology.size()};
    publication->descriptor.expires_by_display={publication->expires.data(),publication->expires.size()};
    publication->epoch=std::move(epoch);publication->ticket=*mNativeReservation;
    publication->batch=mReceipt.observations;publication->sourceRevision=mSourceLifetimes.mRevision;
    publication->registryRevision=mNativeClaims.mRevision;
    publication->intent=mNativeIntent;
    for (const auto& geometry:mNativeClaims.mCurrent->rows) {
        const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),
                [&](const auto& row){return row.value.key==geometry.claim;});
        if (claim==mNativeClaims.mRows.end() || geometry.row>=mCompleteBatch->windowInfos.size()) {
            invalidateNativeRoutes();return false;
        }
        if (claim->value.kind!=WS_WINDOW_CLAIM) continue;
        auto stock=stockNativeWindow(mCompleteBatch->windowInfos[geometry.row]);
        if (!stock) {invalidateNativeRoutes();return false;}
        publication->windows.push_back({claim->value.key,claim->value.lifetime,claim->value.root_claim,std::move(*stock)});
    }
    // Identities refer to existing native lifetime claims, never WindowInfo
    // pointers, channels, public tags or geometry-epoch-derived surrogates.
    for (const auto& row:mNativeClaims.mRows) {
        const auto& claim=row.value;
        if (claim.kind!=WS_ROOT_CLAIM && claim.kind!=WS_WINDOW_CLAIM) continue;
        auto prior=std::find_if(mRouteLifetimes.begin(),mRouteLifetimes.end(),[&](const auto& identity) {
            return identity.kind==claim.kind && identity.key==claim.key && identity.lifetime==claim.lifetime;
        });
        if (prior!=mRouteLifetimes.end()) continue;
        if (mRouteLifetimes.size()>=ExternalDesktopNativeClaims::kMaxClaims) {invalidateNativeRoutes();return false;}
        mRouteLifetimes.push_back({claim.kind,claim.key,claim.lifetime,
                std::shared_ptr<const ExternalDesktopSubmissionIdentity>(new ExternalDesktopSubmissionIdentity)});
    }
    const auto now=android::elapsedRealtime();
    if (mNativeRenewal && (!mRoutePublication || now<0 || now>=mNativeRenewal->expiresAt ||
            !sameNativeRenewalContinuity(*mRoutePublication,*publication))) {
        invalidateNativeRoutes();return false; // mismatched renewal cannot mint a replacement producer
    }
    if (mRoutePublication && mRoutePublication->producer && now>=0 &&
            mRoutePublication->producer->mState!=ExternalDesktopSubmissionIdentity::State::INERT &&
            (mRoutePublication->producer->mState!=ExternalDesktopSubmissionIdentity::State::SUSPENDED ||
                now<mRoutePublication->producer->mSuspendedUntil) &&
            (sameNativeRouteContinuity(*mRoutePublication,*publication) ||
                sameNativeRenewalContinuity(*mRoutePublication,*publication))) {
        publication->producer=mRoutePublication->producer;
    } else {
        invalidateNativeRoutes();
        publication->producer=std::shared_ptr<ExternalDesktopSubmissionIdentity>(new ExternalDesktopSubmissionIdentity);
    }
    publication->producer->mState=ExternalDesktopSubmissionIdentity::State::ROUTABLE;
    publication->producer->mSuspendedUntil=0;
    mNativeRenewal.reset(); // a single explicit bridge cannot authorize a later epoch
    mRoutePublication=std::move(publication);return true;
}

ExternalDesktopStageOwner::NativeRouteSelection ExternalDesktopStageOwner::routeNativeWindow(const gui::WindowInfo& info) {
    NativeRouteSelection selected;
    const bool claimed=ownsNativeWindow(info);
    if (!claimed) {selected.route=WS_ORDINARY;return selected;}
    auto* publication=mRoutePublication.get();
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    const int64_t observedNow=now();
    if (publication && publication->producer &&
            publication->producer->mState==ExternalDesktopSubmissionIdentity::State::SUSPENDED) {
        if (observedNow<0 || observedNow>=publication->producer->mSuspendedUntil) invalidateNativeRoutes();
        return selected; // never destroy a suspended producer merely for refusing a route
    }
    if (!publication || !publication->producer || publication->producer->mState!=ExternalDesktopSubmissionIdentity::State::ROUTABLE || !mActiveTagged ||
            mPendingTagged || mNativeSuperseding || !mNativeReservation || !mNativeClaims.mCurrent ||
            !mSourceLifetimes.mCurrent || !mCompleteBatch || observedNow<0 ||
            !ExternalDesktopNativeClaims::sameTicket(publication->ticket,*mNativeReservation) ||
            !ExternalDesktopNativeClaims::sameTicket(publication->ticket,mNativeClaims.mCurrent->ticket) ||
            publication->batch!=mReceipt.observations || publication->sourceRevision!=mSourceLifetimes.mRevision ||
            publication->registryRevision!=mNativeClaims.mRevision ||
            !matchesCurrentNativeBatch(publication->epoch->batch.view)) {invalidateNativeRoutes();return selected;}
    size_t index=mCompleteBatch->windowInfos.size();
    for (size_t i=0;i<mCompleteBatch->windowInfos.size();++i) if (mCompleteBatch->windowInfos[i].id==info.id) {
        const auto& raw=mCompleteBatch->windowInfos[i];
        // WindowInfo::operator== omits these ordinary routing/receiver fields.
        if (index!=mCompleteBatch->windowInfos.size() || raw!=info || raw.windowToken!=info.windowToken ||
                raw.alpha!=info.alpha || raw.focusTransferTarget!=info.focusTransferTarget ||
                raw.cloneLayerStackTransform!=info.cloneLayerStackTransform) {invalidateNativeRoutes();return selected;}
        index=i;
    }
    if (index>=publication->epoch->batch.rawOrder.size()) {invalidateNativeRoutes();return selected;}
    const auto [structural,rowIndex]=publication->epoch->batch.rawOrder[index];
    if (structural || rowIndex>=publication->epoch->batch.windows.size()) return selected;
    auto& live=publication->epoch->live;live.now=observedNow; // real clock, not a new WMS sample/batch
    const auto& identity=publication->epoch->batch.windows[rowIndex].identity;
    struct Call {ExternalDesktopStageContext* context;const WsLive* live;const WsIdentity* identity;WsResult result{};bool accepted=false;}
        call{mContext.get(),&live,&identity};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);
        c.accepted=authority && external_desktop_stage_route(c.context,authority,c.live,c.identity,&c.result)==1 &&
            !c.result.revoked && c.result.route==WS_VALID && c.result.phase==WS_ACTIVE &&
            c.result.has_active && !c.result.has_pending && c.result.refusal==WS_NO_REFUSAL;
        return c.accepted;
    };
    if (external_desktop_input_with_wms_authority(&publication->descriptor,&call,now,receive)!=1 ||
            !ExternalDesktopNativeClaims::sameKey(call.result.active,mActiveTagged->key)) {
        // Lender mismatch alone (including renewal) is not authority-generation
        // death. The real core's explicit revoked result remains terminal.
        if (call.result.revoked) revokeNativeAuthority();else invalidateNativeRoutes();
        return selected;
    }
    const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& row) {
        return row.value.kind==WS_WINDOW_CLAIM && row.value.window_id==identity.id &&
                row.value.lifetime==identity.lifetime && row.value.channel==identity.channel && row.value.client==identity.client;
    });
    if (claim==mNativeClaims.mRows.end()) {invalidateNativeRoutes();return selected;}
    for (const auto& lifetime:mRouteLifetimes) {
        if (lifetime.kind==WS_WINDOW_CLAIM && lifetime.key==claim->value.key && lifetime.lifetime==identity.lifetime)
            selected.window=lifetime.identity;
        if (lifetime.kind==WS_ROOT_CLAIM && lifetime.key==claim->value.root_claim) selected.presentation=lifetime.identity;
    }
    if (!selected.window || !selected.presentation) {invalidateNativeRoutes();return selected;}
    selected.route=WS_VALID;selected.domain=publication->ticket.owner;selected.generation=publication->ticket.generation;
    selected.windowLifetime=identity.lifetime;selected.presentationLifetime=claim->value.root_claim;
    selected.producer=publication->producer;return selected;
}

ExternalDesktopStageOwner::NativeRouteSelection ExternalDesktopStageOwner::routeNativeStream(uint64_t window,uint64_t presentation,
        const std::shared_ptr<const ExternalDesktopSubmissionIdentity>& producer) {
    if (!producer || producer->mState!=ExternalDesktopSubmissionIdentity::State::ROUTABLE || !mRoutePublication || mRoutePublication->producer!=producer ||
            !mCompleteBatch) return {};
    const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& row) {
        return row.value.kind==WS_WINDOW_CLAIM && row.value.lifetime==window && row.value.root_claim==presentation;
    });
    if (claim==mNativeClaims.mRows.end()) return {};
    const auto info=std::find_if(mCompleteBatch->windowInfos.begin(),mCompleteBatch->windowInfos.end(),
            [&](const auto& row){return row.id==claim->value.window_id;});
    return info==mCompleteBatch->windowInfos.end()?NativeRouteSelection{}:routeNativeWindow(*info);
}

bool ExternalDesktopStageOwner::prepareRootOperation(int32_t operation,WsKey key,RootOperation& out) {
    if (operation<0 || operation>2 || mRootOperationInFlight || !mWmsRoots ||
            mWmsRoots->records.empty() || mWmsRoots->records.size()>1024 || !mNativeReservation ||
            !mCompleteBatch || !mReceipt.hasCompleteBatch || !mCompleteBatch->layerSourcesComplete ||
            mReceipt.observations<=mRootOperationBatchBarrier ||
            mRootOperationHigh==UINT64_MAX || key.generation!=mNativeReservation->generation ||
            key.epoch!=mNativeReservation->epoch || key.epoch<=0 || key.atlas<0 || key.geometry<0 ||
            !out.roots.empty() || !gui::layerSourcesValid(mCompleteBatch->layerSources,mCompleteBatch->windowInfos,true)) return false;
    out.ticket=*mNativeReservation;out.key=key;out.operation=operation;out.batch=mReceipt.observations;
    out.cohort=mWmsRoots;
    std::set<uint32_t> ids;std::set<const IBinder*> origins;
    bool needsRepark=false;
    for (const auto& record:mWmsRoots->records) {
        if (!record.observed || !record.observed->root || !record.observed->source ||
                !ExternalDesktopNativeClaims::sameTicket(record.ticket,out.ticket)) return false;
        const auto& sc=record.observed->root;
        if (!sc->isValid() || sc->getLayerId()<=0 || !ids.insert(static_cast<uint32_t>(sc->getLayerId())).second) return false;
        const gui::LayerSourceRoot* actual=nullptr;bool tagged=false;
        for (size_t row=0;row<mCompleteBatch->layerSources.size();++row) {
            const auto& facts=mCompleteBatch->layerSources[row];
            for (const auto& root:facts.roots) if (root.requested.id==static_cast<uint32_t>(sc->getLayerId())) {
                if (actual && (!externalDesktopSameSourceNode(actual->requested,root.requested) || actual->applied!=root.applied)) return false;
                actual=&root;tagged|=mCompleteBatch->windowInfos[row].stageTag.has_value();
            }
        }
        if (!actual || !externalDesktopSameNativeSource(actual->requested,sc) ||
                actual->requested.mirrorSource!=static_cast<uint32_t>(record.observed->source->getLayerId()) ||
                !actual->requested.ownedInputBlockOrigin || !actual->applied.stockProjectionComplete ||
                !gui::layerSourceStockProjectionValid(actual->applied) ||
                actual->applied.ownedInputBlockOrigin!=actual->requested.ownedInputBlockOrigin ||
                actual->applied.ownedInputBlockRevision==0 || actual->applied.ownedInputBlockRevision>=uint64_t(INT64_MAX) ||
                !origins.insert(actual->requested.ownedInputBlockOrigin.get()).second) return false;
        const auto client=sc->getClient();
        if (!client || (out.client && client!=out.client)) return false;
        out.client=client;
        uint64_t presentation=0;
        std::optional<gui::WindowInfo::StageTag> expectedTag;
        if (operation) {
            // ARM/ENABLE keys are copied ONLY from the successful private
            // Rust pending output and its exact B0 claims, never observed tags.
            if (!mPendingTagged || !ExternalDesktopNativeClaims::sameKey(mPendingTagged->key,key) || mNativeSuperseding) return false;
            const auto& plan=*mPendingTagged;
            const auto expected=std::find_if(plan.expected.begin(),plan.expected.end(),[&](const auto& r) {
                return r.root==actual->requested.id;
            });
            if (expected==plan.expected.end() || !expected->tag || expected->revision<=1 ||
                    expected->revision-1!=actual->applied.ownedInputBlockRevision ||
                    expected->origin!=actual->requested.ownedInputBlockOrigin || expected->epoch!=key.epoch ||
                    actual->applied.ownedInputBlockEpoch!=key.epoch ||
                    expected->tag->authorityGeneration!=key.generation || expected->tag->atlasRevision!=key.atlas ||
                    expected->tag->geometryRevision!=key.geometry || expected->tag->epoch!=key.epoch ||
                    !actual->applied.ownedDropAll ||
                    (operation==1 && (plan.phase!=ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP || tagged)) ||
                    (operation==2 && plan.phase!=ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED)) return false;
            presentation=expected->tag->presentationKey;
            expectedTag=expected->tag;
            if (!presentation || presentation>uint64_t(INT64_MAX)) return false; // private SF int64 vector ABI
            if (operation==2) {
                // Exact real B1 was already accepted by RootFacts + Rust before
                // this native phase was advanced. No stock/merged-drop waiver.
                if (plan.lastSequence!=mReceipt.observations || !mNativeClaims.mCurrent ||
                        mNativeClaims.mCurrent->batch!=mReceipt.observations ||
                        !ExternalDesktopNativeClaims::sameOwnedBlocks(mNativeClaims.mCurrent->ownedBlocks,
                            [&] {auto old=plan.expected;for(auto& r:old){r.blocked=true;--r.revision;}return old;}())) return false;
            }
        } else {
            // Restrictive re-PARK never accepts a tag as authority: exact real
            // owned roots/origins/revisions suffice to add ALL and clear tags.
            // The kernel-authenticated SF operation still revalidates ALL roots
            // before mutation; old Rust ownership stays native-inert.
            needsRepark|=!actual->applied.ownedDropAll || tagged || actual->applied.ownedInputBlockEpoch!=key.epoch;
        }
        out.roots.push_back(sc);out.origins.push_back(actual->requested.ownedInputBlockOrigin);
        out.revisions.push_back(static_cast<int64_t>(actual->applied.ownedInputBlockRevision));
        out.presentations.push_back(static_cast<int64_t>(presentation));
        out.expected.push_back({actual->requested.id,actual->requested.ownedInputBlockOrigin,
                actual->applied.ownedInputBlockRevision+1,key.epoch,operation!=2,expectedTag});
    }
    if (!operation && !needsRepark) return false;
    out.serial=++mRootOperationHigh;mRootOperationInFlight=out.serial;
    suspendNativeRoutes();
    // Prepared native operation only; capture must still join its EXACT
    // successor to a genuinely later complete compositor publication.
    mRouteBlockExpectations=out.expected;mRouteBlockBatch=out.batch;
    mNativeClaims.invalidate(); // no old CURRENT can route during synchronous SF work
    return true;
}

bool ExternalDesktopStageOwner::finishRootOperation(const ExternalDesktopStageLeaseView& view,const RootOperation& action,bool succeeded) {
    if (!action.serial || action.serial!=mRootOperationInFlight) return false;
    mRootOperationInFlight=0;
    if (!mNativeReservation || !ExternalDesktopNativeClaims::sameTicket(action.ticket,*mNativeReservation) ||
            mWmsRoots!=action.cohort || (mNativeClosedFrame && ExternalDesktopNativeClaims::sameTicket(action.ticket,*mNativeClosedFrame))) return false;
    if (!borrowAuthority(view)) return revokeNativeAuthority();
    if (!succeeded) return failNativeStage();
    mRootOperationBatchBarrier=std::max(mRootOperationBatchBarrier,action.batch);
    return true; // wait for an ACTUAL newer full native batch; no synthetic B0/B1/B2
}

int32_t ExternalDesktopStageOwner::progressNativeEpoch(const ExternalDesktopStageLeaseView& view,ExternalDesktopStageEpochTicket ticket,
        const std::shared_ptr<const ExternalDesktopStageRoster>& roster,uint64_t batch,const WsLive& observed,
        std::optional<RootOperation>& action) {
    if (action || !mNativeIntentFrame || !mNativeIntent || !mNativeReservation || !roster || !mWmsRoster ||
            !roster->references || !mWmsRoster->references ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,*mNativeReservation) ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,mWmsRosterTicket) ||
            !roster->references->same(*mWmsRoster->references) ||
            roster->nativeOwnerIdentity!=mWmsRoster->nativeOwnerIdentity || !validRosterObservations(view,*roster) ||
            (mNativeClosedFrame && ExternalDesktopNativeClaims::sameTicket(ticket,*mNativeClosedFrame)) ||
            batch!=mReceipt.observations || !mReceipt.hasCompleteBatch || !mCompleteBatch || !mWmsRoots ||
            observed.current.len<14 || observed.current.len>1622 || !observed.current.data ||
            view.observed_at!=observed.now || observed.current.data[0]!=ticket.generation || observed.current.data[1]<0) return 0;
    if (!borrowAuthority(view)) {revokeNativeAuthority();return 0;}
    if (view.observed_at<0 || view.observed_at>=mNativeIntentFrame->expiresAt) {failNativeStage();return 0;}
    if (mNativeRenewal && (!isNativeRenewalTicket(ticket) || !mNativeRenewal->rosterObserved ||
            !mNativeRenewal->rootsObserved || !sameNativeRenewalClaims() ||
            !sameNativeRenewalRoots(*mWmsRoots,true))) {failNativeStage();return 0;}
    if (mRootOperationInFlight || batch<=mRootOperationBatchBarrier) return 1;
    const WsKey next{ticket.generation,mNativeIntentFrame->atlas,observed.current.data[1],ticket.epoch};
    if ((!mPendingTagged && !mActiveTagged) || mNativeSuperseding) {
        RootOperation repark;
        if (prepareRootOperation(0,next,repark)) {action=std::move(repark);return 1;}
        if (!refreshNativeClaims(view,ticket,roster,batch)) return 0;
    }
    const auto phase=mPendingTagged?mPendingTagged->phase:ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP;
    const bool wasPending=mPendingTagged.has_value(),wasActive=mActiveTagged.has_value();
    std::optional<ExternalDesktopNativeClaims::Prepared> stillDropped;
    if (!mNativeSuperseding && (wasPending || wasActive)) {
        if (wasPending && phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED)
            stillDropped=mNativeClaims.prepareStillDropped(view,*mWmsRoots,*mCompleteBatch,batch,mSourceLifetimes,*mPendingTagged);
        if (!stillDropped && !refreshTaggedClaims(view,batch)) return 0;
    }
    const auto* current=stillDropped?&stillDropped->current:mNativeClaims.mCurrent?&*mNativeClaims.mCurrent:nullptr;
    if (!current) return 0;
    auto actual=std::make_unique<NativeEpoch>();
    if (!buildNativeEpoch(view,observed,*current,*actual)) {failNativeStage();return 0;}
    if ((!wasPending && !wasActive) || mNativeSuperseding) {
        if (!sealNativeInputEpoch(view,actual->live,actual->stage,actual->batch.view)) return 0;
        RootOperation arm;
        if (!prepareRootOperation(1,mPendingTagged->key,arm)) {failNativeStage();return 0;}
        action=std::move(arm);return 1;
    }
    if (!observeNativeStage(view,actual->live,actual->batch.view)) return 0;
    if (mActiveTagged) {
        if (!publishNativeRoutes(view,std::move(actual))) {failNativeStage();return 0;}
        return 2; // actual protocol output only; dispatch/APPLIED are separate gates
    }
    if (wasPending && phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP && mPendingTagged &&
            mPendingTagged->phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED) {
        RootOperation enable;
        if (!prepareRootOperation(2,mPendingTagged->key,enable)) {failNativeStage();return 0;}
        action=std::move(enable);
    }
    return 1;
}

bool ExternalDesktopStageOwner::matchesNativeBatch(const WsBatch& batch,const ExternalDesktopNativeClaims::Current& current) const {
    if (!mSourceLifetimes.current() || !mCompleteBatch || !batch.complete ||
            batch.sequence<=0 || static_cast<uint64_t>(batch.sequence)!=mReceipt.observations ||
            batch.observed_at!=mReceipt.nativeObservedAt ||
            current.batch!=mReceipt.observations ||
            batch.windows.len>8192 || batch.structurals.len>8192-batch.windows.len ||
            (batch.windows.len && !batch.windows.data) || (batch.structurals.len && !batch.structurals.data)) return false;
    std::set<size_t> windows,structurals;
    for (const auto& geometry:current.rows) {
        const auto claim=std::find_if(mNativeClaims.mRows.begin(),mNativeClaims.mRows.end(),[&](const auto& r) {
            return r.value.key==geometry.claim;
        });
        if (claim==mNativeClaims.mRows.end() || geometry.row>=mCompleteBatch->windowInfos.size()) return false;
        const auto& info=mCompleteBatch->windowInfos[geometry.row];const auto& a=geometry.facts.applied;
        const auto identity=[&](const auto& id) {
            if (id.id!=info.id || id.lifetime!=claim->value.lifetime ||
                    !ExternalDesktopNativeClaims::sameEnd(id.destination,claim->value.destination) ||
                    bool(id.has_tag)!=info.stageTag.has_value() || !id.has_lineage ||
                    id.source_layer!=geometry.facts.sourceId || id.roots.len!=geometry.facts.mirrorRoots.size() ||
                    (id.roots.len && !id.roots.data) ||
                    !std::equal(geometry.facts.mirrorRoots.begin(),geometry.facts.mirrorRoots.end(),id.roots.data)) return false;
            if (!info.stageTag) return true;
            const auto& tag=*info.stageTag;
            return id.tag.generation==tag.authorityGeneration && id.tag.atlas==tag.atlasRevision &&
                id.tag.geometry==tag.geometryRevision && id.tag.epoch==tag.epoch && id.presentation==tag.presentationKey;
        };
        const auto geometryMatches=[&](const auto& row) {
            if (!std::equal(a.inputTransform.begin(),a.inputTransform.end(),row.transform) ||
                    a.inputFrame!=std::array<int32_t,4>{row.crop.left,row.crop.top,row.crop.right,row.crop.bottom} ||
                    row.stock.input_config!=a.stockInputConfig || row.stock.drop_mode!=a.stockDropMode ||
                    (geometry.stockCandidate && row.stock.candidate!=(*geometry.stockCandidate?1u:0u)) ||
                    bool(row.external_desktop_drop)!=a.ownedDropAll || row.region.len>1024 ||
                    (row.region.len && !row.region.data)) return false;
            std::vector<WsRect> region;
            if (row.region.len) region.assign(row.region.data,row.region.data+row.region.len);
            return ExternalDesktopNativeClaims::sameRegion(geometry.touchable,region);
        };
        bool matched=false;
        if (claim->value.kind==WS_WINDOW_CLAIM) {
            for (size_t i=0;i<batch.windows.len;++i) {
                const auto& row=batch.windows.data[i];
                if (row.identity.lifetime!=claim->value.lifetime) continue;
                if (!windows.insert(i).second || !identity(row.identity) || !geometryMatches(row) ||
                        row.identity.channel!=claim->value.channel || row.identity.client!=claim->value.client) return false;
                matched=true;
            }
        } else {
            for (size_t i=0;i<batch.structurals.len;++i) {
                const auto& row=batch.structurals.data[i];
                if (row.identity.lifetime!=claim->value.lifetime) continue;
                if (!structurals.insert(i).second || !identity(row.identity) || !geometryMatches(row) ||
                        row.stock.candidate || bool(row.has_input_token)!=a.hasInputToken ||
                        bool(row.has_client_token)!=a.hasClientToken ||
                        bool(row.snapshot_can_receive_input)!=a.snapshotCanReceiveInput) return false;
                matched=true;
            }
        }
        if (!matched) return false;
    }
    NativeBatch projected;
    if (!projectNativeBatch(current,projected) || batch.windows.len!=projected.windows.size() ||
            batch.structurals.len!=projected.structurals.size()) return false;
    const auto sameIdentity=[](const auto& a,const auto& b) {
        if (a.id!=b.id || a.lifetime!=b.lifetime || !ExternalDesktopNativeClaims::sameEnd(a.destination,b.destination) ||
                a.has_tag!=b.has_tag || (a.has_tag && (!ExternalDesktopNativeClaims::sameKey(a.tag,b.tag) ||
                a.presentation!=b.presentation)) || a.has_lineage!=b.has_lineage ||
                a.source_layer!=b.source_layer || a.roots.len!=b.roots.len ||
                (a.roots.len && (!a.roots.data || !b.roots.data ||
                    !std::equal(a.roots.data,a.roots.data+a.roots.len,b.roots.data)))) return false;
        if constexpr (requires { a.channel; }) return a.channel==b.channel && a.client==b.client;
        return true;
    };
    const auto sameRow=[&](const auto& a,const auto& b) {
        if (!sameIdentity(a.identity,b.identity) || !std::equal(std::begin(a.transform),std::end(a.transform),std::begin(b.transform)) ||
                a.crop.left!=b.crop.left || a.crop.top!=b.crop.top || a.crop.right!=b.crop.right || a.crop.bottom!=b.crop.bottom ||
                a.stock.input_config!=b.stock.input_config || a.stock.drop_mode!=b.stock.drop_mode ||
                a.stock.candidate!=b.stock.candidate || a.external_desktop_drop!=b.external_desktop_drop || a.region.len!=b.region.len ||
                (a.region.len && (!a.region.data || !b.region.data))) return false;
        for (size_t i=0;i<a.region.len;++i) {
            const auto& x=a.region.data[i];const auto& y=b.region.data[i];
            if (x.left!=y.left || x.top!=y.top || x.right!=y.right || x.bottom!=y.bottom) return false;
        }
        if constexpr (requires { a.snapshot_can_receive_input; }) return
            a.snapshot_can_receive_input==b.snapshot_can_receive_input &&
            a.has_input_token==b.has_input_token && a.has_client_token==b.has_client_token;
        return true;
    };
    for (size_t i=0;i<batch.windows.len;++i) if (!sameRow(batch.windows.data[i],projected.windows[i])) return false;
    for (size_t i=0;i<batch.structurals.len;++i) if (!sameRow(batch.structurals.data[i],projected.structurals[i])) return false;
    return true; // also every real ordinary/phone/system row, in original per-kind order
}

bool ExternalDesktopStageOwner::observeNativeStage(const ExternalDesktopStageLeaseView& view,
        const WsLive& live,const WsBatch& batch) {
#if defined(__cpp_exceptions)
    try {
#endif
    if (!mPendingTagged && !mActiveTagged) return false;
    const bool active=!mPendingTagged;
    auto& owned=mPendingTagged?*mPendingTagged:*mActiveTagged;
    if (mNativeSuperseding) {
        if (!borrowAuthority(view)) return revokeNativeAuthority();
        return false; // old scope stays owned, never routed into successor geometry
    }
    if (batch.sequence<=0 || static_cast<uint64_t>(batch.sequence)!=mReceipt.observations)
        return reconcileNativeStage(view,live);
    // Same existing core progress prerequisites: no independent applied report
    // for this native ingress means WAIT, not an attempt using desired replay.
    // Stale hints/pre-submit B0 cannot certify B1 and cannot revoke a successor.
    if (!live.applied || live.reported_sequence!=batch.sequence ||
            batch.observed_at<owned.stagedAt ||
            static_cast<uint64_t>(batch.sequence)<=owned.b0.batch) return reconcileNativeStage(view,live);
    if (owned.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED &&
            static_cast<uint64_t>(batch.sequence)<=owned.lastSequence) return reconcileNativeStage(view,live);
    if (!ExternalDesktopNativeClaims::sameKey(live.applied_key,owned.key) ||
            !mNativeClaims.matchesRegistry(live) || live.now!=view.observed_at) return failNativeStage();
    std::optional<ExternalDesktopNativeClaims::Prepared> stillDropped;
    if (!active && owned.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED && mWmsRoots && mCompleteBatch)
        stillDropped=mNativeClaims.prepareStillDropped(view,*mWmsRoots,*mCompleteBatch,
                static_cast<uint64_t>(batch.sequence),mSourceLifetimes,owned);
    if (!stillDropped && (owned.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP ||
            owned.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED)) {
        if (!refreshTaggedClaims(view,static_cast<uint64_t>(batch.sequence))) return false;
    }
    if (stillDropped ? !matchesNativeBatch(batch,stillDropped->current):!matchesCurrentNativeBatch(batch)) return failNativeStage();
    auto& plan=owned;
    const bool drop=plan.phase==ExternalDesktopNativeClaims::TaggedPlan::Phase::DROP_PUBLISHED;
    if (!stillDropped && ((!drop && plan.phase!=ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED_PUBLISHED) ||
            batch.sequence<=0 || static_cast<uint64_t>(batch.sequence)!=plan.lastSequence ||
            batch.observed_at!=plan.lastObservedAt)) return failNativeStage();
    struct Call {ExternalDesktopStageContext* context;const WsLive* live;const WsBatch* batch;WsKey key;WsResult result{};
        bool stillDropped,accepted=false;
    } call{mContext.get(),&live,&batch,plan.key,{},stillDropped.has_value()};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);
        c.accepted=authority && external_desktop_stage_observe(c.context,authority,c.live,c.key,c.batch,&c.result)==1 &&
            !c.result.revoked && c.result.refusal==(c.stillDropped ? WS_NOT_ENABLED:WS_NO_REFUSAL);
        return c.accepted;
    };
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,&call,now,receive)!=1) {
        if (call.accepted || call.result.revoked) return revokeNativeAuthority();
        return failNativeStage();
    }
    const auto& result=call.result;
    if (active && !drop && result.phase==WS_ACTIVE && result.has_active && !result.has_pending &&
            result.transition==WS_REFRESHED && ExternalDesktopNativeClaims::sameKey(result.active,plan.key) &&
            ExternalDesktopNativeClaims::sameKey(result.transition_key,plan.key)) {
        plan.phase=ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED;
        return true; // genuine Rust refresh of SAME active epoch/table, no new intent
    }
    if (stillDropped && result.phase==WS_PENDING_ENABLED && result.has_pending &&
            result.transition==WS_NO_TRANSITION && ExternalDesktopNativeClaims::sameKey(result.pending,plan.key)) {
        plan.lastSequence=static_cast<uint64_t>(batch.sequence);plan.lastObservedAt=batch.observed_at;
        mNativeClaims.invalidate(); // consume the unpublished preparation; no CURRENT publication
        return true; // real Rust NotEnabled WAIT; no publication or B2 authorization
    }
    if (drop && result.phase==WS_PENDING_ENABLED && result.has_pending &&
            result.transition==WS_ENABLE_OWNED_DROP && ExternalDesktopNativeClaims::sameKey(result.pending,plan.key) &&
            ExternalDesktopNativeClaims::sameKey(result.transition_key,plan.key)) {
        for (auto& expected:plan.expected) {++expected.revision;expected.blocked=false;}
        plan.phase=ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED;return true;
    }
    if (!drop && result.phase==WS_ACTIVE && result.has_active &&
            result.transition==WS_ACTIVATED && ExternalDesktopNativeClaims::sameKey(result.active,plan.key) &&
            ExternalDesktopNativeClaims::sameKey(result.transition_key,plan.key)) {
        mActiveTagged=std::move(*mPendingTagged);
        mActiveTagged->phase=ExternalDesktopNativeClaims::TaggedPlan::Phase::ENABLED;
        mPendingTagged.reset();return true; // still NO dispatch/routing enable
    }
    return failNativeStage();
#if defined(__cpp_exceptions)
    } catch (...) {
        return failNativeStage();
    }
#endif
}

bool ExternalDesktopStageOwner::reconcileNativeStage(const ExternalDesktopStageLeaseView& view,const WsLive& live) {
    if (!mPendingTagged && !mActiveTagged) return false;
    const auto& plan=mPendingTagged?*mPendingTagged:*mActiveTagged;
    if (live.now!=view.observed_at || live.now>=plan.expiresAt ||
            !mNativeClaims.matchesRegistry(live)) return failNativeStage();
    struct Call {ExternalDesktopStageContext* context;const WsLive* live;WsKey key;WsResult result{};bool accepted=false;
        bool active=false;
    } call{mContext.get(),&live,plan.key,{},false,!mPendingTagged};
    const auto receive=[](void* data,const ExternalDesktopStageAuthority* authority)->uint8_t {
        auto& c=*static_cast<Call*>(data);
        c.accepted=authority && external_desktop_stage_reconcile(c.context,authority,c.live,&c.result)==1 &&
            !c.result.revoked && (c.active ? (c.result.has_active && !c.result.has_pending &&
                ExternalDesktopNativeClaims::sameKey(c.result.active,c.key) && c.result.phase==WS_ACTIVE):
                (c.result.has_pending && ExternalDesktopNativeClaims::sameKey(c.result.pending,c.key) &&
                (c.result.phase==WS_PENDING_DROP || c.result.phase==WS_PENDING_ENABLED))) &&
            c.result.transition==WS_NO_TRANSITION &&
            (c.result.refusal==WS_NO_REFUSAL || c.result.refusal==WS_GEOMETRY_CHANGED);
        return c.accepted;
    };
    const auto now=[]()->int64_t {return android::elapsedRealtime();};
    if (external_desktop_input_with_wms_authority(&view,&call,now,receive)!=1) {
        if (call.accepted || call.result.revoked) return revokeNativeAuthority();
        return failNativeStage();
    }
    return true; // stale/noncandidate WAIT, but actual authority/liveness reconciled
}

bool ExternalDesktopStageOwner::failNativeStage() {
    // Frame/geometry/participant failure is NOT proof the lease died. Keep
    // real Rust intent/pending/active ownership tracked but never route/replay
    // it. Only a fresh real admit_intent + re-PARKed B0 may supersede it.
    invalidateNativeRoutes();
    if (mNativeContextTicket) mNativeSuperseding=true;
    mNativeClaims.invalidate();return false;
}

bool ExternalDesktopStageOwner::revokeNativeAuthority() {
    invalidateNativeRoutes();
    if (mContext && mNativeContextTicket) {
        WsResult ignored{};external_desktop_stage_revoke(mContext.get(),mNativeContextTicket->generation,&ignored);
    }
    mPendingTagged.reset();mActiveTagged.reset();mNativeIntent=0;mNativeContextTicket.reset();
    mNativeIntentFrame.reset();
    mNativeSuperseding=false;mNativeClaims.invalidate();
    return false; // proven authority loss only, never an owned-drop clear
}

void ExternalDesktopStageOwner::supersedeNativeScope(ExternalDesktopStageEpochTicket next) {
    if (!next.owner || next.epoch<=0 || next.generation<=0) return;
    invalidateNativeRoutes();
    if (mNativeContextTicket) mNativeSuperseding=true;
    mNativeReservation=next; // exact successful native reservation/observation
}

bool ExternalDesktopStageOwner::closeNativeFrame(ExternalDesktopStageEpochTicket ticket) {
    const auto current=mNativeReservation.value_or(mWmsRosterTicket.owner ? mWmsRosterTicket:
            mNativeContextTicket.value_or(ExternalDesktopStageEpochTicket{}));
    if (!current.owner || current.epoch<=0 || current.generation<=0 ||
            !ExternalDesktopNativeClaims::sameTicket(ticket,current)) return false;
    mNativeClosedFrame=ticket;failNativeStage();return true;
}

std::vector<ExternalDesktopRootJoinReceipt> ExternalDesktopStageOwner::prepareRootJoins(
        const ExternalDesktopTaskRootCohort* roots,int64_t now,
        const std::vector<ExternalDesktopOwnedBlockExpectation>* expected) const {
    std::vector<ExternalDesktopRootJoinReceipt> joins;
    if (!roots) return joins;
    joins.reserve(roots->records.size());
    for (const auto& record:roots->records) {
        ExternalDesktopRootJoinReceipt joined;
        if (mCompleteBatch && now>=0 && now<record.expires &&
                record.ticket.owner==mWmsRosterTicket.owner && record.ticket.epoch==mWmsRosterTicket.epoch &&
                record.ticket.generation==mWmsRosterTicket.generation) {
            const auto originals=mSourceLifetimes.originalsForRoot(record.ticket,record.observed->task,
                record.observed->source,mReceipt.observations);
            if (!originals) joined.state=ExternalDesktopRootJoin::NO_ORIGINALS;
            else if (mCompleteBatch->layerSourcesComplete)
                joined=joinExternalDesktopTaskRootFacts(*mCompleteBatch,mReceipt.observations,record,*roots,*originals,expected);
            else if (!expected) joined=joinExternalDesktopTaskRoot(*mCompleteBatch,mReceipt.observations,
                static_cast<uint32_t>(record.observed->root->getLayerId()),record.destination,*originals);
        }
        joins.push_back(std::move(joined)); // EVERY current root joins the full batch.
    }
    return joins; // No claim, Live, applied geometry or physical release.
}

void ExternalDesktopStageOwner::joinRootObservation(int64_t now) {
    // A new full batch requires a fresh synchronous WMS lease handoff before
    // claims can become CURRENT again; SF callbacks do not carry that authority.
    mNativeClaims.invalidate();
    auto candidate=prepareRootJoins(mWmsRoots.get(),now);
    auto structural=mSourceLifetimes.prepareStructural(candidate);
    if (!structural || !mSourceLifetimes.commitStructural(std::move(*structural))) {
        for (auto& join:candidate) join.state=ExternalDesktopRootJoin::REJECTED;
    }
    mRootJoins.swap(candidate); // A thrown preparation never exposes partial joins.
}

void ExternalDesktopStageOwner::invalidateRootObservations(ExternalDesktopStageEpochTicket ticket) {
    if (mNativeRenewal && ExternalDesktopNativeClaims::sameTicket(ticket,mNativeRenewal->oldTicket)) return;
    closeNativeFrame(ticket);
    mNativeClaims.invalidate(ticket);
    if (!mWmsRoots) return;
    for (size_t i=0;i<mWmsRoots->records.size();++i) {
        const auto& value=mWmsRoots->records[i].ticket;
        if (value.owner==ticket.owner && value.epoch==ticket.epoch && value.generation==ticket.generation &&
                i<mRootJoins.size()) mRootJoins[i]={};
    }
    // Keep all physical object witnesses/history. Close/callback is NOT retirement.
}

std::shared_ptr<const ExternalDesktopStageRoster> ExternalDesktopStageOwner::takeRosterObservation(
        ExternalDesktopStageEpochTicket ticket) {
    if (mNativeRenewal && ExternalDesktopNativeClaims::sameTicket(ticket,mNativeRenewal->oldTicket)) return {};
    if (mWmsRosterTicket.owner != ticket.owner || mWmsRosterTicket.epoch != ticket.epoch ||
            mWmsRosterTicket.generation != ticket.generation) return {};
    mSourceLifetimes.invalidate(ticket);
    closeNativeFrame(ticket);
    mNativeClaims.invalidate(ticket);
    mWmsRosterTicket = {};
    return std::move(mWmsRoster); // Observation disposal, NEVER claim/tail release.
}

std::shared_ptr<const ExternalDesktopTaskRootCohort> ExternalDesktopStageOwner::takeRootObservations(
        ExternalDesktopStageEpochTicket ticket) {
    if (mNativeRenewal && ExternalDesktopNativeClaims::sameTicket(ticket,mNativeRenewal->oldTicket)) return {};
    if (mWmsRosterTicket.owner!=ticket.owner || mWmsRosterTicket.epoch!=ticket.epoch ||
        mWmsRosterTicket.generation!=ticket.generation) return {};
    mRootJoins.clear();
    closeNativeFrame(ticket);
    mNativeClaims.invalidateScope();
    return std::move(mWmsRoots); // resource witnesses only; caller drops AFTER unlock
}

bool ExternalDesktopStageOwner::invalidateSubmission(ExternalDesktopStageEpochTicket ticket,
                                          int64_t displayToken, int64_t serial) {
    const bool invalidated = mEpochOwner &&
            external_desktop_input_epoch_invalidate(mEpochOwner.get(), ticket, displayToken, serial) == 1;
    if (invalidated && closeNativeFrame(ticket)) {mSourceLifetimes.invalidate(ticket);mNativeClaims.invalidate(ticket);}
    return invalidated;
}

bool ExternalDesktopStageOwner::borrowAuthority(const ExternalDesktopStageLeaseView& view) {
    if (!mContext || mAuthorityBorrows == std::numeric_limits<uint64_t>::max()) return false;
    // The real Rust lender has already run this exact view's existing
    // allowed/owns/topology predicates against borrowed UI_CLAIMS. Its shared
    // AuthorityBorrow envelope exists ONLY during this callback. No fake Live
    // is constructed to force submit/reconcile before the native registry and
    // applied geometry exist. The future complete-stage owner uses it here.
    const auto received = [](void* userdata, const ExternalDesktopStageAuthority* authority) -> uint8_t {
        auto& owner = *static_cast<ExternalDesktopStageOwner*>(userdata);
        if (!authority || !owner.mContext) return 0;
        ++owner.mAuthorityBorrows;
        return 1;
    };
    const auto now = []() -> int64_t { return android::elapsedRealtime(); };
    return external_desktop_input_with_wms_authority(&view, this, now, received) == 1;
}

void ExternalDesktopStageOwner::observeCurrentBatch(bool joinRoots) {
    if (!mCompleteBatch || !mReceipt.hasCompleteBatch) return;
    // This clock is fresh for CURRENT expiry checks only. Cached publication
    // keeps its original native arrival and sequence, including after a new
    // roster/lease handoff. It cannot masquerade as a post-submit B1.
    const int64_t now = android::elapsedRealtime();
    mSourceLifetimes.observe(*mCompleteBatch, mReceipt.observations,
                            mReceipt.nativeObservedAt, now);
    if (joinRoots) joinRootObservation(now);
}

void ExternalDesktopStageOwner::receiveFullBatch(const gui::WindowInfosUpdate& update,
                                      size_t installedWindows, size_t installedDisplayMaps) {
    suspendNativeRoutes(); // also suspended BEFORE installed-map mutation by InputDispatcher
    const int64_t observedAt = android::elapsedRealtime();
    // A previous successful callback must never masquerade as the latest full
    // observation after overflow/exhaustion. This releases evidence references,
    // NOT stage claims, active state, submissions, streams or release tails.
    mNativeClaims.invalidate();
    mCompleteBatch.reset();
    mRootJoins.clear(); // A rejected latest batch cannot reuse older root joins.
    mSourceLifetimes.rejectBatch();
    mReceipt.hasCompleteBatch = false;
    if (mReceipt.observations == std::numeric_limits<uint64_t>::max()) {
        mReceipt.nativeObservedAt = -1;
        mReceipt.state = ReceiptState::EXHAUSTED;
        invalidateNativeRoutes();
        return;
    }
    const uint64_t observations = mReceipt.observations + 1;
    mReceipt = Receipt{};
    mReceipt.observations = observations;
    mReceipt.rawWindows = update.windowInfos.size();
    mReceipt.installedWindows = installedWindows;
    mReceipt.installedDisplayMaps = installedDisplayMaps;
    mReceipt.displayInfos = update.displayInfos.size();
    mReceipt.vsyncId = update.vsyncId;
    mReceipt.timestamp = update.timestamp;
    mReceipt.nativeObservedAt = observedAt;
    if (observedAt < 0 || observedAt < mNativeArrivalHighWater) {
        mReceipt.state = ReceiptState::CLOCK_UNAVAILABLE;
        invalidateNativeRoutes();
        return;
    }
    mNativeArrivalHighWater = observedAt; // equal-millisecond callbacks are legitimate
    if (!mContext) {
        mReceipt.state = ReceiptState::CONTEXT_UNAVAILABLE;
        invalidateNativeRoutes();
        return;
    }
    if (update.windowInfos.size() > kMaxRawWindows ||
        update.displayInfos.size() > kMaxRawWindows) {
        mReceipt.state = ReceiptState::UNSUPPORTED_SIZE;
        invalidateNativeRoutes();
        return;
    }
    if (!gui::layerSourcesValid(update.layerSources,update.windowInfos,update.layerSourcesComplete)) {
        mReceipt.state = ReceiptState::UNSUPPORTED_SIZE;
        invalidateNativeRoutes();
        return;
    }
    if (!update.layerSourcesComplete) invalidateNativeRoutes(); // unavailable cannot preserve an owned route
    size_t totalRects = 0;
    size_t totalStringBytes = 0;
    for (const gui::WindowInfo& info : update.windowInfos) {
        const size_t rects = rectCount(info.touchableRegion);
        const size_t preRects = info.preViewportTouchableRegion
                ? rectCount(*info.preViewportTouchableRegion) : 0;
        if (rects > kMaxRectsPerWindow || rects > kMaxRects - totalRects ||
            preRects > kMaxRectsPerWindow ||
            preRects > kMaxRects - totalRects - rects ||
            (info.cloneLineage &&
             info.cloneLineage->mirrorRootIds.size() > gui::WindowInfo::kMaxCloneLineageDepth)) {
            mReceipt.state = ReceiptState::UNSUPPORTED_SIZE;
            invalidateNativeRoutes();
            return;
        }
        totalRects += rects + preRects;
        for (const auto* text : {&info.name, &info.packageName, &info.applicationInfo.name}) {
            if (text->size() > kMaxStringBytes - totalStringBytes) {
                mReceipt.state = ReceiptState::UNSUPPORTED_SIZE;
                invalidateNativeRoutes();
                return;
            }
            totalStringBytes += text->size();
        }
        ++mReceipt.inspectedWindows;
        mReceipt.informationalTagRows += info.stageTag.has_value();
        mReceipt.lineageRows += info.cloneLineage.has_value();
    }
    // Exact original order, duplicate channels, filtered/non-touchable rows,
    // all displays, Regions, security flags, transforms and SF lineage survive.
    // Construct only after the complete callback passes; no partial successor.
    mCompleteBatch.emplace(update);
    mReceipt.hasCompleteBatch = true;
    observeCurrentBatch(true);
    // Deliberately NO StageMachine submit/observe/route call. Source-original
    // identities are now owned. A subsequent fresh WMS root handoff may issue
    // claims only from coherent facts; this SF-only callback cannot supply
    // authority or construct Live from informational lineage/tag/vsync ID.
    // Nothing here changes DROP_ALL, focus, hit testing, event queues or routing.
    // Only the owned complete evidence is retained; absence releases no
    // root/window/stream claims. The Context itself remains owned until teardown.
}

} // namespace android::inputdispatcher
#endif
