#pragma once
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopTaskRootObservation.h"
#include <gui/LayerSourceInfo.h>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace android {
namespace inputdispatcher { class ExternalDesktopStageOwner; class ExternalDesktopNativeRootFactsTest; }
struct ExternalDesktopOwnedBlockExpectation;

// NON-Current, suspension-only bridge between TWO genuinely owned epochs.
// In production only the real native owner constructs it after the renewal lender/core
// postcheck and exact physical-binding comparison. No caller/Java scalar,
// claim issuer or generic B0/Current path can manufacture this witness.
class ExternalDesktopSuspendedRootStockScope final {
    friend class inputdispatcher::ExternalDesktopStageOwner;
    friend class inputdispatcher::ExternalDesktopNativeRootFactsTest; // metadata-only fact tests, NOT renewal authority qualification
    ExternalDesktopSuspendedRootStockScope(ExternalDesktopStageEpochTicket binding,WsKey before,WsKey after)
          : mBinding(binding),mBefore(before),mAfter(after) {}
    ExternalDesktopStageEpochTicket mBinding;
    WsKey mBefore,mAfter;
public:
    bool matches(const ExternalDesktopTaskRootCohort& current,
            const std::vector<ExternalDesktopOwnedBlockExpectation>& expected) const;
};
// Actual complete compositor facts + retained WMS objects, NOT claim issuance.
// No strong LayerHandle is retained here. Unknown graph-node identities are
// nonresource tokens; their numeric IDs/flags alone establish no ownership.
inline bool externalDesktopSameNativeSource(const gui::LayerSourceNode& node,
                                  const sp<SurfaceControl>& retained) {
    if (!retained || !retained->isValid() || retained->getLayerId() <= 0 ||
        node.id != static_cast<uint32_t>(retained->getLayerId()) ||
        !node.lifetime.identity || !node.handleAlive) return false;
    const auto borrowed = node.lifetime.publishedHandle.promote();
    // retained already owns this exact handle, so dropping this scoped borrow
    // cannot destroy the resource while the dispatcher lock is held.
    return borrowed && borrowed == retained->getHandle();
}

inline bool externalDesktopSameSourceNode(const gui::LayerSourceNode& a,const gui::LayerSourceNode& b) {
    return a.id==b.id && a.lifetime.identity==b.lifetime.identity &&
        a.lifetime.publishedHandle==b.lifetime.publishedHandle && a.parent==b.parent &&
        a.relativeParent==b.relativeParent && a.mirrorSource==b.mirrorSource && a.croppedBy==b.croppedBy &&
        a.stopAt==b.stopAt && a.ownerUid==b.ownerUid && a.requestedInputConfig==b.requestedInputConfig &&
        a.requestedDropMode==b.requestedDropMode && a.handleAlive==b.handleAlive &&
        a.ownedInputBlockOrigin==b.ownedInputBlockOrigin && a.hasInputToken==b.hasInputToken &&
        a.hasClientToken==b.hasClientToken && a.requestedCrop==b.requestedCrop &&
        a.requestedTransform==b.requestedTransform;
}

// CURRENT transition evidence only, never stable lifetime or authority. The
// private owner supplies the exact expected result of its scoped SF operation.
struct ExternalDesktopOwnedBlockExpectation {
    uint32_t root;
    sp<IBinder> origin;
    uint64_t revision;
    int64_t epoch;
    bool blocked;
    // Present ONLY for a private native pending-stage refresh, never B0.
    std::optional<gui::WindowInfo::StageTag> tag=std::nullopt;
};

