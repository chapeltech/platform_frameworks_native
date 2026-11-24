#pragma once
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopStageRoster.h"
#include "ExternalDesktopTaskRootObservation.h"
#include <gui/WindowInfosUpdate.h>
#include <external_desktop_input_stage_authority.h>
#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace android::inputdispatcher {

// Real-object storage ONLY, not a second StageMachine or lease issuer. These
// keys may eventually populate existing Rust WsChild/WindowIdentity values.
// No fake Live/RootClaim, mirror ownership, geometry or route is inferred here.
// All methods are serialized by the actual dispatcher's mLock. No JNI callback.
class ExternalDesktopSourceLifetimes final {
    struct PreparationOwner {};
public:
    ExternalDesktopSourceLifetimes() = default;
    ExternalDesktopSourceLifetimes(const ExternalDesktopSourceLifetimes&) = delete;
    ExternalDesktopSourceLifetimes& operator=(const ExternalDesktopSourceLifetimes&) = delete;
    ExternalDesktopSourceLifetimes(ExternalDesktopSourceLifetimes&&) = delete;
    ExternalDesktopSourceLifetimes& operator=(ExternalDesktopSourceLifetimes&&) = delete;
    static constexpr size_t kMaxChildren = 8192;
    static constexpr size_t kMaxTasks = 8192; // CURRENT + ALL retained history.
    static constexpr size_t kMaxBinders = 2 * kMaxChildren;
    static constexpr size_t kMaxWindows = 8192;
    static constexpr size_t kMaxCloneIdentityBytes = 16*1024*1024; // counted values, NOT allocator/RSS quota
    enum class Join { NO_ROSTER, INCOMPLETE, REJECTED, ORIGINALS_OBSERVED, EXHAUSTED };
    struct Task {
        uint64_t key;
        int32_t uid, layer;
        ExternalDesktopStageObject identity;
        wp<IBinder> surface; // handle control-block identity only; no SC/resource owner
    };
    struct Child {
        uint64_t key, channelKey, clientKey;
        ExternalDesktopStageObject windowIdentity;
        int32_t layerId;
        wp<IBinder> surface, channel, client; // never promote historical resources
    };
    struct Binder { uint64_t key; wp<IBinder> value; };
    struct Window {
        uint64_t key, child;
        int32_t id;
    };
    struct TaskToken { int64_t generation, token; ExternalDesktopStageObject identity; };
    struct TaskBinding {
        size_t physical;
        int64_t token, incarnation;
        int32_t logicalDisplay;
        sp<SurfaceControl> surface; // CURRENT only; displaced roster owns prior refs until unlock
    };
    struct ChildBinding {
        size_t physical, task;
        WsChild rustValue; // Current association ONLY, never a fabricated Live.
        ExternalDesktopStageChildObservation observed; // exact CURRENT owned resources, not history
    };
    struct WindowBinding { size_t physical, child; uint64_t presentation; };
    struct StructuralWindow {
        uint64_t key;
        int32_t windowId;
        sp<IBinder> source; // NONRESOURCE opaque SF token, never an SF handle
        wp<IBinder> existingHandle;
        std::vector<sp<IBinder>> orderedRoots; // NONRESOURCE tokens only
        sp<IBinder> snapshot; // actual owned snapshot identity, independent of source layer
        uint64_t claimLifetime = 0; // allocated lazily at first Rust exposure, SAME native high-water
    };
    struct StructuralBinding { size_t physical, row; }; // geometry in SAME full batch
    struct CloneWindow {
        uint64_t key, child;
        ExternalDesktopRootJoinReceipt::CloneSnapshot snapshot; // nonresource identities/path ONLY; row is not identity
        wp<IBinder> channel, client;
    };
    struct RetiredClones {
        uint64_t revision=0;
        std::vector<CloneWindow> clones;
        std::vector<StructuralWindow> structurals;
        std::vector<StructuralBinding> current;
        size_t bytes=0;
    };
    struct Frame {
        ExternalDesktopStageEpochTicket ticket;
        int64_t expiresAt;
        std::vector<TaskBinding> tasks;
        std::vector<ChildBinding> children;
        std::vector<WindowBinding> windows;
        uint64_t batchSequence = 0;
        int64_t observedAt = -1; // actual native ingress, NOT a refresh/borrow time
        Join join = Join::INCOMPLETE;
        std::vector<StructuralBinding> structural{};
    };

private:
    // Only the existing native owner prepares this alongside exact physical
    // destruction/publication/transport proofs. No absence/TTL/weak expiry
    // permits deletion. Original phone/source children and allocator maxima
    // are untouched: these are the separately proved replica identities only.
    std::optional<RetiredClones> prepareRetiredClones(WsReleases releases) const {
        if(mRevision==UINT64_MAX)return {};
        const auto removed=[&](uint32_t kind,uint64_t lifetime){
            for(size_t i=0;i<releases.len;++i){const auto& claim=releases.data[i].claim;
                if(claim.kind==kind && claim.lifetime==lifetime)return true;}
            return false;
        };
        RetiredClones out;out.revision=mRevision;
        out.clones.reserve(mCloneWindows.size());out.structurals.reserve(mStructural.size());
        for(const auto& old:mCloneWindows)if(!removed(WS_WINDOW_CLAIM,old.key)){
            const auto dynamic=snapshotOwnedBytes(old.snapshot);if(!dynamic)return {};
            out.bytes+=sizeof(CloneWindow)+*dynamic;
            if(out.bytes>kMaxCloneIdentityBytes)return {};
            out.clones.push_back(old);
        }
        std::vector<size_t> indices(mStructural.size(),SIZE_MAX);
        for(size_t i=0;i<mStructural.size();++i){const auto& old=mStructural[i];
            if(old.claimLifetime && removed(WS_STRUCTURAL_CLAIM,old.claimLifetime))continue;
            indices[i]=out.structurals.size();out.structurals.push_back(old);
        }
        if(mCurrent)for(auto binding:mCurrent->structural){
            if(binding.physical>=indices.size() || indices[binding.physical]==SIZE_MAX)return {};
            binding.physical=indices[binding.physical];out.current.push_back(binding);
        }
        return out;
    }
    void commitRetiredClones(RetiredClones&& prepared) noexcept {
        // Caller holds the SAME dispatcher lock throughout prepare, real core
        // acceptance and these allocation-free swaps. No source mutation can
        // interleave; all potentially refusing work preceded core acceptance.
        mCloneWindows.swap(prepared.clones);mStructural.swap(prepared.structurals);
        if(mCurrent)mCurrent->structural.swap(prepared.current);
        mCloneIdentityBytes=prepared.bytes;++mRevision;
    }
public:

    class PreparedStructural {
    public:
        PreparedStructural(PreparedStructural&&) = default;
        PreparedStructural(const PreparedStructural&) = delete;
    private:
        friend class ExternalDesktopSourceLifetimes;
        PreparedStructural() = default;
        uint64_t revision = 0, high = 0;
        std::shared_ptr<const PreparationOwner> owner;
        std::vector<StructuralWindow> history;
        std::vector<StructuralBinding> current;
    };

    // Native object observations only. Reuses the SAME allocator/history as
    // original windows; no second RootClaim/lease/epoch policy is introduced.
    std::optional<PreparedStructural> prepareStructural(
            std::vector<ExternalDesktopRootJoinReceipt>& joins, bool allowNew = true) const {
        if (!mCurrent || mRevision==std::numeric_limits<uint64_t>::max()) return {};
        PreparedStructural p; p.revision=mRevision; p.high=mHighKey;
        p.owner=mPreparationOwner; p.history=mStructural;
        // Waiting for a complete counterpart clears only the current binding,
        // never deletes any historical identity or emits a partial claim set.
        for (const auto& join:joins) if (join.state!=ExternalDesktopRootJoin::CLONES_OBSERVED || !join.coherentSourceFacts ||
                join.batch!=mCurrent->batchSequence) return p;
        std::unordered_set<size_t> rows;
        for (auto& join:joins) for (auto& source:join.structuralSources) {
            if (!source.lifetime || !source.snapshotIdentity || !rows.insert(source.row).second ||
                source.orderedRootLifetimes.empty()) return {};
            for (const auto& window:mWindows) if (window.id==source.windowId) return {}; // no cross-kind alias
            for (const auto& window:mCloneWindows) if (window.snapshot.windowId==source.windowId) return {};
            size_t index=p.history.size();
            for (size_t i=0;i<p.history.size();++i) {
                const auto& old=p.history[i];
                const bool exact=old.source==source.lifetime && old.snapshot==source.snapshotIdentity &&
                        old.orderedRoots==source.orderedRootLifetimes;
                if (old.windowId==source.windowId && !exact) return {}; // recycled ID, no release proof
                if (exact && old.windowId==source.windowId) index=i;
            }
            if (index==p.history.size()) {
                if (!allowNew) return {}; // Enabled refresh never discovers a new lifetime.
                if (mWindows.size()+p.history.size()+mCloneWindows.size()>=kMaxWindows) return {};
                const auto key=next(p.high); if (!key) return {};
                p.history.push_back({*key,source.windowId,source.lifetime,source.existingHandle,
                        source.orderedRootLifetimes,source.snapshotIdentity});
            }
            source.nativeLifetimeKey=p.history[index].key;
            p.current.push_back({index,source.row});
        }
        return p;
    }
    bool commitStructural(PreparedStructural&& prepared) {
        auto owner=std::move(prepared.owner);
        if (!mCurrent || owner!=mPreparationOwner || prepared.revision!=mRevision ||
            mRevision==std::numeric_limits<uint64_t>::max()) return false;
        mStructural.swap(prepared.history); mCurrent->structural.swap(prepared.current);
        mHighKey=prepared.high; ++mRevision; return true;
    }
    struct Prepared {
    public:
        ~Prepared() = default;
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;
        Prepared& operator=(Prepared&&) = delete;
        Prepared(Prepared&& other) noexcept
              : revision(other.revision), highKey(other.highKey), domain(other.domain),
                nativeOwner(std::move(other.nativeOwner)), tasks(std::move(other.tasks)),
                children(std::move(other.children)), binders(std::move(other.binders)),
                tokens(std::move(other.tokens)), frame(std::move(other.frame)),
                owner(std::move(other.owner)) {}
    private:
        friend class ExternalDesktopSourceLifetimes;
        Prepared() = default;
        uint64_t revision, highKey, domain;
        ExternalDesktopStageObject nativeOwner;
        std::vector<Task> tasks;
        std::vector<Child> children;
        std::vector<Binder> binders;
        std::vector<TaskToken> tokens;
        Frame frame;
        std::shared_ptr<const PreparationOwner> owner;
    };

