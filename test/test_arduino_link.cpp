#include "BoardLinkArduino.h"
#include "BoardPairingRecord.h"
#include "FakeBoardIo.h"
#include "nvs.h"
#include <ArduinoJson.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace babytech::boardlink;
using namespace babytech::v4;
using fake::io;
using Bytes = std::vector<uint8_t>;

namespace {
constexpr uint64_t kLocalBoot = UINT64_C(0x1234567812345678);
constexpr uint64_t kPeerBoot = 22;

Pairing pairing(Role role = Role::Brain) {
    Pairing result{};
    result.role = role;
    std::strcpy(result.deviceId, "bt-host-arduino_9");
    std::strcpy(result.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(result.localPhysicalId, role == Role::Brain ? "112233445566" : "aabbccddeeff");
    std::strcpy(result.peerPhysicalId, role == Role::Brain ? "aabbccddeeff" : "112233445566");
    return result;
}

Bytes record(const Pairing& value) {
    Bytes bytes(kPairingRecordMaxSize);
    const size_t length = encodePairingRecord(value, bytes.data(), bytes.size());
    assert(length);
    bytes.resize(length);
    return bytes;
}

void setup(Role role = Role::Brain) {
    fake::reset();
    io.blob = record(pairing(role));
    if (role == Role::Motion) io.mac = {{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}};
    io.randomWords = {0x12345678, 0x12345678};
}

void equalFields(const Pairing& left, const Pairing& right) {
    assert(left.role == right.role);
    assert(std::strcmp(left.deviceId, right.deviceId) == 0);
    assert(std::strcmp(left.epoch, right.epoch) == 0);
    assert(std::strcmp(left.localPhysicalId, right.localPhysicalId) == 0);
    assert(std::strcmp(left.peerPhysicalId, right.peerPhysicalId) == 0);
}

void noPollIo(ArduinoBoardLink& adapter) {
    io.rx.assign(600, 0);
    const auto reads = io.byteReads;
    const auto checks = io.availableCalls;
    const auto idle = io.idleChecks;
    const auto room = io.roomChecks;
    const auto writes = io.writeCalls;
    Status status;
    for (uint32_t now : {0u, 250u, 1500u, 5000u}) adapter.poll(now, &status);
    assert(io.rx.size() == 600 && io.byteReads == reads && io.availableCalls == checks);
    assert(io.writeCalls == writes && io.idleChecks == idle && io.roomChecks == room);
    assert(io.tx.empty());
    fake::assertReadOnly();
}

void loadCase(const char* name, const std::function<void()>& configure,
              PairingLoad expected, unsigned queries, unsigned reads, unsigned macReads,
              Role role = Role::Brain) {
    setup(role);
    configure();
    const fake::State initial = io;
    Pairing output = pairing(Role::Motion);
    std::strcpy(output.deviceId, "untouched-output");
    std::array<uint8_t, sizeof(Pairing)> before{};
    std::memcpy(before.data(), &output, sizeof(output));
    assert(loadBoardPairing(role, output) == expected);
    assert(io.opens == 1 && io.queries == queries && io.reads == reads && io.macReads == macReads);
    assert(io.closes == (io.openError == ESP_OK ? 1u : 0u));
    fake::assertReadOnly();
    if (expected == PairingLoad::Ready) {
        Pairing stored;
        assert(decodePairingRecord(io.blob.data(), io.blob.size(), stored));
        equalFields(output, stored);
        assert((io.calls == std::vector<std::string>{"open", "query", "read", "close", "mac"}));
    } else {
        assert(std::memcmp(before.data(), &output, sizeof(output)) == 0);
        // The same storage failure must also gate all serial I/O in begin/poll.
        io = initial;
        ArduinoBoardLink adapter;
        assert(!adapter.begin(role, 44, 43));
        assert(adapter.verifiedPairing() == nullptr);
        assert(adapter.pairingState() == expected);
        assert(!adapter.link().configured() && !adapter.link().freshStatus(0));
        assert(io.begins == 0 && io.rxConfigs == 0 && io.txConfigs == 0);
        assert(io.randomReads == 0);
        noPollIo(adapter);
    }
    std::printf("PASS NVS %s\n", name);
}

void nvsMatrix() {
    loadCase("valid brain", [] {}, PairingLoad::Ready, 1, 1, 1);
    loadCase("valid motion", [] {}, PairingLoad::Ready, 1, 1, 1, Role::Motion);
    loadCase("namespace missing", [] { io.openError = ESP_ERR_NVS_NOT_FOUND; },
             PairingLoad::Missing, 0, 0, 0);
    loadCase("namespace open failure", [] { io.openError = ESP_FAIL; }, PairingLoad::IoError, 0, 0, 0);
    loadCase("record key missing", [] { io.queryError = ESP_ERR_NVS_NOT_FOUND; },
             PairingLoad::Missing, 1, 0, 0);
    for (esp_err_t error : {ESP_FAIL, ESP_ERR_TIMEOUT, ESP_ERR_NVS_TYPE_MISMATCH,
                            ESP_ERR_NVS_INVALID_LENGTH}) {
        loadCase("size query error/type", [=] { io.queryError = error; }, PairingLoad::IoError, 1, 0, 0);
        loadCase("blob read error/type", [=] { io.readError = error; }, PairingLoad::IoError, 1, 1, 0);
    }
    loadCase("key disappears between reads", [] { io.readError = ESP_ERR_NVS_NOT_FOUND; },
             PairingLoad::IoError, 1, 1, 0);
    for (size_t size : {size_t(0), size_t(257), size_t(65536)})
        loadCase("invalid queried length", [=] { io.queryLength = size; }, PairingLoad::Corrupt, 1, 0, 0);
    for (size_t size : {size_t(1), size_t(256)})
        loadCase("in-budget invalid record", [=] { io.blob.resize(size); }, PairingLoad::Corrupt, 1, 1, 0);
    for (int delta : {-1, 1})
        loadCase("length changes between reads", [=] {
            io.readLength = size_t(int(io.blob.size()) + delta);
        }, PairingLoad::IoError, 1, 1, 0);
    loadCase("empty second read", [] { io.readLength = 0; }, PairingLoad::IoError, 1, 1, 0);
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(14),
                          size_t(46), size_t(58), size_t(70)})
        loadCase("corrupt header/CRC/identity", [=] { io.blob[offset] ^= 1; },
                 PairingLoad::Corrupt, 1, 1, 0);
    loadCase("MAC read failure", [] { io.macError = ESP_FAIL; }, PairingLoad::IoError, 1, 1, 1);
    loadCase("local physical ID mismatch", [] { io.mac[5] ^= 1; }, PairingLoad::IdentityMismatch, 1, 1, 1);
    loadCase("role mismatch", [] {
        Pairing other = pairing();
        other.role = Role::Motion;
        io.blob = record(other);
    }, PairingLoad::IdentityMismatch, 1, 1, 1);
    loadCase("max device ID", [] {
        Pairing maximum = pairing();
        std::memset(maximum.deviceId, 'Z', 64);
        io.blob = record(maximum);
    }, PairingLoad::Ready, 1, 1, 1);
}

