#include "controller_link.h"
#include "BoardPairingRecord.h"
#include "FakeBoardIo.h"
#include "nvs.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace babytech;
using namespace babytech::boardlink;
using namespace babytech::v4;
using fake::io;
using Bytes = std::vector<uint8_t>;

namespace {
constexpr uint64_t kLocalBoot = UINT64_C(0x1234567812345678);
constexpr uint64_t kPeerBoot = 22;

Pairing pairing() {
    Pairing p;
    std::strcpy(p.deviceId, "bt-brain-controller-test");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "112233445566");
    std::strcpy(p.peerPhysicalId, "aabbccddeeff");
    return p;
}

Bytes wire(const Message& message) {
    Bytes result;
    size_t offset = 0;
    do {
        Frame frame;
        assert(fragment(message, offset, frame));
        uint8_t bytes[kMaxFrame];
        const size_t length = encode(frame, bytes, sizeof(bytes));
        assert(length);
        result.insert(result.end(), bytes, bytes + length);
        offset += frame.length;
    } while (offset < message.length);
    return result;
}

Message envelope(Kind kind, uint64_t boot, uint32_t id) {
    Message message;
    message.kind = kind;
    message.senderBoot = boot;
    message.receiverBoot = kLocalBoot;
    message.messageId = id;
    return message;
}

Message hello(Kind kind, uint64_t boot, uint32_t id, uint32_t replyTo = 0) {
    const auto p = pairing();
    Hello identity;
    identity.role = Role::Motion;
    identity.replyTo = replyTo;
    std::strcpy(identity.deviceId, p.deviceId);
    std::strcpy(identity.epoch, p.epoch);
    std::strcpy(identity.physicalId, p.peerPhysicalId);
    auto message = envelope(kind, boot, id);
    assert(encodeHello(identity, message, kind));
    message.senderBoot = boot;
    message.receiverBoot = kind == Kind::Hello ? 0 : kLocalBoot;
    message.messageId = id;
    return message;
}

Message telemetry(const char* error, uint32_t sample, uint32_t id, uint64_t boot = kPeerBoot) {
    Status status;
    std::strcpy(status.productError, error);
    const bool fault = std::strcmp(error, "NONE") != 0;
    std::strcpy(status.productProgress, fault ? "error" : "ready");
    status.snapshot.stage = fault ? display::DisplayStage::Error : display::DisplayStage::Ready;
    status.snapshot.error = fault ? display::DisplayError::CanFault : display::DisplayError::None;
    status.snapshot.startEnabled = true;
    status.sampleUptimeMs = sample;
    status.powderValid = status.lowWaterValid = true;
    status.powderGrams = 275;
    status.contextVersion = 77;
    status.feedingContextConfigured = true;
    std::memset(status.babyId, 'b', sizeof(status.babyId) - 1);
    Message message;
    assert(encodeStatus(status, message));
    message.senderBoot = boot;
    message.receiverBoot = kLocalBoot;
    message.messageId = id;
    return message;
}

std::string payload(const Status* status) {
    assert(status);
    Message message;
    assert(encodeStatus(*status, message));
    return std::string(reinterpret_cast<const char*>(message.payload), message.length);
}

struct Fixture {
    display::ControllerLink controller;
    explicit Fixture(uint32_t now = 0) {
        fake::reset();
        io.blob.resize(kPairingRecordMaxSize);
        const size_t length = encodePairingRecord(pairing(), io.blob.data(), io.blob.size());
        assert(length);
        io.blob.resize(length);
        io.randomWords = {0x12345678, 0x12345678};
        assert(!controller.lastTelemetry());
        assert(controller.begin());
        assert(!controller.lastTelemetry() && controller.lastTelemetryReceivedAtMs() == 0);
        drain(now);
    }
    ~Fixture() { fake::assertReadOnly(); }

