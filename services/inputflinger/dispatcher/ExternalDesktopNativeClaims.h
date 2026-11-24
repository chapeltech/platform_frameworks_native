#pragma once
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopNativeRootFacts.h"
#include "ExternalDesktopSourceLifetimes.h"
#include <gui/NativeLifetimeFacts.h>
#include <external_desktop_native_stage.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

namespace android::inputdispatcher {
// Owned native ISSUER storage, not another lease/geometry/dispatch policy.
// Only the actual StageOwner and explicitly named owner fixtures can prepare.
// mLock serializes all calls. History has nonresource/weak identities only.
class ExternalDesktopNativeClaims final {
    friend class ExternalDesktopStageOwner;
    friend class InputDispatcherTest;
    friend class ExternalDesktopNativeClaimFactoryTest;
    ExternalDesktopNativeClaims()=default;
    ExternalDesktopNativeClaims(const ExternalDesktopNativeClaims&)=delete;
    ExternalDesktopNativeClaims& operator=(const ExternalDesktopNativeClaims&)=delete;
    ExternalDesktopNativeClaims(ExternalDesktopNativeClaims&&)=delete;
    ExternalDesktopNativeClaims& operator=(ExternalDesktopNativeClaims&&)=delete;
    static constexpr size_t kMaxClaims=8192+128*8;
    static constexpr size_t kMaxHistoryBytes=16*1024*1024; // counted values, not allocator/RSS proof
    struct PreparationOwner {};
    struct Row {
        WsClaim value{}; // roots is rebound into this row's OWNED path when viewing
        std::vector<uint32_t> path;
        std::shared_ptr<const ExternalDesktopRootPhysicalHistory> physical; // weak/nonresource
        sp<IBinder> rootSource,rootIdentity;
        sp<IBinder> rootOwnedInputBlockOrigin; // NONRESOURCE, retained with its exact root history
        ExternalDesktopRootJoinReceipt::CloneSnapshot snapshot{}; // nonresource identities + full paths
    };
    struct Geometry {
        uint64_t claim=0,rootClaim=0;
        size_t row=0;
        gui::LayerSourceInfo facts; // requested != actual applied; all Binder refs nonresource/weak
        Region touchable,preViewport;
        bool hasPreViewport=false;
        std::array<int32_t,8> rootCropTarget{};
        std::optional<bool> stockCandidate=std::nullopt; // exact sealed table bit, NOT a new eligibility predicate
        // No guessed StockState::input_eligible or subtraction of merged drop.
        // Exact stock-vs-owned enable contract remains a later reviewed adapter.
    };
    struct Current {
        ExternalDesktopStageEpochTicket ticket{};
        uint64_t batch=0,revision=0;
        int64_t expiresAt=0;
        int32_t user=0,clientUid=0;
        std::vector<Geometry> rows;
        // Own the exact scoped expectations, never a caller span or authority.
        std::vector<ExternalDesktopOwnedBlockExpectation> ownedBlocks;
    };
    struct ParkedPublication {
        ExternalDesktopStageEpochTicket ticket{};
        uint64_t sourceRevision=0,registryRevision=0,batch=0;
        int64_t observedAt=-1,expiresAt=0;
        std::vector<uint64_t> claims; // sorted exact current root AND descendant keys
        std::vector<ExternalDesktopOwnedBlockExpectation> roots; // nonresource origins only
    };
    // Built before the actual Rust submit/seal call, retained ONLY after that
    // call returns a successful matching pending result. No authority/spans/SC.
    struct TaggedPlan {
        enum class Phase { DROP, DROP_PUBLISHED, ENABLED, ENABLED_PUBLISHED };
        struct Replica { WsReplica value; std::vector<uint32_t> roots; };
        struct Binding { WsBinding value; std::vector<WsRect> region; };
        struct Structural { WsStructuralBinding value; std::vector<WsRect> region; };
        WsKey key{};
        ParkedPublication b0;
        int64_t stagedAt=-1;
        int64_t expiresAt=0;
        uint64_t lastSequence=0;
        int64_t lastObservedAt=-1;
        Phase phase=Phase::DROP;
        std::vector<Replica> replicas;
        std::vector<Binding> bindings;
        std::vector<Structural> structurals;
        std::vector<ExternalDesktopOwnedBlockExpectation> expected;
        std::vector<Geometry> droppedRows; // actual published B1, nonresource/weak facts only
    };
    struct Prepared {
        Prepared()=default;
        Prepared(const Prepared&)=delete;
        Prepared& operator=(const Prepared&)=delete;
        Prepared(Prepared&&)=default; // shared owner moves out of the source
        Prepared& operator=(Prepared&&)=default;
        std::shared_ptr<const PreparationOwner> owner;
        uint64_t sourceRevision=0,registryRevision=0,preparation=0;
        Current current;
        std::optional<ParkedPublication> parked;
    };

    // Call-only nonresource values. These never retain a Connection, ledger
    // row, SC or lease, so querying them cannot prevent the teardown proved by
    // the dispatcher. Only StageOwner can obtain/consume a plan.
    struct RetirementQuery {
        uint64_t claim=0;
        uint32_t kind=0;
        sp<IBinder> token;
    };
    struct RetirementPlan {
        uint64_t revision=0;
        gui::NativePublication publication;
        std::vector<RetirementQuery> queries;
        bool more=false; // pagination only, never acceptance or authority
    };
    struct PreparedRetirement {
        uint64_t revision=0;
        int64_t sequence=0;
        gui::NativePublication publication;
        std::vector<Row> rows;
        std::vector<WsRelease> releases;
        std::vector<uint64_t> present;
        size_t bytes=0;
    };