void assertUartConfigured() {
    assert(io.constructors == 1 && io.begins == 1 && io.boolChecks == 1);
    assert(io.requestedRx == 256 && io.rxConfigs == 1);
    assert(io.txRingSize == 0 && io.txConfigs == 0 && io.requestedTx == SIZE_MAX);
    assert(io.baud == 115200 && io.config == SERIAL_8N1 && io.rxPin == 44 && io.txPin == 43);
    assert(io.randomReads == 2);
    fake::assertReadOnly();
}

void uartStartup() {
    setup();
    {
        ArduinoBoardLink adapter;
        assert(adapter.verifiedPairing() == nullptr);
        noPollIo(adapter);
        assert(adapter.pairingState() == PairingLoad::Missing);
    }
    for (Role role : {Role::Brain, Role::Motion}) {
        setup(role);
        ArduinoBoardLink adapter;
        assert(adapter.begin(role, 44, 43));
        assertUartConfigured();
        assert(adapter.pairingState() == PairingLoad::Ready);
        assert(adapter.verifiedPairing());
        equalFields(*adapter.verifiedPairing(), pairing(role));
        assert(std::strcmp(adapter.deviceId(), pairing(role).deviceId) == 0);
        assert(adapter.link().configured() && !adapter.link().freshStatus(0));
        assert(io.tx.empty());
        const unsigned opens = io.opens;
        assert(!adapter.begin(role, 12, 13, 9600));
        assert(io.opens == opens && io.begins == 1 && io.randomReads == 2);
        assert(adapter.pairingState() == PairingLoad::Ready);
        equalFields(*adapter.verifiedPairing(), pairing(role));
        adapter.poll(0);
        assert(!io.tx.empty());
    }
    for (const auto pins : {std::array<int, 2>{{-1, 43}}, std::array<int, 2>{{44, -1}},
                            std::array<int, 2>{{44, 44}}}) {
        setup();
        ArduinoBoardLink adapter;
        assert(!adapter.begin(Role::Brain, pins[0], pins[1]));
        assert(adapter.pairingState() == PairingLoad::UartError);
        assert(adapter.verifiedPairing());
        equalFields(*adapter.verifiedPairing(), pairing());
        assert(io.randomReads == 0 && io.begins == 0 && io.rxConfigs == 0);
        noPollIo(adapter);
    }
    for (uint32_t baud : {0u, 9600u, 230400u}) {
        setup();
        ArduinoBoardLink adapter;
        assert(!adapter.begin(Role::Brain, 44, 43, baud));
        assert(adapter.pairingState() == PairingLoad::UartError);
        assert(adapter.verifiedPairing());
        equalFields(*adapter.verifiedPairing(), pairing());
        assert(io.randomReads == 0 && io.begins == 0);
        noPollIo(adapter);
    }
    for (size_t rxResult : {size_t(0), size_t(255), size_t(257)}) {
        setup();
        io.rxResult = rxResult;
        ArduinoBoardLink adapter;
        assert(!adapter.begin(Role::Brain, 44, 43));
        assert(adapter.pairingState() == PairingLoad::UartError);
        assert(adapter.verifiedPairing());
        equalFields(*adapter.verifiedPairing(), pairing());
        assert(io.rxConfigs == 1 && io.txConfigs == 0 && io.begins == 0);
        noPollIo(adapter);
    }
    setup();
    {
        io.serialReady = false;
        ArduinoBoardLink adapter;
        assert(!adapter.begin(Role::Brain, 44, 43));
        assertUartConfigured();
        assert(adapter.pairingState() == PairingLoad::UartError);
        assert(adapter.verifiedPairing());
        equalFields(*adapter.verifiedPairing(), pairing());
        noPollIo(adapter);
    }
    setup();
    {
        io.randomWords = {0, 0};
        ArduinoBoardLink adapter;
        assert(!adapter.begin(Role::Brain, 44, 43));
        assert(adapter.pairingState() == PairingLoad::IoError);
        assert(adapter.verifiedPairing());
        equalFields(*adapter.verifiedPairing(), pairing());
        assert(io.rxConfigs == 0 && io.begins == 0);
        noPollIo(adapter);
    }
    for (const auto words : {std::array<uint32_t, 2>{{0, 1}}, std::array<uint32_t, 2>{{1, 0}}}) {
        setup();
        io.randomWords = {words[0], words[1]};
        ArduinoBoardLink adapter;
        assert(adapter.begin(Role::Brain, 44, 43));
    }
    std::puts("PASS UART begin, default TX ring 0, RX256, bool failure, guards and no-TX failures");
}

Bytes wire(const Message& message) {
    Bytes bytes;
    size_t offset = 0;
    do {
        Frame frame;
        assert(fragment(message, offset, frame));
        uint8_t buffer[kMaxFrame];
        const size_t size = encode(frame, buffer, sizeof(buffer));
        assert(size);
        bytes.insert(bytes.end(), buffer, buffer + size);
        offset += frame.length;
    } while (offset < message.length);
    return bytes;
}

