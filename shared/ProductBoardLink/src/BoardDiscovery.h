#pragma once

#include "BoardPairingRecord.h"

namespace babytech { namespace boardlink {

enum class DiscoveryPairState : uint8_t { Missing, Ready, Corrupt, IoError, IdentityMismatch };
enum class DiscoveryState : uint8_t { Idle, Pending, Found, Conflict, Unavailable, TimedOut };

struct DiscoveryResult {
    DiscoveryState state = DiscoveryState::Idle;
    uint64_t peerBoot = 0;
    char physicalId[13]{};
    DiscoveryPairState pairingState = DiscoveryPairState::Missing;
    v4::Pairing pairing{};
};

// Explicit first-install/support discovery, not ordinary pairing or action
// authorization. One loop owner; the existing transmitter owns all UART bytes.
class BoardDiscovery {
public:
    static constexpr uint32_t kTimeoutMs = 1000;
    bool begin(v4::Role role, const char* physicalId, uint64_t boot,
               DiscoveryPairState state, const v4::Pairing* pairing = nullptr);
    bool request(const char* deviceId, uint32_t nowMs);
    void receive(const v4::Frame& frame, uint32_t nowMs);
    void poll(uint32_t nowMs);
    const v4::Frame* outgoing() const { return outgoingPending_ ? &outgoing_ : nullptr; }
    void queued() { outgoingPending_ = false; }
    const DiscoveryResult& result() const { return result_; }
private:
    v4::Role role_ = v4::Role::Brain;
    DiscoveryPairState pairingState_ = DiscoveryPairState::Missing;
    v4::Pairing pairing_{};
    char physicalId_[13]{};
    char requestedDeviceId_[65]{};
    uint64_t boot_ = 0;
    uint32_t nextId_ = 1, requestId_ = 0, requestedAt_ = 0, outgoingAt_ = 0;
    v4::Frame outgoing_{};
    DiscoveryResult result_{};
    bool initialized_ = false, outgoingPending_ = false;
};

} }