// One scoped SF operation covers ONE exact current cohort and one state.
// This validates expectations, not their provenance or a permission to enable.
inline bool externalDesktopOwnedBlockCohortValid(const ExternalDesktopTaskRootCohort& current,
        const std::vector<ExternalDesktopOwnedBlockExpectation>& expected) {
    if (current.records.empty() || current.records.size()>128*8 ||
            expected.size()!=current.records.size()) return false;
    const auto ticket=current.records.front().ticket;
    if (!ticket.owner || ticket.epoch<=0 || ticket.generation<=0) return false;
    std::set<uint32_t> actual;
    for (const auto& record:current.records) {
        if (!record.observed || !record.observed->root || record.observed->root->getLayerId()<=0 ||
                record.ticket.owner!=ticket.owner || record.ticket.epoch!=ticket.epoch ||
                record.ticket.generation!=ticket.generation ||
                !actual.insert(static_cast<uint32_t>(record.observed->root->getLayerId())).second) return false;
    }
    std::set<uint32_t> roots;
    std::set<const IBinder*> origins;
    std::set<uint64_t> presentations;
    for (const auto& entry:expected) {
        if (!entry.origin || !roots.insert(entry.root).second || !actual.count(entry.root) ||
                !origins.insert(entry.origin.get()).second || entry.epoch!=ticket.epoch ||
                entry.revision==0 || entry.revision>uint64_t(INT64_MAX) ||
                entry.blocked!=expected.front().blocked ||
                entry.tag.has_value()!=expected.front().tag.has_value()) return false;
        if (entry.tag) {
            const auto& tag=*entry.tag;const auto& first=*expected.front().tag;
            if (tag.authorityGeneration!=ticket.generation || tag.atlasRevision<0 ||
                tag.geometryRevision<0 || tag.epoch!=ticket.epoch || !tag.presentationKey ||
                tag.authorityGeneration!=first.authorityGeneration || tag.atlasRevision!=first.atlasRevision ||
                tag.geometryRevision!=first.geometryRevision || tag.epoch!=first.epoch ||
                !presentations.insert(tag.presentationKey).second) return false;
        }
    }
    return true;
}

inline bool ExternalDesktopSuspendedRootStockScope::matches(const ExternalDesktopTaskRootCohort& current,
        const std::vector<ExternalDesktopOwnedBlockExpectation>& expected) const {
    if (!mBinding.owner || mBinding.generation!=mBefore.generation || mBefore.generation!=mAfter.generation ||
            mBefore.atlas!=mAfter.atlas || mBefore.geometry!=mAfter.geometry || mBefore.epoch<=0 ||
            mAfter.epoch<=mBefore.epoch ||
            (mBinding.epoch!=mBefore.epoch && mBinding.epoch!=mAfter.epoch) ||
            current.records.empty() || current.records.size()>128*8 || expected.size()!=current.records.size()) return false;
    std::set<uint32_t> actual;
    for (const auto& record:current.records) if (!record.observed || !record.observed->root ||
            record.observed->root->getLayerId()<=0 || record.ticket.owner!=mBinding.owner ||
            record.ticket.generation!=mBinding.generation || record.ticket.epoch!=mBinding.epoch ||
            !actual.insert(static_cast<uint32_t>(record.observed->root->getLayerId())).second) return false;
    std::set<uint32_t> roots;std::set<const IBinder*> origins;std::set<uint64_t> presentations;
    for (const auto& entry:expected) {
        if (!entry.origin || !roots.insert(entry.root).second || !actual.count(entry.root) ||
                !origins.insert(entry.origin.get()).second || !entry.revision || entry.revision>uint64_t(INT64_MAX) ||
                (entry.epoch!=mBefore.epoch && entry.epoch!=mAfter.epoch) ||
                entry.epoch!=expected.front().epoch || entry.blocked!=expected.front().blocked ||
                entry.tag.has_value()!=expected.front().tag.has_value()) return false;
        if (entry.tag) {
            const auto& tag=*entry.tag;
            if (tag.authorityGeneration!=mBefore.generation || tag.atlasRevision!=mBefore.atlas ||
                    tag.geometryRevision!=mBefore.geometry || tag.epoch!=entry.epoch ||
                    !tag.presentationKey || !presentations.insert(tag.presentationKey).second) return false;
        }
    }
    return true; // expected bytes still must match actual SF origins/revisions/tags below
}

inline bool externalDesktopOwnedBlockMatches(const gui::LayerSourceApplied& applied,
        const sp<IBinder>& origin, const ExternalDesktopOwnedBlockExpectation* expected) {
    if (!applied.stockProjectionComplete || applied.ownedInputBlockOrigin != origin) return false;
    if (!expected) return origin && applied.ownedDropAll; // discovery is PARKED-only
    return origin && expected->origin == origin && expected->revision != 0 &&
            expected->epoch > 0 && applied.ownedDropAll == expected->blocked &&
            applied.ownedInputBlockRevision == expected->revision &&
            applied.ownedInputBlockEpoch == expected->epoch;
}