std::vector<Message> messages(const Bytes& bytes) {
    Parser parser;
    Assembler assembler;
    std::vector<Message> result;
    Frame frame;
    Message message;
    for (uint8_t byte : bytes) {
        if (!parser.push(byte, 0, frame)) continue;
        if (frame.kind == Kind::Heartbeat) {
            Message heartbeat;
            heartbeat.kind = frame.kind;
            heartbeat.senderBoot = frame.senderBoot;
            heartbeat.receiverBoot = frame.receiverBoot;
            heartbeat.messageId = frame.messageId;
            result.push_back(heartbeat);
        } else if (assembler.accept(frame, 0, message) == AssemblyResult::Complete) {
            result.push_back(message);
        }
    }
    return result;
}

Message expectedHello() {
    Session session;
    assert(session.begin(pairing(), kLocalBoot));
    Message hello;
    assert(encodeHello(session.localHello(), hello));
    hello.senderBoot = kLocalBoot;
    hello.receiverBoot = 0;
    hello.messageId = 1;
    return hello;
}

void rxBudget(bool enableDiscovery = false, bool missing = false) {
    setup();
    if (missing) io.openError = ESP_ERR_NVS_NOT_FOUND;
    ArduinoBoardLink adapter;
    assert(adapter.begin(Role::Brain, 44, 43, 115200, enableDiscovery));
    io.writeRoom = 0;
    io.rx.assign(600, 0);
    adapter.poll(0);
    assert(io.byteReads == 256 && io.rx.size() == 344 && io.availableCalls == 256);
    adapter.poll(1);
    assert(io.byteReads == 512 && io.rx.size() == 88);
    adapter.poll(2);
    assert(io.byteReads == 600 && io.rx.empty());
    io.rx.assign(3, 0);
    io.negativeRead = true;
    adapter.poll(3);
    assert(io.byteReads == 601 && io.rx.size() == 3);
    adapter.poll(4);
    assert(io.byteReads == 604 && io.rx.empty());
    assert(io.tx.empty() && !adapter.link().freshStatus(4));
    std::puts("PASS poll RX budget 256 and negative-read break");
}

void txBackpressure() {
    setup();
    ArduinoBoardLink adapter;
    assert(adapter.begin(Role::Brain, 44, 43));
    const Bytes expected = wire(expectedHello());
    io.idleResult = ESP_ERR_TIMEOUT;
    adapter.poll(0);
    assert(io.idleChecks == 1 && io.roomChecks == 0 && io.writeCalls == 0);
    io.idleResult = ESP_FAIL;
    adapter.poll(1);
    assert(io.writeCalls == 0);
    io.idleResult = ESP_OK;
    for (int room : {0, -1}) {
        io.writeRoom = room;
        adapter.poll(2);
        assert(io.writeCalls == 0 && adapter.link().healthy());
    }
    io.writeRoom = 512;
    io.maxWrite = 0;
    adapter.poll(3);
    assert(io.writeCalls == 1 && io.tx.empty());
    io.idleResult = ESP_ERR_TIMEOUT;
    adapter.poll(4);
    assert(io.writeCalls == 1);  // Zero-byte write must recheck idle next time.
    io.idleResult = ESP_OK;
    io.maxWrite = SIZE_MAX;
    io.roomScript = {64, 0};
    adapter.poll(5);
    assert(io.tx.empty() && io.writeRequests.back() == 0);
    io.roomScript = {64, 3};
    adapter.poll(6);
    assert(io.tx.size() == 3 && io.writeRequests.back() == 3);
    assert(std::equal(io.tx.begin(), io.tx.end(), expected.begin()));

    // A partial frame must continue, even while UART is not yet idle, and
    // finish before another frame is admitted. Every short write keeps bytes.
    io.idleResult = ESP_ERR_TIMEOUT;
    io.writeRoom = 19;
    io.maxWrite = 7;
    Frame first;
    assert(fragment(expectedHello(), 0, first));
    const size_t firstSize = kHeaderSize + first.length + 2;
    const unsigned checks = io.idleChecks;
    for (unsigned poll = 0; io.tx.size() < firstSize && poll < 100; ++poll) {
        adapter.poll(7);
        assert(io.idleChecks == checks);
        assert(std::equal(io.tx.begin(), io.tx.end(), expected.begin()));
    }
    assert(io.tx.size() == firstSize);
    const unsigned writes = io.writeCalls;
    adapter.poll(8);
    assert(io.idleChecks == checks + 1 && io.writeCalls == writes);
    io.idleResult = ESP_OK;
    for (unsigned poll = 0; io.tx.size() < expected.size() && poll < 100; ++poll)
        adapter.poll(9);
    assert(io.tx == expected && adapter.link().healthy());
    const auto emitted = messages(io.tx);
    assert(emitted.size() == 1 && emitted[0].kind == Kind::Hello);
    Hello hello;
    assert(decodeHello(emitted[0], hello));
    assert(std::strcmp(hello.deviceId, pairing().deviceId) == 0);
    assert(std::strcmp(hello.physicalId, pairing().localPhysicalId) == 0);
    assert(std::strcmp(hello.epoch, pairing().epoch) == 0 && hello.role == Role::Brain);
    assert(hello.replyTo == 0 && emitted[0].senderBoot == kLocalBoot);
    fake::assertReadOnly();
    std::puts("PASS UART idle, zero/negative/shrinking room, zero/short writes and exact HELLO bytes");
}

void invalidWriteCount() {
    setup();
    ArduinoBoardLink adapter;
    assert(adapter.begin(Role::Brain, 44, 43));
    io.overreportWrite = true;
    adapter.poll(0);
    assert(!adapter.link().healthy());
    const auto writes = io.writeCalls;
    adapter.poll(1);
    assert(io.writeCalls == writes && !adapter.link().freshStatus(1));
    std::puts("PASS impossible write count fails closed");
}