    // Classify the WHOLE real callback using nonresource token identities,
    // including non-input traversal rows. A numeric ID/weak expiry is never
    // evidence of absence, and absence here is NOT evidence of destruction.
    std::optional<std::vector<uint64_t>> presentClaims(
            const gui::WindowInfosUpdate& full,uint64_t sequence) const {
        if (!sequence || sequence>uint64_t(INT64_MAX) || !full.layerSourcesComplete ||
                !full.nativePublication.valid() || !full.nativePublication.available() ||
                !gui::layerSourcesValid(full.layerSources,full.windowInfos,true)) return {};
        std::unordered_set<const IBinder*> layers,snapshots;
        const auto node=[&](const auto& value) {
            if(value.lifetime.identity)layers.insert(value.lifetime.identity.get());
        };
        const auto applied=[&](const auto& value) {
            if(value.snapshotIdentity)snapshots.insert(value.snapshotIdentity.get());
        };
        for(const auto& facts:full.layerSources){
            applied(facts.applied);
            for(const auto& value:facts.ancestors)node(value);
            for(const auto& value:facts.roots){node(value.requested);applied(value.applied);}
            for(const auto& value:facts.traversal){node(value.requested);applied(value.applied);}
        }
        std::vector<uint64_t> result;result.reserve(mRows.size());
        for(const auto& row:mRows){
            const bool present=row.value.kind==WS_ROOT_CLAIM ?
                layers.count(row.rootIdentity.get()) : snapshots.count(row.snapshot.snapshot.get());
            if(present)result.push_back(row.value.key);
        }
        return result; // mRows is the original monotonically allocated sorted history.
    }
    std::optional<RetirementPlan> retirementPlan(const gui::WindowInfosUpdate& full,
            uint64_t sequence,uint64_t after) const {
        const auto present=presentClaims(full,sequence);if(!present)return {};
        RetirementPlan out;out.revision=mRevision;out.publication=full.nativePublication;
        // Bound one asynchronous query job. The owner rotates its cursor; an
        // early still-live object must not starve later destroyed objects.
        constexpr size_t kMaxQueries=64;
        for(const auto& row:mRows){
            if(row.value.key<=after || std::binary_search(present->begin(),present->end(),row.value.key))continue;
            auto token=row.value.kind==WS_ROOT_CLAIM?row.rootIdentity:row.snapshot.snapshot;
            if(!token)continue;
            if(out.queries.size()==kMaxQueries){out.more=true;break;}
            out.queries.push_back({row.value.key,row.value.kind,std::move(token)});
        }
        if(out.queries.empty())return {};
        return out;
    }
    std::optional<PreparedRetirement> prepareRetirement(const RetirementPlan& plan,
            const std::vector<gui::NativeLifetimeFact>& facts,const gui::WindowInfosUpdate& full,
            uint64_t sequence,const std::vector<uint64_t>& drained) const {
        const auto present=presentClaims(full,sequence);
        if(!present || plan.revision!=mRevision || mRevision==UINT64_MAX ||
                facts.size()!=plan.queries.size() || plan.queries.empty() ||
                plan.publication.origin!=full.nativePublication.origin ||
                plan.publication.serial>full.nativePublication.serial ||
                plan.publication.mainPass>full.nativePublication.mainPass ||
                !std::is_sorted(drained.begin(),drained.end()) ||
                std::adjacent_find(drained.begin(),drained.end())!=drained.end())return {};
        std::map<uint64_t,uint64_t> removals;
        for(size_t i=0;i<plan.queries.size();++i){
            const auto& query=plan.queries[i];const auto& fact=facts[i];
            const auto row=std::find_if(mRows.begin(),mRows.end(),[&](const auto& value){return value.value.key==query.claim;});
            if(row==mRows.end() || row->value.kind!=query.kind ||
                    query.token!=(query.kind==WS_ROOT_CLAIM?row->rootIdentity:row->snapshot.snapshot))return {};
            const auto kind=query.kind==WS_ROOT_CLAIM?gui::NativeLifetimeKind::RootResourceGroup:
                    gui::NativeLifetimeKind::Snapshot;
            if(fact.kind!=kind || !gui::nativeLifetimeFactWithinPublication(fact,full.nativePublication) ||
                    std::binary_search(present->begin(),present->end(),query.claim) ||
                    !std::binary_search(drained.begin(),drained.end(),query.claim))continue;
            removals.emplace(query.claim,fact.removalPass);
        }
        // A root is indivisible from ALL retained descendants. A bounded
        // partial query job may release independently destroyed children, but
        // must not release a root while any unqueried/unproved child survives.
        for(const auto& row:mRows)if(row.value.kind!=WS_ROOT_CLAIM && !removals.count(row.value.key))
            removals.erase(row.value.root_claim);
        if(removals.empty())return {};
        PreparedRetirement out;out.revision=mRevision+1;out.sequence=sequence;
        out.publication=full.nativePublication;out.present=*present;
        out.rows.reserve(mRows.size()-removals.size());out.releases.reserve(removals.size());
        for(const auto& row:mRows){
            const auto found=removals.find(row.value.key);
            if(found!=removals.end()){
                // Each original claim/path is still owned by mRows while the
                // synchronous C facade makes its own complete nested copy.
                auto claim=row.value;claim.roots={row.path.data(),row.path.size()};
                out.releases.push_back({claim,1,found->second,int64_t(sequence),1,1});
            }else{
                const auto dynamic=ExternalDesktopSourceLifetimes::snapshotOwnedBytes(row.snapshot);
                if(!dynamic)return {};
                out.bytes+=sizeof(Row)+*dynamic+row.path.size()*sizeof(uint32_t)+
                        (row.physical?sizeof(ExternalDesktopRootPhysicalHistory):0);
                if(out.bytes>kMaxHistoryBytes)return {};
                out.rows.push_back(row);
            }
        }
        return out;
    }
    bool commitRetirement(PreparedRetirement&& prepared,ExternalDesktopStageContext* context) {
        if(!context || mRevision==UINT64_MAX || prepared.revision!=mRevision+1 ||
                prepared.releases.empty())return false;
        // Native preparation burns rows BEFORE any successful core call. A
        // boundary/copy failure can therefore leave genuine native history
        // which the Rust owner never accepted. Query its exact ledger instead
        // of inventing a Live to expose those rows just so they can be freed.
        std::vector<uint64_t> known(kMaxClaims);uint64_t revision=0;size_t count=0;
        if(external_desktop_stage_claim_inventory(context,known.data(),known.size(),&revision,&count)!=1 ||
                count>known.size() || revision>mRevision || (!revision && count))return false;
        known.resize(count);
        if(!std::is_sorted(known.begin(),known.end()) ||
                std::adjacent_find(known.begin(),known.end())!=known.end())return false;
        for(const auto key:known)if(std::none_of(mRows.begin(),mRows.end(),
                [&](const auto& row){return row.value.key==key;}))return false;
        const auto retained=[&](uint64_t key){return std::binary_search(known.begin(),known.end(),key);};
        std::vector<WsClaim> rows;std::vector<WsRelease> releases;std::vector<uint64_t> present;
        rows.reserve(prepared.rows.size());releases.reserve(prepared.releases.size());
        for(const auto& row:prepared.rows)if(retained(row.value.key)){
            auto value=row.value;value.roots={row.path.data(),row.path.size()};rows.push_back(value);}
        for(const auto& value:prepared.releases)if(retained(value.claim.key))releases.push_back(value);
        for(const auto key:prepared.present)if(retained(key))present.push_back(key);
        // If unexposed native rows survive, the next full native registry must
        // use a distinct revision from this subset accepted by the Rust owner.
        // Burn that revision too; never give two different tables one revision.
        const bool different=!releases.empty() && rows.size()!=prepared.rows.size();
        if(different && prepared.revision==UINT64_MAX)return false;
        if(!releases.empty()){
            const WsRetirement input{prepared.sequence,prepared.publication.serial,
                prepared.publication.mainPass,1,{present.data(),present.size()},
                prepared.revision,{rows.data(),rows.size()},{releases.data(),releases.size()}};
            WsResult result{};
            if(external_desktop_stage_retire_claims(context,&input,&result)!=1 || result.refusal!=WS_NO_REFUSAL)return false;
        }
        // Only no-throw owner updates follow the real Rust atomic acceptance.
        // Burned high-watermarks, sticky exhaustion and stage generation stay
        // unchanged. This cannot mint Current, a lease, input or APPLIED.
        const bool parked=mParked && std::none_of(prepared.releases.begin(),prepared.releases.end(),
            [&](const auto& value){return std::binary_search(mParked->claims.begin(),mParked->claims.end(),value.claim.key);});
        mRows.swap(prepared.rows);mRevision=prepared.revision+(different?1:0);mHistoryBytes=prepared.bytes;
        // The exact unchanged B0 cohort may retain its physical/publication
        // facts. Only its GLOBAL registry anchor changes after real core
        // acceptance. Current remains unavailable until a fresh WMS handoff.
        if(parked)mParked->registryRevision=mRevision;else mParked.reset();
        invalidate();return true;
    }

