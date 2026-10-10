#pragma once

#include "BoardDiscovery.h"

namespace babytech { namespace boardlink {

class BoardMaintenanceTarget {
public:
    virtual ~BoardMaintenanceTarget() = default;
    virtual bool safeToAcquire() const = 0;
    // The loop owner cannot release midway through a synchronous import.
    // Dropping the reservation never rolls back Flash; an uncertain write
    // faults its importer, not necessarily independent local debugging.
    virtual bool safeToRelease() const = 0;
};

enum class BoardMaintenanceState { Idle, Pending, Active, Releasing, Released,
                                   Unsafe, Busy, Unavailable, TimedOut };

// One explicit installation session, not a normal boot/action requirement.
// All output is copied to the existing UART transmitter by the loop owner.
class BoardMaintenance {
public:
    static constexpr uint32_t kResponseMs = 1000;
    static constexpr uint32_t kRenewMs = 500;
    static constexpr uint32_t kLeaseMs = 3000;
    bool begin(v4::Role role, const char* physicalId, uint64_t boot,
               const v4::Pairing* pairing = nullptr);
    bool setTarget(BoardMaintenanceTarget* target);
    bool request(const char* device, const DiscoveryResult& peer,
                 const char* nonce, uint32_t nowMs);
    bool release(uint32_t nowMs);
    void receive(const v4::Frame& frame, uint32_t nowMs);
    void poll(uint32_t nowMs);
    const v4::Frame* outgoing() const { return pendingOutput_ ? &output_ : nullptr; }
    void queued() { pendingOutput_ = false; }
    BoardMaintenanceState state() const { return state_; }
    bool active() const { return role_ == v4::Role::Motion && held_; }
    bool owns(const char* device, const char* nonce, uint64_t requesterBoot) const;
    bool owns(const char* device, const char* nonce, uint64_t requesterBoot,
              const char* requesterPhysicalId) const;
    const char* nonce() const { return nonce_; }
    uint64_t peerBoot() const { return peerBoot_; }
private:
    bool send(uint8_t operation, uint32_t nowMs);
    void reply(const v4::Frame& request, uint8_t result, const char* nonce);
    v4::Role role_ = v4::Role::Brain;
    v4::Pairing pairing_{};
    char physical_[13]{}, device_[65]{}, requester_[13]{}, nonce_[33]{};
    uint64_t boot_ = 0, peerBoot_ = 0;
    uint32_t nextId_ = 1, requestId_ = 0, lastPeerId_ = 0;
    uint32_t requestedAt_ = 0, renewedAt_ = 0;
    uint32_t confirmedAt_ = 0;
    v4::Frame output_{};
    BoardMaintenanceTarget* target_ = nullptr;
    BoardMaintenanceState state_ = BoardMaintenanceState::Idle;
    bool initialized_ = false, paired_ = false, pendingOutput_ = false;
    bool awaiting_ = false, held_ = false, expired_ = false;
};

} }