void inject(ArduinoBoardLink& adapter, const Message& message, uint32_t now) {
    const Bytes bytes = wire(message);
    io.rx.insert(io.rx.end(), bytes.begin(), bytes.end());
    unsigned loops = 0;
    while (!io.rx.empty() && ++loops <= 16) {
        const unsigned reads = io.byteReads;
        adapter.poll(now);
        assert(io.byteReads - reads <= 256);
    }
    assert(io.rx.empty());
}

Message peerHello(Kind kind, uint32_t reply, uint32_t id) {
    Session peer;
    assert(peer.begin(pairing(Role::Motion), kPeerBoot));
    Hello hello = peer.localHello();
    hello.replyTo = reply;
    Message message;
    assert(encodeHello(hello, message, kind));
    message.senderBoot = kPeerBoot;
    message.receiverBoot = kind == Kind::Hello ? 0 : kLocalBoot;
    message.messageId = id;
    return message;
}

Message heartbeat(uint32_t id) {
    Message message;
    message.kind = Kind::Heartbeat;
    message.senderBoot = kPeerBoot;
    message.receiverBoot = kLocalBoot;
    message.messageId = id;
    return message;
}

Message peerStatus(uint32_t sample, uint32_t id) {
    Status status;
    status.sampleUptimeMs = sample;
    status.contextVersion = 77;
    status.snapshot.stage = babytech::display::DisplayStage::Ready;
    status.snapshot.startEnabled = true;
    status.snapshot.waterMl = 150;
    status.stationary = true;
    std::strcpy(status.productProgress, "ready");
    status.lowWaterValid = status.powderValid = true;
    status.powderGrams = 275;
    status.actuatorOperational = status.actuatorConfigValid = true;
    status.actuatorBusHealthy = status.actuatorPositionReferenced = true;
    status.executionAuthorized = status.feedingContextConfigured = true;
    std::memset(status.babyId, 'b', sizeof(status.babyId) - 1);
    Message message;
    assert(encodeStatus(status, message));
    message.senderBoot = kPeerBoot;
    message.receiverBoot = kLocalBoot;
    message.messageId = id;
    return message;
}

void drainTx(ArduinoBoardLink& adapter, uint32_t now) {
    for (unsigned i = 0; i < 20; ++i) adapter.poll(now);
}

std::vector<Frame> frames(const Bytes& bytes) {
    Parser parser;
    Frame frame;
    Bytes reconstructed;
    std::vector<Frame> result;
    for (uint8_t byte : bytes) {
        if (!parser.push(byte, 0, frame)) continue;
        uint8_t encoded[kMaxFrame];
        const size_t size = encode(frame, encoded, sizeof(encoded));
        assert(size);
        reconstructed.insert(reconstructed.end(), encoded, encoded + size);
        result.push_back(frame);
    }
    // A permissive parser alone would hide dropped/interleaved UART bytes.
    assert(reconstructed == bytes);
    return result;
}

Bytes discoveryBytes(const Bytes& bytes) {
    Bytes result;
    for (const auto& frame : frames(bytes)) {
        if (frame.kind != Kind::Discovery) continue;
        assert(!isControl(frame.kind) && frame.offset == 0 && frame.length == frame.total);
        uint8_t encoded[kMaxFrame];
        const size_t size = encode(frame, encoded, sizeof(encoded));
        result.insert(result.end(), encoded, encoded + size);
    }
    return result;
}

void storageState(PairingLoad state) {
    switch (state) {
        case PairingLoad::Ready: break;
        case PairingLoad::Missing: io.openError = ESP_ERR_NVS_NOT_FOUND; break;
        case PairingLoad::Corrupt: io.blob[0] ^= 1; break;
        case PairingLoad::IoError: io.openError = ESP_FAIL; break;
        case PairingLoad::IdentityMismatch: io.mac[5] ^= 1; break;
        default: assert(false);
    }
}

const char* physical(Role role, PairingLoad state) {
    if (role == Role::Brain)
        return state == PairingLoad::IdentityMismatch ? "112233445567" : "112233445566";
    return state == PairingLoad::IdentityMismatch ? "aabbccddeefe" : "aabbccddeeff";
}

void pairingBoundary(const ArduinoBoardLink& adapter, Role role, PairingLoad state, uint32_t now) {
    assert(adapter.pairingState() == state);
    assert(bool(adapter.verifiedPairing()) == (state == PairingLoad::Ready));
    assert(adapter.link().configured() == (state == PairingLoad::Ready));
    if (state == PairingLoad::Ready) {
        equalFields(*adapter.verifiedPairing(), pairing(role));
        assert(std::strcmp(adapter.deviceId(), pairing(role).deviceId) == 0);
    } else {
        assert(!adapter.deviceId()[0] && !adapter.link().healthy());
    }
    assert(!adapter.link().connected(now) && !adapter.link().freshStatus(now));
    assert(!adapter.link().peerStatus().snapshot.startEnabled);
    assert(!adapter.link().peerStatus().executionAuthorized);
}

// HardwareSerial stubs use one global I/O state. Switch only that state, never
// copy either production adapter/session/discovery object between boards.
template<class Action>
void onBoard(fake::State& state, Action action) {
    fake::assertReadOnly();
    io = state;
    action();
    fake::assertReadOnly();
    state = io;
}

