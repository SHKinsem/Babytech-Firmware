#include "ReadOnlyBoardLink.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace babytech::boardlink;
using namespace babytech::v4;

namespace {
Pairing pairing(Role role) {
    Pairing result;
    result.role = role;
    std::strcpy(result.deviceId, "bt-test-device");
    std::strcpy(result.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(result.localPhysicalId, role == Role::Brain ? "112233445566" : "aabbccddeeff");
    std::strcpy(result.peerPhysicalId, role == Role::Brain ? "aabbccddeeff" : "112233445566");
    return result;
}

class Wire : public ByteSink {
public:
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> history;
    size_t capacity = 64, writeLimit = 64;
    bool zeroWrite = false;
    bool idle() const override { return bytes.empty(); }
    size_t available() const override { return capacity - bytes.size(); }
    size_t write(const uint8_t* data, size_t size) override {
        assert(size <= available());
        if (zeroWrite) return 0;
        if (size > writeLimit) size = writeLimit;
        bytes.insert(bytes.end(), data, data + size);
        history.insert(history.end(), data, data + size);
        return size;
    }
    void deliver(ReadOnlyLink& peer, uint32_t now, bool drop) {
        for (uint8_t byte : bytes) if (!drop) peer.receive(byte, now);
        bytes.clear();
    }
};

struct Fixture {
    ReadOnlyLink brain, motion;
    Wire toMotion, toBrain;
    Status status;
    uint32_t now = 0;
    Fixture() {
        assert(brain.begin(pairing(Role::Brain), 11));
        assert(motion.begin(pairing(Role::Motion), 22));
        status.snapshot.stage = babytech::display::DisplayStage::Ready;
        status.snapshot.startEnabled = true;
        status.stationary = true;
        std::strcpy(status.productProgress, "ready");
        status.lowWaterValid = status.powderValid = true;
        status.powderGrams = 275;
        status.actuatorOperational = status.actuatorConfigValid = true;
        status.actuatorBusHealthy = status.actuatorPositionReferenced = true;
        status.executionAuthorized = status.feedingContextConfigured = true;
        std::memset(status.babyId, 'b', sizeof(status.babyId) - 1);
    }
    void step(bool publish = true, bool dropMotion = false, bool dropBrain = false,
              bool sample = true) {
        if (sample) status.sampleUptimeMs = now;
        brain.poll(now, toMotion);
        motion.poll(now, toBrain, publish ? &status : nullptr);
        toMotion.deliver(motion, now, dropMotion);
        toBrain.deliver(brain, now, dropBrain);
        now += 5;
    }
    void run(unsigned milliseconds, bool publish = true, bool dropMotion = false,
             bool dropBrain = false) {
        for (unsigned n = 0; n < milliseconds; n += 5) step(publish, dropMotion, dropBrain);
    }
};

void deliver(const Message& message, ReadOnlyLink& peer, uint32_t now) {
    for (size_t offset = 0; offset < message.length;) {
        Frame frame;
        assert(fragment(message, offset, frame));
        uint8_t bytes[kMaxFrame];
        const size_t size = encode(frame, bytes, sizeof(bytes));
        assert(size);
        for (size_t i = 0; i < size; ++i) peer.receive(bytes[i], now);
        offset += frame.length;
    }
}

Message lastStatus(const std::vector<uint8_t>& bytes) {
    Parser parser;
    Assembler assembler;
    Frame frame;
    Message message, latest;
    for (uint8_t byte : bytes) {
        if (parser.push(byte, 0, frame) &&
            assembler.accept(frame, 0, message) == AssemblyResult::Complete &&
            message.kind == Kind::Status) latest = message;
    }
    assert(latest.kind == Kind::Status);
    return latest;
}

unsigned statusCount(const std::vector<uint8_t>& bytes) {
    Parser parser;
    Assembler assembler;
    Frame frame;
    Message message;
    unsigned count = 0;
    for (uint8_t byte : bytes) {
        if (parser.push(byte, 0, frame) &&
            assembler.accept(frame, 0, message) == AssemblyResult::Complete &&
            message.kind == Kind::Status) ++count;
    }
    return count;
}

void deliverSample(Fixture& fixture, uint32_t sample, uint32_t messageId) {
    Status status = fixture.status;
    status.sampleUptimeMs = sample;
    Message message;
    assert(encodeStatus(status, message));
    message.senderBoot = 22; message.receiverBoot = 11; message.messageId = messageId;
    deliver(message, fixture.brain, fixture.now);
}

void handshakeAndFreshness() {
    Fixture f;
    assert(!f.brain.connected(0) && !f.brain.freshStatus(0));
    f.run(3000);
    assert(f.brain.connected(f.now) && f.motion.connected(f.now));
    assert(f.brain.freshStatus(f.now));
    assert(f.brain.peerStatus().snapshot.stage == babytech::display::DisplayStage::Ready);
    assert(f.brain.peerStatus().snapshot.startEnabled);
    const auto& observed = f.brain.peerStatus();
    assert(std::strcmp(observed.productProgress, "ready") == 0);
    assert(std::strcmp(observed.productError, "NONE") == 0);
    assert(!observed.isPreparing && observed.lowWaterValid && !observed.lowWater);
    assert(observed.powderValid && observed.powderGrams == 275);
    assert(observed.actuatorOperational && observed.actuatorConfigValid);
    assert(observed.actuatorBusHealthy && observed.actuatorPositionReferenced);
    // Telemetry is preserved even though the read-only link disables UI Start.
    assert(observed.executionAuthorized && observed.feedingContextConfigured);
    assert(std::strlen(observed.babyId) == 96 && std::strcmp(observed.babyId, f.status.babyId) == 0);
    assert(f.brain.takeFailure() == LinkFailure::None);
    assert(f.motion.takeFailure() == LinkFailure::None);

    const Message captured = lastStatus(f.toBrain.history);
    f.run(2000, false);
    assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
    deliver(captured, f.brain, f.now);
    assert(!f.brain.freshStatus(f.now));
    f.run(1000);
    assert(f.brain.freshStatus(f.now));
}

void disconnectAndRestart() {
    Fixture f;
    f.run(3000);
    f.run(2200, true, true, true);
    assert(!f.brain.connected(f.now) && !f.motion.connected(f.now));
    assert(!f.brain.freshStatus(f.now));
    assert(f.brain.takeFailure() == LinkFailure::HeartbeatExpired);
    assert(f.motion.takeFailure() == LinkFailure::HeartbeatExpired);
    assert(f.brain.takeFailure() == LinkFailure::None);
    f.run(3000);
    assert(f.brain.connected(f.now) && f.motion.connected(f.now));
    assert(f.brain.freshStatus(f.now));
    assert(f.brain.begin(pairing(Role::Brain), 33));
    f.toMotion.bytes.clear();
    f.run(3000);
    assert(f.motion.takeFailure() == LinkFailure::PeerRestarted);
    assert(f.brain.connected(f.now) && f.brain.freshStatus(f.now));
}

void wrongBoardAndUnsupported() {
    Fixture f;
    Pairing wrong = pairing(Role::Motion);
    std::strcpy(wrong.deviceId, "bt-other");
    assert(f.motion.begin(wrong, 22));
    f.run(3000);
    assert(!f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
    assert(!f.motion.connected(f.now));
    assert(f.motion.begin(pairing(Role::Motion), 44));
    f.run(3000);
    assert(f.brain.freshStatus(f.now));

    Message command;
    command.kind = Kind::Command;
    command.senderBoot = 11; command.receiverBoot = 44; command.messageId = 500;
    const char payload[] = "{\"command\":\"prepare\"}";
    command.length = sizeof(payload) - 1;
    std::memcpy(command.payload, payload, command.length);
    f.toBrain.history.clear();
    deliver(command, f.motion, f.now);
    f.run(100);
    Parser parser;
    Frame frame;
    bool rejected = false;
    for (uint8_t byte : f.toBrain.history)
        if (parser.push(byte, 0, frame) && frame.kind == Kind::LinkReject) rejected = true;
    assert(rejected);
    assert(f.brain.peerStatus().snapshot.startEnabled);

    ReadOnlyLink unconfigured;
    Wire sink;
    assert(!unconfigured.begin(Pairing{}, 1));
    unconfigured.poll(0, sink);
    assert(sink.history.empty() && !unconfigured.healthy());
}

void capturedBootCannotRestoreStatus() {
    Fixture f;
    f.run(3000);
    const auto oldWire = f.toBrain.history;
    assert(f.motion.begin(pairing(Role::Motion), 44));
    f.toBrain.bytes.clear();
    f.run(3000);
    assert(f.brain.freshStatus(f.now));
    assert(f.brain.takeFailure() == LinkFailure::PeerRestarted);
    f.run(2200, false, true, true);
    assert(!f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
    for (uint8_t byte : oldWire) f.brain.receive(byte, f.now);
    assert(!f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
    f.run(3000);
    assert(f.brain.connected(f.now) && f.brain.freshStatus(f.now));
    f.run(2000, false);
    assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
    for (uint8_t byte : oldWire) f.brain.receive(byte, f.now);
    assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
}

void oneWayLoss() {
    for (bool towardMotion : {false, true}) {
        Fixture f;
        f.run(3000);
        f.run(2500, true, towardMotion, !towardMotion);
        if (towardMotion) assert(!f.motion.connected(f.now));
        else assert(!f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
        f.run(3500);
        assert(f.brain.connected(f.now) && f.motion.connected(f.now));
        assert(f.brain.freshStatus(f.now));
    }
}

void frozenSamplesCannotStayReady() {
    Fixture f;
    f.run(3000);
    assert(f.brain.freshStatus(f.now));
    const Status frozen = f.status;
    for (unsigned n = 0; n < 3000; n += 5) f.step(true, false, false, false);
    assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));

    // Even a peer re-encoding the same sample with a newer transport ID must
    // not refresh it. Do not compare absolute uptimes of different boards.
    Message replay;
    assert(encodeStatus(frozen, replay));
    replay.senderBoot = 22; replay.receiverBoot = 11; replay.messageId = 100000;
    deliver(replay, f.brain, f.now);
    assert(!f.brain.freshStatus(f.now));
    f.run(1000);
    assert(f.brain.freshStatus(f.now));
}

void sampleAgeSendBoundary() {
    for (uint32_t age : {UINT32_C(0), UINT32_C(499), UINT32_C(500),
                         UINT32_C(1500), UINT32_MAX}) {
        Fixture f;
        f.run(3000);
        f.run(2000, false);
        assert(f.brain.connected(f.now) && f.motion.connected(f.now));
        assert(!f.brain.freshStatus(f.now));
        f.toBrain.history.clear();
        // UINT32_MAX age represents a sample one millisecond in the future.
        const uint32_t sample = f.now - age;
        f.status.sampleUptimeMs = sample;
        f.step(true, false, false, false);
        f.run(200, false);
        assert(f.brain.connected(f.now) && f.motion.connected(f.now));
        assert(statusCount(f.toBrain.history) == (age < 500 ? 1u : 0u));
        assert(f.brain.freshStatus(f.now) == (age < 500));
        if (age < 500) assert(f.brain.peerStatus().sampleUptimeMs == sample);
    }
}

void sampleFreshnessExpiryBoundary() {
    Fixture f;
    f.run(3000);
    f.run(2000, false);
    assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
    const uint32_t acceptedAt = f.now;
    deliverSample(f, f.now, 1000);
    assert(f.brain.freshStatus(f.now));
    // Keep heartbeats flowing without any new status during the entire window.
    f.run(1495, false);
    assert(f.brain.connected(acceptedAt + 1499));
    assert(f.brain.freshStatus(acceptedAt + 1499));
    f.run(5, false);
    assert(f.now == acceptedAt + 1500);
    assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
}

void sampleOrderingAndReconnectWatermark() {
    for (bool reconnect : {false, true}) {
        Fixture f;
        f.run(3000);
        const uint32_t previousSample = f.brain.peerStatus().sampleUptimeMs;
        if (reconnect) {
            f.run(2200, false, true, true);
            assert(!f.brain.connected(f.now) && !f.motion.connected(f.now));
            assert(!f.brain.freshStatus(f.now));
            // Reconnect the existing boots without supplying a replacement sample.
            f.run(3500, false);
        } else {
            f.run(2000, false);
        }
        assert(f.brain.connected(f.now) && f.motion.connected(f.now));
        assert(!f.brain.freshStatus(f.now));
        uint32_t messageId = 100000;
        for (uint32_t delta : {UINT32_C(0), UINT32_MAX, UINT32_C(0x80000000),
                               UINT32_C(0x80000001)}) {
            deliverSample(f, previousSample + delta, messageId++);
            assert(f.brain.connected(f.now) && !f.brain.freshStatus(f.now));
        }
        // Rejected samples must not advance either the sample or transport watermark.
        deliverSample(f, previousSample + 1, 1000);
        assert(f.brain.freshStatus(f.now));
        assert(f.brain.peerStatus().sampleUptimeMs == previousSample + 1);
    }
}

void sampleClockWraparound() {
    Fixture f;
    f.now = UINT32_MAX - 2000;
    f.run(5000);
    assert(f.brain.connected(f.now) && f.motion.connected(f.now));
    assert(f.brain.freshStatus(f.now));
    assert(f.brain.peerStatus().sampleUptimeMs < 4000);
}

unsigned queryCalls = 0;
bool answerQuery(const ResultQuery& query, QueriedResult& result) {
    ++queryCalls;
    result = QueriedResult{};
    result.query = query;
    result.status = ResultQueryStatus::Known;
    result.accepted = true;
    std::strcpy(result.reason, "accepted");
    std::memset(result.requestDigestHex, 'a', 64);
    return true;
}

ResultQuery query(uint64_t sequence = 1) {
    ResultQuery result;
    result.sequence = sequence;
    std::strcpy(result.deviceId, "bt-test-device");
    std::strcpy(result.commandId, "original-request");
    return result;
}

Message answer(const ResultQuery& query, uint64_t sender = 22, uint64_t receiver = 11) {
    QueriedResult result;
    answerQuery(query, result);
    Message message;
    assert(encodeQueriedResult(result, message));
    message.senderBoot = sender; message.receiverBoot = receiver; message.messageId = 90000;
    return message;
}

void resultQueryRuntime() {
    Fixture f;
    assert(!f.brain.setResultQueryHandler(answerQuery));
    assert(f.motion.setResultQueryHandler(answerQuery));
    assert(!f.brain.requestResult(query(), f.now));
    f.run(3000);
    // Heartbeats keep the session alive even without fresh display samples.
    f.run(2000, false);
    assert(!f.brain.freshStatus(f.now));
    queryCalls = 0;
    assert(f.brain.requestResult(query(), f.now));
    assert(!f.brain.requestResult(query(2), f.now));
    f.run(500, false);
    assert(queryCalls == 1);
    assert(f.brain.resultLookupState() == ResultLookupState::Complete);
    assert(sameResultQuery(f.brain.resultQueryResponse().query, query()));
    assert(f.brain.resultQueryResponse().accepted);
    assert(f.brain.peerStatus().snapshot.startEnabled);
    assert(f.brain.requestResult(f.brain.resultQueryResponse().query, f.now));
    assert(sameResultQuery(f.brain.resultQueryResponse().query, query()));
    f.run(500, false);
    assert(f.brain.resultLookupState() == ResultLookupState::Complete);
    assert(queryCalls == 2);
    f.brain.cancelResultQuery();
    assert(f.brain.resultLookupState() == ResultLookupState::Idle);

    ResultQuery foreign = query();
    std::strcpy(foreign.deviceId, "bt-other");
    assert(!f.brain.requestResult(foreign, f.now));
    assert(!f.motion.requestResult(query(), f.now));
    f.motion.setResultQueryHandler(nullptr);
    assert(f.brain.requestResult(query(), f.now));
    f.run(500, false);
    assert(f.brain.resultLookupState() == ResultLookupState::Unavailable);
}

void resultQueryResponseMatching() {
    Fixture f;
    f.run(3000);
    assert(f.brain.requestResult(query(), f.now));
    for (const auto& message : {answer(query(2)), answer(query(), 33), answer(query(), 22, 99)}) {
        deliver(message, f.brain, f.now);
        assert(f.brain.resultLookupState() == ResultLookupState::Pending);
    }
    // A UART receipt is not the original request's business result.
    Message receipt;
    receipt.kind = Kind::LinkAck;
    receipt.senderBoot = 22; receipt.receiverBoot = 11; receipt.messageId = 90001;
    const char payload[] = "{\"message_id\":1}";
    receipt.length = sizeof(payload) - 1;
    std::memcpy(receipt.payload, payload, receipt.length);
    deliver(receipt, f.brain, f.now);
    assert(f.brain.resultLookupState() == ResultLookupState::Pending);
    deliver(answer(query()), f.brain, f.now);
    assert(f.brain.resultLookupState() == ResultLookupState::Complete);
    // Unsolicited duplicate/wrong responses cannot replace resolved evidence.
    deliver(answer(query(2)), f.brain, f.now);
    assert(sameResultQuery(f.brain.resultQueryResponse().query, query()));
}

void resultQueryTimeoutAndRestart() {
    for (bool wrap : {false, true}) {
        Fixture f;
        if (wrap) f.now = UINT32_MAX - 3500;
        f.run(3000);
        assert(f.brain.requestResult(query(), f.now));
        f.run(1005, false, true, true);
        assert(f.brain.resultLookupState() == ResultLookupState::TimedOut);
        deliver(answer(query()), f.brain, f.now);
        assert(f.brain.resultLookupState() == ResultLookupState::TimedOut);
    }
    Fixture f;
    f.run(3000);
    assert(f.brain.requestResult(query(), f.now));
    assert(f.motion.begin(pairing(Role::Motion), 44));
    f.run(3000, false);
    assert(f.brain.resultLookupState() == ResultLookupState::Unavailable);
    assert(f.brain.takeFailure() == LinkFailure::PeerRestarted);
    f.motion.setResultQueryHandler(answerQuery);
    assert(f.brain.requestResult(query(), f.now));
    deliver(answer(query()), f.brain, f.now);
    assert(f.brain.resultLookupState() == ResultLookupState::Pending);
    f.run(500, false);
    assert(f.brain.resultLookupState() == ResultLookupState::Complete);
}

void resultReplyBackpressure() {
    Fixture f;
    f.run(3000);
    f.motion.setResultQueryHandler(answerQuery);
    queryCalls = 0;
    // Hold the ordinary transmitter with a fragmented STATUS. The result is
    // retained once, not dropped or replaced by the next query.
    f.status.sampleUptimeMs = f.now;
    f.motion.poll(f.now, f.toBrain, &f.status);
    Message request;
    assert(encodeResultQuery(query(), request));
    request.senderBoot = 11; request.receiverBoot = 22; request.messageId = 90002;
    deliver(request, f.motion, f.now);
    assert(queryCalls == 1);
    assert(encodeResultQuery(query(2), request));
    request.senderBoot = 11; request.receiverBoot = 22; request.messageId = 90003;
    deliver(request, f.motion, f.now);
    assert(queryCalls == 1);
    f.toBrain.deliver(f.brain, f.now, false);
    f.run(500);
    Parser parser;
    Assembler assembler;
    Frame frame;
    Message message;
    unsigned responses = 0;
    for (uint8_t byte : f.toBrain.history) {
        if (parser.push(byte, 0, frame) && assembler.accept(frame, 0, message) == AssemblyResult::Complete &&
            message.kind == Kind::Result) {
            QueriedResult result;
            assert(decodeQueriedResult(message, result));
            assert(sameResultQuery(result.query, query()));
            ++responses;
        }
    }
    assert(responses == 1);
}

void resultQueryCancellationAndTransportFault() {
    Fixture f;
    f.run(3000);
    assert(f.brain.requestResult(query(), f.now));
    f.brain.cancelResultQuery();
    deliver(answer(query()), f.brain, f.now);
    assert(f.brain.resultLookupState() == ResultLookupState::Idle);
    // Drain the cancelled read-only frame; cancellation never turns it into an action.
    f.run(200);
    assert(f.brain.requestResult(query(), f.now));
    class BrokenSink : public ByteSink {
    public:
        bool idle() const override { return true; }
        size_t available() const override { return 64; }
        size_t write(const uint8_t*, size_t size) override { return size + 1; }
    } broken;
    f.brain.poll(f.now, broken);
    assert(!f.brain.healthy());
    f.brain.poll(f.now + 1, broken);
    assert(f.brain.resultLookupState() == ResultLookupState::Unavailable);
}

unsigned commandCalls = 0, stopCalls = 0;
uint16_t observedTtl = 0;
bool finalCommandReady = true;
bool commandReady(CommandResult&) { return finalCommandReady; }
bool handleCommand(const CommandMessage& command, uint32_t, CommandResult& result) {
    ++commandCalls;
    observedTtl = command.remainingTtlMs;
    result = CommandResult{};
    result.source = command.request.source;
    result.sequence = command.request.sequence;
    std::strcpy(result.commandId, command.request.commandId);
    result.accepted = command.remainingTtlMs != 0;
    std::strcpy(result.reason, result.accepted ? "accepted" : "request_expired");
    return true;
}
StopRequest observedStop;
bool stopAccepted = true;
bool handleStop(const StopRequest& request, uint32_t) {
    ++stopCalls;
    observedStop = request;
    return stopAccepted;
}

Message commandMessage(uint32_t id, uint16_t ttl = 500) {
    CommandMessage command;
    command.request.command = ProductCommand::Prepare;
    command.request.sequence = id;
    std::strcpy(command.request.deviceId, "bt-test-device");
    assert(makeLocalCommandId(pairing(Role::Brain), id, command.request.commandId));
    std::strcpy(command.request.babyId, "baby-id");
    command.request.profileVersion = 1;
    command.request.waterMl = 180;
    command.request.temperatureC = 45;
    command.request.powderGPer100Ml = 13;
    command.remainingTtlMs = ttl;
    Message message;
    assert(encodeCommand(command, message));
    message.senderBoot = 11; message.receiverBoot = 22; message.messageId = id;
    assert(message.length > kMaxFragment);
    return message;
}

void restOfCommand(const Message& message, Fixture& f, size_t offset = kMaxFragment) {
    while (offset < message.length) {
        Frame frame;
        assert(fragment(message, offset, frame));
        f.motion.receiveFrame(frame, f.now);
        offset += frame.length;
    }
}

void commandAndStopRuntime() {
    Fixture f;
    f.run(3000);
    assert(!f.brain.setCommandHandler(handleCommand));
    assert(!f.brain.setStopHandler(handleStop));
    assert(f.motion.setCommandHandler(handleCommand));
    assert(f.motion.setStopHandler(handleStop));
    commandCalls = stopCalls = 0;
    auto message = commandMessage(10000);
    Frame first;
    assert(fragment(message, 0, first));
    f.motion.receiveFrame(first, f.now);
    f.now += 100;
    f.motion.receiveFrame(first, f.now); // Duplicate fragment cannot renew TTL.
    f.now += 100;
    restOfCommand(message, f);
    assert(commandCalls == 1 && observedTtl == 300);
    // A pending reply is not overwritten by a second accepted command.
    deliver(commandMessage(10001), f.motion, f.now);
    assert(commandCalls == 1);
    f.toBrain.history.clear();
    f.run(200, false);
    Parser parser;
    Assembler assembler;
    Frame frame;
    Message decoded;
    CommandResult result;
    bool received = false;
    for (uint8_t byte : f.toBrain.history)
        if (parser.push(byte, f.now, frame) &&
            assembler.accept(frame, f.now, decoded) == AssemblyResult::Complete &&
            decodeCommandResult(decoded, result)) received = true;
    assert(received && result.accepted && result.sequence == 10000);
    deliver(message, f.motion, f.now);
    assert(commandCalls == 2 && observedTtl == 0); // Lost/replayed transmission gains no time.
    f.run(200, false);

    message = commandMessage(11000, 100);
    assert(fragment(message, 0, first));
    f.motion.receiveFrame(first, f.now);
    f.now += 100;
    restOfCommand(message, f);
    assert(commandCalls == 3 && observedTtl == 0);
    f.run(200, false);

    message = commandMessage(12000);
    assert(fragment(message, 0, first));
    f.motion.receiveFrame(first, f.now);
    StopRequest stop;
    std::strcpy(stop.commandId, "stop-000000000000000b-000030d5"); // transport ID 12501
    stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
    Frame urgent;
    urgent.kind = Kind::Stop;
    urgent.senderBoot = 11; urgent.receiverBoot = 22; urgent.messageId = 12501;
    urgent.total = urgent.length = uint16_t(encodeStop(stop, urgent.payload, sizeof(urgent.payload)));
    assert(urgent.length);
    f.motion.receiveFrame(urgent, f.now);
    assert(stopCalls == 1); // Immediate, no ordinary-slot or ACK drain prerequisite.
    restOfCommand(message, f);
    deliver(message, f.motion, f.now);
    assert(commandCalls == 3);
    urgent.receiverBoot = 33;
    f.motion.receiveFrame(urgent, f.now);
    assert(stopCalls == 1);
    urgent.receiverBoot = 22;
    ++urgent.messageId; // Doesn't match canonical local Stop ID.
    f.motion.receiveFrame(urgent, f.now);
    assert(stopCalls == 1);
    f.run(200, false);
    deliver(commandMessage(13000), f.motion, f.now);
    assert(commandCalls == 4 && observedTtl == 500);
    f.run(200, false);
    assert(f.brain.peerStatus().snapshot.startEnabled); // Observation, not action authorization.
}

void commandDeferralAndFirstFragmentExpiry() {
    Fixture f;
    f.run(3000, false);
    assert(f.motion.setCommandHandler(handleCommand));
    assert(f.motion.setCommandReadyHandler(commandReady));
    assert(f.motion.setStopHandler(handleStop));
    commandCalls = stopCalls = 0;
    auto command = commandMessage(10000);
    Frame first;
    assert(fragment(command, 0, first));
    f.motion.receiveFrame(first, f.now);
    f.run(1100, false); // Assembly expires while heartbeats still work.
    restOfCommand(command, f);
    assert(commandCalls == 0);
    deliver(command, f.motion, f.now);
    assert(commandCalls == 1 && observedTtl == 0);
    f.run(200, false);

    finalCommandReady = false;
    f.toBrain.history.clear();
    deliver(commandMessage(11000), f.motion, f.now);
    f.run(500, false);
    Parser parser;
    Frame frame;
    for (uint8_t byte : f.toBrain.history)
        if (parser.push(byte, f.now, frame)) assert(frame.kind != Kind::CommandResult);
    assert(f.brain.connected(f.now) && f.motion.connected(f.now));
    StopRequest stop;
    std::strcpy(stop.commandId, "stop-000000000000000b-00002ee0"); // ID 12000
    stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
    Frame urgent;
    urgent.kind = Kind::Stop;
    urgent.senderBoot = 11; urgent.receiverBoot = 22; urgent.messageId = 12000;
    urgent.total = urgent.length = uint16_t(encodeStop(stop, urgent.payload, sizeof(urgent.payload)));
    f.motion.receiveFrame(urgent, f.now);
    assert(stopCalls == 1); // Flash-delayed rejection doesn't block safety controls.
    finalCommandReady = true;
    f.run(200, false);
    Assembler assembler;
    Message message;
    CommandResult result;
    bool found = false;
    parser.reset();
    for (uint8_t byte : f.toBrain.history)
        if (parser.push(byte, f.now, frame) && assembler.accept(frame, f.now, message) == AssemblyResult::Complete &&
            decodeCommandResult(message, result)) found = true;
    assert(found && result.sequence == 11000);

    Fixture wrap;
    wrap.now = UINT32_MAX - 4000;
    wrap.run(3000, false);
    wrap.motion.setCommandHandler(handleCommand);
    wrap.now = UINT32_MAX - 50;
    command = commandMessage(10000, 300);
    assert(fragment(command, 0, first));
    wrap.motion.receiveFrame(first, wrap.now);
    wrap.now += 100;
    restOfCommand(command, wrap);
    assert(observedTtl == 200);
}

unsigned senderScenarios = 0;
std::vector<Message> messages(const std::vector<uint8_t>& bytes, Kind kind) {
    Parser parser;
    Assembler assembler;
    Frame frame;
    Message message;
    std::vector<Message> result;
    for (uint8_t byte : bytes)
        if (parser.push(byte, 0, frame) &&
            assembler.accept(frame, 0, message) == AssemblyResult::Complete && message.kind == kind)
            result.push_back(message);
    return result;
}
CommandMessage senderCommand(uint32_t id = 10000, uint16_t ttl = 500,
                             Source source = Source::LocalTouch) {
    CommandMessage command;
    assert(decodeCommand(commandMessage(id, ttl), command));
    command.request.source = source;
    if (source == Source::CloudCommand) std::strcpy(command.request.commandId, "cloud-original");
    return command;
}
ResultQuery senderQuery(const CommandMessage& command) {
    ResultQuery result;
    result.source = command.request.source;
    result.sequence = command.request.sequence;
    std::strcpy(result.deviceId, command.request.deviceId);
    std::strcpy(result.commandId, command.request.commandId);
    return result;
}
Message senderReply(const CommandMessage& command) {
    CommandResult result;
    result.source = command.request.source;
    result.sequence = command.request.sequence;
    std::strcpy(result.commandId, command.request.commandId);
    result.accepted = true;
    std::strcpy(result.reason, "accepted");
    Message message;
    assert(encodeCommandResult(result, message));
    message.senderBoot = 22; message.receiverBoot = 11; message.messageId = 90000;
    return message;
}
Message senderReceipt(uint32_t id, Kind kind = Kind::LinkAck) {
    Message message;
    message.kind = kind;
    message.senderBoot = 22; message.receiverBoot = 11; message.messageId = 90001;
    message.length = uint16_t(std::snprintf(reinterpret_cast<char*>(message.payload),
                                           sizeof(message.payload), "{\"message_id\":%u}", id));
    return message;
}
void senderSetup(Fixture& f) {
    ++senderScenarios;
    commandCalls = stopCalls = queryCalls = 0;
    finalCommandReady = stopAccepted = true;
    assert(f.motion.setCommandHandler(handleCommand));
    assert(f.motion.setCommandReadyHandler(commandReady));
    assert(f.motion.setStopHandler(handleStop));
    assert(f.motion.setResultQueryHandler(answerQuery));
    f.run(3000, false);
    assert(f.brain.connected(f.now) && f.motion.connected(f.now));
    assert(!f.brain.freshStatus(f.now));
    f.toMotion.history.clear(); f.toBrain.history.clear();
}
void senderExactExchangeAndQuery() {
    for (Source source : {Source::LocalTouch, Source::CloudCommand}) {
        Fixture f;
        senderSetup(f);
        const auto command = senderCommand(10000, 500, source);
        assert(f.brain.commandSendState() == CommandSendState::Idle);
        assert(f.brain.requestCommand(command, f.now));
        assert(f.brain.commandSendState() == CommandSendState::Pending);
        assert(!f.brain.requestCommand(senderCommand(10001), f.now));
        assert(!f.brain.requestResult(senderQuery(command), f.now));
        f.run(300, false);
        assert(commandCalls == 1 && observedTtl > 0 && observedTtl <= 450);
        assert(f.brain.commandSendState() == CommandSendState::Complete);
        const auto& result = f.brain.commandResponse();
        assert(result.accepted && !std::strcmp(result.reason, "accepted"));
        assert(result.source == source && result.sequence == command.request.sequence);
        assert(!std::strcmp(result.commandId, command.request.commandId));
        const auto sent = messages(f.toMotion.history, Kind::Command);
        assert(sent.size() == 1 && sent[0].senderBoot == 11 && sent[0].receiverBoot == 22);
        CommandMessage encoded;
        assert(decodeCommand(sent[0], encoded));
        assert(sameProductRequest(encoded.request, command.request));
        assert(encoded.remainingTtlMs == command.remainingTtlMs - 50);
        assert(f.brain.requestResult(senderQuery(command), f.now));
        assert(!f.brain.requestCommand(senderCommand(10001), f.now));
        f.run(1500, false);
        assert(queryCalls == 1 && commandCalls == 1);
        assert(f.brain.resultLookupState() == ResultLookupState::Complete);
        assert(sameResultQuery(f.brain.resultQueryResponse().query, senderQuery(command)));
        assert(messages(f.toMotion.history, Kind::Command).size() == 1);
    }
}
void senderResponseIdentity() {
    for (unsigned mismatch = 0; mismatch < 7; ++mismatch) {
        Fixture f;
        senderSetup(f);
        const auto command = senderCommand();
        assert(f.brain.requestCommand(command, f.now));
        f.run(150, false, false, true);
        assert(commandCalls == 1 && f.brain.commandSendState() == CommandSendState::Pending);
        auto other = command;
        if (mismatch == 0) other.request.source = Source::CloudCommand;
        if (mismatch == 1) ++other.request.sequence;
        if (mismatch == 2) std::strcpy(other.request.commandId, "wrong-id");
        Message reply = senderReply(other);
        if (mismatch == 3) reply.senderBoot = 33;
        if (mismatch == 4) reply.receiverBoot = 99;
        if (mismatch == 5) {
            const auto sent = messages(f.toMotion.history, Kind::Command);
            assert(sent.size() == 1);
            reply = senderReceipt(sent[0].messageId);
        }
        if (mismatch == 6) {
            QueriedResult queried;
            answerQuery(senderQuery(command), queried);
            assert(encodeQueriedResult(queried, reply));
            reply.senderBoot = 22; reply.receiverBoot = 11; reply.messageId = 90000;
        }
        deliver(reply, f.brain, f.now);
        assert(f.brain.commandSendState() == CommandSendState::Pending);
        assert(!f.brain.commandResponse().accepted);
        deliver(senderReply(command), f.brain, f.now);
        assert(f.brain.commandSendState() == CommandSendState::Complete);
        deliver(senderReply(senderCommand(10001)), f.brain, f.now);
        assert(f.brain.commandResponse().sequence == command.request.sequence);
    }
}
void senderRoleDeviceAndBusy() {
    Fixture f;
    ++senderScenarios;
    const auto command = senderCommand();
    assert(!f.brain.requestCommand(command, 0));
    assert(!f.motion.requestCommand(command, 0));
    f.run(3000, false);
    auto foreign = command;
    std::strcpy(foreign.request.deviceId, "bt-other");
    assert(!f.brain.requestCommand(foreign, f.now));
    auto zero = command;
    for (uint16_t ttl : {uint16_t(0), uint16_t(1), uint16_t(49), uint16_t(50), uint16_t(5001)}) {
        zero.remainingTtlMs = ttl;
        assert(!f.brain.requestCommand(zero, f.now));
    }
    StopRequest stop;
    assert(!f.motion.requestStop(stop, f.now));
    ReadOnlyLink unconfigured;
    assert(!unconfigured.requestCommand(command, f.now));
    assert(!unconfigured.requestStop(stop, f.now));
    assert(messages(f.toMotion.history, Kind::Command).empty());
    assert(f.brain.requestResult(query(), f.now));
    assert(!f.brain.requestCommand(command, f.now));
    f.brain.cancelResultQuery();
    f.run(300, false);
    assert(f.brain.requestCommand(command, f.now));
    f.run(300, false);
    assert(f.brain.commandSendState() == CommandSendState::Unavailable);
    Frame support;
    support.kind = Kind::Discovery;
    support.senderBoot = 11; support.receiverBoot = 22; support.messageId = 90000;
    support.total = support.length = 1;
    assert(f.brain.queueSupportFrame(support));
    assert(!f.brain.requestCommand(command, f.now)); // No hidden queue behind existing TX.
    f.run(300, false);
    assert(messages(f.toMotion.history, Kind::Command).size() == 1);
}
void senderTimeoutCancellationAndBoot() {
    for (bool wrap : {false, true}) {
        Fixture f;
        if (wrap) f.now = UINT32_MAX - 3500;
        senderSetup(f);
        finalCommandReady = false;
        const auto command = senderCommand();
        const uint32_t start = f.now;
        assert(f.brain.requestCommand(command, start));
        f.run(995, false);
        f.brain.poll(start + 999, f.toMotion);
        assert(f.brain.commandSendState() == CommandSendState::Pending);
        f.now = start + 1000;
        f.brain.poll(f.now, f.toMotion);
        assert(f.brain.commandSendState() == CommandSendState::TimedOut);
        deliver(senderReply(command), f.brain, f.now);
        assert(f.brain.commandSendState() == CommandSendState::TimedOut);
        finalCommandReady = true;
        f.run(1500, false);
        assert(commandCalls == 1 && messages(f.toMotion.history, Kind::Command).size() == 1);
        assert(f.brain.requestResult(senderQuery(command), f.now));
        f.run(300, false);
        assert(f.brain.resultLookupState() == ResultLookupState::Complete && commandCalls == 1);
    }
    for (unsigned phase = 0; phase < 4; ++phase) {
        Fixture f;
        senderSetup(f);
        finalCommandReady = false;
        const auto command = senderCommand();
        assert(f.brain.requestCommand(command, f.now));
        if (phase == 1) f.step(false); // Cancel after a partial first frame.
        if (phase >= 2) f.run(300, false);
        if (phase < 2) f.brain.cancelCommand();
        else if (phase == 2) {
            f.run(2200, false, true, true);
            assert(!f.brain.connected(f.now));
        } else {
            assert(f.motion.begin(pairing(Role::Motion), 44));
            f.toBrain.bytes.clear();
        }
        f.run(3000, false);
        assert(f.brain.commandSendState() == (phase < 2 ? CommandSendState::Cancelled :
                                            phase == 2 ? CommandSendState::TimedOut : CommandSendState::Unavailable));
        deliver(senderReply(command), f.brain, f.now);
        assert(f.brain.commandSendState() != CommandSendState::Complete);
        assert(commandCalls == (phase >= 2 ? 1u : 0u));
        assert(messages(f.toMotion.history, Kind::Command).size() == (phase >= 2 ? 1u : 0u));
    }
}
void senderFirstFrameBudget() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        Fixture f;
        senderSetup(f);
        const auto command = senderCommand();
        const uint32_t start = f.now;
        assert(f.brain.requestCommand(command, start));
        f.toMotion.zeroWrite = mode == 0;
        if (mode == 1) f.toMotion.capacity = 0;
        if (mode == 2) f.toMotion.writeLimit = 1;
        if (mode == 3) f.step(false); // Expiry after real partial UART output.
        if (mode < 3) f.run(55, false);
        else f.now = start + 50;
        f.brain.poll(f.now, f.toMotion);
        f.toMotion.deliver(f.motion, f.now, false);
        f.toMotion.zeroWrite = false;
        f.toMotion.capacity = f.toMotion.writeLimit = 64;
        f.run(1500, false);
        assert(commandCalls == 0 && messages(f.toMotion.history, Kind::Command).empty());
        assert(f.brain.commandSendState() == CommandSendState::TimedOut);
        assert(f.brain.healthy() && f.motion.healthy());
        assert(f.brain.requestCommand(senderCommand(10001), f.now));
        f.run(300, false);
        assert(commandCalls == 1 && f.brain.commandSendState() == CommandSendState::Complete);
    }
    Fixture f;
    senderSetup(f);
    const auto command = senderCommand();
    assert(f.brain.requestCommand(command, f.now));
    f.now += 49;
    // Multiple bounded UART drain opportunities within the last budget tick.
    for (unsigned i = 0; i < 8; ++i) {
        f.brain.poll(f.now, f.toMotion);
        f.toMotion.deliver(f.motion, f.now, false);
    }
    f.run(300, false);
    assert(commandCalls == 1 && observedTtl <= 450 && observedTtl > 0);
    assert(f.brain.commandSendState() == CommandSendState::Complete);
}
void senderStopPriority() {
    for (Source source : {Source::LocalTouch, Source::CloudCommand})
        for (bool querying : {false, true}) {
            Fixture f;
            senderSetup(f);
            finalCommandReady = false;
            const auto command = senderCommand();
            if (querying) assert(f.brain.requestResult(senderQuery(command), f.now));
            else assert(f.brain.requestCommand(command, f.now));
            f.step(false); // Start ordinary frame; urgent Stop must not interleave bytes.
            StopRequest stop;
            stop.source = source;
            stop.sequence = source == Source::CloudCommand ? 77 : 0;
            if (source == Source::CloudCommand) {
                std::strcpy(stop.commandId, "cloud-stop-original");
                stop.commandIdLength = uint8_t(std::strlen(stop.commandId));
            }
            assert(f.brain.requestStop(stop, f.now));
            assert(!f.brain.requestStop(stop, f.now));
            assert(f.brain.stopSendState() == StopSendState::Pending);
            f.run(300, false);
            assert(stopCalls == 1 && f.brain.stopSendState() == StopSendState::Received);
            assert(!f.brain.freshStatus(f.now) && !f.brain.peerStatus().stationary);
            assert(observedStop.source == source && observedStop.sequence == stop.sequence);
            const auto sent = messages(f.toMotion.history, Kind::Stop);
            assert(sent.size() == 1);
            if (source == Source::LocalTouch) {
                char expected[40];
                std::snprintf(expected, sizeof(expected), "stop-%016llx-%08x", 11ULL, sent[0].messageId);
                assert(!std::strcmp(observedStop.commandId, expected));
            } else assert(!std::strcmp(observedStop.commandId, stop.commandId));
            if (!querying) {
                assert(f.brain.commandSendState() == CommandSendState::Cancelled && commandCalls == 0);
                deliver(senderReply(command), f.brain, f.now);
                assert(f.brain.commandSendState() == CommandSendState::Cancelled);
            }
            f.run(1500, false);
            assert(messages(f.toMotion.history, Kind::Stop).size() == 1);
        }
}
void senderStopReceiptAndTimeout() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f;
        if (mode == 3) f.now = UINT32_MAX - 3500;
        senderSetup(f);
        stopAccepted = mode != 0;
        StopRequest stop;
        assert(f.brain.requestStop(stop, f.now));
        f.run(100, false, false, mode >= 1);
        assert(stopCalls == 1);
        const auto sent = messages(f.toMotion.history, Kind::Stop);
        assert(sent.size() == 1);
        if (mode == 0) assert(f.brain.stopSendState() == StopSendState::Rejected);
        else {
            auto wrong = senderReceipt(sent[0].messageId + 1);
            deliver(wrong, f.brain, f.now);
            assert(f.brain.stopSendState() == StopSendState::Pending);
            wrong = senderReceipt(sent[0].messageId);
            wrong.senderBoot = 33;
            deliver(wrong, f.brain, f.now);
            assert(f.brain.stopSendState() == StopSendState::Pending);
            if (mode == 1) {
                deliver(senderReceipt(sent[0].messageId), f.brain, f.now);
                assert(f.brain.stopSendState() == StopSendState::Received);
            } else {
                f.run(1000, false, false, true);
                assert(f.brain.stopSendState() == StopSendState::TimedOut);
                deliver(senderReceipt(sent[0].messageId), f.brain, f.now);
                assert(f.brain.stopSendState() == StopSendState::TimedOut);
            }
        }
        f.run(1500, false);
        assert(stopCalls == 1 && messages(f.toMotion.history, Kind::Stop).size() == 1);
        if (mode == 3) {
            assert(f.brain.requestStop(stop, f.now)); // Explicit retry, never an automatic replay.
            f.run(300, false);
            assert(stopCalls == 2 && f.brain.stopSendState() == StopSendState::Received);
            const auto retried = messages(f.toMotion.history, Kind::Stop);
            assert(retried.size() == 2 && retried[0].messageId != retried[1].messageId);
        }
    }
    Fixture f;
    senderSetup(f);
    StopRequest stop;
    assert(f.brain.requestStop(stop, f.now));
    f.run(100, false, false, true);
    assert(f.brain.stopSendState() == StopSendState::Pending && stopCalls == 1);
    assert(f.motion.begin(pairing(Role::Motion), 44));
    f.toBrain.bytes.clear();
    f.run(3000, false);
    assert(f.brain.stopSendState() == StopSendState::Unavailable);
    const auto sent = messages(f.toMotion.history, Kind::Stop);
    assert(sent.size() == 1);
    deliver(senderReceipt(sent[0].messageId), f.brain, f.now);
    assert(f.brain.stopSendState() == StopSendState::Unavailable && stopCalls == 1);
}
void senderControlSlotDoesNotGateOrRenew() {
    for (bool blocked : {false, true}) {
        Fixture f;
        senderSetup(f);
        deliverSample(f, f.now, 90000); // Real STATUS parser queues a control ACK.
        assert(f.brain.freshStatus(f.now));
        const uint32_t start = f.now;
        if (blocked) f.toMotion.writeLimit = 1;
        assert(f.brain.requestCommand(senderCommand(), start));
        f.step(false);
        assert(messages(f.toMotion.history, Kind::Command).empty());
        if (!blocked) {
            assert(messages(f.toMotion.history, Kind::LinkAck).size() == 1);
            f.run(300, false);
            assert(commandCalls == 1 && f.brain.commandSendState() == CommandSendState::Complete);
        } else {
            f.run(40, false);
            f.now = start + 49;
            f.brain.poll(f.now, f.toMotion);
            f.toMotion.deliver(f.motion, f.now, false);
            assert(f.brain.commandSendState() == CommandSendState::Pending);
            f.now = start + 50;
            f.brain.poll(f.now, f.toMotion);
            f.toMotion.deliver(f.motion, f.now, false);
            assert(f.brain.commandSendState() == CommandSendState::TimedOut);
            f.toMotion.writeLimit = 64;
            f.run(1500, false);
            assert(commandCalls == 0 && messages(f.toMotion.history, Kind::Command).empty());
            assert(messages(f.toMotion.history, Kind::LinkAck).size() == 1);
        }
    }
}
void senderSameBootHelloCannotGateStop() {
    for (bool firstFragmentComplete : {false, true}) {
        Fixture f;
        ++senderScenarios;
        commandCalls = stopCalls = 0;
        finalCommandReady = stopAccepted = true;
        assert(f.motion.setCommandHandler(handleCommand));
        assert(f.motion.setStopHandler(handleStop));
        f.run(3000, false);
        const auto hellos = messages(f.toBrain.history, Kind::Hello);
        assert(!hellos.empty());
        const auto hello = hellos.back(); // Captured from the real Motion peer.
        assert(hello.senderBoot == 22 && hello.receiverBoot == 0);
        f.toMotion.history.clear(); f.toBrain.history.clear();
        assert(f.brain.requestCommand(senderCommand(), f.now));
        for (unsigned i = 0; i < (firstFragmentComplete ? 4u : 1u); ++i) f.step(false);
        assert(commandCalls == 0 && messages(f.toMotion.history, Kind::Command).empty());
        Parser before;
        Frame frame;
        unsigned fragments = 0;
        for (uint8_t byte : f.toMotion.history)
            if (before.push(byte, f.now, frame) && frame.kind == Kind::Command) ++fragments;
        assert(fragments == (firstFragmentComplete ? 1u : 0u));
        deliver(hello, f.brain, f.now); // Production parser, same boot, HELLO_ACK pending.
        assert(f.brain.connected(f.now));
        assert(f.brain.takeFailure() == LinkFailure::None);
        StopRequest stop;
        assert(f.brain.requestStop(stop, f.now));
        assert(f.brain.commandSendState() == CommandSendState::Cancelled);
        f.run(300, false);
        assert(stopCalls == 1 && commandCalls == 0);
        assert(f.brain.stopSendState() == StopSendState::Received);
        assert(f.brain.commandSendState() == CommandSendState::Cancelled);
        const auto sent = messages(f.toMotion.history, Kind::Stop);
        assert(sent.size() == 1 && sent[0].senderBoot == 11 && sent[0].receiverBoot == 22);
        char expected[40];
        std::snprintf(expected, sizeof(expected), "stop-%016llx-%08x", 11ULL, sent[0].messageId);
        assert(observedStop.sequence == 0 && !std::strcmp(observedStop.commandId, expected));
        Parser after;
        Assembler assembler;
        Message message;
        bool stopSeen = false, helloAckSeen = false;
        for (uint8_t byte : f.toMotion.history)
            if (after.push(byte, f.now, frame) &&
                assembler.accept(frame, f.now, message) == AssemblyResult::Complete) {
                if (message.kind == Kind::Stop) {
                    stopSeen = true;
                    // Mirror Motion's Stop barrier: the passive observer must
                    // also release the unfinished ordinary reassembly slot.
                    assembler.reset();
                }
                if (message.kind == Kind::HelloAck) { assert(stopSeen); helloAckSeen = true; }
                assert(message.kind != Kind::Command);
            }
        assert(stopSeen && helloAckSeen);
        assert(f.brain.connected(f.now) && f.motion.connected(f.now));
    }
}
} // namespace

int main() {
    handshakeAndFreshness();
    disconnectAndRestart();
    wrongBoardAndUnsupported();
    capturedBootCannotRestoreStatus();
    oneWayLoss();
    frozenSamplesCannotStayReady();
    sampleAgeSendBoundary();
    sampleFreshnessExpiryBoundary();
    sampleOrderingAndReconnectWatermark();
    sampleClockWraparound();
    resultQueryRuntime();
    resultQueryResponseMatching();
    resultQueryTimeoutAndRestart();
    resultReplyBackpressure();
    resultQueryCancellationAndTransportFault();
    commandAndStopRuntime();
    commandDeferralAndFirstFragmentExpiry();
    senderExactExchangeAndQuery();
    senderResponseIdentity();
    senderRoleDeviceAndBusy();
    senderTimeoutCancellationAndBoot();
    senderFirstFrameBudget();
    senderStopPriority();
    senderStopReceiptAndTimeout();
    senderControlSlotDoesNotGateOrRenew();
    senderSameBootHelloCannotGateStop();
    std::printf("%u Brain sender scenarios (existing receive/query/status regressions retained)\n", senderScenarios);
    std::puts("PASS v4 two-peer link, Motion commands/Stop/TTL, query, stale status, backpressure and restarts");
}