    // Complete candidate, including all hidden/non-input child observations.
    // No history or ID is published until commit AFTER the real borrow's
    // postcheck. A rejected local candidate has issued no externally used key.
    std::optional<Prepared> prepare(const ExternalDesktopStageLeaseView& view,
            ExternalDesktopStageEpochTicket ticket, const ExternalDesktopStageRoster& roster) const {
        if (!roster.nativeOwnerIdentity || !roster.references || ticket.owner == 0 || view.observed_at < 0 ||
                ticket.epoch <= 0 || ticket.generation != view.generation || view.generation <= 0 ||
                (mDomain != 0 && (mDomain != ticket.owner || mNativeOwner != roster.nativeOwnerIdentity)) ||
                mRevision == std::numeric_limits<uint64_t>::max() ||
                roster.tasks.size() > 128 || roster.windows.size() > kMaxChildren ||
                !view.topology_snapshot.data || !view.expires_by_display.data ||
                view.topology_snapshot.len < 14 || view.topology_snapshot.len > 86) return {};
        const int64_t count = view.topology_snapshot.data[3];
        if (count < 1 || count > 9 || view.topology_snapshot.len != 5 + static_cast<size_t>(count) * 9 ||
                view.expires_by_display.len != static_cast<size_t>(count)) return {};
        Prepared result;
        result.owner = mPreparationOwner; result.revision = mRevision; result.highKey = mHighKey;
        result.domain = ticket.owner; result.nativeOwner = roster.nativeOwnerIdentity;
        result.tasks = mTasks; result.children = mChildren; result.binders = mBinders; result.tokens = mTaskTokens;
        result.frame = Frame{ticket, std::numeric_limits<int64_t>::max(), {}, {}, {}};
        for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
            if (view.expires_by_display.data[i] <= view.observed_at) return {};
            result.frame.expiresAt = std::min(result.frame.expiresAt, view.expires_by_display.data[i]);
        }
        std::unordered_set<const ExternalDesktopStageObjectIdentity*> taskObjects, windowObjects;
        std::unordered_set<int64_t> tokens;
        std::unordered_set<int32_t> layers;
        // wp equality/order includes the RefBase control block, not merely the
        // recyclable raw object address. No historical weak promotion occurs.
        std::map<wp<IBinder>, size_t> binderIndex;
        for (size_t i = 0; i < result.binders.size(); ++i) binderIndex.emplace(result.binders[i].value, i);
        std::unordered_map<const ExternalDesktopStageObjectIdentity*, std::vector<size_t>> childrenByObject;
        for (size_t i = 0; i < result.children.size(); ++i)
            childrenByObject[result.children[i].windowIdentity.get()].push_back(i);
        const auto binderKey = [&](const sp<IBinder>& value) -> std::optional<uint64_t> {
            if (!value) return uint64_t{0};
            const wp<IBinder> identity(value);
            const auto found = binderIndex.find(identity);
            if (found != binderIndex.end()) return result.binders[found->second].key;
            if (result.binders.size() == kMaxBinders) return {};
            const auto key = next(result.highKey); if (!key) return {};
            binderIndex.emplace(identity, result.binders.size());
            result.binders.push_back({*key, identity}); return key;
        };
        size_t cursor = 0;
        for (const auto& task : roster.tasks) {
            if (!task.taskIdentity || task.taskToken <= 0 || task.uid < 0 || task.uid / 100000 != view.user ||
                    !taskObjects.insert(task.taskIdentity.get()).second || !tokens.insert(task.taskToken).second ||
                    !task.surface || !task.surface->isValid() || task.layerId <= 0 ||
                    task.surface->getLayerId() != task.layerId || !layers.insert(task.layerId).second ||
                    task.start != cursor || task.end < task.start || task.end - task.start > 256 ||
                    task.end > roster.windows.size()) return {};
            cursor = task.end;
            int32_t logical = -1;
            for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
                const auto* display = view.topology_snapshot.data + 5 + i * 9;
                if (display[0] != task.displayToken) continue;
                if (display[1] < 0 || display[1] > std::numeric_limits<int32_t>::max() ||
                        view.expires_by_display.data[i] <= view.observed_at) return {};
                logical = static_cast<int32_t>(display[1]);
                result.frame.expiresAt = std::min(result.frame.expiresAt, view.expires_by_display.data[i]);
            }
            if (logical < 0) return {};
            bool knownToken = false;
            for (const auto& saved : result.tokens) {
                if (saved.generation != view.generation) continue;
                if ((saved.token == task.taskToken && saved.identity != task.taskIdentity) ||
                        (saved.identity == task.taskIdentity && saved.token != task.taskToken)) return {};
                knownToken |= saved.token == task.taskToken;
            }
            if (!knownToken) {
                if (result.tokens.size() == kMaxTasks) return {};
                result.tokens.push_back({view.generation, task.taskToken, task.taskIdentity});
            }
            size_t taskIndex = result.tasks.size();
            for (size_t i = 0; i < result.tasks.size(); ++i) {
                const auto& saved = result.tasks[i];
                if (saved.identity == task.taskIdentity && sameHistoricalSurface(saved.surface, task.surface) && saved.layer == task.layerId) {
                    if (saved.uid != task.uid) return {};
                    taskIndex = i;
                }
            }
            if (taskIndex == result.tasks.size()) {
                if (result.tasks.size() == kMaxTasks) return {};
                const auto key = next(result.highKey); if (!key) return {};
                result.tasks.push_back({*key, task.uid, task.layerId, task.taskIdentity, wp<IBinder>(task.surface->getHandle())});
            }
            const size_t taskBinding = result.frame.tasks.size();
            result.frame.tasks.push_back({taskIndex, task.taskToken, task.displayToken, logical, task.surface});
            for (size_t w = task.start; w < task.end; ++w) {
                const auto& child = roster.windows[w];
                const bool applicationType = child.type >= 1 && child.type <= 99 && child.type != 3;
                const bool attachedType = child.type >= 1000 && child.type <= 1999;
                if (!child.windowIdentity || child.taskIdentity != task.taskIdentity || !child.client ||
                        !(applicationType || attachedType) || child.baseType < 1 || child.baseType > 99 || child.baseType == 3 ||
                        !windowObjects.insert(child.windowIdentity.get()).second || (child.state & ~63u) != 0 ||
                        (child.surface ? !child.surface->isValid() || child.layerId <= 0 ||
                            child.surface->getLayerId() != child.layerId || !layers.insert(child.layerId).second :
                            child.layerId != 0)) return {};
                const auto channel = binderKey(child.channel), client = binderKey(child.client);
                if (!channel || !client) return {};
                size_t childIndex = result.children.size();
                for (size_t i : childrenByObject[child.windowIdentity.get()]) {
                    const auto& saved = result.children[i];
                    // Mutable flags/visibility are current observations, NOT a
                    // reason to recreate the physical object lifetime.
                    if (sameHistoricalSurface(saved.surface, child.surface) &&
                            saved.layerId == child.layerId && saved.channel == child.channel &&
                            saved.client == child.client) childIndex = i;
                }
                if (childIndex == result.children.size()) {
                    if (result.children.size() >= kMaxChildren || result.children.size() >= mChildLimit) return {};
                    const auto key = next(result.highKey); if (!key) return {};
                    result.children.push_back({*key, *channel, *client, child.windowIdentity, child.layerId,
                            wp<IBinder>(child.surface ? child.surface->getHandle() : nullptr),
                            wp<IBinder>(child.channel), wp<IBinder>(child.client)});
                    childrenByObject[child.windowIdentity.get()].push_back(childIndex);
                }
                const auto& physical = result.children[childIndex];
                result.frame.children.push_back({childIndex, taskBinding,
                    WsChild{task.taskToken, physical.key, static_cast<uint32_t>(child.layerId),
                            physical.channelKey, physical.clientKey, WS_CHILD_RECEIVER}, child}); // Never deduplicate by channel/client.
            }
        }
        if (cursor != roster.windows.size()) return {};
        return result;
    }

    bool commit(Prepared&& candidate) {
        auto owner = std::move(candidate.owner); // Burn EVERY attempted commit.
        if (owner != mPreparationOwner || candidate.revision != mRevision || mRevision == std::numeric_limits<uint64_t>::max()) return false;
        mTasks = std::move(candidate.tasks); mChildren = std::move(candidate.children);
        mBinders = std::move(candidate.binders); mHighKey = candidate.highKey;
        mTaskTokens = std::move(candidate.tokens);
        mDomain = candidate.domain; mNativeOwner = std::move(candidate.nativeOwner);
        mCurrent = std::move(candidate.frame); ++mRevision;
        return true;
    }

    // Explicit deterministic fixture adapter. Production has to supply BOTH
    // retained actual ingress and a separate fresh current clock below.
    void observeForTest(const gui::WindowInfosUpdate& batch, uint64_t sequence, int64_t now) {
        observe(batch, sequence, now, now);
    }

    void observe(const gui::WindowInfosUpdate& batch, uint64_t sequence,
                 int64_t observedAt, int64_t now) {
        if (!mCurrent) return;
        mCurrent->join = Join::INCOMPLETE; mCurrent->windows.clear(); mCurrent->structural.clear();
        mCurrent->batchSequence = 0; mCurrent->observedAt = -1;
        // Even a rejected observation invalidates any older prepared view.
        if (mRevision == std::numeric_limits<uint64_t>::max()) { mCurrent->join = Join::EXHAUSTED; return; }
        ++mRevision;
        if (sequence == 0 || sequence < mHighestSeenSequence) return;
        if (sequence > mHighestSeenSequence) {
            // Burn EVERY newer callback before checking its clock. A malformed
            // latest clock cannot make an older exact cached callback current.
            mHighestSeenSequence = sequence; mHighestSeenObservedAt = observedAt;
            mHighestSeenClockValid = observedAt >= 0 && observedAt >= mLastValidObservedAt &&
                    now >= observedAt && now >= mLastCheckedNow;
        }
        // Same sequence names ONE immutable actual ingress. A clock-invalid
        // callback cannot be repaired by retiming it. A valid cached callback
        // may rejoin a fresh roster, but only with its original arrival.
        if (!mHighestSeenClockValid || observedAt != mHighestSeenObservedAt) return;
        if (now < observedAt || now < mLastCheckedNow) {
            mHighestSeenClockValid = false; return;
        }
        mLastValidObservedAt = observedAt; mLastCheckedNow = now;
        if (now >= mCurrent->expiresAt ||
                batch.windowInfos.size() > kMaxChildren) return;
        std::unordered_map<int32_t, size_t> byLayer;
        for (size_t i = 0; i < mCurrent->children.size(); ++i) {
            const auto& child = mCurrent->children[i].observed;
            // Inspect ALL actual current surfaces, not a connection-filtered
            // inventory: an emitted real-client/no-channel row is unsupported,
            // never evidence that this child has no exported row.
            if (child.surface) byLayer.emplace(child.layerId, i);
        }
        std::unordered_set<size_t> seen;
        auto candidate = mWindows;
        std::unordered_map<int32_t, std::vector<size_t>> windowsById;
        for (size_t i = 0; i < candidate.size(); ++i) windowsById[candidate[i].id].push_back(i);
        std::vector<WindowBinding> joined;
        uint64_t high = mHighKey;
        std::unordered_set<uint32_t> exportedSources;
        for (const auto& info : batch.windowInfos) {
            exportedSources.insert(info.cloneLineage ? info.cloneLineage->sourceLayerId :
                    static_cast<uint32_t>(info.id));
            // Clone IDs/path are SF observations, NOT owned presentation roots.
            // Until real WMS root transport exists, NO clone identity is issued.
            if (info.cloneLineage) continue;
            const auto found = byLayer.find(info.id);
            if (found == byLayer.end()) continue; // Ordinary phone/system rows untouched.
            const auto& childBinding = mCurrent->children[found->second];
            const auto& child = mChildren[childBinding.physical];
            const auto& observed = childBinding.observed;
            const auto& taskBinding = mCurrent->tasks[childBinding.task];
            const auto& task = mTasks[taskBinding.physical];
            for (const auto& saved:mCloneWindows) if (saved.snapshot.windowId==info.id) {
                mCurrent->join=Join::REJECTED; return;
            }
            for (const auto& saved:mStructural) if (saved.windowId==info.id) {
                mCurrent->join=Join::REJECTED; return; // unified Window/Structural identity domain
            }
            if (!observed.channel || !seen.insert(found->second).second || info.stageTag ||
                    info.inputConfig.test(gui::WindowInfo::InputConfig::TRUSTED_OVERLAY) ||
                    info.token != observed.channel || info.windowToken != observed.client ||
                    info.ownerUid != gui::Uid{static_cast<uint32_t>(task.uid)} || info.displayId.val() != taskBinding.logicalDisplay) {
                mCurrent->join = Join::REJECTED; return;
            }
            size_t index = candidate.size();
            for (size_t i : windowsById[info.id]) {
                const auto& saved = candidate[i];
                if (saved.id != info.id) continue;
                if (saved.child != child.key) {
                    mCurrent->join = Join::REJECTED; return; // No unproven recycled-ID rebinding.
                }
                index = i;
            }
            if (index == candidate.size()) {
                if (candidate.size()+mStructural.size()+mCloneWindows.size() >= kMaxWindows) { mCurrent->join = Join::EXHAUSTED; return; }
                const auto key = next(high); if (!key) { mCurrent->join = Join::EXHAUSTED; return; }
                candidate.push_back({*key, child.key, info.id});
                windowsById[info.id].push_back(index);
            }
            joined.push_back({index, found->second, task.key});
        }
        const bool hasAbsentChild = seen.size() != mCurrent->children.size();
        if (hasAbsentChild && (!batch.layerSourcesComplete ||
                !gui::layerSourcesValid(batch.layerSources, batch.windowInfos, true))) return;
        for (size_t i = 0; i < mCurrent->children.size(); ++i) {
            const auto layer = mCurrent->children[i].observed.layerId;
            if (!seen.count(i) && layer > 0 && exportedSources.count(static_cast<uint32_t>(layer))) {
                mCurrent->join = Join::REJECTED; return;
            }
        }
        // Only this exact full raw SF batch can establish absence. Retain every
        // WMS child, channel/client identity and historical claim; no absence is
        // retirement. A later emitted counterpart changes current participation
        // and must invalidate/re-stage through the existing full Live equality.
        for (size_t i = 0; i < mCurrent->children.size(); ++i) {
            mCurrent->children[i].rustValue.participation = seen.count(i) ?
                    WS_CHILD_RECEIVER : WS_CHILD_NO_EXPORTED_ROW;
        }
        mWindows = std::move(candidate); mHighKey = high;
        mCurrent->windows = std::move(joined); mCurrent->batchSequence = sequence;
        mCurrent->observedAt = observedAt;
        mCurrent->join = Join::ORIGINALS_OBSERVED;
    }

    void invalidate(ExternalDesktopStageEpochTicket ticket) {
        if (mCurrent && mCurrent->ticket.owner == ticket.owner && mCurrent->ticket.epoch == ticket.epoch &&
                mCurrent->ticket.generation == ticket.generation) {
            mCurrent.reset();
            if (mRevision != std::numeric_limits<uint64_t>::max()) ++mRevision;
        }
        // No history deletion API: actual object retirement/submission/physical
        // queue+release-tail proof remains REQUIRED and unimplemented.
    }
    void rejectBatch() {
        if (mRevision != std::numeric_limits<uint64_t>::max()) ++mRevision;
        if (mCurrent) {
            mCurrent->join = Join::INCOMPLETE; mCurrent->windows.clear(); mCurrent->structural.clear();
            mCurrent->batchSequence = 0; mCurrent->observedAt = -1;
        }
        // Never erase historical identities because a full callback was absent
        // or malformed. No source window/presentation is physically retired.
    }
    const std::optional<Frame>& current() const { return mCurrent; }
    std::optional<std::vector<ExternalDesktopRootOriginal>> originalsForRoot(ExternalDesktopStageEpochTicket ticket,
            const ExternalDesktopStageObject& taskIdentity, const sp<SurfaceControl>& source,
            uint64_t sequence) const {
        if (!mCurrent || mCurrent->join!=Join::ORIGINALS_OBSERVED ||
                mCurrent->ticket.owner!=ticket.owner || mCurrent->ticket.epoch!=ticket.epoch ||
                mCurrent->ticket.generation!=ticket.generation || mCurrent->batchSequence!=sequence) return {};
        std::vector<ExternalDesktopRootOriginal> result;
        for (size_t t=0;t<mCurrent->tasks.size();++t) {
            const auto& task=mTasks[mCurrent->tasks[t].physical];
            if (task.identity!=taskIdentity || !sameSurface(mCurrent->tasks[t].surface,source)) continue;
            for (size_t c=0;c<mCurrent->children.size();++c) {
                const auto& binding=mCurrent->children[c];
                if (binding.task!=t) continue;
                const auto& child=mChildren[binding.physical];
                const auto& observed=binding.observed;
                if (binding.rustValue.participation == WS_CHILD_NO_EXPORTED_ROW) continue;
                if (!observed.surface || !observed.channel) return {};
                const auto found=std::find_if(mCurrent->windows.begin(),mCurrent->windows.end(),
                    [c](const auto& v) { return v.child==c; });
                if (found==mCurrent->windows.end()) return {};
                const auto& window=mWindows[found->physical];
                result.push_back({child.key,child.channelKey,child.clientKey,window.key,
                    child.layerId,window.id,task.uid,observed.channel,observed.client});
            }
            return result;
        }
        return {};
    }
    size_t retainedChildren() const { return mChildren.size(); }
    size_t retainedWindows() const { return mWindows.size(); }
    uint64_t highKey() const { return mHighKey; }