void discoveryRoundTrips() {
    constexpr PairingLoad states[] = {PairingLoad::Missing, PairingLoad::Ready,
        PairingLoad::Corrupt, PairingLoad::IoError, PairingLoad::IdentityMismatch};
    constexpr DiscoveryPairState diagnostics[] = {DiscoveryPairState::Missing,
        DiscoveryPairState::Ready, DiscoveryPairState::Corrupt,
        DiscoveryPairState::IoError, DiscoveryPairState::IdentityMismatch};
    constexpr auto found = DiscoveryState::Found;
    constexpr auto unavailable = DiscoveryState::Unavailable;
    constexpr DiscoveryState outcomes[5][5] = {
        {found, found, unavailable, unavailable, unavailable},
        {DiscoveryState::Conflict, found, unavailable, unavailable, unavailable},
        {unavailable, unavailable, unavailable, unavailable, unavailable},
        {unavailable, unavailable, unavailable, unavailable, unavailable},
        {unavailable, unavailable, unavailable, unavailable, unavailable},
    };
    for (unsigned b = 0; b < 5; ++b) {
        for (unsigned m = 0; m < 5; ++m) {
            setup();
            storageState(states[b]);
            ArduinoBoardLink brain;
            assert(brain.begin(Role::Brain, 44, 43, 115200, true));
            assertUartConfigured();
            assert(io.macReads == (states[b] == PairingLoad::IdentityMismatch ? 2u : 1u));
            pairingBoundary(brain, Role::Brain, states[b], 0);
            assert(brain.requestDiscovery(pairing().deviceId, 0));
            assert(!brain.requestDiscovery(pairing().deviceId, 0));
            drainTx(brain, 0);
            const Bytes query = discoveryBytes(io.tx);
            const auto queryFrames = frames(query);
            assert(queryFrames.size() == 1 && query[5] == 17);
            const auto& q = queryFrames[0];
            assert(q.senderBoot == kLocalBoot && !q.receiverBoot && q.messageId == 1);
            assert(q.payload[0] == 1 && q.payload[1] == std::strlen(pairing().deviceId));
            assert(q.length == 14 + std::strlen(pairing().deviceId));
            assert(!std::memcmp(q.payload + 2, pairing().deviceId, q.payload[1]));
            assert(!std::memcmp(q.payload + 2 + q.payload[1], physical(Role::Brain, states[b]), 12));
            if (states[b] != PairingLoad::Ready) assert(io.tx == query);
            fake::State brainIo = io;

            setup(Role::Motion);
            storageState(states[m]);
            io.randomWords = {0, uint32_t(kPeerBoot)};
            ArduinoBoardLink motion;
            assert(motion.begin(Role::Motion, 44, 43, 115200, true));
            assertUartConfigured();
            assert(io.macReads == (states[m] == PairingLoad::IdentityMismatch ? 2u : 1u));
            pairingBoundary(motion, Role::Motion, states[m], 0);
            assert(!motion.requestDiscovery(pairing().deviceId, 0));
            // Deliver the actual encoded query in two RX chunks, not a core receive call.
            io.rx.insert(io.rx.end(), query.begin(), query.end() - 1);
            motion.poll(10);
            assert(discoveryBytes(io.tx).empty());
            io.rx.push_back(query.back());
            Status local;
            local.sampleUptimeMs = 10;
            local.executionAuthorized = local.snapshot.startEnabled = true;
            for (unsigned i = 0; i < 20; ++i) motion.poll(10, &local);
            const Bytes reply = discoveryBytes(io.tx);
            const auto replyFrames = frames(reply);
            assert(replyFrames.size() == 1);
            const auto& r = replyFrames[0];
            assert(r.senderBoot == kPeerBoot && r.receiverBoot == kLocalBoot);
            assert(r.messageId == q.messageId && r.payload[0] == 2);
            assert(!std::memcmp(r.payload + 1, physical(Role::Motion, states[m]), 12));
            assert(r.payload[13] == uint8_t(diagnostics[m]));
            if (states[m] == PairingLoad::Ready) {
                Pairing peer;
                assert(decodePairingRecord(r.payload + 15, r.payload[14], peer));
                equalFields(peer, pairing(Role::Motion));
            } else {
                assert(r.length == 15 && r.payload[14] == 0);
                assert(io.tx == reply);
            }
            pairingBoundary(motion, Role::Motion, states[m], 10);
            fake::State motionIo = io;

            onBoard(brainIo, [&] {
                io.rx.insert(io.rx.end(), reply.begin(), reply.end());
                brain.poll(20);
                const auto& result = brain.discoveryResult();
                assert(result.state == outcomes[b][m] && result.peerBoot == kPeerBoot);
                assert(result.pairingState == diagnostics[m]);
                assert(std::strcmp(result.physicalId, physical(Role::Motion, states[m])) == 0);
                if (states[m] == PairingLoad::Ready) equalFields(result.pairing, pairing(Role::Motion));
                else assert(!result.pairing.deviceId[0]);
                // Discovery is not a HELLO_ACK, normal status or execution permission.
                inject(brain, peerHello(Kind::HelloAck, 1, 40), 21);
                inject(brain, heartbeat(41), 21);
                inject(brain, peerStatus(21, 42), 21);
                if (states[b] != PairingLoad::Ready)
                    pairingBoundary(brain, Role::Brain, states[b], 21);
                assert(io.blob == brainIo.blob && io.mutations == 0);
            });
            onBoard(motionIo, [&] {
                assert(io.blob == motionIo.blob && io.mutations == 0);
                pairingBoundary(motion, Role::Motion, states[m], 21);
            });
            std::printf("PASS actual UART discovery brain=%u motion=%u diagnostic=%u outcome=%u\n",
                        b, m, unsigned(diagnostics[m]), unsigned(outcomes[b][m]));
        }
    }
}

