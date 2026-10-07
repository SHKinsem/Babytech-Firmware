#include "controller_link.h"
#include "brain_pairing_console.h"
#include "BoardPairingRecord.h"
#include "FakeBoardIo.h"
#include "nvs.h"

#include <cassert>
#include <array>
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
    io.macError = ESP_FAIL;
    display::ControllerLink controller;
    assert(!controller.begin());
    controller.poll(123);
    assert(!controller.lastTelemetry() && controller.lastTelemetryReceivedAtMs() == 0);
    assert(!controller.connected(123) && !controller.hasSnapshot());
    fake::assertReadOnly();
    std::puts("PASS failed discovery MAC begin has no telemetry");
}

Bytes record(const Pairing& value) {
    Bytes result(kPairingRecordMaxSize);
    const size_t length = encodePairingRecord(value, result.data(), result.size());
    assert(length);
    result.resize(length);
    return result;
}

void verifiedLocalPairing() {
    for (bool uartUnavailable : {false, true}) {
        fake::reset();
        io.blob = record(pairing());
        io.serialReady = !uartUnavailable;
        display::ControllerLink controller;
        assert(controller.verifiedPairing() == nullptr);
        assert(controller.begin() == !uartUnavailable);
        const auto* verified = controller.verifiedPairing();
        assert(verified && verified->role == Role::Brain);
        assert(!std::strcmp(verified->deviceId, pairing().deviceId));
        assert(!std::strcmp(verified->epoch, pairing().epoch));
        assert(!std::strcmp(verified->localPhysicalId, pairing().localPhysicalId));
        assert(!std::strcmp(verified->peerPhysicalId, pairing().peerPhysicalId));
        assert(!controller.connected(0) && !controller.hasSnapshot());
        assert(!controller.sendIntent(display::DisplayIntent::StartFeeding, 0));
        if (uartUnavailable) assert(controller.deviceId() == nullptr);
        fake::assertReadOnly();
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        fake::reset();
        io.blob = record(pairing());
        if (fault == 0) io.openError = ESP_ERR_NVS_NOT_FOUND;
        if (fault == 1) io.blob[0] ^= 1;
        if (fault == 2) io.mac[5] ^= 1;
        if (fault == 3) io.macError = ESP_FAIL;
        display::ControllerLink controller;
        controller.begin();
        assert(controller.verifiedPairing() == nullptr);
        assert(!controller.connected(0) && !controller.hasSnapshot());
        fake::assertReadOnly();
    }
    std::puts("PASS verified local pairing survives UART failure, never substitutes missing/corrupt/foreign identity or permits action");
}

template<class Action>
void onBoard(fake::State& state, Action action) {
    fake::assertReadOnly();
    io = state;
    action();
    fake::assertReadOnly();
    state = io;
}

Frame onlyDiscovery(const Bytes& bytes) {
    Parser parser;
    Frame decoded, result;
    unsigned count = 0;
    size_t consumed = 0;
    for (uint8_t byte : bytes) {
        if (!parser.push(byte, 0, decoded)) continue;
        consumed += kHeaderSize + decoded.length + 2;
        assert(decoded.kind == Kind::Discovery && decoded.offset == 0 && decoded.length == decoded.total);
        result = decoded;
        ++count;
    }
    assert(count == 1 && consumed == bytes.size());
    return result;
}

void discoveryOffline(const display::ControllerLink& controller, uint32_t now) {
    assert(!controller.connected(now) && !controller.hasSnapshot());
    assert(!controller.protocolIncompatible(now));
    assert(controller.snapshot().stage == display::DisplayStage::NotReady);
    assert(!controller.snapshot().startEnabled && !controller.snapshot().cloudConnected);
    assert(!controller.lastTelemetry() && !controller.lastTelemetryReceivedAtMs());
    assert(!controller.intentPending());
}

std::string console(const char* command, bool maintenance,
                    display::ControllerLink& controller, uint32_t now) {
    std::array<char, 256> output;
    output.fill('#');
    assert(brain::BrainPairingConsole::handle(command, maintenance, controller, now,
                                             output.data(), output.size()));
    const std::string result(output.data());
    // Exact expected replies below also preclude any extra configuration dump.
    for (const char* forbidden : {"password", "secret", "baby", "formula", "powder",
                                 "0123456789abcdef0123456789abcdef", "paired"})
        assert(result.find(forbidden) == std::string::npos);
    return result;
}

void discoveryStorage(DiscoveryPairState state) {
    switch (state) {
        case DiscoveryPairState::Missing: io.openError = ESP_ERR_NVS_NOT_FOUND; break;
        case DiscoveryPairState::Ready: break;
        case DiscoveryPairState::Corrupt: io.blob[0] ^= 1; break;
        case DiscoveryPairState::IoError: io.openError = ESP_FAIL; break;
        case DiscoveryPairState::IdentityMismatch: io.mac[5] ^= 1; break;
    }
}

