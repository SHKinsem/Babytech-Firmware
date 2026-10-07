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
    bool idle() const override { return bytes.empty(); }
    size_t available() const override { return 64 - bytes.size(); }
    size_t write(const uint8_t* data, size_t size) override {
        assert(size <= available());
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
    assert(!f.brain.peerStatus().snapshot.startEnabled);
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
    assert(!f.brain.peerStatus().snapshot.startEnabled);

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
    assert(!f.brain.peerStatus().snapshot.startEnabled);
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
bool handleStop(const StopRequest&, uint32_t) { ++stopCalls; return true; }

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
    assert(!f.brain.peerStatus().snapshot.startEnabled); // Brain sender is still readonly.
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
    std::puts("PASS v4 two-peer link, Motion commands/Stop/TTL, query, stale status, backpressure and restarts");
}
