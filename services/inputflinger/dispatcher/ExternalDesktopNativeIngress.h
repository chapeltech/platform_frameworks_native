#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include <cstdint>
#include <memory>
#include <utility>

namespace android::inputdispatcher {
class InputDispatcher;
class InputDispatcherTest;

// Private native bookkeeping domain, NOT a lease/stream/epoch/claim issuer.
// A new actual dispatcher owns a new referent. No public wire pointer/number
// can construct it or compare ordinals across distinct owner incarnations.
class ExternalDesktopNativeIngressOwner final {
private:
    friend class InputDispatcher;
    ExternalDesktopNativeIngressOwner() = default;
};

class ExternalDesktopNativeIngress final {
public:
    enum class Kind { NOTIFY_MOTION, INJECTED, NATIVE_SYNTHETIC, TEST_DOUBLE };
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest; // private diagnostic assertions, not a production factory
    ExternalDesktopNativeIngress(std::shared_ptr<const ExternalDesktopNativeIngressOwner> owner,
                        uint64_t ordinal, Kind kind)
          : mOwner(std::move(owner)), mOrdinal(ordinal), mKind(kind) {}
    const std::shared_ptr<const ExternalDesktopNativeIngressOwner> mOwner;
    const uint64_t mOrdinal;
    const Kind mKind;
};
using ExternalDesktopNativeIngressPtr = std::shared_ptr<const ExternalDesktopNativeIngress>;
} // namespace android::inputdispatcher
#endif
