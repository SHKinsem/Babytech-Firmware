#pragma once

#include "ProductBoardMessages.h"
#include "BoardTransmitV4.h"

namespace babytech { namespace boardlink {

// Single-owner, non-reentrant link core. No action dispatch is present.
// Keep this ~8 KiB object off the MCU task stack (static/member storage).
class ReadOnlyLink {
public:
    void reset();
    bool begin(const v4::Pairing& pairing, uint64_t boot);
    void receive(uint8_t byte, uint32_t nowMs);
    void receiveFrame(const v4::Frame& frame, uint32_t nowMs);
    bool queueSupportFrame(const v4::Frame& frame);
    bool queueInstallMessage(const v4::Message& message);
    // Explicit installer borrows the sole receive assembler/scratch. The same
    // loop owns both paths; interleaved ordinary fragments remain backpressure.
    v4::Assembler& installAssembler() { return assembler_; }
    v4::Message& installScratch() { return scratch_; }
    void poll(uint32_t nowMs, v4::ByteSink& sink, const Status* localStatus = nullptr);
    bool connected(uint32_t nowMs) const { return session_.connected(nowMs); }
    bool freshStatus(uint32_t nowMs) const { return session_.freshStatus(nowMs); }
    const Status& peerStatus() const { return peerStatus_; }
    // Pair with peerStatus() while freshStatus() is true. Frozen/replayed
    // samples, heartbeats and poll calls never advance this receipt time.
    uint32_t peerStatusReceivedAtMs() const { return session_.lastStatusReceivedAtMs(); }
    bool configured() const { return configured_; }
    bool healthy() const { return configured_ && tx_.healthy() && nextId_ != 0; }
    v4::LinkFailure takeFailure() { return session_.takeFailure(); }
private:
    bool queue(v4::Message& message, bool discovery = false, uint64_t helloReceiver = 0);
    void handle(const v4::Message& message, uint32_t nowMs);
    void receipt(uint32_t id, bool accepted);
    v4::Parser parser_{};
    v4::Assembler assembler_{};
    v4::Session session_{};
    v4::Transmitter tx_{};
    v4::Message scratch_{};
    Status peerStatus_{};
    v4::Role role_ = v4::Role::Brain;
    uint32_t nextId_ = 1;
    uint32_t lastHelloAt_ = 0;
    uint32_t lastHeartbeatAt_ = 0;
    uint32_t lastStatusAt_ = 0;
    uint32_t awaitingStatusId_ = 0;
    uint32_t awaitingStatusAt_ = 0;
    uint64_t helloAckBoot_ = 0;
    uint32_t helloAckId_ = 0;
    uint32_t peerSample_ = 0;
    bool configured_ = false;
    bool helloSent_ = false;
    bool heartbeatSent_ = false;
    bool statusSent_ = false;
    bool helloAckPending_ = false;
    bool probeRequired_ = false;
    bool peerSampleSeen_ = false;
    bool statusRequested_ = false;
};

} }