private:
    friend class InputDispatcherTest;
    friend class ExternalDesktopNativeClaims;
    friend class ExternalDesktopStageOwner; // same real owner: preserve issued original IDs in complete-batch projection
    // ONE native allocation high-water, shared by original windows, clone
    // receivers and structural windows. Externally exposed keys NEVER rollback.
    std::optional<uint64_t> reserveClaimLifetime() {
        if (!mCurrent || mRevision==std::numeric_limits<uint64_t>::max()) return {};
        const auto value=next(mHighKey);if (!value) return {};
        ++mRevision;return value;
    }
    static bool sameClone(const ExternalDesktopRootJoinReceipt::CloneSnapshot& a,
                          const ExternalDesktopRootJoinReceipt::CloneSnapshot& b) {
        return a.windowId==b.windowId && a.snapshot==b.snapshot && a.source==b.source &&
            a.orderedRootLifetimes==b.orderedRootLifetimes && a.variant==b.variant &&
            a.detached==b.detached && a.relativeRoots==b.relativeRoots;
        // Actual composed-parent traversal is checked in EVERY complete facts
        // join/current binding, not frozen as a geometry-independent lifetime.
    }
    static std::optional<size_t> snapshotOwnedBytes(const ExternalDesktopRootJoinReceipt::CloneSnapshot& value) {
        if (value.orderedRootLifetimes.size()>50 || value.relativeRoots.size()>64 || value.actualParents.size()>64) return {};
        size_t bytes=value.orderedRootLifetimes.size()*sizeof(sp<IBinder>)+
            value.relativeRoots.size()*sizeof(uint32_t)+value.actualParents.size()*sizeof(ExternalDesktopRootJoinReceipt::CloneSnapshot::Path);
        for (const auto& parent:value.actualParents) {
            if (parent.roots.size()>50 || parent.relatives.size()>64) return {};
            bytes+=(parent.roots.size()+parent.relatives.size())*sizeof(uint32_t);
        }
        return bytes; // all multiplicands bounded above before arithmetic
    }
    std::optional<uint64_t> reserveCloneLifetime(const ExternalDesktopRootJoinReceipt::CloneSnapshot& snapshot,
            const ExternalDesktopRootOriginal& original,const gui::WindowInfo& info) {
        if (!mCurrent || mCurrent->join!=Join::ORIGINALS_OBSERVED || !snapshot.snapshot ||
            !snapshot.source || snapshot.windowId==-1 || snapshot.windowId!=info.id ||
            snapshot.orderedRootLifetimes.empty() || info.token!=original.channel ||
            info.windowToken!=original.client || !info.token || !info.windowToken) return {};
        for (const auto& old:mWindows) if (old.id==info.id) return {};
        for (const auto& old:mStructural) if (old.windowId==info.id) return {};
        for (const auto& old:mCloneWindows) if (old.snapshot.windowId==info.id) {
            if (!sameClone(old.snapshot,snapshot) || old.child!=original.child ||
                    old.channel!=info.token || old.client!=info.windowToken) return {};
            return old.key;
        }
        if (mWindows.size()+mStructural.size()+mCloneWindows.size()>=kMaxWindows) return {};
        const auto dynamic=snapshotOwnedBytes(snapshot);
        if (!dynamic || sizeof(CloneWindow)+*dynamic>kMaxCloneIdentityBytes-mCloneIdentityBytes) return {};
        const auto key=reserveClaimLifetime();if (!key) return {};
        auto identity=snapshot;identity.row=0; // batch ordering is CURRENT geometry, not physical identity
        mCloneWindows.push_back({*key,original.child,std::move(identity),
                wp<IBinder>(info.token),wp<IBinder>(info.windowToken)});
        mCloneIdentityBytes+=sizeof(CloneWindow)+*dynamic;
        return key;
    }
    std::optional<uint64_t> structuralClaimLifetime(const ExternalDesktopRootJoinReceipt::CloneSnapshot& snapshot) {
        if (!mCurrent || mCurrent->join!=Join::ORIGINALS_OBSERVED) return {};
        for (const auto& binding:mCurrent->structural) if (binding.row==snapshot.row) {
            auto& old=mStructural[binding.physical];
            if (old.windowId==snapshot.windowId && old.snapshot==snapshot.snapshot &&
                old.source==snapshot.source && old.orderedRoots==snapshot.orderedRootLifetimes) {
                if (!old.claimLifetime) {
                    const auto value=reserveClaimLifetime();if (!value) return {};
                    old.claimLifetime=*value;
                }
                return old.claimLifetime;
            }
        }
        return {};
    }
    static bool sameHistoricalSurface(const wp<IBinder>& previous,
                                     const sp<SurfaceControl>& current) {
        // A live current roster owns this handle. Compare exact weak control
        // blocks without reviving old resources or trusting a numeric layer ID.
        return current ? previous == current->getHandle() : previous == nullptr;
    }
    static bool sameSurface(const sp<SurfaceControl>& a, const sp<SurfaceControl>& b) {
        // Native wrapper copies are not new physical SF layers. Retain actual
        // controls and compare their exact strong layer handles, never ID alone.
        return (!a && !b) || (a && b && a->getHandle() == b->getHandle());
    }
    static std::optional<uint64_t> next(uint64_t& value) {
        if (value == std::numeric_limits<uint64_t>::max()) return {};
        return ++value;
    }
    uint64_t mRevision = 0, mHighKey = 0, mDomain = 0;
    uint64_t mHighestSeenSequence = 0;
    int64_t mHighestSeenObservedAt = -1;
    bool mHighestSeenClockValid = false;
    int64_t mLastValidObservedAt = -1, mLastCheckedNow = -1;
    // Strong, private per-owner cookie also prevents address-reuse after this
    // owner dies while a stale preparation still exists. Not a lease issuer.
    const std::shared_ptr<const PreparationOwner> mPreparationOwner = std::make_shared<const PreparationOwner>();
    size_t mChildLimit = kMaxChildren;
    ExternalDesktopStageObject mNativeOwner;
    std::vector<Task> mTasks;
    std::vector<Child> mChildren;
    std::vector<Binder> mBinders;
    std::vector<TaskToken> mTaskTokens;
    std::vector<Window> mWindows;
    std::vector<StructuralWindow> mStructural; // bounded/non-evicting, NO strong handles
    std::vector<CloneWindow> mCloneWindows; // SAME nonrecycled Window/Structural domain
    size_t mCloneIdentityBytes=0;
    std::optional<Frame> mCurrent;
};
} // namespace android::inputdispatcher
#endif
