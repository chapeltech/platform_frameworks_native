#pragma once
#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <binder/IBinder.h>
#include <gui/SurfaceControl.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

namespace android {
// An owning same-process Java object identity, resolved by the actual JNI owner
// BEFORE dispatcher mLock. Native comparisons use shared object identity only;
// no JNI callback, object-address number or identityHashCode enters a ledger.
class ExternalDesktopStageObjectIdentity {
public:
    virtual ~ExternalDesktopStageObjectIdentity() = default;
};
using ExternalDesktopStageObject = std::shared_ptr<const ExternalDesktopStageObjectIdentity>;
// JNI owns ONE global packet root plus separate bounded object witnesses via
// JavaVM (never a thread's JNIEnv).
// Its exact NativeImpl, LayerFrame and immutable roster referents are retained.
// No JObject/number is a claim, public handle, or object-retirement proof.
class ExternalDesktopStageRosterReferences {
public:
    virtual ~ExternalDesktopStageRosterReferences() = default;
    virtual bool same(const ExternalDesktopStageRosterReferences&) const = 0;
    virtual const void* implementation() const = 0; // private type discriminator ONLY
};
struct ExternalDesktopStageTaskObservation {
    int64_t displayToken;
    int32_t uid;
    size_t start;
    size_t end;
    sp<SurfaceControl> surface;
    int32_t layerId;
    int64_t taskToken = 0; // Existing WMS opaque Task token, never a layer/Java hash.
    ExternalDesktopStageObject taskIdentity;
};
struct ExternalDesktopStageChildObservation {
    sp<SurfaceControl> surface;
    sp<IBinder> channel;
    sp<IBinder> client;
    int32_t layerId;
    int32_t type;
    int32_t baseType;
    int32_t flags;
    int32_t privateFlags;
    uint32_t state; // exact observed secure/hasSurface/visible/drawn/shown/touch bits
    ExternalDesktopStageObject taskIdentity;
    ExternalDesktopStageObject windowIdentity;
    bool operator==(const ExternalDesktopStageChildObservation&) const = default;
};
struct ExternalDesktopStageRoster {
    std::shared_ptr<const ExternalDesktopStageRosterReferences> references;
    std::vector<ExternalDesktopStageTaskObservation> tasks;
    std::vector<ExternalDesktopStageChildObservation> windows;
    ExternalDesktopStageObject nativeOwnerIdentity;
};
// JNI reads these from EVERY actual private LayerDisplay/LayerSubmission of
// the same LayerFrame, including displays with no presentation roots. Neither
// caller arrays nor presence of one root attest the other display listeners.
struct ExternalDesktopStageSubmissionObservation {
    ExternalDesktopStageObject frame,submission;
    int64_t displayToken=0,serial=0;
    bool committed=false,reported=false;
};
} // namespace android
#endif