inline bool externalDesktopRootAppliedTarget(const gui::LayerSourceRoot& root,
        const ExternalDesktopTaskRootRecord& record, const std::vector<gui::DisplayInfo>& displays,
        const ExternalDesktopOwnedBlockExpectation* expected = nullptr) {
    if (!record.observed || !gui::layerSourceFinite(root.requested) ||
        !gui::layerSourceFinite(root.applied)) return false;
    const auto& g = record.observed->geometry;
    for (int32_t value:g) if (value < -32768 || value > 32768) return false;
    if (g[2]<=g[0] || g[3]<=g[1] || g[6]<=g[4] || g[7]<=g[5] ||
        int64_t{g[2]}-g[0]!=int64_t{g[6]}-g[4] ||
        int64_t{g[3]}-g[1]!=int64_t{g[7]}-g[5]) return false;
    const bool separateOwnedBlock = root.requested.ownedInputBlockOrigin != nullptr;
    if (separateOwnedBlock) {
        if ((expected && expected->root != root.requested.id) ||
                !externalDesktopOwnedBlockMatches(root.applied, root.requested.ownedInputBlockOrigin, expected)) return false;
    } else if (root.requested.requestedDropMode != static_cast<uint32_t>(gui::DropInputMode::ALL) ||
               root.applied.ownedDropAll || expected) return false;
    // Every integer was bounded above to +/-32768, exactly representable in
    // float. Compare exact facts; never round/truncate an observed FloatRect.
    if (root.requested.requestedCrop != std::array<float,4>{float(g[0]),float(g[1]),float(g[2]),float(g[3])} ||
        root.applied.displayId != record.destination ||
        root.applied.composedDropMode != (expected && !expected->blocked ? root.applied.stockDropMode :
                static_cast<uint32_t>(gui::DropInputMode::ALL))) return false;
    const gui::DisplayInfo* display = nullptr;
    for (const auto& d : displays) if (d.displayId.val() == record.destination) {
        if (display) return false;
        display = &d;
    }
    if (!display) return false;
    // DisplayInfo.transform maps display->SF screen. Independently invert in
    // double before applying actual composed layer geometry; no integer Region
    // transform and no requested geometry masquerading as applied pixels.
    const auto d = gui::layerSourceInputMatrix(display->transform);
    for (float value : d) if (!std::isfinite(value)) return false;
    const double det = double(d[0])*d[3] - double(d[1])*d[2];
    if (!std::isfinite(det) || det == 0) return false;
    const auto& a = root.applied.layerTransform;
    // WMS submitted translation-only equal-size crop->target. Matching AABBs
    // alone would accept reflection or a square's rotation. Require the full
    // actual display-relative affine/corresponding-corner mapping as well.
    const std::array<double,6> local{
        (d[3]*double(a[0])-d[1]*double(a[2]))/det,
        (d[3]*double(a[1])-d[1]*double(a[3]))/det,
        (-d[2]*double(a[0])+d[0]*double(a[2]))/det,
        (-d[2]*double(a[1])+d[0]*double(a[3]))/det,
        (d[3]*(double(a[4])-d[4])-d[1]*(double(a[5])-d[5]))/det,
        (-d[2]*(double(a[4])-d[4])+d[0]*(double(a[5])-d[5]))/det};
    if (local!=std::array<double,6>{1,0,0,1,double(g[4])-g[0],double(g[5])-g[1]}) return false;
    const std::array<double,4> requested{double(g[0]),double(g[1]),double(g[2]),double(g[3])};
    const auto& b = root.applied.layerBounds;
    const std::array<double,4> actual{b[0],b[1],b[2],b[3]};
    // A correct requested placement with an actually clipped/different root
    // is NOT accepted as the full applied target.
    for (const auto& bounds : {requested,actual}) {
      if (bounds[2]<=bounds[0] || bounds[3]<=bounds[1]) return false;
      double left = std::numeric_limits<double>::infinity(), top = left;
      double right = -left, bottom = -left;
      for (double x : {bounds[0],bounds[2]}) for (double y : {bounds[1],bounds[3]}) {
        const double sx = double(a[0])*x + double(a[1])*y + a[4] - d[4];
        const double sy = double(a[2])*x + double(a[3])*y + a[5] - d[5];
        const double px = (d[3]*sx - d[1]*sy)/det;
        const double py = (-d[2]*sx + d[0]*sy)/det;
        if (!std::isfinite(px) || !std::isfinite(py) ||
            std::abs(px)>32768 || std::abs(py)>32768) return false;
        left=std::min(left,px); right=std::max(right,px);
        top=std::min(top,py); bottom=std::max(bottom,py);
      }
      if (!(left==g[4] && top==g[5] && right==g[6] && bottom==g[7])) return false;
    }
    return true;
}

