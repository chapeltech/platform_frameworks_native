#pragma once

#if defined(EXTERNAL_DESKTOP_EXPERIMENTAL_NATIVE_STAGE)
#include "ExternalDesktopPointerOwnership.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace android::inputdispatcher {
class Connection;
class InputDispatcher;
class InputDispatcherTest;
class ExternalDesktopStageOwner;
class ExternalDesktopPublishedMotion;
struct DispatchEntry;
struct EventEntry;

enum class ExternalDesktopPublicationState { ORDINARY, ROUTABLE, SUSPENDED, REFUSED };

// Nonresource counters survive queue abort and Connection destruction. Neither
// an ACK nor local-FD closure is a claim/SF-removal proof.
class ExternalDesktopSentTail final {
private:
    friend class Connection;
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    size_t mSent = 0;
    bool mHasLivePointers = false;
    // Per-row delivery facts only. They do NOT prove a global Reader exit,
    // physical/root destruction, or authority to erase this row/claim.
    bool mHoverWithdrawalPublished = false;
    bool mHoverWithdrawalAcknowledged = false;
};

class ExternalDesktopPublisherLifetime final {
private:
    friend class Connection;
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    friend class ExternalDesktopPublisherDestruction;
    struct Sent {
        uint32_t seq = 0;
        const DispatchEntry* packet = nullptr; // identity ONLY; never dereferenced after abort
        const EventEntry* event = nullptr;
        std::vector<std::shared_ptr<ExternalDesktopSentTail>> tails;
        // Nonowning subset of tails (which pins every referent). Only a
        // validated saved-hover withdrawal may populate this before send.
        std::vector<ExternalDesktopSentTail*> hoverWithdrawals;
    };
    static constexpr size_t kMaxSent = 1024;
    std::vector<std::unique_ptr<Sent>> mSent;
    uint64_t mRevision = 1;
    bool mComplete = true;
    std::atomic<bool> mLocalFdDestroyed{false};
};

// MUST precede Connection::inputPublisher in declaration order. Its destructor
// runs AFTER the complete InputPublisher destructor, its unique InputChannel,
// InputChannelCore base and owned local fd. No resource is retained here. This
// does not attest that dup()ed fds or the remote endpoint have been destroyed.
class ExternalDesktopPublisherDestruction final {
private:
    friend class Connection;
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    const std::shared_ptr<ExternalDesktopPublisherLifetime> mState{
            std::make_shared<ExternalDesktopPublisherLifetime>()};
public:
    ~ExternalDesktopPublisherDestruction() { mState->mLocalFdDestroyed.store(true, std::memory_order_release); }
    ExternalDesktopPublisherDestruction() = default;
    ExternalDesktopPublisherDestruction(const ExternalDesktopPublisherDestruction&) = delete;
    ExternalDesktopPublisherDestruction& operator=(const ExternalDesktopPublisherDestruction&) = delete;
};

// Dedicated NONRESOURCE identity. Never subclassed into a Java/Binder/SC/FD
// owner. A row cannot keep alive the resource whose teardown is still unproven.
// StageOwner alone mints these from real retained claim/root lifetimes. The
// producer bit is dispatcher-lock serialized; clearing it is NOT retirement.
class ExternalDesktopSubmissionIdentity final {
private:
    friend class ExternalDesktopStageOwner;
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    ExternalDesktopSubmissionIdentity() = default;
    // Only the producer identity transitions. Suspension is nonterminal and
    // authorizes NO publication; INERT is irreversible and closes old streams.
    enum class State { ROUTABLE, SUSPENDED, INERT };
    State mState = State::ROUTABLE;
    int64_t mSuspendedUntil = 0; // BOOTTIME ms; StageOwner supplies REAL authorized coverage only
};

class ExternalDesktopPhysicalPointer final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    enum class Kind { READER, TEST_DOUBLE };
    ExternalDesktopPhysicalPointer(Kind kind, DeviceId device, uint32_t source,
                         ui::LogicalDisplayId display, int32_t id, bool hover)
          : mKind(kind), mDevice(device), mSource(source), mDisplay(display),
            mId(id), mHover(hover) {}
    const Kind mKind;
    const DeviceId mDevice;
    const uint32_t mSource;
    const ui::LogicalDisplayId mDisplay;
    const int32_t mId;
    const bool mHover;
    // Sticky for this exact raw referent, independently of reclaimable rows.
    // A missing/expired logical row must never make retained Reader input
    // eligible for a fresh owned admission. Only successful target commit sets
    // this; it is neither physical completion nor authority for a rebind.
    bool mOwnedAdmissionSeen = false;
    // Observations are NOT retirement. UP, reset, cancellation and hover
    // withdrawal are intentionally different facts. No flag erases a row.
    bool mActualUp = false;
    bool mReaderCancel = false;
    bool mDeviceReset = false;
    bool mReaderHoverExit = false;
    bool mButtonRelease = false;
    bool mRawRosterWithdrawn = false; // raw hover roster change, NOT logical routing withdrawal
    bool mRawRestart = false; // replacement DOWN/ENTER, NOT an observed CANCEL/UP
};

class ExternalDesktopPhysicalPacket final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    std::array<std::shared_ptr<ExternalDesktopPhysicalPointer>, ExternalDesktopPointerOwnership::kMaxIds> mPointers{};
};
using ExternalDesktopPhysicalPacketPtr = std::shared_ptr<const ExternalDesktopPhysicalPacket>;