void consoleDiscoveryRoundTrip(DiscoveryPairState brainState, DiscoveryPairState motionState) {
    assert(brainState != DiscoveryPairState::Ready);
    const bool motionPaired = motionState == DiscoveryPairState::Ready;
    const bool unavailable = brainState != DiscoveryPairState::Missing ||
        (motionState != DiscoveryPairState::Missing && !motionPaired);
    fake::reset();
    io.blob = record(pairing());
    discoveryStorage(brainState);
    io.randomWords = {0x12345678, 0x12345678};
    display::ControllerLink controller;
    assert(controller.begin());
    assert(io.begins == 1 && io.constructors == 1 && io.rxConfigs == 1);
    assert(io.macReads == (brainState == DiscoveryPairState::IdentityMismatch ? 2u : 1u));
    assert(controller.deviceId() && !controller.deviceId()[0]);
    discoveryOffline(controller, 0);
    const auto before = io;
    assert(console("PAIR STATUS", false, controller, 0) == "[pair] idle motion=unknown pairing=0\n");
    assert(console("PAIR DISCOVER bt-brain-controller-test", false, controller, 0) ==
           "[pair] maintenance_required\n");
    assert(controller.discoveryResult().state == DiscoveryState::Idle);
    assert(io.opens == before.opens && io.writeCalls == before.writeCalls && io.byteReads == before.byteReads);
    assert(console("PAIR DISCOVER bt-brain-controller-test", true, controller, 10) ==
           "[pair] discovery_pending\n");
    assert(console("PAIR DISCOVER bt-brain-controller-test", true, controller, 10) ==
           "[pair] discovery_unavailable\n");
    assert(console("PAIR STATUS", false, controller, 10) == "[pair] pending motion=unknown pairing=0\n");
    for (unsigned i = 0; i < 32; ++i) controller.poll(10);
    const Bytes query = io.tx;
    const auto q = onlyDiscovery(query);
    assert(q.senderBoot == kLocalBoot && q.receiverBoot == 0 && q.payload[0] == 1);
    assert(!controller.sendIntent(display::DisplayIntent::StartFeeding, 10));
    assert(!controller.sendIntent(display::DisplayIntent::Initialize, 10));
    discoveryOffline(controller, 10);
    fake::State brainIo = io;

    fake::reset();
    io.mac = {{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}};
    io.randomWords = {0, uint32_t(kPeerBoot)};
    auto peer = pairing();
    peer.role = Role::Motion;
    std::strcpy(peer.localPhysicalId, "aabbccddeeff");
    std::strcpy(peer.peerPhysicalId, "112233445566");
    io.blob = record(peer);
    discoveryStorage(motionState);
    ArduinoBoardLink motion;
    assert(motion.begin(Role::Motion, 44, 43, 115200, true));
    const Bytes stored = io.blob;
    io.rx.insert(io.rx.end(), query.begin(), query.end());
    motion.poll(20);
    // On the first poll the single ordinary slot sends the discovery reply,
    // before a paired Motion can enqueue its normal HELLO on subsequent polls.
    const Bytes reply = io.tx;
    const auto r = onlyDiscovery(reply);
    assert(r.receiverBoot == kLocalBoot && r.senderBoot == kPeerBoot && r.messageId == q.messageId);
    assert(r.payload[0] == 2 && r.payload[13] == uint8_t(motionState));
    assert(io.blob == stored && io.mutations == 0);
    assert(io.constructors == 1 && io.begins == 1 && io.txConfigs == 0);
    fake::assertReadOnly();

    onBoard(brainIo, [&] {
        const Bytes storedBrain = io.blob;
        io.rx.insert(io.rx.end(), reply.begin(), reply.end() - 1);
        controller.poll(30);
        assert(controller.discoveryResult().state == DiscoveryState::Pending);
        io.rx.push_back(reply.back());
        controller.poll(31);
        const auto expectedState = unavailable ? DiscoveryState::Unavailable : DiscoveryState::Found;
        assert(controller.discoveryResult().state == expectedState);
        assert(controller.discoveryResult().peerBoot == kPeerBoot);
        assert(controller.discoveryResult().pairingState == motionState);
        const char* physical = motionState == DiscoveryPairState::IdentityMismatch
            ? "aabbccddeefe" : "aabbccddeeff";
        assert(std::strcmp(controller.discoveryResult().physicalId, physical) == 0);
        const auto writes = io.writeCalls, opens = io.opens, reads = io.byteReads;
        const std::string expected = std::string("[pair] ") +
            (unavailable ? "pairing_record_unavailable" : "found") +
            " motion=" + physical + " pairing=" + std::to_string(unsigned(motionState)) + "\n";
        assert(console("PAIR STATUS", false, controller, 31) == expected);
        assert(io.writeCalls == writes && io.opens == opens && io.byteReads == reads);
        assert(!controller.deviceId()[0]);
        // A discovered paired Motion still cannot install identity or telemetry
        // on an unpaired Brain, even if it later sends normal-session traffic.
        const Bytes status = wire(telemetry("NONE", 31, 40));
        const Bytes ack = wire(hello(Kind::HelloAck, kPeerBoot, 41, 1));
        io.rx.insert(io.rx.end(), ack.begin(), ack.end());
        io.rx.insert(io.rx.end(), status.begin(), status.end());
        for (unsigned i = 0; i < 32; ++i) controller.poll(32);
        discoveryOffline(controller, 32);
        assert(!controller.sendIntent(display::DisplayIntent::StartFeeding, 32));
        assert(!controller.sendIntent(display::DisplayIntent::Initialize, 32));
        assert(!controller.deviceId()[0] && io.tx == query);
        assert(controller.discoveryResult().state == expectedState);
        assert(io.blob == storedBrain && io.opens == opens && io.mutations == 0);
        assert(console("PAIR RECORDS", false, controller, 33) == "[pair] records_idle\n");
        assert(console("PAIR HOLD", false, controller, 33) == "[pair] hold_idle\n");
        assert(console("PAIR HOLD bt-brain-controller-test", false, controller, 33) ==
               "[pair] maintenance_required\n");
        assert(console("PAIR RELEASE", false, controller, 33) == "[pair] hold_unavailable\n");
        assert(console("PAIR READ bt-brain-controller-test", false, controller, 33) ==
               "[pair] maintenance_required\n");
        if (unavailable) {
            assert(console("PAIR READ bt-brain-controller-test", true, controller, 34) ==
                   "[pair] records_unavailable\n");
            assert(controller.recordsState() == ExportTransferState::Idle && !controller.recordsSnapshot());
        } else {
            io.randomWords = {1, 2, 3, 4};
            assert(console("PAIR READ bt-brain-controller-test", true, controller, 34) ==
                   "[pair] records_pending\n");
            assert(console("PAIR RECORDS", false, controller, 34) == "[pair] records_pending\n");
            for (unsigned i = 0; i < 32; ++i) controller.poll(34);
            controller.poll(1034);
            assert(console("PAIR RECORDS", false, controller, 1034) == "[pair] records_timed_out\n");
            assert(!controller.recordsSnapshot());
            discoveryOffline(controller, 1034);
        }
        assert(!controller.deviceId()[0] && !controller.sendIntent(display::DisplayIntent::StartFeeding, 1034));
        if (unavailable) {
            assert(console("PAIR HOLD bt-brain-controller-test", true, controller, 1040) ==
                   "[pair] hold_unavailable\n");
        } else {
            io.randomWords = {5, 6, 7, 8};
            assert(console("PAIR HOLD bt-brain-controller-test", true, controller, 1040) ==
                   "[pair] hold_pending\n");
            assert(console("PAIR HOLD", false, controller, 1040) == "[pair] hold_pending\n");
            // Safe cancellation is permitted even after the local USB session
            // ended, so no manual second-board END is needed.
            assert(console("PAIR RELEASE", false, controller, 1041) == "[pair] hold_releasing\n");
            for (unsigned i = 0; i < 32; ++i) controller.poll(1041);
            controller.poll(2041);
            assert(console("PAIR HOLD", false, controller, 2041) == "[pair] hold_timed_out\n");
        }
        assert(io.blob == storedBrain && io.opens == opens && io.mutations == 0);
    });
    std::printf("PASS real PAIR console/ControllerLink UART brain=%u motion=%u -> %s; never paired/ready/action\n",
                unsigned(brainState), unsigned(motionState),
                unavailable ? "pairing_record_unavailable" : "found");
}