    void drain(uint32_t now) {
        for (unsigned i = 0; i < 32; ++i) controller.poll(now);
    }
    void injectBytes(const Bytes& bytes, uint32_t now) {
        io.rx.insert(io.rx.end(), bytes.begin(), bytes.end());
        unsigned iterations = 0;
        while (!io.rx.empty() && ++iterations <= 32) controller.poll(now);
        assert(io.rx.empty());
    }
    void inject(const Message& message, uint32_t now) { injectBytes(wire(message), now); }
    uint32_t probe() const {
        Parser parser;
        Assembler assembler;
        Frame frame;
        Message message;
        uint32_t id = 0;
        for (uint8_t byte : io.tx) {
            if (parser.push(byte, 0, frame) &&
                assembler.accept(frame, 0, message) == AssemblyResult::Complete && message.kind == Kind::Hello)
                id = message.messageId;
        }
        assert(id);
        return id;
    }
    void handshake(uint32_t now, uint64_t boot = kPeerBoot, uint32_t ackId = 10) {
        drain(now);
        inject(hello(Kind::HelloAck, boot, ackId, probe()), now);
        inject(envelope(Kind::Heartbeat, boot, ackId + 1), now);
        drain(now);
    }
    void expect(const char* error, uint32_t receivedAt) const {
        const auto* status = controller.lastTelemetry();
        assert(status && std::strcmp(status->productError, error) == 0);
        assert(controller.lastTelemetryReceivedAtMs() == receivedAt);
        assert(status->powderGrams == 275 && status->contextVersion == 77);
        assert(std::strlen(status->babyId) == 96);
        assert(!status->snapshot.startEnabled);
    }
    void expectStatus(const Message& source, uint32_t receivedAt) const {
        Status expected;
        assert(decodeStatus(source, expected));
        expected.snapshot.startEnabled = false;  // Read-only link normalization.
        assert(payload(controller.lastTelemetry()) == payload(&expected));
        assert(controller.lastTelemetryReceivedAtMs() == receivedAt);
    }
    void offline() const {
        assert(!controller.hasSnapshot());
        assert(controller.snapshot().stage == display::DisplayStage::NotReady);
        assert(controller.snapshot().primaryCondition == display::DisplayCondition::ControllerOffline);
        assert(!controller.snapshot().startEnabled);
    }
};

void noTelemetry() {
    Fixture f;
    f.handshake(0);
    f.drain(100);
    assert(!f.controller.lastTelemetry() && !f.controller.hasSnapshot());
    f.drain(1500);
    assert(!f.controller.lastTelemetry() && f.controller.lastTelemetryReceivedAtMs() == 0);
    f.offline();
    std::puts("PASS no telemetry fabricated before first accepted STATUS");
}

void heartbeatExpiry() {
    Fixture f;
    f.handshake(0);
    f.inject(telemetry("E_CAN_FAULT", 90000, 12), 100);
    f.expect("E_CAN_FAULT", 100);
    const auto before = payload(f.controller.lastTelemetry());
    const auto* held = f.controller.lastTelemetry();
    for (uint32_t now : {100u, 200u, 1000u, 1499u}) {
        f.drain(now);
        f.expect("E_CAN_FAULT", 100);
        assert(f.controller.connected(now));
    }
    f.drain(1500);
    assert(!f.controller.connected(1500));
    f.offline();
    f.expect("E_CAN_FAULT", 100);
    assert(held == f.controller.lastTelemetry() && payload(held) == before);
    f.inject(telemetry("NONE", 90001, 13), 1501);  // Not handshaken.
    f.expect("E_CAN_FAULT", 100);
    f.handshake(1502, kPeerBoot, 20);
    f.offline();
    f.expect("E_CAN_FAULT", 100);
    // New transport IDs cannot make the same or older mechanical sample fresh.
    f.inject(telemetry("NONE", 90000, 22), 1503);
    assert(payload(f.controller.lastTelemetry()) == before);
    f.expect("E_CAN_FAULT", 100);
    f.inject(telemetry("NONE", 89999, 23), 1504);
    assert(payload(f.controller.lastTelemetry()) == before);
    f.expect("E_CAN_FAULT", 100);
    f.inject(telemetry("NONE", 90001, 24), 1505);
    f.expect("NONE", 1505);
    assert(f.controller.connected(1505) && f.controller.hasSnapshot());
    f.drain(1510);
    f.expect("NONE", 1505);
    std::puts("PASS fault survives heartbeat expiry/reconnect until fresh NONE");
}

