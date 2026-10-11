#pragma once

#include "BoardCommissioning.h"

namespace babytech { namespace boardlink {

class BoardInstallTarget {
public:
    virtual ~BoardInstallTarget() = default;
    // The runtime owner authorizes maintenance/nonce/boot ownership, fresh
    // stationary feedback, credential handoff and every durable NVS phase.
    virtual CommissioningResult install(const CommissioningImport& request,
                                        const char* nonce, uint64_t requesterBoot) = 0;
};

enum class BoardInstallState { Idle, Pending, Complete, TimedOut, Unavailable };

// Controlled first-install channel only, not action or identity authorization.
// Single UART loop owner: enqueue outgoing() on its existing transmitter, then
// call queued() only after the transmitter has copied the entire message.
// poll() withdraws an unqueued Brain request at the response deadline; bytes
// already copied to the transmitter and uncertain NVS writes remain unknown.
// Wire request: 1, nonce[32], pair length u16 LE, canonical PairingRecord,
// hasContext u8, context length u16 LE, full ContextIdentity (or zero bytes),
// explicit credential-handoff confirmation u8 (=1). Response: 2, nonce[32],
// result u8: Installed=0, AlreadyInstalled=1, Invalid=2, IdentityMismatch=3,
// Conflict=4, Unsafe=5, LegacyPending=6, StorageFault=7, StateMissing=8.
class BoardInstall {
public:
    static constexpr uint32_t kResponseMs = 3000;
    // Borrow the existing UART core's reassembly and RX scratch before begin.
    // Buffers must outlive this object; the sole loop routes install frames
    // here instead of also dispatching them through the ordinary endpoint.
    // Rebinding the identical buffers is idempotent for setup retries.
    bool bindReceiveBuffers(v4::Assembler& assembler, v4::Message& scratch);
    // Initialization retry only; does not undo NVS or prove a timed-out install
    // did not execute. Clears borrowed buffers in place, preserving binding;
    // the owner must resolve uncertain writes before reuse and exclude normal
    // RX while explicitly resetting an install session.
    void reset();
    bool begin(v4::Role role, const char* physicalId, uint64_t boot);
    bool setTarget(BoardInstallTarget* target);
    // Only the controlled Brain installer may call this after explicit handoff.
    bool request(const CommissioningImport& motionRequest, const char* nonce,
                 uint64_t peerBoot, uint32_t nowMs);
    void receive(const v4::Frame& frame, uint32_t nowMs);
    void poll(uint32_t nowMs);
    const v4::Message* outgoing() const { return pendingOutput_ ? &output_ : nullptr; }
    void queued() { pendingOutput_ = false; }
    BoardInstallState state() const { return state_; }
    uint64_t boot() const { return boot_; }
    const char* physicalId() const { return physical_; }
    bool matchesRequestedPair(const v4::Pairing& expected) const;
    // Meaningful only after Complete (or Motion's Unavailable response).
    // TimedOut does not establish whether any durable write occurred.
    CommissioningResult result() const { return result_; }
private:
    struct Replay {
        uint64_t boot = 0;
        uint32_t id = 0;
        uint16_t length = 0;
        uint8_t digest[32]{};
        CommissioningResult result = CommissioningResult::Invalid;
    };
    bool decodeRequest();
    void reply(const v4::Message& request, CommissioningResult result);
    v4::Assembler* receiveAssembler_ = nullptr;
    v4::Message* receiveScratch_ = nullptr;
    v4::Message output_{};
    Replay ownerReplay_, rejectedReplay_;
    CommissioningImport decoded_;
    BoardInstallTarget* target_ = nullptr;
    char physical_[13]{}, nonce_[33]{};
    v4::Role role_ = v4::Role::Brain;
    uint64_t boot_ = 0, peerBoot_ = 0;
    uint32_t nextId_ = 1, requestId_ = 0, requestedAt_ = 0;
    BoardInstallState state_ = BoardInstallState::Idle;
    CommissioningResult result_ = CommissioningResult::Invalid;
    bool initialized_ = false, pendingOutput_ = false;
};

static_assert(sizeof(BoardInstall) <= 4 * 1024, "Install staging must not duplicate UART RX buffers");

} }