    std::optional<uint64_t> nextClaim() {
        if (mRows.size()>=kMaxClaims || mHigh==std::numeric_limits<uint64_t>::max() ||
                mRevision==std::numeric_limits<uint64_t>::max()) {mIssuanceStopped=true;return {};}
        ++mRevision;return ++mHigh; // burn BEFORE any external Rust exposure
    }
    static bool sameEnd(WsEndpoint a,WsEndpoint b) {
        return a.token==b.token && a.incarnation==b.incarnation;
    }
    bool appendRow(Row&& row) {
        const auto dynamic=ExternalDesktopSourceLifetimes::snapshotOwnedBytes(row.snapshot);
        if (!dynamic || row.path.size()>50) {mIssuanceStopped=true;return false;}
        const size_t bytes=sizeof(Row)+*dynamic+row.path.size()*sizeof(uint32_t)+
                (row.physical ? sizeof(ExternalDesktopRootPhysicalHistory):0);
        if (bytes>kMaxHistoryBytes-mHistoryBytes) {mIssuanceStopped=true;return false;}
        mRows.push_back(std::move(row));mHistoryBytes+=bytes;return true;
    }
    std::optional<uint64_t> rootClaim(const ExternalDesktopTaskRootRecord& record,
            const gui::LayerSourceInfo& facts,ExternalDesktopSourceLifetimes& source,bool allowNew=true) {
        const auto physical=externalDesktopRootPhysical(record);if (!physical) return {};
        const auto id=static_cast<uint32_t>(record.observed->root->getLayerId());
        const gui::LayerSourceRoot* root=nullptr;
        for (const auto& entry:facts.roots) if (entry.requested.id==id) root=&entry;
        if (!root || !root->requested.lifetime.identity || facts.mirrorRoots.empty()) return {};
        const auto originalSource=std::find_if(facts.ancestors.begin(),facts.ancestors.end(),[&](const auto& node) {
            return node.id==root->requested.mirrorSource;
        });
        if (originalSource==facts.ancestors.end()) return {};
        const WsEndpoint from{record.observed->endpoints[0],static_cast<uint64_t>(record.observed->endpoints[1])};
        const WsEndpoint to{record.observed->endpoints[2],static_cast<uint64_t>(record.observed->endpoints[3])};
        for (const auto& row:mRows) if (row.value.kind==WS_ROOT_CLAIM &&
                (row.value.root_id==id || row.rootIdentity==root->requested.lifetime.identity)) {
            const auto& old=*row.physical;
            if (old.root!=physical->root || old.source!=physical->source ||
                old.producer!=physical->producer || old.task!=physical->task ||
                old.owner!=physical->owner || old.generation!=physical->generation ||
                old.user!=physical->user || old.clientUid!=physical->clientUid ||
                old.endpoints!=physical->endpoints || old.destination!=physical->destination ||
                row.rootIdentity!=root->requested.lifetime.identity ||
                row.rootOwnedInputBlockOrigin!=root->requested.ownedInputBlockOrigin ||
                row.rootSource!=originalSource->lifetime.identity ||
                row.path!=facts.mirrorRoots || !sameEnd(row.value.source,from) ||
                !sameEnd(row.value.destination,to)) return {};
            return row.value.key; // Geometry/frame/submission NEVER identifies this lifetime.
        }
        if (!allowNew) return {};
        const auto lifetime=source.reserveClaimLifetime();if (!lifetime) return {};
        const auto key=nextClaim();if (!key) return {}; // lifetime stays burned on refusal
        Row row;row.value.key=*key;row.value.generation=record.ticket.generation;
        row.value.kind=WS_ROOT_CLAIM;row.value.lifetime=*lifetime;row.value.root_id=id;
        row.value.source=from;row.value.destination=to;row.path=facts.mirrorRoots;
        row.physical=std::make_shared<const ExternalDesktopRootPhysicalHistory>(*physical);
        row.rootIdentity=root->requested.lifetime.identity;
        row.rootOwnedInputBlockOrigin=root->requested.ownedInputBlockOrigin;
        row.rootSource=originalSource->lifetime.identity;
        if (!appendRow(std::move(row))) return {};return key;
    }
    std::optional<uint64_t> descendant(uint32_t kind,uint64_t lifetime,uint64_t root,
            const ExternalDesktopTaskRootRecord& record,const ExternalDesktopRootJoinReceipt::CloneSnapshot& snapshot,
            const gui::LayerSourceInfo& facts,const ExternalDesktopRootOriginal* original,bool allowNew=true) {
        for (const auto& row:mRows) if (row.value.kind!=WS_ROOT_CLAIM && row.value.lifetime==lifetime) {
            if (row.value.kind!=kind || row.value.root_claim!=root ||
                row.value.generation!=record.ticket.generation ||
                !ExternalDesktopSourceLifetimes::sameClone(row.snapshot,snapshot) ||
                (original && (row.value.channel!=original->channelKey || row.value.client!=original->clientKey))) return {};
            return row.value.key;
        }
        if (!allowNew) return {};
        const auto key=nextClaim();if (!key) return {};
        Row row;row.value.key=*key;row.value.generation=record.ticket.generation;
        row.value.kind=kind;row.value.lifetime=lifetime;row.value.root_claim=root;
        row.value.window_id=snapshot.windowId;row.value.source_layer=facts.sourceId;
        row.value.destination={record.observed->endpoints[2],static_cast<uint64_t>(record.observed->endpoints[3])};
        if (original) {row.value.channel=original->channelKey;row.value.client=original->clientKey;}
        row.snapshot=snapshot;row.snapshot.row=0;
        if (!appendRow(std::move(row))) return {};return key;
    }
    std::vector<WsClaim> views() const {
        std::vector<WsClaim> rows;rows.reserve(mRows.size());
        for (const auto& row:mRows) {
            auto value=row.value;value.roots={row.path.data(),row.path.size()};rows.push_back(value);
        }
        return rows; // views only live synchronously while this serialized owner is unchanged
    }
    static bool sameTicket(ExternalDesktopStageEpochTicket a,ExternalDesktopStageEpochTicket b) {
        return a.owner==b.owner && a.epoch==b.epoch && a.generation==b.generation;
    }
    static std::vector<uint64_t> currentClaimKeys(const Current& current) {
        std::vector<uint64_t> keys;keys.reserve(current.rows.size()*2);
        for (const auto& row:current.rows) {keys.push_back(row.claim);keys.push_back(row.rootClaim);}
        std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
        return keys;
    }
    static bool sameOwnedBlocks(const std::vector<ExternalDesktopOwnedBlockExpectation>& a,
            const std::vector<ExternalDesktopOwnedBlockExpectation>& b) {
        if (a.size()!=b.size()) return false;
        for (size_t i=0;i<a.size();++i) if (a[i].root!=b[i].root || a[i].origin!=b[i].origin ||
                a[i].revision!=b[i].revision || a[i].epoch!=b[i].epoch || a[i].blocked!=b[i].blocked ||
                a[i].tag!=b[i].tag) return false;
        return true;
    }
    std::optional<uint64_t> knownDescendant(uint32_t kind,uint64_t root,
            const ExternalDesktopRootJoinReceipt::CloneSnapshot& snapshot,const ExternalDesktopRootOriginal* original) const {
        for (const auto& row:mRows) if (row.value.kind==kind && row.value.root_claim==root &&
                ExternalDesktopSourceLifetimes::sameClone(row.snapshot,snapshot) &&
                (!original || (row.value.channel==original->channelKey && row.value.client==original->clientKey)))
            return row.value.lifetime;
        return {};
    }
    static bool sameKey(WsKey a,WsKey b) {
        return a.generation==b.generation && a.atlas==b.atlas && a.geometry==b.geometry && a.epoch==b.epoch;
    }
    bool matchesRegistry(const WsLive& live) const {
        if (live.registry_revision!=mRevision || live.claims.len!=mRows.size() ||
                (live.claims.len && !live.claims.data) || live.releases.len) return false;
        const auto actual=views();
        for (size_t i=0;i<actual.size();++i) {
            const auto& a=actual[i];const auto& b=live.claims.data[i];
            if (a.key!=b.key || a.generation!=b.generation || a.kind!=b.kind || a.lifetime!=b.lifetime ||
                    a.root_id!=b.root_id || !sameEnd(a.source,b.source) || !sameEnd(a.destination,b.destination) ||
                    a.window_id!=b.window_id || a.source_layer!=b.source_layer || a.channel!=b.channel ||
                    a.client!=b.client || a.root_claim!=b.root_claim || a.roots.len!=b.roots.len ||
                    (b.roots.len && !b.roots.data) ||
                    (a.roots.len && !std::equal(a.roots.data,a.roots.data+a.roots.len,b.roots.data))) return false;
        }
        return true; // No release inference/API exists in this slice.
    }
    std::optional<TaggedPlan> copyTaggedPlan(const WsStage& stage,const WsLive& live) const {
        if (!mCurrent || !mParked || mCurrent->batch!=mParked->batch ||
                currentClaimKeys(*mCurrent)!=mParked->claims || mRevision!=mParked->registryRevision ||
                live.registry_revision!=mRevision || live.now<0 || live.now>=mParked->expiresAt ||
                stage.key.generation!=mParked->ticket.generation || stage.key.atlas<0 ||
                stage.key.geometry<0 || stage.key.epoch!=mParked->ticket.epoch ||
                stage.replicas.len!=mParked->roots.size() || stage.replicas.len==0 ||
                stage.replicas.len>1024 || !stage.replicas.data || stage.bindings.len>8192 ||
                stage.structural_bindings.len>8192-stage.bindings.len ||
                (stage.bindings.len && !stage.bindings.data) ||
                (stage.structural_bindings.len && !stage.structural_bindings.data)) return {};
        TaggedPlan plan;plan.key=stage.key;plan.b0=*mParked;plan.stagedAt=live.now;
        plan.expiresAt=std::min(stage.expires_at,mParked->expiresAt);
        plan.lastSequence=mParked->batch;plan.lastObservedAt=mParked->observedAt;
        std::set<uint64_t> presentations,rootClaims;
        size_t rects=0;
        for (size_t i=0;i<stage.replicas.len;++i) {
            auto value=stage.replicas.data[i];
            const auto row=std::find_if(mRows.begin(),mRows.end(),[&](const auto& r) {
                return r.value.kind==WS_ROOT_CLAIM && r.value.key==value.root_claim;
            });
            if (row==mRows.end() || !presentations.insert(value.key).second || !value.key ||
                    !rootClaims.insert(value.root_claim).second || value.roots.len!=row->path.size() ||
                    (value.roots.len && !value.roots.data) ||
                    !std::equal(row->path.begin(),row->path.end(),value.roots.data) ||
                    !sameEnd(value.source,row->value.source) || !sameEnd(value.destination,row->value.destination)) return {};
            const auto root=std::find_if(plan.b0.roots.begin(),plan.b0.roots.end(),[&](const auto& r) {
                return r.root==row->value.root_id;
            });
            if (root==plan.b0.roots.end() || !root->blocked || root->tag ||
                    root->revision>uint64_t(INT64_MAX)-2 || root->epoch!=stage.key.epoch) return {};
            auto expected=*root;++expected.revision;
            expected.tag=gui::WindowInfo::StageTag{stage.key.generation,stage.key.atlas,
                    stage.key.geometry,stage.key.epoch,value.key};
            plan.expected.push_back(std::move(expected));
            value.roots={};plan.replicas.push_back({value,row->path});
        }
        const auto copyRegion=[&](WsRects region)->std::optional<std::vector<WsRect>> {
            if (region.len>1024 || region.len>32768-rects || (region.len && !region.data)) return {};
            rects+=region.len;
            if (!region.len) return std::vector<WsRect>{};
            return std::vector<WsRect>(region.data,region.data+region.len);
        };
        for (size_t i=0;i<stage.bindings.len;++i) {
            auto value=stage.bindings.data[i];auto region=copyRegion(value.region);if (!region) return {};
            value.region={};plan.bindings.push_back({value,std::move(*region)});
        }
        for (size_t i=0;i<stage.structural_bindings.len;++i) {
            auto value=stage.structural_bindings.data[i];auto region=copyRegion(value.region);if (!region) return {};
            value.region={};plan.structurals.push_back({value,std::move(*region)});
        }
        return plan;
    }
    static bool sameRegion(const Region& actual,const std::vector<WsRect>& expected) {
        const size_t count=actual.isEmpty()?0:static_cast<size_t>(actual.end()-actual.begin());
        if (count!=expected.size()) return false;
        for (size_t i=0;i<count;++i) {
            const auto& a=actual.begin()[i];const auto& b=expected[i];
            if (a.left!=b.left || a.top!=b.top || a.right!=b.right || a.bottom!=b.bottom) return false;
        }
        return true;
    }
    bool taggedGeometryMatches(const TaggedPlan& plan,Current& current,
            const ExternalDesktopTaskRootCohort& roots,const ExternalDesktopSourceLifetimes& source) const {
        std::set<size_t> bindings,structurals,replicas;
        for (auto& geometry:current.rows) {
            const auto replica=std::find_if(plan.replicas.begin(),plan.replicas.end(),[&](const auto& r) {
                return r.value.root_claim==geometry.rootClaim;
            });
            const auto claim=std::find_if(mRows.begin(),mRows.end(),[&](const auto& r) {return r.value.key==geometry.claim;});
            if (replica==plan.replicas.end() || claim==mRows.end()) return false;
            const auto& r=replica->value;replicas.insert(replica-plan.replicas.begin());
            const std::array<int32_t,8> crop{r.source_rect.left,r.source_rect.top,r.source_rect.right,r.source_rect.bottom,
                    r.destination_rect.left,r.destination_rect.top,r.destination_rect.right,r.destination_rect.bottom};
            if (geometry.rootCropTarget!=crop) return false;
            const auto matches=[&](const auto& binding) {
                const auto& b=binding.value;const auto& a=geometry.facts.applied;
                return b.presentation==r.key && std::equal(a.inputTransform.begin(),a.inputTransform.end(),b.transform) &&
                    a.inputFrame==std::array<int32_t,4>{b.crop.left,b.crop.top,b.crop.right,b.crop.bottom} &&
                    a.stockProjectionComplete && a.stockInputConfig==b.stock.input_config &&
                    a.stockDropMode==b.stock.drop_mode && sameRegion(geometry.touchable,binding.region);
            };
            if (claim->value.kind==WS_STRUCTURAL_CLAIM) {
                const auto b=std::find_if(plan.structurals.begin(),plan.structurals.end(),[&](const auto& candidate) {
                    return candidate.value.claim==geometry.claim && candidate.value.presentation==r.key;
                });
                if (b==plan.structurals.end() || !structurals.insert(b-plan.structurals.begin()).second ||
                        !matches(*b) || b->value.snapshot_can_receive_input!=geometry.facts.applied.snapshotCanReceiveInput) return false;
                geometry.stockCandidate=b->value.stock.candidate!=0;
            } else {
                const auto root=std::find_if(roots.records.begin(),roots.records.end(),[&](const auto& candidate) {
                    return candidate.observed && candidate.observed->root &&
                        static_cast<uint32_t>(candidate.observed->root->getLayerId())==
                        geometry.facts.mirrorRoots.back();
                });
                if (root==roots.records.end()) return false;
                const auto originals=source.originalsForRoot(root->ticket,root->observed->task,
                        root->observed->source,current.batch);if (!originals) return false;
                const auto original=std::find_if(originals->begin(),originals->end(),[&](const auto& o) {
                    return o.sourceLayer==static_cast<int32_t>(claim->value.source_layer) &&
                        o.channelKey==claim->value.channel && o.clientKey==claim->value.client;
                });
                if (original==originals->end()) return false;
                const auto b=std::find_if(plan.bindings.begin(),plan.bindings.end(),[&](const auto& candidate) {
                    return candidate.value.child==original->child && candidate.value.presentation==r.key;
                });
                if (b==plan.bindings.end() || !bindings.insert(b-plan.bindings.begin()).second || !matches(*b)) return false;
                geometry.stockCandidate=b->value.stock.candidate!=0;
            }
        }
        return bindings.size()==plan.bindings.size() && structurals.size()==plan.structurals.size() &&
                replicas.size()==plan.replicas.size();
    }
    std::optional<Prepared> prepareTagged(const ExternalDesktopStageLeaseView& view,
            const ExternalDesktopTaskRootCohort& roots,const gui::WindowInfosUpdate& full,uint64_t sequence,
            ExternalDesktopSourceLifetimes& source,const TaggedPlan& plan) {
        invalidate();
        if ((plan.phase!=TaggedPlan::Phase::DROP && plan.phase!=TaggedPlan::Phase::ENABLED) ||
                !mParked || !sameTicket(mParked->ticket,plan.b0.ticket) ||
                mParked->registryRevision!=plan.b0.registryRevision || mRevision!=plan.b0.registryRevision ||
                mParked->claims!=plan.b0.claims || !sameOwnedBlocks(mParked->roots,plan.b0.roots) ||
                !source.mCurrent || !sameTicket(source.mCurrent->ticket,plan.b0.ticket) ||
                source.mRevision<plan.b0.sourceRevision ||
                sequence<=plan.lastSequence || source.mCurrent->observedAt<plan.stagedAt ||
                source.mCurrent->observedAt<plan.lastObservedAt || view.observed_at<source.mCurrent->observedAt ||
                view.observed_at>=plan.expiresAt || !externalDesktopOwnedBlockCohortValid(roots,plan.expected)) return {};
        const bool drop=plan.phase==TaggedPlan::Phase::DROP;
        for (const auto& entry:plan.expected) {
            const auto old=std::find_if(plan.b0.roots.begin(),plan.b0.roots.end(),[&](const auto& r){return r.root==entry.root;});
            const auto replica=std::find_if(plan.replicas.begin(),plan.replicas.end(),[&](const auto& r) {
                return !r.roots.empty() && r.roots.back()==entry.root;
            });
            if (old==plan.b0.roots.end() || replica==plan.replicas.end()) return {};
            const gui::WindowInfo::StageTag wanted{plan.key.generation,plan.key.atlas,
                    plan.key.geometry,plan.key.epoch,replica->value.key};
            if (entry.origin!=old->origin ||
                    entry.revision!=old->revision+(drop?1u:2u) || entry.epoch!=plan.key.epoch || entry.blocked!=drop ||
                    entry.tag!=std::optional<gui::WindowInfo::StageTag>{wanted}) return {};
        }
        auto result=prepareImpl(view,roots,full,sequence,source,&plan.expected,true);
        if (!result || currentClaimKeys(result->current)!=plan.b0.claims ||
                !taggedGeometryMatches(plan,result->current,roots,source)) return {};
        return result;
    }
    std::optional<Prepared> prepareStillDropped(const ExternalDesktopStageLeaseView& view,
            const ExternalDesktopTaskRootCohort& roots,const gui::WindowInfosUpdate& full,uint64_t sequence,
            ExternalDesktopSourceLifetimes& source,const TaggedPlan& plan) {
        if (plan.phase!=TaggedPlan::Phase::ENABLED) return {};
        auto previous=plan;previous.phase=TaggedPlan::Phase::DROP;
        for (auto& entry:previous.expected) {entry.blocked=true;--entry.revision;}
        // Exact former B1 state, SAME identities/tags/geometry/complete rows.
        // This is unpublished evidence for the real Rust NotEnabled decision,
        // never a C++ WAIT policy or a new claim/epoch publication.
        auto candidate=prepareTagged(view,roots,full,sequence,source,previous);
        if (!candidate || candidate->current.rows.size()!=plan.droppedRows.size()) return {};
        for (const auto& row:candidate->current.rows) {
            const auto old=std::find_if(plan.droppedRows.begin(),plan.droppedRows.end(),[&](const auto& priorRow) {
                return priorRow.claim==row.claim && priorRow.rootClaim==row.rootClaim;
            });
            if (old==plan.droppedRows.end() || !row.touchable.hasSameRects(old->touchable) ||
                    row.hasPreViewport!=old->hasPreViewport || !row.preViewport.hasSameRects(old->preViewport) ||
                    row.rootCropTarget!=old->rootCropTarget || row.stockCandidate!=old->stockCandidate ||
                    !sameFacts(row.facts,old->facts)) return {};
        }
        return candidate;
    }
    static bool sameFacts(const gui::LayerSourceInfo& a,const gui::LayerSourceInfo& b) {
        // A row's current array index is not its physical snapshot identity.
        if (a.sourceId!=b.sourceId || a.variant!=b.variant || a.detached!=b.detached ||
                a.relativeLoop!=b.relativeLoop || a.mirrorRoots!=b.mirrorRoots || a.relativeRoots!=b.relativeRoots ||
                !(a.applied==b.applied) || a.ancestors.size()!=b.ancestors.size() ||
                a.roots.size()!=b.roots.size() || a.traversal.size()!=b.traversal.size()) return false;
        for (size_t i=0;i<a.ancestors.size();++i) if (!externalDesktopSameSourceNode(a.ancestors[i],b.ancestors[i])) return false;
        for (size_t i=0;i<a.roots.size();++i) if (!externalDesktopSameSourceNode(a.roots[i].requested,b.roots[i].requested) ||
                !(a.roots[i].applied==b.roots[i].applied)) return false;
        for (size_t i=0;i<a.traversal.size();++i) {
            const auto& x=a.traversal[i];const auto& y=b.traversal[i];
            if (x.sourceId!=y.sourceId || x.variant!=y.variant || x.detached!=y.detached || x.relativeLoop!=y.relativeLoop ||
                    x.mirrorRoots!=y.mirrorRoots || x.relativeRoots!=y.relativeRoots ||
                    !externalDesktopSameSourceNode(x.requested,y.requested) || !(x.applied==y.applied)) return false;
        }
        return true;
    }
    std::optional<Prepared> prepare(const ExternalDesktopStageLeaseView& view,
            const ExternalDesktopTaskRootCohort& roots,const gui::WindowInfosUpdate& full,
            uint64_t sequence,ExternalDesktopSourceLifetimes& source,
            const std::vector<ExternalDesktopOwnedBlockExpectation>* expected=nullptr) {
        // An arbitrary expected vector is NOT a pending native stage. In
        // particular, tagged owned-ALL must never enter this minting path.
        if (expected) {invalidate();return {};}
        return prepareImpl(view,roots,full,sequence,source,nullptr,false);
    }
    std::optional<Prepared> prepareImpl(const ExternalDesktopStageLeaseView& view,
            const ExternalDesktopTaskRootCohort& roots,const gui::WindowInfosUpdate& full,
            uint64_t sequence,ExternalDesktopSourceLifetimes& source,
            const std::vector<ExternalDesktopOwnedBlockExpectation>* expected,bool reuseOnly) {
        invalidate(); // Refusal invalidates availability, NOT retained history or burned IDs.
        if (mPreparationExhausted || mIssuanceStopped) return {};
        if (roots.records.empty() || roots.records.size()>128*8 || !full.layerSourcesComplete ||
                !gui::layerSourcesValid(full.layerSources,full.windowInfos,true) || !source.mCurrent ||
                source.mCurrent->join!=ExternalDesktopSourceLifetimes::Join::ORIGINALS_OBSERVED ||
                source.mCurrent->batchSequence!=sequence || sequence==0) return {};
        if (expected && !externalDesktopOwnedBlockCohortValid(roots,*expected)) return {};
        Prepared result;result.owner=mOwner;result.current.batch=sequence;
        result.current.ticket=roots.records.front().ticket;
        result.preparation=mPreparation;mPreparingTicket=result.current.ticket;
        result.current.expiresAt=source.mCurrent->expiresAt;
        result.current.user=view.user;result.current.clientUid=view.expected_client_uid;
        if (expected) result.current.ownedBlocks=*expected;
        std::vector<ExternalDesktopOwnedBlockExpectation> parkedRoots;
        std::set<size_t> assignedRows;
        size_t rects=0;
        for (const auto& record:roots.records) {
            if (!record.observed || record.ticket.owner!=source.mDomain ||
                record.ticket.owner!=result.current.ticket.owner || record.ticket.epoch!=result.current.ticket.epoch ||
                record.ticket.generation!=view.generation || record.user!=view.user ||
                record.clientUid!=view.expected_client_uid || view.observed_at<0 || view.observed_at>=record.expires ||
                record.ticket.owner!=source.mCurrent->ticket.owner ||
                record.ticket.epoch!=source.mCurrent->ticket.epoch ||
                record.ticket.generation!=source.mCurrent->ticket.generation) return {};
            result.current.expiresAt=std::min(result.current.expiresAt,record.expires);
            const auto originals=source.originalsForRoot(record.ticket,record.observed->task,
                    record.observed->source,sequence);if (!originals) return {};
            const auto joined=joinExternalDesktopTaskRootFacts(full,sequence,record,roots,*originals,expected);
            if (!joined.coherentSourceFacts || joined.state!=ExternalDesktopRootJoin::CLONES_OBSERVED ||
                    joined.inspected!=full.windowInfos.size() || joined.cloneSnapshots.empty()) return {};
            const auto& first=full.layerSources[joined.cloneSnapshots.front().row];
            const auto root=rootClaim(record,first,source,!reuseOnly);
            if (!root) return {};
            if (!reuseOnly) for (const auto& entry:first.roots)
                if (entry.requested.id==static_cast<uint32_t>(record.observed->root->getLayerId()) &&
                        entry.requested.ownedInputBlockOrigin)
                    parkedRoots.push_back({entry.requested.id,entry.requested.ownedInputBlockOrigin,
                        entry.applied.ownedInputBlockRevision,entry.applied.ownedInputBlockEpoch,true});
            for (const auto& snapshot:joined.cloneSnapshots) {
                if (snapshot.row>=full.windowInfos.size() || !assignedRows.insert(snapshot.row).second ||
                        assignedRows.size()>8192) return {}; // SAME core disallows ambiguously nested owned roots.
                const auto& info=full.windowInfos[snapshot.row];const auto& facts=full.layerSources[snapshot.row];
                // Every descendant of this physical root must use the SAME
                // ordered root path. No claimant subset or unknown outer root.
                const auto rootRow=std::find_if(mRows.begin(),mRows.end(),[&](const auto& row){return row.value.key==*root;});
                if (rootRow==mRows.end() || rootRow->path!=facts.mirrorRoots) return {};
                const ExternalDesktopRootOriginal* original=nullptr;
                for (const auto& item:*originals) if (item.sourceLayer==static_cast<int32_t>(facts.sourceId)) {
                    if (original) return {};original=&item;
                }
                uint32_t kind=WS_STRUCTURAL_CLAIM;std::optional<uint64_t> lifetime;
                if (original) {
                    kind=WS_WINDOW_CLAIM;
                    lifetime=reuseOnly ? knownDescendant(kind,*root,snapshot,original):
                        source.reserveCloneLifetime(snapshot,*original,info);
                } else {
                    if (info.token || info.windowToken || !info.inputConfig.test(gui::WindowInfo::InputConfig::NO_INPUT_CHANNEL)) return {};
                    lifetime=reuseOnly ? knownDescendant(kind,*root,snapshot,nullptr):
                        source.structuralClaimLifetime(snapshot);
                }
                if (!lifetime) return {};
                const auto claim=descendant(kind,*lifetime,*root,record,snapshot,facts,original,!reuseOnly);
                if (!claim) return {};
                const auto count=info.touchableRegion.isEmpty()?0:static_cast<size_t>(info.touchableRegion.end()-info.touchableRegion.begin());
                const auto pre=info.preViewportTouchableRegion && !info.preViewportTouchableRegion->isEmpty() ?
                    static_cast<size_t>(info.preViewportTouchableRegion->end()-info.preViewportTouchableRegion->begin()):0;
                if (count>1024 || pre>1024 || count+pre>32768-rects) return {};
                rects+=count+pre;
                result.current.rows.push_back({*claim,*root,snapshot.row,facts,info.touchableRegion,
                        info.preViewportTouchableRegion.value_or(Region{}),info.preViewportTouchableRegion.has_value(),
                        record.observed->geometry});
            }
        }
        const auto keys=currentClaimKeys(result.current);
        if (reuseOnly && (!mParked || keys!=mParked->claims)) return {};
        if (!reuseOnly && parkedRoots.size()==roots.records.size()) {
            std::sort(parkedRoots.begin(),parkedRoots.end(),[](const auto& a,const auto& b){return a.root<b.root;});
            result.parked=ParkedPublication{result.current.ticket,source.mRevision,mRevision,sequence,
                source.mCurrent->observedAt,result.current.expiresAt,keys,std::move(parkedRoots)};
            // Same full callback names immutable actual ingress and state.
            if (mParked && sameTicket(mParked->ticket,result.current.ticket) &&
                    (sequence<mParked->batch || result.parked->observedAt<mParked->observedAt ||
                     (sequence==mParked->batch && (result.parked->observedAt!=mParked->observedAt ||
                        keys!=mParked->claims || !sameOwnedBlocks(result.parked->roots,mParked->roots))))) return {};
        }
        // Rows/lifetimes/claim keys are OWNED history already. Failure here or
        // at the later fresh authority postcheck cannot roll them back/reuse.
        const auto registry=views();
        if (external_desktop_stage_validate_claim_registry(mRevision,{registry.data(),registry.size()})!=1) {
            mIssuanceStopped=true;return {};
        }
        result.sourceRevision=source.mRevision;result.registryRevision=mRevision;
        result.current.revision=mRevision;return result;
    }
    bool publish(Prepared&& prepared,const ExternalDesktopSourceLifetimes& source,int64_t now) {
        // Consume even a refused/moved-from input. A stale token cannot consume
        // an unrelated newer reservation or overwrite an already-published rowset.
        auto owner=std::move(prepared.owner);
        const auto preparation=std::exchange(prepared.preparation,0);
        const bool reserved=owner==mOwner && preparation!=0 && preparation==mPreparation &&
                mPreparingTicket.has_value() && !mPreparationExhausted;
        if (reserved) {
            mPreparingTicket.reset();
            if (mPreparation==std::numeric_limits<uint64_t>::max()) mPreparationExhausted=true;
            else ++mPreparation;
        }
        if (!reserved || mPreparationExhausted || mIssuanceStopped || prepared.registryRevision!=mRevision ||
                prepared.sourceRevision!=source.mRevision || !source.mCurrent ||
                now<source.mCurrent->observedAt || now<0 || now>=prepared.current.expiresAt ||
                prepared.current.batch!=source.mCurrent->batchSequence) return false;
        // Allocations were prepared BEFORE the fresh authority postcheck.
        // A failed publication cannot promote a burned/observed PARKED keyset.
        if (prepared.parked) mParked=std::move(prepared.parked);
        mCurrent=std::move(prepared.current);return true;
    }
    void invalidate() {
        mCurrent.reset();mPreparingTicket.reset();
        if (mPreparation==std::numeric_limits<uint64_t>::max()) mPreparationExhausted=true;
        else ++mPreparation;
    } // NO history removal, release proof or tail inference; sequence is staleness ONLY
    void invalidateScope() {invalidate();mParked.reset();}
    void invalidate(ExternalDesktopStageEpochTicket ticket) {
        const auto matches=[&](ExternalDesktopStageEpochTicket value) {
            return value.owner==ticket.owner && value.epoch==ticket.epoch && value.generation==ticket.generation;
        };
        if ((mCurrent && matches(mCurrent->ticket)) || (mPreparingTicket && matches(*mPreparingTicket))) invalidate();
        if (mParked && matches(mParked->ticket)) mParked.reset();
    }
    uint64_t mHigh=0,mRevision=0;
    uint64_t mPreparation=0;bool mPreparationExhausted=false;
    bool mIssuanceStopped=false; // No smaller/shrunk later row may first expose an older reserved lifetime.
    std::optional<ExternalDesktopStageEpochTicket> mPreparingTicket;
    size_t mHistoryBytes=0;
    const std::shared_ptr<const PreparationOwner> mOwner=std::make_shared<const PreparationOwner>();
    std::vector<Row> mRows; // NO release API without actual destruction+producer+ALL-tail proof
    std::optional<Current> mCurrent;
    std::optional<ParkedPublication> mParked; // one bounded successful publication, NOT authority/activation
};
} // namespace android::inputdispatcher
#endif