void discoveryTxArbitration() {
    for (bool paired : {false, true}) {
        setup();
        if (!paired) storageState(PairingLoad::Missing);
        ArduinoBoardLink adapter;
        assert(adapter.begin(Role::Brain, 44, 43, 115200, true));
        Bytes helloBytes;
        if (paired) {
            helloBytes = wire(expectedHello());
            io.maxWrite = 7;
            adapter.poll(0);
            assert(io.tx.size() == 7);
        }
        assert(adapter.requestDiscovery(pairing().deviceId, 1));
        io.maxWrite = 7;
        io.writeRoom = 19;
        if (paired) {
            io.idleResult = ESP_ERR_TIMEOUT;
            const unsigned checks = io.idleChecks;
            Frame first;
            assert(fragment(expectedHello(), 0, first));
            const size_t size = kHeaderSize + first.length + 2;
            for (unsigned i = 0; io.tx.size() < size && i < 100; ++i) {
                adapter.poll(1);
                assert(io.idleChecks == checks);
                assert(std::equal(io.tx.begin(), io.tx.end(), helloBytes.begin()));
            }
            assert(io.tx.size() == size);
            const size_t sent = io.tx.size();
            adapter.poll(2);
            assert(io.tx.size() == sent);
            io.idleResult = ESP_OK;
            for (unsigned i = 0; io.tx.size() < helloBytes.size() && i < 100; ++i) adapter.poll(2);
            assert(io.tx == helloBytes);
        }
        for (esp_err_t error : {ESP_ERR_TIMEOUT, ESP_FAIL}) {
            io.idleResult = error;
            adapter.poll(3);
            assert(io.tx == helloBytes);
        }
        io.idleResult = ESP_OK;
        for (int room : {0, -1}) {
            io.writeRoom = room;
            adapter.poll(4);
            assert(io.tx == helloBytes);
        }
        io.writeRoom = 512;
        io.maxWrite = 0;
        adapter.poll(5);
        assert(io.tx == helloBytes);
        io.roomScript = {64, 0};
        io.maxWrite = SIZE_MAX;
        adapter.poll(6);
        assert(io.tx == helloBytes && io.writeRequests.back() == 0);
        io.roomScript = {64, 3};
        adapter.poll(7);
        assert(io.tx.size() == helloBytes.size() + 3);
        io.maxWrite = 7;
        io.writeRoom = 19;
        io.idleResult = ESP_ERR_TIMEOUT;
        const unsigned checks = io.idleChecks;
        const size_t querySize = kHeaderSize + 14 + std::strlen(pairing().deviceId) + 2;
        for (unsigned i = 0; io.tx.size() < helloBytes.size() + querySize && i < 100; ++i) {
            adapter.poll(8);
            assert(io.idleChecks == checks);
        }
        assert(io.tx.size() == helloBytes.size() + querySize);
        const Bytes query = discoveryBytes(io.tx);
        assert(frames(query).size() == 1 && query.size() == querySize);
        assert(std::equal(helloBytes.begin(), helloBytes.end(), io.tx.begin()));
        const auto writes = io.writeCalls;
        adapter.poll(9);
        assert(io.writeCalls == writes);
        assert(adapter.discoveryResult().state == DiscoveryState::Pending);
        assert(io.begins == 1 && io.constructors == 1 && io.txConfigs == 0);
        fake::assertReadOnly();
    }
    std::puts("PASS discovery shares ordinary TX slot with HELLO; idle/room/zero/short writes never interleave frames");
}

void discoveryStartupFailures() {
    for (bool paired : {false, true}) {
        for (unsigned failure = 0; failure < 10; ++failure) {
            setup();
            if (!paired) storageState(PairingLoad::Missing);
            int rx = 44, tx = 43;
            uint32_t baud = 115200;
            if (failure == 0) rx = -1;
            if (failure == 1) tx = -1;
            if (failure == 2) tx = rx;
            if (failure == 3) baud = 9600;
            if (failure == 4) io.randomWords = {0, 0};
            if (failure == 5) io.macError = ESP_FAIL;
            if (failure == 6) io.rxResult = 0;
            if (failure == 7) io.rxResult = 255;
            if (failure == 8) io.rxResult = 257;
            if (failure == 9) io.serialReady = false;
            ArduinoBoardLink adapter;
            assert(!adapter.begin(Role::Brain, rx, tx, baud, true));
            const bool verified = paired && failure != 5;
            assert(bool(adapter.verifiedPairing()) == verified);
            if (verified) {
                equalFields(*adapter.verifiedPairing(), pairing());
                assert(std::strcmp(adapter.deviceId(), pairing().deviceId) == 0);
            } else assert(!adapter.deviceId()[0]);
            assert(adapter.pairingState() == (failure == 4 || failure == 5
                ? PairingLoad::IoError : PairingLoad::UartError));
            assert(!adapter.requestDiscovery(pairing().deviceId, 0));
            noPollIo(adapter);
        }
        for (const auto words : {std::array<uint32_t, 2>{{0, 1}}, std::array<uint32_t, 2>{{1, 0}}}) {
            setup();
            if (!paired) storageState(PairingLoad::Missing);
            io.randomWords = {words[0], words[1]};
            ArduinoBoardLink adapter;
            assert(adapter.begin(Role::Brain, 44, 43, 115200, true));
            assert(adapter.requestDiscovery(pairing().deviceId, 0));
            drainTx(adapter, 0);
            const auto emitted = frames(discoveryBytes(io.tx));
            assert(emitted.size() == 1);
            assert(emitted[0].senderBoot == ((uint64_t(words[0]) << 32) | words[1]));
            fake::assertReadOnly();
        }
    }
    setup();
    storageState(PairingLoad::Missing);
    io.mac.fill(0);
    ArduinoBoardLink zeroMac;
    assert(!zeroMac.begin(Role::Brain, 44, 43, 115200, true));
    assert(zeroMac.pairingState() == PairingLoad::IoError && !zeroMac.verifiedPairing());
    assert(io.begins == 0 && io.rxConfigs == 0);
    noPollIo(zeroMac);
    std::puts("PASS discovery startup MAC/random/pin/baud/RX/bool failures remain read-only; SDK failure preserves verified pair");
}