void peerRestart() {
    Fixture f;
    f.handshake(0);
    f.inject(telemetry("E_CONTEXT_STORAGE_2", 100, 12), 100);
    const auto before = payload(f.controller.lastTelemetry());
    const uint32_t oldProbe = f.probe();
    f.inject(hello(Kind::Hello, 33, 1), 700);
    f.drain(700);
    assert(f.probe() != oldProbe);
    f.expect("E_CONTEXT_STORAGE_2", 100);
    f.handshake(701, 33, 2);
    f.offline();
    f.expect("E_CONTEXT_STORAGE_2", 100);
    assert(payload(f.controller.lastTelemetry()) == before);
    f.inject(telemetry("NONE", 101, 50, kPeerBoot), 702);  // Old boot.
    f.expect("E_CONTEXT_STORAGE_2", 100);
    f.drain(750);
    f.expect("E_CONTEXT_STORAGE_2", 100);
    // The new boot may legitimately reuse the previous sample uptime.
    f.inject(telemetry("NONE", 100, 4, 33), 800);
    f.expect("NONE", 800);
    assert(f.controller.connected(800));
    std::puts("PASS fault survives new boot/old-boot replay until fresh NONE");
}

void sampleExpiryAndReplays() {
    Fixture f;
    f.handshake(0);
    const auto fault = telemetry("E_CAN_FAULT", 500000, 12);
    f.inject(fault, 100);
    const auto before = payload(f.controller.lastTelemetry());
    f.inject(fault, 200);
    f.expectStatus(fault, 100);
    f.inject(telemetry("NONE", 500000, 13), 250);
    f.expectStatus(fault, 100);
    f.inject(telemetry("NONE", 499999, 14), 300);
    f.expectStatus(fault, 100);
    f.inject(telemetry("NONE", 500001, 12), 350);  // Replayed transport ID.
    f.expectStatus(fault, 100);
    auto invalid = telemetry("NONE", 500001, 15);
    invalid.payload[0] = '[';
    f.inject(invalid, 400);
    f.expectStatus(fault, 100);
    f.expect("E_CAN_FAULT", 100);
    assert(payload(f.controller.lastTelemetry()) == before);
    f.inject(envelope(Kind::Heartbeat, kPeerBoot, 20), 1400);
    f.drain(1599);
    assert(f.controller.connected(1599));
    f.expect("E_CAN_FAULT", 100);
    f.drain(1600);  // STATUS expires although heartbeat remains fresh.
    f.offline();
    f.expect("E_CAN_FAULT", 100);
    f.inject(telemetry("NONE", 500000, 21), 1601);
    f.offline();
    f.expect("E_CAN_FAULT", 100);
    f.inject(telemetry("NONE", 500001, 22), 1700);
    f.expect("NONE", 1700);
    // Distinct accepted samples can arrive within the same Brain millisecond.
    f.inject(telemetry("E_CAN_FAULT", 500002, 23), 1700);
    f.expect("E_CAN_FAULT", 1700);
    f.drain(1800);
    f.expect("E_CAN_FAULT", 1700);
    std::puts("PASS sample expiry, poll/heartbeat/replay rejection and same-ms updates");
}

void partialNoneDoesNotClearFault() {
    Fixture f;
    f.handshake(0);
    const auto fault = telemetry("E_CONTEXT_STORAGE_2", 20000, 12);
    f.inject(fault, 100);
    f.expectStatus(fault, 100);
    const auto clear = telemetry("NONE", 20001, 13);
    const auto bytes = wire(clear);
    f.injectBytes(Bytes(bytes.begin(), bytes.end() - 1), 200);
    f.expectStatus(fault, 100);
    f.drain(210);
    f.expectStatus(fault, 100);
    f.injectBytes(Bytes(bytes.end() - 1, bytes.end()), 220);
    f.expectStatus(clear, 220);
    f.expect("NONE", 220);
    f.drain(221);
    f.expectStatus(clear, 220);
    std::puts("PASS incomplete NONE cannot clear fault; full receipt atomically updates all 35 fields/time");
}