void consoleTimeoutAndRetry() {
    fake::reset();
    io.openError = ESP_ERR_NVS_NOT_FOUND;
    io.randomWords = {0x12345678, 0x12345678};
    display::ControllerLink controller;
    assert(controller.begin());
    char output[32] = "untouched";
    assert(!brain::BrainPairingConsole::handle("NET STATUS", false, controller, 0, output, sizeof(output)));
    assert(!std::strcmp(output, "untouched"));
    assert(console("PAIR UNKNOWN", true, controller, 0) == "[pair] unknown_command\n");
    for (const std::string& invalid : {std::string(""), std::string("bad/id"), std::string("bad id"),
                                     std::string("valid "), std::string(65, 'a')}) {
        assert(console(("PAIR DISCOVER " + invalid).c_str(), true, controller, 0) ==
               "[pair] discovery_unavailable\n");
        assert(controller.discoveryResult().state == DiscoveryState::Idle);
    }
    assert(console("PAIR DISCOVER bt-brain-controller-test", true, controller, 10) == "[pair] discovery_pending\n");
    controller.poll(10);
    const Bytes first = io.tx;
    const Frame firstFrame = onlyDiscovery(first);
    const auto opens = io.opens, macReads = io.macReads;
    controller.poll(1009);
    assert(console("PAIR STATUS", false, controller, 1009) == "[pair] pending motion=unknown pairing=0\n");
    // STATUS only reads; timeouts are advanced by the runtime poll owner.
    assert(console("PAIR STATUS", false, controller, 1010) == "[pair] pending motion=unknown pairing=0\n");
    controller.poll(1010);
    assert(console("PAIR STATUS", false, controller, 1010) == "[pair] timed_out motion=unknown pairing=0\n");
    assert(io.tx == first && io.opens == opens && io.macReads == macReads);
    assert(console("PAIR DISCOVER bt-brain-controller-test", false, controller, 1010) == "[pair] maintenance_required\n");
    assert(controller.discoveryResult().state == DiscoveryState::TimedOut);
    assert(console("PAIR DISCOVER bt-brain-controller-test", true, controller, 1010) == "[pair] discovery_pending\n");
    controller.poll(1010);
    const Frame retry = onlyDiscovery(Bytes(io.tx.begin() + first.size(), io.tx.end()));
    assert(retry.messageId != firstFrame.messageId && retry.senderBoot == firstFrame.senderBoot);
    assert(retry.length == firstFrame.length && !std::memcmp(retry.payload, firstFrame.payload, retry.length));
    controller.poll(2010);
    assert(controller.discoveryResult().state == DiscoveryState::TimedOut);
    assert(io.opens == opens && io.macReads == macReads && io.mutations == 0);
    discoveryOffline(controller, 2010);
    fake::assertReadOnly();
    std::puts("PASS PAIR maintenance gate, read-only STATUS, exact 1000ms timeout and explicit retry; invalid IDs do not start discovery");
}

