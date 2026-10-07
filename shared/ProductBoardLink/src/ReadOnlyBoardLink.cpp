#include "ReadOnlyBoardLink.h"

#include <ArduinoJson.h>
#include <cstring>

namespace babytech { namespace boardlink {
using namespace v4;

void ReadOnlyLink::reset() {
    parser_.reset(); assembler_.reset(); tx_.reset();
    session_ = Session{};
    peerStatus_ = Status{};
    nextId_ = 1;
    lastHelloAt_ = lastHeartbeatAt_ = lastStatusAt_ = 0;
    awaitingStatusId_ = awaitingStatusAt_ = 0;
    helloAckBoot_ = 0; helloAckId_ = 0;
    helloSent_ = heartbeatSent_ = statusSent_ = false;
    helloAckPending_ = statusRequested_ = false;
    probeRequired_ = false;
    peerSample_ = 0; peerSampleSeen_ = false;
    role_ = Role::Brain;
    configured_ = false;
}

bool ReadOnlyLink::begin(const Pairing& pairing, uint64_t boot) {
    reset();
    role_ = pairing.role;
    configured_ = session_.begin(pairing, boot);
    return configured_;
}

bool ReadOnlyLink::queue(Message& message, bool discovery, uint64_t helloReceiver) {
    if (!healthy() || (!discovery && !helloReceiver && !session_.canExchange())) return false;
    message.senderBoot = session_.localBoot();
    message.receiverBoot = helloReceiver ? helloReceiver : (discovery ? 0 : session_.peerBoot());
    message.messageId = nextId_;
    if (!tx_.enqueue(message)) return false;
    ++nextId_;
    return true;
}

void ReadOnlyLink::receipt(uint32_t id, bool accepted) {
    if (!healthy() || !session_.canExchange()) return;
    StaticJsonDocument<96> document;
    document["message_id"] = id;
    Frame reply;
    reply.kind = accepted ? Kind::LinkAck : Kind::LinkReject;
    if (document.overflowed()) return;
    reply.length = uint16_t(serializeJson(document, reply.payload, sizeof(reply.payload)));
    reply.total = reply.length;
    reply.senderBoot = session_.localBoot(); reply.receiverBoot = session_.peerBoot();
    reply.messageId = nextId_;
    if (tx_.enqueueControl(reply)) ++nextId_;
}

void ReadOnlyLink::handle(const Message& message, uint32_t nowMs) {
    if (message.kind == Kind::Hello || message.kind == Kind::HelloAck) {
        Hello hello;
        if (!decodeHello(message, hello)) return;
        const uint64_t previousBoot = session_.peerBoot();
        if (session_.hello(message, hello, nowMs) != HelloResult::Accepted) return;
        if (message.kind == Kind::Hello) {
            helloAckPending_ = true;
            helloAckBoot_ = message.senderBoot;
            helloAckId_ = message.messageId;
            if (!session_.connected(nowMs) || message.senderBoot != previousBoot)
                probeRequired_ = true;
            return;
        }
        probeRequired_ = false;
        if (previousBoot != session_.peerBoot()) {
            assembler_.reset();
            if (previousBoot) tx_.cancelOrdinary();
            awaitingStatusId_ = 0;
            heartbeatSent_ = statusSent_ = false;
            peerStatus_ = Status{};
            peerSampleSeen_ = false;
        }
        return;
    }
    if (!session_.matches(message.senderBoot, message.receiverBoot)) return;
    if (message.kind == Kind::Status && role_ == Role::Brain) {
        Status status;
        const bool valid = decodeStatus(message, status);
        const uint32_t sampleStep = status.sampleUptimeMs - peerSample_;
        if (valid && (!peerSampleSeen_ || (sampleStep && sampleStep < UINT32_C(0x80000000))) &&
            session_.status(message, nowMs)) {
            // Read-only migration stage never authorizes a product action.
            status.snapshot.startEnabled = false;
            peerStatus_ = status;
            peerSample_ = status.sampleUptimeMs; peerSampleSeen_ = true;
        }
        receipt(message.messageId, valid);
    } else if (message.kind == Kind::StatusQuery && role_ == Role::Motion) {
        if (session_.connected(nowMs)) statusRequested_ = true;
    } else if (message.kind == Kind::LinkAck || message.kind == Kind::LinkReject) {
        StaticJsonDocument<96> document;
        if (deserializeJson(document, message.payload, message.length) ||
            !document.is<JsonObject>() || document.size() != 1 ||
            document["message_id"].is<bool>() || !document["message_id"].is<uint32_t>()) return;
        if (document["message_id"].as<uint32_t>() == awaitingStatusId_) awaitingStatusId_ = 0;
    } else {
        // Commands, Stop, configuration and event receipts cannot mutate hardware here.
        receipt(message.messageId, false);
    }
}

void ReadOnlyLink::receive(uint8_t byte, uint32_t nowMs) {
    if (!healthy()) return;
    Frame frame;
    if (!parser_.push(byte, nowMs, frame)) return;
    receiveFrame(frame, nowMs);
}

void ReadOnlyLink::receiveFrame(const Frame& frame, uint32_t nowMs) {
    if (!healthy() || !validFrame(frame) || frame.kind == Kind::Discovery ||
        frame.kind == Kind::MigrationRead || frame.kind == Kind::MigrationMaintenance) return;
    session_.poll(nowMs);
    if (frame.kind == Kind::Heartbeat) {
        session_.heartbeat(frame, nowMs);
        return;
    }
    const bool hello = frame.kind == Kind::Hello || frame.kind == Kind::HelloAck;
    if (!hello && !session_.matches(frame.senderBoot, frame.receiverBoot)) return;
    if (hello && frame.receiverBoot != session_.localBoot() &&
        !(frame.kind == Kind::Hello && frame.receiverBoot == 0)) return;
    const auto result = assembler_.accept(frame, nowMs, scratch_);
    if (result == AssemblyResult::Complete) handle(scratch_, nowMs);
}

bool ReadOnlyLink::queueSupportFrame(const Frame& frame) {
    if ((configured_ && !healthy()) || (frame.kind != Kind::Discovery && frame.kind != Kind::MigrationRead &&
                                      frame.kind != Kind::MigrationMaintenance) ||
        !validFrame(frame) || frame.offset || frame.total != frame.length) return false;
    scratch_.kind = frame.kind;
    scratch_.senderBoot = frame.senderBoot;
    scratch_.receiverBoot = frame.receiverBoot;
    scratch_.messageId = frame.messageId;
    scratch_.length = frame.length;
    std::memcpy(scratch_.payload, frame.payload, frame.length);
    return tx_.enqueue(scratch_);
}

void ReadOnlyLink::poll(uint32_t nowMs, ByteSink& sink, const Status* localStatus) {
    if (!configured_) { tx_.pump(sink); return; }
    if (!healthy()) return;
    session_.poll(nowMs);
    assembler_.expire(nowMs);
    if (!session_.canExchange()) {
        peerStatus_ = Status{};
        awaitingStatusId_ = 0;
    }
    if (helloAckPending_ && !tx_.ordinaryPending()) {
        Hello identity = session_.localHello();
        identity.replyTo = helloAckId_;
        if (encodeHello(identity, scratch_, Kind::HelloAck) && queue(scratch_, false, helloAckBoot_))
            helloAckPending_ = false;
    }
    if ((!session_.connected(nowMs) || probeRequired_) && !session_.awaitingHelloAck(nowMs) &&
        !helloAckPending_ && !tx_.ordinaryPending() &&
        (!helloSent_ || uint32_t(nowMs - lastHelloAt_) >= kHeartbeatIntervalMs)) {
        if (encodeHello(session_.localHello(), scratch_) && queue(scratch_, true)) {
            session_.expectHelloAck(scratch_.messageId, nowMs);
            lastHelloAt_ = nowMs; helloSent_ = true;
        }
    }
    if (session_.canExchange() &&
        (!heartbeatSent_ || uint32_t(nowMs - lastHeartbeatAt_) >= kHeartbeatIntervalMs)) {
        Frame heartbeat;
        heartbeat.kind = Kind::Heartbeat;
        heartbeat.senderBoot = session_.localBoot(); heartbeat.receiverBoot = session_.peerBoot();
        heartbeat.messageId = nextId_;
        if (tx_.enqueueControl(heartbeat)) {
            ++nextId_; lastHeartbeatAt_ = nowMs; heartbeatSent_ = true;
        }
    }
    // A lost status ACK cannot hold the sole normal-message slot indefinitely.
    if (awaitingStatusId_ && uint32_t(nowMs - awaitingStatusAt_) >= kMessageTimeoutMs)
        awaitingStatusId_ = 0;
    if (role_ == Role::Motion && localStatus &&
        uint32_t(nowMs - localStatus->sampleUptimeMs) < kStatusIntervalMs &&
        session_.connected(nowMs) &&
        !helloAckPending_ && !awaitingStatusId_ && !tx_.ordinaryPending() &&
        (!statusSent_ || statusRequested_ || uint32_t(nowMs - lastStatusAt_) >= kStatusIntervalMs)) {
        if (encodeStatus(*localStatus, scratch_) && queue(scratch_)) {
            awaitingStatusId_ = scratch_.messageId;
            awaitingStatusAt_ = lastStatusAt_ = nowMs;
            statusSent_ = true; statusRequested_ = false;
        }
    }
    tx_.pump(sink);
}

} }