void discoveryReplyShortWrites() {
    setup();
    storageState(PairingLoad::Missing);
    ArduinoBoardLink brain;
    assert(brain.begin(Role::Brain, 44, 43, 115200, true));
    assert(brain.requestDiscovery(pairing().deviceId, 0));
    drainTx(brain, 0);
    Bytes query = io.tx;
    assert(frames(query).size() == 1);
    fake::State brainIo = io;
    for (bool paired : {false, true}) {
        setup(Role::Motion);
        if (!paired) storageState(PairingLoad::Missing);
        io.randomWords = {0, uint32_t(kPeerBoot)};
        ArduinoBoardLink motion;
        assert(motion.begin(Role::Motion, 44, 43, 115200, true));
        drainTx(motion, 0);
        io.tx.clear();
        io.rx.insert(io.rx.end(), query.begin(), query.end());
        io.idleResult = ESP_ERR_TIMEOUT;
        motion.poll(10);
        assert(io.rx.empty() && io.tx.empty());
        io.idleResult = ESP_OK;
        io.maxWrite = 0;
        motion.poll(11);
        assert(io.tx.empty());
        io.maxWrite = 3;
        motion.poll(12);
        assert(io.tx.size() == 3);
        io.idleResult = ESP_ERR_TIMEOUT;
        io.maxWrite = 7;
        io.writeRoom = 19;
        const unsigned checks = io.idleChecks;
        const size_t replySize = kHeaderSize + 15 + (paired ? record(pairing(Role::Motion)).size() : 0) + 2;
        for (unsigned i = 0; io.tx.size() < replySize && i < 100; ++i) {
            motion.poll(13);
            assert(io.idleChecks == checks);
        }
        const Bytes reply = io.tx;
        assert(reply.size() == replySize);
        const auto emitted = frames(reply);
        assert(emitted.size() == 1 && emitted[0].kind == Kind::Discovery);
        assert(emitted[0].payload[13] == uint8_t(paired ? DiscoveryPairState::Ready : DiscoveryPairState::Missing));
        assert(io.constructors == 1 && io.begins == 1 && io.txConfigs == 0);
        onBoard(brainIo, [&] {
            io.rx.insert(io.rx.end(), reply.begin(), reply.end());
            brain.poll(paired ? 40 : 20);
            assert(brain.discoveryResult().state == DiscoveryState::Found);
            assert(uint8_t(brain.discoveryResult().pairingState) == emitted[0].payload[13]);
        });
        if (!paired) {
            onBoard(brainIo, [&] {
                // Bind the second round trip to a new actual Brain query.
                assert(brain.requestDiscovery(pairing().deviceId, 30));
                io.tx.clear();
                drainTx(brain, 30);
                query = io.tx;
            });
        }
    }
    std::puts("PASS Motion discovery reply uses same idle/short-write transmitter and decodes at actual Brain RX");
}

void failedInitRetry() {
    for (bool enableDiscovery : {false, true}) {
        for (PairingLoad state : {PairingLoad::Missing, PairingLoad::Corrupt,
                                 PairingLoad::IoError, PairingLoad::IdentityMismatch}) {
            for (bool rxFailure : {false, true}) {
                setup();
                if (rxFailure) io.rxResult = 255;
                else io.serialReady = false;
                ArduinoBoardLink adapter;
                assert(!adapter.begin(Role::Brain, 44, 43, 115200, true));
                assert(adapter.verifiedPairing());
                equalFields(*adapter.verifiedPairing(), pairing());
                assert(!adapter.requestDiscovery(pairing().deviceId, 0));
                noPollIo(adapter);

                // Reset only the simulated SDK state to permit another begin;
                // retain the failed production object to exercise its retry path.
                setup();
                storageState(state);
                io.randomWords = {0, 77};
                const Bytes stored = io.blob;
                assert(adapter.begin(Role::Brain, 44, 43, 115200, enableDiscovery) == enableDiscovery);
                assert(!adapter.verifiedPairing() && !adapter.deviceId()[0]);
                assert(adapter.discoveryResult().state == DiscoveryState::Idle);
                if (enableDiscovery) {
                    assert(adapter.requestDiscovery(pairing().deviceId, 20));
                    drainTx(adapter, 20);
                    assert(io.tx == discoveryBytes(io.tx));
                    const auto emitted = frames(io.tx);
                    assert(emitted.size() == 1 && emitted[0].senderBoot == 77);
                    pairingBoundary(adapter, Role::Brain, state, 20);
                } else {
                    assert(!adapter.requestDiscovery(pairing().deviceId, 20));
                    noPollIo(adapter);
                }
                assert(!adapter.link().configured());
                assert(io.blob == stored);
                fake::assertReadOnly();
            }
        }
    }
    std::puts("PASS failed SDK init retry cannot retain old verified pair/device/discovery or transmit old HELLO");
}

