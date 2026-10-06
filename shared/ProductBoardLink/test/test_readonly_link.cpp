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
    std::puts("PASS v4 two-peer readonly link, stale status, reconnect and rejected actions");
}