namespace detail {
inline ExternalDesktopRootJoinReceipt joinExternalDesktopRootFactsChecked(const gui::WindowInfosUpdate& batch,
        uint64_t sequence, const ExternalDesktopTaskRootRecord& record,
        const ExternalDesktopTaskRootCohort& current, const std::vector<ExternalDesktopRootOriginal>& originals,
        const std::vector<ExternalDesktopOwnedBlockExpectation>* expected,
        const ExternalDesktopSuspendedRootStockScope* suspendedStock) {
    ExternalDesktopRootJoinReceipt out; out.batch=sequence; out.state=ExternalDesktopRootJoin::INCOMPLETE;
    if (!sequence || !record.observed || originals.empty() || !batch.layerSourcesComplete ||
        !gui::layerSourcesValid(batch.layerSources,batch.windowInfos,true)) return out;
    const auto& observed = *record.observed;
    if (!observed.root || !observed.source || !observed.destinationParent) return out;
    const uint32_t owned = static_cast<uint32_t>(observed.root->getLayerId());
    const auto expectation = [&](uint32_t root) -> const ExternalDesktopOwnedBlockExpectation* {
        if (!expected) return nullptr;
        const auto found = std::find_if(expected->begin(), expected->end(),
                [&](const auto& entry) { return entry.root == root; });
        return found == expected->end() ? nullptr : &*found;
    };
    if (expected && !(suspendedStock ? suspendedStock->matches(current,*expected) :
            externalDesktopOwnedBlockCohortValid(current,*expected))) return out;
    const auto* ownedExpected = expectation(owned);
    const bool parked = !ownedExpected || ownedExpected->blocked;
    std::set<size_t> seenOriginals;
    std::set<const IBinder*> seenSnapshots;
    std::set<int32_t> seenWindowIds;
    std::map<uint32_t,const gui::LayerSourceNode*> nodes;
    std::map<const IBinder*,uint32_t> identities;
    std::map<const IBinder*,uint32_t> ownedBlockOrigins;
    // Same full batch, never a claimant-selected row subset. A recycled ID,
    // foreign token or contradictory source geometry invalidates the proof.
    const auto inspectNode = [&](const gui::LayerSourceNode& node) {
        if (!gui::layerSourceFinite(node)) return false;
        const auto [identity, fresh] = identities.emplace(node.lifetime.identity.get(),node.id);
        if (!fresh && identity->second != node.id) return false;
        if (node.ownedInputBlockOrigin) {
            const auto [origin, freshOrigin] = ownedBlockOrigins.emplace(node.ownedInputBlockOrigin.get(),node.id);
            if (!freshOrigin && origin->second != node.id) return false;
        }
        const auto [saved, inserted] = nodes.emplace(node.id,&node);
        if (inserted) return true;
        const auto& before = *saved->second;
        return externalDesktopSameSourceNode(before,node);
    };
    for (const auto& fact : batch.layerSources) {
        for (const auto& node : fact.ancestors) if (!inspectNode(node)) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
        for (const auto& root : fact.roots) if (!inspectNode(root.requested)) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
        for (const auto& edge : fact.traversal) if (!inspectNode(edge.requested)) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
    }
    for (size_t row=0;row<batch.windowInfos.size();++row) {
        ++out.inspected;
        const auto& info=batch.windowInfos[row]; const auto& fact=batch.layerSources[row];
        if (std::find(fact.mirrorRoots.begin(),fact.mirrorRoots.end(),owned)==fact.mirrorRoots.end()) continue;
        const auto rootForBlock=std::find_if(fact.roots.begin(),fact.roots.end(),[&](const auto& root) {
            return root.requested.id==owned;
        });
        if (rootForBlock==fact.roots.end() ||
                (rootForBlock->requested.ownedInputBlockOrigin &&
                 !externalDesktopOwnedBlockMatches(fact.applied,rootForBlock->requested.ownedInputBlockOrigin,ownedExpected)) ||
                (!rootForBlock->requested.ownedInputBlockOrigin && fact.applied.ownedDropAll)) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
        // SF deliberately does NOT set the receiver DROP_INPUT bit for a
        // NO_INPUT_CHANNEL row. This typed structural exception is not flag
        // authority: exact exported token absence/composed ALL is required,
        // then ALL actual WMS-root/source/parent proofs below still must pass.
        const bool nonReceiver=!info.token && !info.windowToken &&
            !fact.applied.hasInputToken && !fact.applied.hasClientToken &&
            info.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL);
        if (!seenSnapshots.insert(fact.applied.snapshotIdentity.get()).second ||
            !seenWindowIds.insert(info.id).second || fact.relativeLoop ||
            info.stageTag!=(ownedExpected ? ownedExpected->tag:std::nullopt) ||
            info.displayId.val()!=record.destination ||
            !info.inputConfig.test(gui::WindowInfo::InputConfig::CLONE) ||
            (parked && !nonReceiver && !info.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT)) ||
            (parked && nonReceiver && fact.applied.composedDropMode!=static_cast<uint32_t>(gui::DropInputMode::ALL)) ||
            info.inputConfig.test(gui::WindowInfo::InputConfig::TRUSTED_OVERLAY)) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
        std::vector<sp<IBinder>> rootIdentities;
        for (size_t r=0;r<fact.roots.size();++r) {
            const auto& root=fact.roots[r]; const ExternalDesktopTaskRootRecord* trusted=nullptr;
            for (const auto& candidate:current.records) if (candidate.observed &&
                    candidate.observed->root && candidate.observed->root->getLayerId()==
                            static_cast<int32_t>(root.requested.id)) {
                if (trusted) { out.state=ExternalDesktopRootJoin::REJECTED; return out; }
                trusted=&candidate;
            }
            // Unknown outer/nested roots are NOT ignored because an inner
            // root is ours. A display-mirror is not a Task mirror by assertion.
            if (!trusted || !trusted->observed || !trusted->observed->source ||
                !trusted->observed->root || !trusted->observed->destinationParent ||
                trusted->ticket.owner!=record.ticket.owner ||
                trusted->ticket.epoch!=record.ticket.epoch || trusted->ticket.generation!=record.ticket.generation ||
                !externalDesktopSameNativeSource(root.requested,trusted->observed->root) ||
                root.requested.mirrorSource!=static_cast<uint32_t>(trusted->observed->source->getLayerId()) ||
                root.requested.parent!=static_cast<uint32_t>(trusted->observed->destinationParent->getLayerId()) ||
                !externalDesktopRootAppliedTarget(root,*trusted,batch.displayInfos,expectation(root.requested.id))) {
                out.state=ExternalDesktopRootJoin::REJECTED; return out;
            }
            const auto source=nodes.find(root.requested.mirrorSource);
            const auto parent=nodes.find(root.requested.parent);
            if (source==nodes.end() || parent==nodes.end() ||
                !externalDesktopSameNativeSource(*source->second,trusted->observed->source) ||
                !externalDesktopSameNativeSource(*parent->second,trusted->observed->destinationParent)) {
                out.state=ExternalDesktopRootJoin::REJECTED; return out;
            }
            std::vector<uint32_t> prefix(fact.mirrorRoots.begin(),fact.mirrorRoots.begin()+r);
            const auto anchor=std::find_if(fact.traversal.begin(),fact.traversal.end(),[&](const auto& edge) {
                return edge.sourceId==root.requested.id && edge.mirrorRoots==prefix && !edge.relativeLoop;
            });
            if (anchor==fact.traversal.end() || anchor->requested.lifetime.identity!=root.requested.lifetime.identity ||
                !(anchor->applied==root.applied)) {
                out.state=ExternalDesktopRootJoin::REJECTED; return out;
            }
            rootIdentities.push_back(root.requested.lifetime.identity);
        }
        const auto sourceAncestor=std::find_if(fact.ancestors.begin(),fact.ancestors.end(),[&](const auto& node) {
            return externalDesktopSameNativeSource(node,observed.source);
        });
        if (sourceAncestor==fact.ancestors.end()) { out.state=ExternalDesktopRootJoin::REJECTED; return out; }
        for (size_t p=1;p<fact.ancestors.size();++p) if (fact.ancestors[p-1].parent!=fact.ancestors[p].id) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
        size_t found=originals.size();
        for (size_t i=0;i<originals.size();++i) if (originals[i].sourceLayer==static_cast<int32_t>(fact.sourceId)) {
            if (found!=originals.size()) { out.state=ExternalDesktopRootJoin::REJECTED; return out; }
            found=i;
        }
        if (found==originals.size()) {
            if (info.token || info.windowToken ||
                !info.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL)) {
                out.state=ExternalDesktopRootJoin::UNRESOLVED_ANCESTOR; return out;
            }
            out.structuralSources.push_back({row,info.id,fact.ancestors.front().lifetime.identity,
                    fact.ancestors.front().lifetime.publishedHandle,rootIdentities,0,fact.applied.snapshotIdentity});
        } else {
            const auto& original=originals[found];
            if (original.uid<0 || info.token!=original.channel ||
                info.windowToken!=original.client || info.ownerUid!=gui::Uid{static_cast<uint32_t>(original.uid)}) {
                out.state=ExternalDesktopRootJoin::REJECTED; return out;
            }
            seenOriginals.insert(found); // multiple real snapshots may share this original/channel
            ++out.matched; out.originalLifetimes.emplace_back(original.child,original.window);
        }
        ExternalDesktopRootJoinReceipt::CloneSnapshot snapshot{row,info.id,fact.applied.snapshotIdentity,
                fact.ancestors.front().lifetime.identity,std::move(rootIdentities),fact.variant,
                fact.detached,fact.relativeRoots,{}};
        for (const auto& edge:fact.traversal) snapshot.actualParents.push_back({edge.sourceId,
                edge.variant,edge.detached,edge.relativeLoop,edge.mirrorRoots,edge.relativeRoots});
        out.cloneSnapshots.push_back(std::move(snapshot));
        out.cloneRows.push_back(row);
    }
    if (seenOriginals.size()==originals.size()) {
        out.state=ExternalDesktopRootJoin::CLONES_OBSERVED;
        out.coherentSourceFacts=true;
    }
    return out; // NOT a RootClaim/Live, DROP clear, stream issuance or release.
}
} // namespace detail