void readOnlySession(bool enableDiscovery = false) {
    setup();
    ArduinoBoardLink adapter;
    assert(adapter.begin(Role::Brain, 44, 43, 115200, enableDiscovery));
    drainTx(adapter, 0);
    assert(io.tx == wire(expectedHello()));
    inject(adapter, peerStatus(1, 9), 1);
    assert(!adapter.link().freshStatus(1));

    // Deliver valid peer discovery; it is answered but cannot establish trust.
    inject(adapter, peerHello(Kind::Hello, 0, 10), 10);
    drainTx(adapter, 10);
    assert(!adapter.link().connected(10));
    bool sawAck = false;
    for (const auto& message : messages(io.tx)) {
        if (message.kind != Kind::HelloAck) continue;
        Hello hello;
        assert(decodeHello(message, hello));
        assert(hello.replyTo == 10 && message.receiverBoot == kPeerBoot);
        sawAck = true;
    }
    assert(sawAck);

    // Wrong identity, wrong reply and wrong receiver cannot consume our probe.
    for (unsigned field = 0; field < 4; ++field) {
        Message bad = peerHello(Kind::HelloAck, 1, 11);
        Hello hello;
        assert(decodeHello(bad, hello));
        if (field == 0) std::strcpy(hello.deviceId, "wrong-device");
        if (field == 1) hello.epoch[0] = 'f';
        if (field == 2) hello.physicalId[0] = '0';
        if (field == 3) hello.role = Role::Brain;
        Message encoded;
        assert(encodeHello(hello, encoded, Kind::HelloAck));
        encoded.senderBoot = bad.senderBoot;
        encoded.receiverBoot = bad.receiverBoot;
        encoded.messageId = bad.messageId;
        const size_t sent = io.tx.size();
        inject(adapter, encoded, 20);
        drainTx(adapter, 20);
        assert(io.tx.size() == sent && !adapter.link().connected(20));
    }
    inject(adapter, peerHello(Kind::HelloAck, 999, 11), 20);
    Message wrongBoot = peerHello(Kind::HelloAck, 1, 11);
    ++wrongBoot.receiverBoot;
    inject(adapter, wrongBoot, 20);
    inject(adapter, heartbeat(12), 20);
    inject(adapter, peerStatus(20, 13), 20);
    assert(!adapter.link().connected(20) && !adapter.link().freshStatus(20));

    inject(adapter, peerHello(Kind::HelloAck, 1, 11), 30);
    inject(adapter, heartbeat(12), 31);
    inject(adapter, peerStatus(32, 13), 32);
    drainTx(adapter, 32);
    assert(adapter.link().connected(32) && adapter.link().freshStatus(32));
    assert(adapter.link().peerStatus().contextVersion == 77);
    assert(adapter.link().peerStatus().snapshot.waterMl == 150);
    assert(adapter.link().peerStatus().snapshot.stage == babytech::display::DisplayStage::Ready);
    assert(!adapter.link().peerStatus().snapshot.startEnabled);
    const auto& observed = adapter.link().peerStatus();
    assert(std::strcmp(observed.productProgress, "ready") == 0);
    assert(std::strcmp(observed.productError, "NONE") == 0);
    assert(!observed.isPreparing && observed.lowWaterValid && !observed.lowWater);
    assert(observed.powderValid && observed.powderGrams == 275);
    assert(observed.actuatorOperational && observed.actuatorConfigValid);
    assert(observed.actuatorBusHealthy && observed.actuatorPositionReferenced);
    assert(observed.executionAuthorized && observed.feedingContextConfigured);
    assert(std::strlen(observed.babyId) == 96);
    for (size_t i = 0; i < 96; ++i) assert(observed.babyId[i] == 'b');
    bool sawHeartbeat = false, sawStatusAck = false;
    for (const auto& message : messages(io.tx)) {
        if (message.kind == Kind::Heartbeat) sawHeartbeat = true;
        if (message.kind == Kind::LinkAck) {
            StaticJsonDocument<96> doc;
            assert(!deserializeJson(doc, message.payload, message.length));
            if (doc["message_id"].as<uint32_t>() == 13) sawStatusAck = true;
        }
    }
    assert(sawHeartbeat && sawStatusAck);

    if (enableDiscovery) {
        io.tx.clear();
        assert(adapter.requestDiscovery(pairing().deviceId, 33));
        io.maxWrite = 7;
        adapter.poll(33);
        assert(io.tx.size() == 7);
        io.idleResult = ESP_ERR_TIMEOUT;
        const unsigned checks = io.idleChecks;
        inject(adapter, peerStatus(33, 14), 33);
        assert(adapter.link().peerStatus().sampleUptimeMs == 33);
        const size_t querySize = kHeaderSize + 14 + std::strlen(pairing().deviceId) + 2;
        for (unsigned i = 0; io.tx.size() < querySize && i < 100; ++i) adapter.poll(33);
        assert(io.tx.size() == querySize && io.idleChecks == checks);
        const auto queryFrames = frames(io.tx);
        assert(queryFrames.size() == 1 && queryFrames[0].kind == Kind::Discovery);
        adapter.poll(33);
        assert(io.tx.size() == querySize);  // Queued STATUS ACK cannot splice into discovery.
        io.idleResult = ESP_OK;
        io.maxWrite = SIZE_MAX;
        drainTx(adapter, 33);
        const auto sent = frames(io.tx);
        assert(sent.size() == 2 && sent[1].kind == Kind::LinkAck);
        assert(adapter.link().connected(33) && adapter.link().freshStatus(33));
        assert(adapter.discoveryResult().state == DiscoveryState::Pending);
        std::puts("PASS paired STATUS RX/ACK remains live during partial discovery TX, with no frame-byte interleaving");
    }

    Message before;
    assert(encodeStatus(adapter.link().peerStatus(), before));
    uint32_t id = 50;
    for (Kind kind : {Kind::Command, Kind::Stop, Kind::Context, Kind::CloudReceipt, Kind::Terminal}) {
        Message action;
        action.kind = kind;
        action.senderBoot = kPeerBoot;
        action.receiverBoot = kLocalBoot;
        action.messageId = id++;
        const char payload[] = "{\"action\":\"start\"}";
        action.length = sizeof(payload) - 1;
        std::memcpy(action.payload, payload, action.length);
        const size_t sent = io.tx.size();
        inject(adapter, action, 40);
        drainTx(adapter, 40);
        const auto replies = messages(Bytes(io.tx.begin() + sent, io.tx.end()));
        assert(replies.size() == 1 && replies[0].kind == Kind::LinkReject);
        StaticJsonDocument<96> doc;
        assert(!deserializeJson(doc, replies[0].payload, replies[0].length));
        assert(doc["message_id"].as<uint32_t>() == action.messageId);
        Message after;
        assert(encodeStatus(adapter.link().peerStatus(), after));
        assert(after.length == before.length && !std::memcmp(after.payload, before.payload, before.length));
        assert(!adapter.link().peerStatus().snapshot.startEnabled);
    }
    Message stale = peerStatus(100, 100);
    ++stale.senderBoot;
    inject(adapter, stale, 41);
    assert(adapter.link().peerStatus().sampleUptimeMs == (enableDiscovery ? 33u : 32u));
    // Real heartbeat keeps the session alive but does not refresh old status.
    inject(adapter, heartbeat(101), 1400);
    drainTx(adapter, 1400);
    const uint32_t expiresAt = enableDiscovery ? 1533 : 1532;
    adapter.poll(expiresAt);
    assert(adapter.link().connected(expiresAt) && !adapter.link().freshStatus(expiresAt));
    fake::assertReadOnly();
    std::puts("PASS real HELLO/ACK/heartbeat/STATUS UART session, wrong identity and no-action rejects");
}
}  // namespace

int main() {
    nvsMatrix();
    uartStartup();
    rxBudget();
    txBackpressure();
    invalidWriteCount();
    readOnlySession();
    readOnlySession(true);
    rxBudget(true);
    rxBudget(true, true);
    discoveryRoundTrips();
    discoveryTxArbitration();
    discoveryStartupFailures();
    discoveryReplyShortWrites();
    failedInitRetry();
    fake::assertReadOnly();
    std::puts("PASS Arduino adapter host suite (real core + pair codec; I/O fakes only)");
}