void pairedConsoleDoesNotDegradeStatus() {
    Fixture f;
    f.handshake(0);
    const Message status = telemetry("E_CAN_FAULT", 100, 12);
    f.inject(status, 100);
    f.expectStatus(status, 100);
    assert(f.controller.connected(100));
    assert(console("PAIR DISCOVER wrong-device", true, f.controller, 101) == "[pair] discovery_unavailable\n");
    assert(console("PAIR DISCOVER bt-brain-controller-test", false, f.controller, 101) == "[pair] maintenance_required\n");
    assert(console("PAIR STATUS", false, f.controller, 101) == "[pair] idle motion=unknown pairing=0\n");
    assert(console("PAIR DISCOVER bt-brain-controller-test", true, f.controller, 101) == "[pair] discovery_pending\n");
    f.drain(101);
    f.expectStatus(status, 100);
    assert(f.controller.connected(101) && f.controller.hasSnapshot());
    assert(std::strcmp(f.controller.deviceId(), pairing().deviceId) == 0);
    assert(!f.controller.sendIntent(display::DisplayIntent::StartFeeding, 101));
    f.inject(telemetry("NONE", 101, 13), 102);
    f.expect("NONE", 102);
    f.drain(1101);
    assert(console("PAIR STATUS", false, f.controller, 1101) == "[pair] timed_out motion=unknown pairing=0\n");
    f.expect("NONE", 102);
    assert(f.controller.connected(1101) && !f.controller.snapshot().startEnabled);
    std::puts("PASS paired ControllerLink STATUS/cache still updates during discovery and after timeout");
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
    verifiedLocalPairing();
    consoleDiscoveryRoundTrip(DiscoveryPairState::Missing, DiscoveryPairState::Missing);
    consoleDiscoveryRoundTrip(DiscoveryPairState::Missing, DiscoveryPairState::Ready);
    for (DiscoveryPairState fault : {DiscoveryPairState::Corrupt, DiscoveryPairState::IoError,
                                     DiscoveryPairState::IdentityMismatch}) {
        consoleDiscoveryRoundTrip(fault, DiscoveryPairState::Ready);
        consoleDiscoveryRoundTrip(DiscoveryPairState::Missing, fault);
    }
    consoleTimeoutAndRetry();
    pairedConsoleDoesNotDegradeStatus();
    std::puts("PASS production ControllerLink cache/receipt-time suite (SDK I/O fakes only)");
}