void longOfflineFaultAcrossRollover() {
    Fixture f(UINT32_MAX - 200);
    f.handshake(UINT32_MAX - 200);
    const auto fault = telemetry("E_CAN_FAULT", UINT32_MAX - 50, 12);
    const uint32_t receivedAt = UINT32_MAX - 100;
    f.inject(fault, receivedAt);
    f.expectStatus(fault, receivedAt);
    for (uint32_t now : {UINT32_MAX, 0u, 1299u, 1500u, 5000u, 6000u}) {
        f.drain(now);
        f.expectStatus(fault, receivedAt);
        if (now != UINT32_MAX && now >= 1299u) f.offline();
    }
    assert(uint32_t(6000u - f.controller.lastTelemetryReceivedAtMs()) == 6101u);
    f.handshake(6001, 33, 2);
    f.offline();
    f.expectStatus(fault, receivedAt);
    const auto newFault = telemetry("E_CONTEXT_STORAGE_2", 0, 4, 33);
    f.inject(newFault, 6002);
    f.expectStatus(newFault, 6002);
    f.expect("E_CONTEXT_STORAGE_2", 6002);
    // A newer fault replaces the old fault; its frozen NONE replay must not.
    f.inject(telemetry("NONE", 0, 5, 33), 6003);
    f.expectStatus(newFault, 6002);
    const auto clear = telemetry("NONE", 1, 6, 33);
    f.inject(clear, 6004);
    f.expectStatus(clear, 6004);
    f.expect("NONE", 6004);
    f.inject(fault, 6005);  // Old-boot fault cannot replace the fresh clear either.
    f.expectStatus(clear, 6004);
    std::puts("PASS full fault cache survives rollover/long offline/new boot; only new sampling updates it");
}

void receiptTimeAndRollover() {
    Fixture f(UINT32_MAX - 200);
    f.handshake(UINT32_MAX - 200);
    const auto bytes = wire(telemetry("E_CAN_FAULT", 800000, 12));
    f.injectBytes(Bytes(bytes.begin(), bytes.end() - 1), UINT32_MAX - 20);
    assert(!f.controller.lastTelemetry());
    f.injectBytes(Bytes(bytes.end() - 1, bytes.end()), UINT32_MAX - 10);
    f.expect("E_CAN_FAULT", UINT32_MAX - 10);
    f.drain(UINT32_MAX);
    f.expect("E_CAN_FAULT", UINT32_MAX - 10);
    f.inject(telemetry("NONE", 800001, 13), 0);
    f.expect("NONE", 0);  // Zero is a valid receipt timestamp, not "unseen".
    f.drain(10);
    f.expect("NONE", 0);
    f.inject(envelope(Kind::Heartbeat, kPeerBoot, 20), 1200);
    f.drain(1500);
    f.offline();
    f.expect("NONE", 0);
    std::puts("PASS completed-message receipt time, millis rollover and valid zero timestamp");
}

void failedBegin() {
    fake::reset();
    io.openError = ESP_ERR_NVS_NOT_FOUND;
    display::ControllerLink controller;
    assert(!controller.begin());
    controller.poll(123);
    assert(!controller.lastTelemetry() && controller.lastTelemetryReceivedAtMs() == 0);
    assert(!controller.connected(123) && !controller.hasSnapshot());
    fake::assertReadOnly();
    std::puts("PASS failed begin has no telemetry");
}
}  // namespace

int main() {
    noTelemetry();
    heartbeatExpiry();
    peerRestart();
    sampleExpiryAndReplays();
    partialNoneDoesNotClearFault();
    longOfflineFaultAcrossRollover();
    receiptTimeAndRollover();
    failedBegin();
    std::puts("PASS production ControllerLink cache/receipt-time suite (SDK I/O fakes only)");
}