inline ExternalDesktopRootJoinReceipt joinExternalDesktopTaskRootFacts(const gui::WindowInfosUpdate& batch,
        uint64_t sequence,const ExternalDesktopTaskRootRecord& record,const ExternalDesktopTaskRootCohort& current,
        const std::vector<ExternalDesktopRootOriginal>& originals,
        const std::vector<ExternalDesktopOwnedBlockExpectation>* expected=nullptr) {
    // Generic Current/claim discovery is STILL exact-ticket/epoch, including
    // B0's original untagged PARKED rules. There is no public permissive flag.
    return detail::joinExternalDesktopRootFactsChecked(batch,sequence,record,current,originals,expected,nullptr);
}

struct ExternalDesktopSuspendedRootStockResult { bool exact=false; }; // deliberately NOT a Current/join/claim table
inline ExternalDesktopSuspendedRootStockResult joinExternalDesktopSuspendedRootStockFacts(const gui::WindowInfosUpdate& batch,
        uint64_t sequence,const ExternalDesktopTaskRootRecord& record,const ExternalDesktopTaskRootCohort& current,
        const std::vector<ExternalDesktopRootOriginal>& originals,const std::vector<ExternalDesktopOwnedBlockExpectation>& expected,
        const ExternalDesktopSuspendedRootStockScope& scope) {
    const auto inspected=detail::joinExternalDesktopRootFactsChecked(batch,sequence,record,current,originals,&expected,&scope);
    return {inspected.batch==sequence && inspected.coherentSourceFacts &&
        inspected.state==ExternalDesktopRootJoin::CLONES_OBSERVED}; // no snapshots/keys escape into an issuer
}
} // namespace android
#endif
