#include "ReadOnlyBoardLink.h"

#include <ArduinoJson.h>
#include <cstring>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <new>

namespace babytech { namespace boardlink {
using namespace v4;

void ReadOnlyLink::reset() {
    parser_.reset(); assembler_.reset(); tx_.reset();
    session_ = Session{};
    peerStatus_ = Status{};
    queriedResult_ = QueriedResult{};
    resultHandler_ = nullptr;
    commandHandler_ = nullptr;
    stopHandler_ = nullptr;
    commandReadyHandler_ = nullptr;
    contextHandler_ = nullptr;
    contextResult_ = sentContext_ = ContextResult{};
    contextState_ = ContextSendState::Idle;
    contextId_ = contextAt_ = 0;
    receivedContextId_ = 0;
    contextReplyPending_ = false;
    commandResult_ = CommandResult{};
    commandReplyPending_ = false;
    commandId_ = commandAt_ = highestCommandId_ = stopBarrier_ = 0;
    lookupState_ = ResultLookupState::Idle;
    lookupId_ = lookupAt_ = 0;
    resultReplyPending_ = false;
    sentResult_ = CommandResult{};
    sendState_ = CommandSendState::Idle;
    sentCommandId_ = sentCommandAt_ = 0;
    commandInTransmitter_ = false;
    stopState_ = StopSendState::Idle;
    sentStopId_ = sentStopAt_ = 0;
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

bool ReadOnlyLink::setResultQueryHandler(ResultQueryHandler handler) {
    if (!configured_ || role_ != Role::Motion || resultReplyPending_) return false;
    resultHandler_ = handler;
    return true;
}

bool ReadOnlyLink::setCommandHandler(CommandHandler handler) {
    if (!configured_ || role_ != Role::Motion || commandReplyPending_) return false;
    commandHandler_ = handler;
    return true;
}

bool ReadOnlyLink::setStopHandler(StopHandler handler) {
    if (!configured_ || role_ != Role::Motion) return false;
    stopHandler_ = handler;
    return true;
}

bool ReadOnlyLink::setCommandReadyHandler(CommandReadyHandler handler) {
    if (!configured_ || role_ != Role::Motion || commandReplyPending_) return false;
    commandReadyHandler_ = handler;
    return true;
}

bool ReadOnlyLink::setContextHandler(ContextHandler handler) {
    if (!configured_ || role_ != Role::Motion || contextReplyPending_) return false;
    contextHandler_ = handler;
    return true;
}

bool ReadOnlyLink::requestContext(const ProductContext& context, uint32_t nowMs) {
    session_.poll(nowMs);
    expireCommand(nowMs); expireResultQuery(nowMs); expireStop(nowMs); expireContext(nowMs);
    if (!commandAvailable(nowMs) || !validProductContext(context) ||
        std::strcmp(context.deviceId, session_.localHello().deviceId)) return false;
    ContextResult expected;
    std::strcpy(expected.deviceId, context.deviceId);
    expected.profileVersion = context.profileVersion;
    expected.cleared = context.cleared;
    if (!contextDigest(context, expected.digest) || !encodeContextMessage(context, scratch_) ||
        !queue(scratch_)) return false;
    expected.replyTo = contextId_ = scratch_.messageId;
    sentContext_ = expected;
    contextAt_ = nowMs;
    contextState_ = ContextSendState::Pending;
    return true;
}

void ReadOnlyLink::cancelContext() {
    if (role_ != Role::Brain) return;
    if (contextState_ == ContextSendState::Pending && contextId_)
        tx_.invalidateOrdinary(Kind::Context, contextId_);
    contextState_ = contextState_ == ContextSendState::Pending ?
        ContextSendState::Cancelled : ContextSendState::Idle;
}

void ReadOnlyLink::expireContext(uint32_t nowMs) {
    if (contextState_ != ContextSendState::Pending && contextState_ != ContextSendState::Complete) return;
    if (!healthy() || !session_.connected(nowMs)) {
        if (contextState_ == ContextSendState::Pending)
            tx_.invalidateOrdinary(Kind::Context, contextId_);
        contextState_ = ContextSendState::Unavailable;
    } else if (contextState_ == ContextSendState::Pending && uint32_t(nowMs - contextAt_) >= kMessageTimeoutMs) {
        tx_.invalidateOrdinary(Kind::Context, contextId_);
        contextState_ = ContextSendState::TimedOut;
    }
}

bool ReadOnlyLink::commandAvailable(uint32_t nowMs) const {
    return role_ == Role::Brain && healthy() && session_.connected(nowMs) &&
        !helloAckPending_ && !probeRequired_ && sendState_ != CommandSendState::Pending &&
        lookupState_ != ResultLookupState::Pending && stopState_ != StopSendState::Pending &&
        contextState_ != ContextSendState::Pending &&
        !tx_.ordinaryPending();
}

bool ReadOnlyLink::requestCommand(const CommandMessage& command, uint32_t nowMs) {
    constexpr uint16_t firstFrameBudgetMs = kCommandFirstFrameBudgetMs;
    session_.poll(nowMs);
    expireResultQuery(nowMs);
    expireCommand(nowMs);
    expireStop(nowMs);
    expireContext(nowMs);
    if (role_ != Role::Brain || !healthy() || !session_.connected(nowMs) ||
        helloAckPending_ || probeRequired_ || sendState_ == CommandSendState::Pending ||
        lookupState_ == ResultLookupState::Pending || stopState_ == StopSendState::Pending ||
        contextState_ == ContextSendState::Pending ||
        !validProductRequest(command.request) ||
        std::strcmp(command.request.deviceId, session_.localHello().deviceId) ||
        command.remainingTtlMs <= firstFrameBudgetMs || command.remainingTtlMs > 5000 ||
        tx_.ordinaryPending()) return false;
    // Admission is immediate, not a queue waiting for mechanical readiness.
    // The frame guard enforces this deducted scheduling/first-frame budget.
    auto outgoing = command;
    outgoing.remainingTtlMs -= firstFrameBudgetMs;
    if (!encodeCommand(outgoing, scratch_)) return false;
    scratch_.senderBoot = session_.localBoot();
    scratch_.receiverBoot = session_.peerBoot();
    scratch_.messageId = nextId_;
    if (!tx_.enqueueTimedOrdinary(scratch_, nowMs, command.remainingTtlMs, firstFrameBudgetMs))
        return false;
    ++nextId_;
    sentResult_ = CommandResult{};
    sentResult_.source = command.request.source;
    sentResult_.sequence = command.request.sequence;
    std::strcpy(sentResult_.commandId, command.request.commandId);
    sentCommandId_ = scratch_.messageId;
    sentCommandAt_ = nowMs;
    commandInTransmitter_ = true;
    sendState_ = CommandSendState::Pending;
    return true;
}

void ReadOnlyLink::cancelCommand() {
    if (role_ != Role::Brain) return;
    if (commandInTransmitter_) tx_.invalidateOrdinary();
    commandInTransmitter_ = false;
    sendState_ = sendState_ == CommandSendState::Pending
        ? CommandSendState::Cancelled : CommandSendState::Idle;
}

void ReadOnlyLink::expireCommand(uint32_t nowMs) {
    if (sendState_ != CommandSendState::Pending) return;
    if (!healthy() || !session_.connected(nowMs)) sendState_ = CommandSendState::Unavailable;
    else if (tx_.ordinaryTimedOut() || uint32_t(nowMs - sentCommandAt_) >= kMessageTimeoutMs)
        sendState_ = CommandSendState::TimedOut;
    else return;
    if (commandInTransmitter_) tx_.invalidateOrdinary();
    commandInTransmitter_ = false;
}

bool ReadOnlyLink::requestStop(const StopRequest& request, uint32_t nowMs) {
    session_.poll(nowMs);
    expireStop(nowMs);
    if (role_ != Role::Brain || !healthy() || !session_.connected(nowMs) ||
        probeRequired_ || stopState_ == StopSendState::Pending) return false;
    auto outgoing = request;
    if (outgoing.source == Source::LocalTouch) {
        outgoing.sequence = 0;
        const int length = std::snprintf(outgoing.commandId, sizeof(outgoing.commandId),
            "stop-%016" PRIx64 "-%08" PRIx32, session_.localBoot(), nextId_);
        if (length <= 0 || size_t(length) >= sizeof(outgoing.commandId)) return false;
        outgoing.commandIdLength = uint8_t(length);
    }
    Frame frame;
    frame.kind = Kind::Stop;
    const size_t length = encodeStop(outgoing, frame.payload, sizeof(frame.payload));
    if (!length || outgoing.commandId[outgoing.commandIdLength] != 0) return false;
    frame.length = frame.total = uint16_t(length);
    frame.senderBoot = session_.localBoot(); frame.receiverBoot = session_.peerBoot();
    frame.messageId = nextId_;
    if (!tx_.enqueueControl(frame)) return false;
    ++nextId_;
    // Cancellation preserves uncertain business evidence in the caller's Store.
    cancelCommand();
    if (contextState_ == ContextSendState::Pending) cancelContext();
    sentStopId_ = frame.messageId;
    sentStopAt_ = nowMs;
    stopState_ = StopSendState::Pending;
    return true;
}

void ReadOnlyLink::expireStop(uint32_t nowMs) {
    if (stopState_ != StopSendState::Pending) return;
    if (!healthy() || !session_.connected(nowMs)) stopState_ = StopSendState::Unavailable;
    else if (uint32_t(nowMs - sentStopAt_) >= kMessageTimeoutMs) stopState_ = StopSendState::TimedOut;
}

void ReadOnlyLink::cancelResultQuery() {
    if (role_ != Role::Brain) return;
    if (lookupId_) tx_.invalidateOrdinary(Kind::ResultQuery, lookupId_);
    lookupState_ = ResultLookupState::Idle;
    lookupId_ = 0;
}

void ReadOnlyLink::expireResultQuery(uint32_t nowMs) {
    if (lookupState_ != ResultLookupState::Pending) return;
    if (!healthy() || !session_.connected(nowMs)) lookupState_ = ResultLookupState::Unavailable;
    else if (uint32_t(nowMs - lookupAt_) >= kMessageTimeoutMs)
        lookupState_ = ResultLookupState::TimedOut;
}

bool ReadOnlyLink::requestResult(const ResultQuery& query, uint32_t nowMs) {
    session_.poll(nowMs);
    expireResultQuery(nowMs);
    expireCommand(nowMs);
    expireContext(nowMs);
    if (role_ != Role::Brain || !healthy() || !session_.connected(nowMs) ||
        helloAckPending_ || probeRequired_ || lookupState_ == ResultLookupState::Pending ||
        sendState_ == CommandSendState::Pending || contextState_ == ContextSendState::Pending ||
        !validResultQuery(query) || std::strcmp(query.deviceId, session_.localHello().deviceId) ||
        !encodeResultQuery(query, scratch_) || !queue(scratch_)) return false;
    // A caller may requery resultQueryResponse().query, which aliases our state.
    const ResultQuery acceptedQuery = query;
    queriedResult_ = QueriedResult{};
    queriedResult_.query = acceptedQuery;
    lookupId_ = scratch_.messageId;
    lookupAt_ = nowMs;
    lookupState_ = ResultLookupState::Pending;
    return true;
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
        if (previousBoot != session_.peerBoot()) {
            if (contextState_ == ContextSendState::Pending) cancelContext();
            contextState_ = ContextSendState::Unavailable;
            contextReplyPending_ = false;
            receivedContextId_ = 0;
            if (lookupState_ == ResultLookupState::Pending)
                lookupState_ = ResultLookupState::Unavailable;
            resultReplyPending_ = false;
            commandReplyPending_ = false;
            commandId_ = commandAt_ = highestCommandId_ = stopBarrier_ = 0;
            if (sendState_ == CommandSendState::Pending) {
                cancelCommand();
                sendState_ = CommandSendState::Unavailable;
            }
            if (stopState_ == StopSendState::Pending) stopState_ = StopSendState::Unavailable;
        }
        if (message.kind == Kind::Hello) {
            helloAckPending_ = true;
            helloAckBoot_ = message.senderBoot;
            helloAckId_ = message.messageId;
            if (!session_.connected(nowMs) || message.senderBoot != previousBoot)
                probeRequired_ = true;
            return;
        }
        probeRequired_ = false;
        if (message.messageId > highestCommandId_) highestCommandId_ = message.messageId;
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
    expireResultQuery(nowMs);
    expireCommand(nowMs);
    expireStop(nowMs);
    expireContext(nowMs);
    if (message.kind == Kind::Context && role_ == Role::Motion) {
        const uint32_t receivedId = message.messageId;
        std::unique_ptr<ProductContext> context(new (std::nothrow) ProductContext);
        if (!context || !session_.connected(nowMs) || contextReplyPending_ || !contextHandler_ ||
            !decodeContextMessage(message, session_.localHello().deviceId, *context) ||
            !contextHandler_(*context, nowMs, contextResult_)) {
            receipt(receivedId, false);
            return;
        }
        contextResult_.replyTo = receivedId;
        if (!matchesContextResult(contextResult_, *context) || !encodeContextResult(contextResult_, scratch_)) {
            receipt(receivedId, false);
            return;
        }
        contextReplyPending_ = true;
    } else if (message.kind == Kind::ContextResult && role_ == Role::Brain) {
        ContextResult result;
        if (contextState_ == ContextSendState::Pending && decodeContextResult(message, result) &&
            result.replyTo == contextId_ && result.profileVersion == sentContext_.profileVersion &&
            result.cleared == sentContext_.cleared && !std::strcmp(result.deviceId, sentContext_.deviceId) &&
            !std::memcmp(result.digest, sentContext_.digest, sizeof(result.digest))) {
            sentContext_ = result;
            contextState_ = ContextSendState::Complete;
        }
    } else if (message.kind == Kind::Command && role_ == Role::Motion) {
        CommandMessage command;
        const uint32_t receivedId = message.messageId;
        if (!session_.connected(nowMs) || commandReplyPending_ || !commandHandler_ ||
            !decodeCommand(message, command) ||
            std::strcmp(command.request.deviceId, session_.localHello().deviceId)) {
            receipt(receivedId, false);
            return;
        }
        // First-fragment time, not completion or a replayed offset-zero frame.
        const uint32_t elapsed = uint32_t(nowMs - commandAt_);
        if (commandId_ != receivedId || elapsed >= command.remainingTtlMs)
            command.remainingTtlMs = 0;
        else command.remainingTtlMs -= elapsed;
        if (!commandHandler_(command, nowMs, commandResult_) ||
            commandResult_.source != command.request.source ||
            commandResult_.sequence != command.request.sequence ||
            std::strcmp(commandResult_.commandId, command.request.commandId) ||
            !encodeCommandResult(commandResult_, scratch_)) {
            receipt(receivedId, false);
            return;
        }
        commandReplyPending_ = true;
    } else if (message.kind == Kind::CommandResult && role_ == Role::Brain) {
        CommandResult result;
        if (sendState_ == CommandSendState::Pending && decodeCommandResult(message, result) &&
            result.source == sentResult_.source && result.sequence == sentResult_.sequence &&
            !std::strcmp(result.commandId, sentResult_.commandId)) {
            sentResult_ = result;
            sendState_ = CommandSendState::Complete;
        }
    } else if (message.kind == Kind::ResultQuery && role_ == Role::Motion) {
        ResultQuery query;
        if (!session_.connected(nowMs) || resultReplyPending_ || !resultHandler_ ||
            !decodeResultQuery(message, query) ||
            std::strcmp(query.deviceId, session_.localHello().deviceId) ||
            !resultHandler_(query, queriedResult_) ||
            !sameResultQuery(query, queriedResult_.query) ||
            !encodeQueriedResult(queriedResult_, scratch_)) {
            receipt(message.messageId, false);
            return;
        }
        // Preserve one decoded reply until the sole ordinary transmitter is free.
        resultReplyPending_ = true;
    } else if (message.kind == Kind::Result && role_ == Role::Brain) {
        QueriedResult result;
        if (lookupState_ == ResultLookupState::Pending &&
            decodeQueriedResult(message, result) &&
            sameResultQuery(queriedResult_.query, result.query)) {
            queriedResult_ = result;
            lookupState_ = ResultLookupState::Complete;
        }
    } else if (message.kind == Kind::Status && role_ == Role::Brain) {
        Status status;
        const bool valid = decodeStatus(message, status);
        const uint32_t sampleStep = status.sampleUptimeMs - peerSample_;
        if (valid && (!peerSampleSeen_ || (sampleStep && sampleStep < UINT32_C(0x80000000))) &&
            session_.status(message, nowMs)) {
            // Preserve Motion's observation. Consumers apply local ownership,
            // cache and capability gates; telemetry itself grants no action.
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
        if (message.kind == Kind::LinkReject && lookupState_ == ResultLookupState::Pending &&
            document["message_id"].as<uint32_t>() == lookupId_)
            lookupState_ = ResultLookupState::Unavailable;
        const auto acknowledged = document["message_id"].as<uint32_t>();
        if (message.kind == Kind::LinkReject && contextState_ == ContextSendState::Pending && acknowledged == contextId_) {
            cancelContext();
            contextState_ = ContextSendState::Unavailable;
        }
        if (message.kind == Kind::LinkReject && sendState_ == CommandSendState::Pending &&
            acknowledged == sentCommandId_) {
            if (commandInTransmitter_) tx_.invalidateOrdinary();
            commandInTransmitter_ = false;
            sendState_ = CommandSendState::Unavailable;
        }
        if (stopState_ == StopSendState::Pending && acknowledged == sentStopId_)
            stopState_ = message.kind == Kind::LinkAck ? StopSendState::Received : StopSendState::Rejected;
    } else {
        // Unconfigured handlers and unsupported kinds cannot mutate hardware here.
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
        frame.kind == Kind::MigrationRead || frame.kind == Kind::MigrationMaintenance ||
        frame.kind == Kind::MigrationInstall) return;
    session_.poll(nowMs);
    if (frame.kind == Kind::Heartbeat) {
        session_.heartbeat(frame, nowMs);
        return;
    }
    const bool hello = frame.kind == Kind::Hello || frame.kind == Kind::HelloAck;
    if (!hello && !session_.matches(frame.senderBoot, frame.receiverBoot)) return;
    if (hello && frame.receiverBoot != session_.localBoot() &&
        !(frame.kind == Kind::Hello && frame.receiverBoot == 0)) return;
    if (frame.kind == Kind::Stop) {
        StopRequest stop;
        if (role_ != Role::Motion || !session_.connected(nowMs) || !stopHandler_ ||
            !decodeStop(frame.payload, frame.length, stop)) {
            receipt(frame.messageId, false);
            return;
        }
        if (stop.source == Source::LocalTouch) {
            char expected[40];
            std::snprintf(expected, sizeof(expected), "stop-%016" PRIx64 "-%08" PRIx32,
                          frame.senderBoot, frame.messageId);
            if (std::strcmp(stop.commandId, expected)) {
                receipt(frame.messageId, false);
                return;
            }
        }
        // Control handling bypasses the normal reassembly slot and all Flash.
        if (frame.messageId > stopBarrier_) stopBarrier_ = frame.messageId;
        if (commandId_ && commandId_ <= stopBarrier_)
            assembler_.cancelMessage(Kind::Command, frame.senderBoot, frame.receiverBoot, commandId_);
        if (receivedContextId_ && receivedContextId_ <= stopBarrier_) {
            assembler_.cancelMessage(Kind::Context, frame.senderBoot, frame.receiverBoot, receivedContextId_);
            receivedContextId_ = 0;
        }
        if (contextReplyPending_ && contextResult_.replyTo <= stopBarrier_)
            contextReplyPending_ = false;
        receipt(frame.messageId, stopHandler_(stop, nowMs));
        return;
    }
    if (frame.kind == Kind::Command && role_ == Role::Motion && frame.messageId <= stopBarrier_) {
        receipt(frame.messageId, false);
        return;
    }
    assembler_.expire(nowMs);
    const bool starting = frame.kind == Kind::Command && !frame.offset && !assembler_.active();
    const bool startingContext = frame.kind == Kind::Context && !frame.offset && !assembler_.active();
    const auto result = assembler_.accept(frame, nowMs, scratch_);
    if (startingContext && (result == AssemblyResult::Incomplete || result == AssemblyResult::Complete))
        receivedContextId_ = frame.messageId;
    if (starting && (result == AssemblyResult::Incomplete || result == AssemblyResult::Complete)) {
        if (frame.messageId > highestCommandId_) {
            highestCommandId_ = commandId_ = frame.messageId;
            commandAt_ = nowMs;
        } else commandId_ = 0; // Replay can query a cached decision, never regain TTL.
    }
    if (result == AssemblyResult::Complete) handle(scratch_, nowMs);
}

bool ReadOnlyLink::queueInstallMessage(const Message& message) {
    if ((configured_ && !healthy()) || message.kind != Kind::MigrationInstall) return false;
    return tx_.enqueue(message);
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
    if (!configured_) { tx_.pump(nowMs, sink); return; }
    if (!healthy()) { expireResultQuery(nowMs); expireCommand(nowMs); expireStop(nowMs); expireContext(nowMs); return; }
    session_.poll(nowMs);
    assembler_.expire(nowMs);
    expireResultQuery(nowMs);
    expireCommand(nowMs);
    expireStop(nowMs);
    expireContext(nowMs);
    if (!session_.canExchange()) {
        peerStatus_ = Status{};
        awaitingStatusId_ = 0;
        resultReplyPending_ = false;
        commandReplyPending_ = false;
        contextReplyPending_ = false;
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
    if (role_ == Role::Motion && resultReplyPending_ && session_.connected(nowMs) &&
        !helloAckPending_ && !tx_.ordinaryPending()) {
        if (encodeQueriedResult(queriedResult_, scratch_) && queue(scratch_))
            resultReplyPending_ = false;
    }
    if (role_ == Role::Motion && commandReplyPending_ && session_.connected(nowMs) &&
        !helloAckPending_ && !tx_.ordinaryPending()) {
        if ((!commandReadyHandler_ || commandReadyHandler_(commandResult_)) &&
            encodeCommandResult(commandResult_, scratch_) && queue(scratch_))
            commandReplyPending_ = false;
    }
    if (role_ == Role::Motion && contextReplyPending_ && session_.connected(nowMs) &&
        !helloAckPending_ && !tx_.ordinaryPending()) {
        if (encodeContextResult(contextResult_, scratch_) && queue(scratch_)) contextReplyPending_ = false;
    }
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
    tx_.pump(nowMs, sink);
    if (commandInTransmitter_ && !tx_.ordinaryPending()) commandInTransmitter_ = false;
    expireCommand(nowMs);
}

} }
