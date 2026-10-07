#pragma once

#include "ProductBoardMessages.h"
#include "ProductResultQuery.h"
#include "ProductCommandResult.h"
#include "BoardTransmitV4.h"

namespace babytech { namespace boardlink {

enum class ResultLookupState { Idle, Pending, Complete, TimedOut, Unavailable };
enum class CommandSendState { Idle, Pending, Complete, TimedOut, Unavailable, Cancelled };
enum class StopSendState { Idle, Pending, Received, Rejected, TimedOut, Unavailable };

// Single-owner, non-reentrant link core. Motion actions require explicit handlers;
// Sending requires an explicit caller; transmission is never business acceptance.
// Keep this ~8 KiB object off the MCU task stack (static/member storage).
class ReadOnlyLink {
public:
    void reset();
    bool begin(const v4::Pairing& pairing, uint64_t boot);
    void receive(uint8_t byte, uint32_t nowMs);
    void receiveFrame(const v4::Frame& frame, uint32_t nowMs);
    bool queueSupportFrame(const v4::Frame& frame);
    bool queueInstallMessage(const v4::Message& message);
    using ResultQueryHandler = bool (*)(const ResultQuery&, QueriedResult&);
    bool setResultQueryHandler(ResultQueryHandler handler);
    using CommandHandler = bool (*)(const CommandMessage&, uint32_t, CommandResult&);
    using StopHandler = bool (*)(const v4::StopRequest&, uint32_t);
    using CommandReadyHandler = bool (*)(CommandResult&);
    bool setCommandHandler(CommandHandler handler);
    bool setStopHandler(StopHandler handler);
    bool setCommandReadyHandler(CommandReadyHandler handler);
    // Only a newly authorized request may be sent, once. The caller owns local
    // durable reservation / Cloud freshness and must query uncertain outcomes.
    bool requestCommand(const CommandMessage& command, uint32_t nowMs);
    CommandSendState commandSendState() const { return sendState_; }
    const CommandResult& commandResponse() const { return sentResult_; }
    void cancelCommand();
    // Local Stop gets its transient ID here and needs no durable sequence/NVS.
    // Received is the matched Motion receipt, never proof of stationary axes.
    bool requestStop(const v4::StopRequest& request, uint32_t nowMs);
    StopSendState stopSendState() const { return stopState_; }
    bool requestResult(const ResultQuery& query, uint32_t nowMs);
    ResultLookupState resultLookupState() const { return lookupState_; }
    const QueriedResult& resultQueryResponse() const { return queriedResult_; }
    // Cancels this transient query/queued bytes, never a durable request/action.
    // Partial frames drain with invalid CRC before other ordinary bytes may send.
    void cancelResultQuery();
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
    void expireResultQuery(uint32_t nowMs);
    void expireCommand(uint32_t nowMs);
    void expireStop(uint32_t nowMs);
    v4::Parser parser_{};
    v4::Assembler assembler_{};
    v4::Session session_{};
    v4::Transmitter tx_{};
    v4::Message scratch_{};
    Status peerStatus_{};
    QueriedResult queriedResult_{};
    ResultQueryHandler resultHandler_ = nullptr;
    CommandHandler commandHandler_ = nullptr;
    StopHandler stopHandler_ = nullptr;
    CommandReadyHandler commandReadyHandler_ = nullptr;
    CommandResult commandResult_{};
    bool commandReplyPending_ = false;
    uint32_t commandId_ = 0;
    uint32_t commandAt_ = 0;
    uint32_t highestCommandId_ = 0;
    uint32_t stopBarrier_ = 0;
    ResultLookupState lookupState_ = ResultLookupState::Idle;
    uint32_t lookupId_ = 0;
    uint32_t lookupAt_ = 0;
    bool resultReplyPending_ = false;
    CommandResult sentResult_{};
    CommandSendState sendState_ = CommandSendState::Idle;
    uint32_t sentCommandId_ = 0;
    uint32_t sentCommandAt_ = 0;
    bool commandInTransmitter_ = false;
    StopSendState stopState_ = StopSendState::Idle;
    uint32_t sentStopId_ = 0;
    uint32_t sentStopAt_ = 0;
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
