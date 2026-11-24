#pragma once
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopStageRoster.h"
#include <gui/WindowInfosUpdate.h>
#include <array>
#include <algorithm>
#include <optional>
#include <unordered_set>
#include <utility>
#include <external_desktop_input_stage_authority.h>

namespace android {
// Actual JNI-owned objects/controls, never a RootClaim/Live or numeric issuer.
// The packet witness owns the complete Java DTO root via the existing registry.
struct ExternalDesktopTaskRootObservation {
    std::shared_ptr<const ExternalDesktopStageRoster> roster;
    ExternalDesktopStageObject packet, producer, frame, submission, task;
    sp<SurfaceControl> source, root, destinationParent;
    std::array<int64_t, 4> endpoints; // source token/incarnation,destination token/incarnation
    std::array<int32_t, 8> geometry; // bounded Task-local crop and equal-sized destination
    int64_t serial = 0; // Read from the actual private LayerSubmission, not DTO input.
};
// Persistent physical identity has NO SurfaceControl, roster packet or strong
// Java object graph. ExternalDesktopStageObject's JNI referent is weak (native identity
// itself remains owned/nonrecycled), and all three Binder witnesses are WEAK.
struct ExternalDesktopRootPhysicalHistory {
    wp<IBinder> root, source;
    int32_t rootId = 0;
    ExternalDesktopStageObject producer, task;
    uint64_t owner = 0;
    int64_t generation = 0;
    int32_t user = 0, clientUid = 0, destination = 0;
    std::array<int64_t,4> endpoints{};
    // Replay metadata only: never part of physical/root ownership.
    ExternalDesktopStageObject firstFrame, firstSubmission;
    int64_t firstEpoch = 0, firstSerial = 0, firstExpires = 0;
};
struct ExternalDesktopTaskRootRecord {
    ExternalDesktopStageEpochTicket ticket{};
    std::shared_ptr<const ExternalDesktopTaskRootObservation> observed; // CURRENT binding only
    int32_t destination;
    int64_t expires, serial;
    int32_t user = 0, clientUid = 0;
    std::shared_ptr<const ExternalDesktopRootPhysicalHistory> physical{};
};
struct ExternalDesktopTaskRootCohort {
    // CURRENT owns bounded validation witnesses. HISTORY owns physical
    // nonresource/weak metadata only; no expiry/ACK/absence eviction.
    std::vector<ExternalDesktopTaskRootRecord> records;
};
inline std::optional<ExternalDesktopRootPhysicalHistory> externalDesktopRootPhysical(const ExternalDesktopTaskRootRecord& record) {
    const auto& root=record.observed;
    if (!root || !root->producer || !root->frame || !root->submission || !root->task ||
        !root->packet || !root->root || !root->source || !root->destinationParent ||
        !root->root->isValid() || root->root->getLayerId()<=0 || !root->source->isValid() ||
        !root->destinationParent->isValid() || record.ticket.owner==0 || record.ticket.epoch<=0 ||
        record.ticket.generation<=0 || record.destination<=0 || record.serial<=0 || record.expires<=0 ||
        record.user<0 || record.clientUid<0 || record.clientUid/100000!=record.user) return {};
    ExternalDesktopRootPhysicalHistory result;
    result.root=root->root->getHandle(); result.source=root->source->getHandle();
    result.rootId=root->root->getLayerId(); result.producer=root->producer; result.task=root->task;
    result.owner=record.ticket.owner; result.generation=record.ticket.generation;
    result.user=record.user; result.clientUid=record.clientUid; result.destination=record.destination;
    result.endpoints=root->endpoints;
    result.firstFrame=root->frame; result.firstSubmission=root->submission;
    result.firstEpoch=record.ticket.epoch; result.firstSerial=record.serial; result.firstExpires=record.expires;
    return result;
}
inline std::optional<ExternalDesktopTaskRootCohort> prepareExternalDesktopRootCohort(
        const ExternalDesktopTaskRootCohort* previous, const ExternalDesktopTaskRootRecord& incoming,
        bool historyOnly=false) {
    constexpr size_t kMaximum=128*8;
    const auto actual=externalDesktopRootPhysical(incoming);
    if (!actual || (previous && previous->records.size()>kMaximum)) return {};
    ExternalDesktopTaskRootCohort candidate=previous ? *previous:ExternalDesktopTaskRootCohort{};
    for (const auto& saved:candidate.records) {
        const auto local=saved.physical ? std::optional{*saved.physical}:externalDesktopRootPhysical(saved);
        if (!local) return {};
        const auto& old=*local;
        const bool physical=old.root==actual->root, producer=old.producer==actual->producer;
        if (!physical && old.rootId==actual->rootId) return {}; // no numeric-ID recycling proof
        if (!physical && !producer) continue;
        if (!producer || old.task!=actual->task || old.owner!=actual->owner ||
            old.generation!=actual->generation || old.user!=actual->user || old.clientUid!=actual->clientUid ||
            old.destination!=actual->destination || old.endpoints!=actual->endpoints) return {};
        if (physical) {
            if (old.source!=actual->source) return {}; // mirror source is physical, not geometry
            return candidate; // new frame/submission/geometry is a NEW current binding, same physical row
        }
        // Same logical producer + Task/scope may replace its physical source
        // root. Old physical row remains until real destruction/tail release.
    }
    if (candidate.records.size()==kMaximum) return {};
    auto record=incoming;
    if (historyOnly) {
        record.physical=std::make_shared<const ExternalDesktopRootPhysicalHistory>(*actual);
        record.observed.reset(); // NO roster/SC/frame/producer strong Java graph in history
    }
    candidate.records.push_back(std::move(record));
    return candidate;
}
struct ExternalDesktopTaskRootBatch {
    ExternalDesktopTaskRootCohort current, history;
};
inline std::optional<ExternalDesktopTaskRootBatch> prepareExternalDesktopRootBatch(
        const ExternalDesktopTaskRootCohort* history,const std::vector<ExternalDesktopTaskRootRecord>& expected) {
    if (expected.size()>128*8) return {};
    ExternalDesktopTaskRootBatch candidate{{},history ? *history:ExternalDesktopTaskRootCohort{}};
    if (expected.empty()) return candidate; // release CURRENT witnesses only, never retire history/claims
    const auto& first=expected.front();
    if (!first.observed || !first.observed->frame) return {};
    for (const auto& record:expected) {
        if (!record.observed || record.observed->frame!=first.observed->frame ||
                record.ticket.owner!=first.ticket.owner || record.ticket.epoch!=first.ticket.epoch ||
                record.ticket.generation!=first.ticket.generation ||
                record.user!=first.user || record.clientUid!=first.clientUid) return {};
        auto next=prepareExternalDesktopRootCohort(&candidate.history,record,true);
        if (!next) return {};
        auto current=prepareExternalDesktopRootCohort(&candidate.current,record);
        if (!current || current->records.size()!=candidate.current.records.size()+1) return {};
        // Only replay of the EXACT captured binding is clamped. Geometry does
        // not renew authority: a fresh binding still needs the real lease postcheck.
        for (const auto& saved:next->records) if (saved.physical &&
                saved.physical->root==wp<IBinder>(record.observed->root->getHandle()) &&
                saved.physical->firstFrame==record.observed->frame &&
                saved.physical->firstSubmission==record.observed->submission &&
                saved.physical->firstEpoch==record.ticket.epoch && saved.physical->firstSerial==record.serial) {
            current->records.back().expires=saved.physical->firstExpires; break;
        }
        candidate.current=std::move(*current); candidate.history=std::move(*next);
    }
    return candidate; // actual existing authority postcheck precedes publication
}
struct ExternalDesktopRootOriginal {
    uint64_t child, channelKey, clientKey, window;
    int32_t sourceLayer, windowId, uid;
    sp<IBinder> channel, client;
};
enum class ExternalDesktopRootJoin { NO_OBSERVATION, NO_ORIGINALS, INCOMPLETE, REJECTED,
                          UNRESOLVED_ANCESTOR, CLONES_OBSERVED };
struct ExternalDesktopRootJoinReceipt {
    ExternalDesktopRootJoin state = ExternalDesktopRootJoin::NO_OBSERVATION;
    uint64_t batch = 0;
    size_t inspected = 0, matched = 0;
    bool coherentSourceFacts = false; // diagnostic lineage fallback can NEVER mint claims
    // Existing original child/window lifetime keys only; NO root key issuance.
    std::vector<std::pair<uint64_t, uint64_t>> originalLifetimes;
    std::vector<size_t> cloneRows; // Exact row indices in immutable complete batch `batch`.
    struct StructuralSource {
        size_t row;
        int32_t windowId;
        sp<IBinder> lifetime; // NONRESOURCE SF identity, never a Layer/SC/FD.
        wp<IBinder> existingHandle;
        std::vector<sp<IBinder>> orderedRootLifetimes;
        uint64_t nativeLifetimeKey = 0; // existing native allocator, NOT a claim
        sp<IBinder> snapshotIdentity{}; // actual snapshot recreation, NOT layer token/ID
    };
    std::vector<StructuralSource> structuralSources;
    struct CloneSnapshot {
        size_t row;
        int32_t windowId;
        sp<IBinder> snapshot, source;
        std::vector<sp<IBinder>> orderedRootLifetimes;
        uint32_t variant;
        bool detached;
        std::vector<uint32_t> relativeRoots;
        // Actual composed parent/root paths, CURRENT binding evidence only.
        // Geometry stays in the complete batch, not the physical lifetime.
        struct Path { uint32_t source,variant; bool detached,relativeLoop;
            std::vector<uint32_t> roots,relatives;
            bool operator==(const Path&) const = default; };
        std::vector<Path> actualParents;
    };
    std::vector<CloneSnapshot> cloneSnapshots;
};

// Inspect the COMPLETE immutable SF batch, including ordinary/system rows.
// This is a one-presentation observation precursor, NOT whole-frame acceptance.
inline ExternalDesktopRootJoinReceipt joinExternalDesktopTaskRoot(const gui::WindowInfosUpdate& batch,
        uint64_t sequence, uint32_t root, int32_t destination,
        const std::vector<ExternalDesktopRootOriginal>& originals) {
    ExternalDesktopRootJoinReceipt out; out.batch=sequence; out.state=ExternalDesktopRootJoin::INCOMPLETE;
    if (!sequence || !root || originals.empty() || batch.windowInfos.size()>8192) return out;
    for (const auto& original:originals) if (original.uid<0) {
        out.state=ExternalDesktopRootJoin::REJECTED; return out;
    }
    std::unordered_set<size_t> seen;
    for (const auto& info:batch.windowInfos) {
        const size_t row=out.inspected++;
        if (!info.cloneLineage) continue;
        const auto& lineage=*info.cloneLineage;
        if (std::find(lineage.mirrorRootIds.begin(),lineage.mirrorRootIds.end(),root)==lineage.mirrorRootIds.end()) continue;
        if (lineage.mirrorRootIds.size()!=1 || lineage.mirrorRootIds[0]!=root ||
                info.stageTag || info.displayId.val()!=destination ||
                !info.inputConfig.test(gui::WindowInfo::InputConfig::CLONE) ||
                !info.inputConfig.test(gui::WindowInfo::InputConfig::DROP_INPUT) ||
                info.inputConfig.test(gui::WindowInfo::InputConfig::TRUSTED_OVERLAY)) {
            out.state=ExternalDesktopRootJoin::REJECTED; return out;
        }
        size_t found=originals.size();
        for (size_t i=0;i<originals.size();++i) {
            if (originals[i].sourceLayer==static_cast<int32_t>(lineage.sourceLayerId)) {
                if (found!=originals.size()) { out.state=ExternalDesktopRootJoin::REJECTED;return out; }
                found=i;
            }
        }
        // Task/Activity ancestor layers are not silently asserted to be app
        // children. Their complete native hierarchy proof remains OPEN.
        if (found==originals.size()) { out.state=ExternalDesktopRootJoin::UNRESOLVED_ANCESTOR;return out; }
        const auto& original=originals[found];
        if (!seen.insert(found).second || info.token!=original.channel ||
                info.windowToken!=original.client || info.ownerUid!=gui::Uid{static_cast<uint32_t>(original.uid)}) {
            out.state=ExternalDesktopRootJoin::REJECTED;return out;
        }
        ++out.matched;out.originalLifetimes.emplace_back(original.child,original.window);out.cloneRows.push_back(row);
    }
    if (seen.size()==originals.size()) out.state=ExternalDesktopRootJoin::CLONES_OBSERVED;
    return out; // No Rust StageMachine submit/observe/route/claim/release call.
}
} // namespace android
#endif
