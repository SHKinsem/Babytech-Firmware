#pragma once

#include "BoardDiscovery.h"
#include "BoardExportSource.h"
#include "MotionExportSnapshot.h"
#include <memory>

namespace babytech { namespace boardlink {

enum class ExportTransferState : uint8_t { Idle, Pending, Complete, Unavailable, Invalid, TimedOut };

// Single UART loop owner. Pulling records never changes pairing or runs motors.
class BoardExportTransfer {
public:
    static constexpr uint32_t kLifetimeMs = 5000;
    static constexpr uint32_t kChunkTimeoutMs = 1000;
    bool begin(v4::Role role, const char* physicalId, uint64_t boot);
    void reset();
    bool setSource(BoardExportSource* source);
    bool request(const char* device, const DiscoveryResult& peer,
                 const char* challenge, uint32_t nowMs);
    // Installation-only readback: the boot/physical peer stays discovery-bound,
    // but the just-written pairing must match the explicit expected record.
    // Ordinary diagnostic reads retain their original discovery comparison.
    bool requestInstalled(const char* device, const DiscoveryResult& peer,
                          const char* challenge, const v4::Pairing& expected, uint32_t nowMs);
    // Explicit recovery of a previously submitted install may still be Missing
    // or may have exactly the submitted pair. This is not an ordinary read.
    bool requestRecovery(const char* device, const DiscoveryResult& peer,
                         const char* challenge, const v4::Pairing& expected, uint32_t nowMs);
    void receive(const v4::Frame& frame, uint32_t nowMs);
    void poll(uint32_t nowMs);
    const v4::Frame* outgoing() const { return outgoingPending_ ? &outgoing_ : nullptr; }
    void queued();
    ExportTransferState state() const { return state_; }
    const MotionExportSnapshot* snapshot() const { return snapshot_.get(); }
private:
    bool query(uint32_t nowMs);
    void fail(ExportTransferState state);
    void clearInput();
    void replyError(const v4::Frame& query);
    v4::Role role_ = v4::Role::Brain;
    BoardExportSource* source_ = nullptr;
    std::unique_ptr<char[]> input_;
    std::unique_ptr<MotionExportSnapshot> snapshot_;
    DiscoveryResult peer_;
    char physicalId_[13]{};
    char device_[65]{};
    char challenge_[33]{};
    char requesterPhysical_[13]{};
    uint64_t boot_ = 0, requesterBoot_ = 0;
    uint32_t nextId_ = 1, requestId_ = 0, lastReadId_ = 0;
    uint32_t beganAt_ = 0, chunkAt_ = 0;
    size_t position_ = 0, outgoingCount_ = 0;
    v4::Frame outgoing_{};
    ExportTransferState state_ = ExportTransferState::Idle;
    bool initialized_ = false, outgoingPending_ = false, captureActive_ = false;
    bool allowMissingPair_ = false;
};

} }
