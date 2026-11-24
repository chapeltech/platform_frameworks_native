#pragma once

#include <binder/IBinder.h>
#include <binder/Parcel.h>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace android::gui {

// PRIVATE same-build v6 facts. Neither this token nor these facts grant authority.
// Query only outside Dispatcher/WMS locks, then independently rejoin the actual
// complete WindowInfos callback/received sequence. Absence/death is NOT release.
enum class NativeLifetimeKind : uint32_t { Layer = 1, Snapshot = 2, RootResourceGroup = 3 };
struct NativePublication {
    sp<IBinder> origin;
    uint64_t serial = 0, mainPass = 0;
    bool available() const { return origin && serial && mainPass; }
    bool valid() const {
        return available() || (!origin && serial == 0 && mainPass == 0);
    }
};
struct NativeLifetimeFact {
    NativeLifetimeKind kind = NativeLifetimeKind::Layer;
    bool available = false, completed = false;
    uint64_t removalPass = 0;
    NativePublication publication;
    bool valid() const {
        if (kind != NativeLifetimeKind::Layer && kind != NativeLifetimeKind::Snapshot &&
            kind != NativeLifetimeKind::RootResourceGroup) return false;
        if (!publication.valid()) return false;
        if (!available) return !completed && !removalPass && !publication.available();
        return (!removalPass || completed) && (!publication.available() ||
                (completed && removalPass && publication.mainPass >= removalPass));
    }
};
// Necessary fact comparison ONLY, not a retirement/authority decision. The
// caller must supply the ACTUAL complete native callback and independently
// retain/rejoin its received sequence, identities, queue tails and authority.
inline bool nativeLifetimeFactWithinPublication(const NativeLifetimeFact& fact,
                                                const NativePublication& callback) {
    return fact.valid() && fact.available && fact.completed && fact.removalPass &&
            fact.publication.available() && callback.valid() && callback.available() &&
            fact.publication.origin==callback.origin &&
            fact.publication.serial<=callback.serial && fact.removalPass<=callback.mainPass &&
            fact.publication.mainPass<=callback.mainPass;
}
// Ordinary private user transaction: retain Binder's normal stability checks.
// Unpublished v6 only; no public SDK/APK transaction number is changed.
constexpr uint32_t kNativeLifetimeQuery = 0x00574c46; // WLF.
static_assert(kNativeLifetimeQuery>=IBinder::FIRST_CALL_TRANSACTION &&
              kNativeLifetimeQuery<=IBinder::LAST_CALL_TRANSACTION);
constexpr uint32_t kNativeLifetimeQueryVersion = 1;
// Explicit PRIVATE extension queried through the original Layer identity. The
// v1 Layer/Snapshot meanings and v6 WindowInfos bytes remain unchanged.
constexpr uint32_t kNativeRootResourceGroupQueryVersion = 2;

inline status_t writeNativePublication(Parcel* p, const NativePublication& value) {
    if (!value.valid()) return BAD_VALUE;
    if (auto s=p->writeStrongBinder(value.origin); s!=OK) return s;
    if (auto s=p->writeUint64(value.serial); s!=OK) return s;
    return p->writeUint64(value.mainPass);
}
inline status_t readNativePublication(const Parcel* p, NativePublication& out) {
    NativePublication value;
    if (auto s=p->readNullableStrongBinder(&value.origin); s!=OK) return s;
    if (auto s=p->readUint64(&value.serial); s!=OK) return s;
    if (auto s=p->readUint64(&value.mainPass); s!=OK) return s;
    if (!value.valid()) return BAD_VALUE;
    out=std::move(value); return OK;
}
inline status_t writeNativeLifetimeFact(Parcel* p, const NativeLifetimeFact& value) {
    if (!value.valid()) return BAD_VALUE;
    const uint32_t version=value.kind==NativeLifetimeKind::RootResourceGroup ?
            kNativeRootResourceGroupQueryVersion:kNativeLifetimeQueryVersion;
    for (uint32_t n : {version, static_cast<uint32_t>(value.kind),
                       value.available ? 1u:0u, value.completed ? 1u:0u}) {
        if (auto s=p->writeUint32(n); s!=OK) return s;
    }
    if (auto s=p->writeUint64(value.removalPass); s!=OK) return s;
    return writeNativePublication(p,value.publication);
}
inline status_t queryNativeLifetimeFactVersion(const sp<IBinder>& token, NativeLifetimeFact& out,
                                              uint32_t expectedVersion) {
    out={}; if (!token) return BAD_VALUE;
    if (expectedVersion!=kNativeLifetimeQueryVersion &&
        expectedVersion!=kNativeRootResourceGroupQueryVersion) return BAD_VALUE;
    Parcel request,reply;
    if (auto s=request.writeUint32(expectedVersion); s!=OK) return s;
    if (auto s=token->transact(kNativeLifetimeQuery,request,&reply); s!=OK) return s;
    uint32_t version,kind,available,completed;
    for (uint32_t* n : {&version,&kind,&available,&completed}) {
        if (auto s=reply.readUint32(n); s!=OK) return s;
    }
    if (version!=expectedVersion || available>1 || completed>1 ||
        ((kind==static_cast<uint32_t>(NativeLifetimeKind::RootResourceGroup)) !=
         (version==kNativeRootResourceGroupQueryVersion))) return BAD_VALUE;
    NativeLifetimeFact value{static_cast<NativeLifetimeKind>(kind),available!=0,completed!=0};
    if (auto s=reply.readUint64(&value.removalPass); s!=OK) return s;
    if (auto s=readNativePublication(&reply,value.publication); s!=OK) return s;
    if (reply.dataAvail()!=0 || !value.valid()) return BAD_VALUE;
    out=std::move(value); return OK;
}
inline status_t queryNativeLifetimeFact(const sp<IBinder>& token, NativeLifetimeFact& out) {
    return queryNativeLifetimeFactVersion(token,out,kNativeLifetimeQueryVersion);
}
inline status_t queryNativeRootResourceGroupFact(const sp<IBinder>& token, NativeLifetimeFact& out) {
    return queryNativeLifetimeFactVersion(token,out,kNativeRootResourceGroupQueryVersion);
}
} // namespace android::gui