class ExternalDesktopSubmissionRow final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    friend class ExternalDesktopPublishedMotion;
    ExternalDesktopSubmissionRow(std::weak_ptr<Connection> connection,
                       std::shared_ptr<const ExternalDesktopSubmissionIdentity> window,
                       std::shared_ptr<const ExternalDesktopSubmissionIdentity> presentation,
                       std::shared_ptr<const ExternalDesktopSubmissionIdentity> producer,
                       std::shared_ptr<const ExternalDesktopPhysicalPointer> physical,
                       ExternalDesktopPointerStreamKey key, int32_t id,
                       std::shared_ptr<const ExternalDesktopPublisherLifetime> publisher = {},
                       int32_t windowId = 0, int32_t destinationDisplay = -1)
          : mConnection(std::move(connection)), mWindow(std::move(window)),
            mPresentation(std::move(presentation)), mProducer(std::move(producer)),
            mPhysical(std::move(physical)), mKey(key), mId(id), mPublisher(std::move(publisher)),
            mWindowId(windowId), mDestinationDisplay(destinationDisplay) {}
    const std::weak_ptr<Connection> mConnection;
    const std::shared_ptr<const ExternalDesktopSubmissionIdentity> mWindow;
    const std::shared_ptr<const ExternalDesktopSubmissionIdentity> mPresentation;
    const std::shared_ptr<const ExternalDesktopSubmissionIdentity> mProducer;
    const std::shared_ptr<const ExternalDesktopPhysicalPointer> mPhysical;
    const ExternalDesktopPointerStreamKey mKey;
    const int32_t mId;
    const std::shared_ptr<const ExternalDesktopPublisherLifetime> mPublisher;
    const int32_t mWindowId, mDestinationDisplay;
    const std::shared_ptr<ExternalDesktopSentTail> mTail{std::make_shared<ExternalDesktopSentTail>()};
    bool mClosing = false;
    bool mTransportDrained = false; // tombstone fact ONLY; never a WsRelease/row erasure
    // SF removal, root/window destruction and full claim retirement remain
    // separate mandatory fences. No transport fact releases a claim.
};

class ExternalDesktopSubmissionReservation final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    friend class ExternalDesktopPublishedMotion;
    std::array<std::shared_ptr<ExternalDesktopSubmissionRow>, ExternalDesktopPointerOwnership::kMaxIds> mRows{};
};
using ExternalDesktopSubmissionReservationPtr = std::shared_ptr<const ExternalDesktopSubmissionReservation>;

class ExternalDesktopClosingHandle final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    std::array<std::shared_ptr<ExternalDesktopSubmissionRow>, ExternalDesktopPointerOwnership::kMaxIds> mRows{};
    bool mSealed = false;
};

// Minted only for actual saved-geometry cancellation packets produced by the
// Connection queue planner. ACTION_CANCEL/native-synthetic alone is not proof.
class ExternalDesktopClosingTail final {
private:
    friend class InputDispatcher;
    ExternalDesktopClosingTail(const DispatchEntry* packet,uint32_t seq,const EventEntry* event,
            std::weak_ptr<Connection> connection,std::shared_ptr<const ExternalDesktopClosingHandle> closing)
          : mPacket(packet),mSeq(seq),mEvent(event),mConnection(std::move(connection)),mClosing(std::move(closing)) {}
    const DispatchEntry* const mPacket;
    const uint32_t mSeq;
    const EventEntry* const mEvent;
    const std::weak_ptr<Connection> mConnection;
    const std::shared_ptr<const ExternalDesktopClosingHandle> mClosing;
};

// Counts actual notify callbacks even while external policy/filter releases
// mLock. It owns only the counter; destruction never dereferences a dispatcher.
class ExternalDesktopCallbackInFlight final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    explicit ExternalDesktopCallbackInFlight(std::shared_ptr<std::atomic<size_t>> counter)
          : mCounter(std::move(counter)) { mCounter->fetch_add(1); }
public:
    ~ExternalDesktopCallbackInFlight() { mCounter->fetch_sub(1); }
    ExternalDesktopCallbackInFlight(const ExternalDesktopCallbackInFlight&) = delete;
    ExternalDesktopCallbackInFlight& operator=(const ExternalDesktopCallbackInFlight&) = delete;
private:
    const std::shared_ptr<std::atomic<size_t>> mCounter;
};

class ExternalDesktopSubmissionLedger final {
private:
    friend class InputDispatcher;
    friend class InputDispatcherTest;
    // Non-evicting submission/tombstone history. Capacity loss is sticky for
    // PRIVATE admission; ordinary dispatch continues. There is NO release API.
    static constexpr size_t kMaxRows = 256;
    static constexpr size_t kMaxPhysicalPointers = 64;
    static constexpr size_t kMaxPhysicalPackets = 1024;
    // Observations only: no packet, input target, or resource is retained by
    // this index. Never evict a live weak referent to manufacture coverage.
    std::array<std::weak_ptr<const ExternalDesktopPhysicalPacket>, kMaxPhysicalPackets> mPhysicalPackets{};
    struct PhysicalOwnerCensus {
        size_t submissionRows = 0;
        size_t activeSlots = 0;
        size_t packetSlots = 0;
        size_t total() const { return submissionRows + activeSlots + packetSlots; }
    };
    std::array<std::shared_ptr<ExternalDesktopSubmissionRow>, kMaxRows> mRows{};
    std::array<std::shared_ptr<ExternalDesktopPhysicalPointer>, kMaxPhysicalPointers> mActivePhysical{};
    std::shared_ptr<std::atomic<size_t>> mCallbacks{std::make_shared<std::atomic<size_t>>(0)};
    size_t mRowsUsed = 0;
    bool mCoverageAvailable = true;
    uint64_t mStreamHighWater = 0; // native logical admissions only; never a tag/downTime
};
} // namespace android::inputdispatcher
#endif
