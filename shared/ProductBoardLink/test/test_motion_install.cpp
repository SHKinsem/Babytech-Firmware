#include "MotionInstallTarget.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
namespace extra = fake_commissioning;
using fake::Bytes;
using fake::Op;
using fake::io;
using Result = CommissioningResult;

namespace {
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr char kMotion[] = "012345abcdef", kBrain[] = "fedcba987654";
constexpr char kForeign[] = "a123456789ab", kDevice[] = "Babytech_01-test";
constexpr char kEpoch[] = "0123456789abcdef0123456789abcdef";
constexpr char kNonce[] = "123456789abcdef0123456789abcdef0";
constexpr char kFresh[] = "fedcba9876543210fedcba9876543210";
constexpr char kExecution[] = "a123456789abcdefa123456789abcdef";
constexpr uint64_t kBrainBoot = UINT64_C(0xfedcba9876543210);
constexpr uint64_t kMotionBoot = UINT64_C(0x0123456789abcdef);
unsigned scenarios = 0, failures = 0, wireFrames = 0, safeChecks = 0;
uint32_t currentNow = 100;
bool stationary = true;
bool safeNow() { ++safeChecks; return stationary; }
uint32_t nowMs() { return currentNow; }

fake::Value stringValue(const std::string& text) {
    Bytes bytes(text.begin(), text.end());
    bytes.push_back(0);
    return {bytes, fake::Type::String};
}

std::string repeat(const char* unit, unsigned times) {
    std::string text;
    while (times--) text += unit;
    return text;
}

// Full-size multibyte labels, not the UI's shortened ASCII presentation.
std::string legacyJson(int kind) {
    const std::string prefix = "{\"type\":\"feeding_context\",\"device_id\":\"" +
        std::string(kDevice) + "\",\"profile_version\":2147483647";
    if (kind == 2) return prefix + ",\"cleared\":true}";
    return prefix + ",\"baby_id\":\"" + std::string(96, 'b') +
        "\",\"baby_name\":\"" + repeat("\xe5\xae\x9d", 106) + "AB" +
        "\",\"formula_brand\":\"" + repeat("\xe5\xa5\xb6", 160) +
        "\",\"water_ml\":180,\"temp\":45,\"powder_g_per_100ml\":25.125}";
}

CommissioningImport request(int kind = 1) {
    CommissioningImport r;
    r.pairing.role = v4::Role::Motion;
    std::strcpy(r.pairing.deviceId, kDevice);
    std::strcpy(r.pairing.epoch, kEpoch);
    std::strcpy(r.pairing.localPhysicalId, kMotion);
    std::strcpy(r.pairing.peerPhysicalId, kBrain);
    r.hasContext = kind != 0;
    if (r.hasContext) {
        const auto json = legacyJson(kind);
        CHECK(decodeProductContext(reinterpret_cast<const uint8_t*>(json.data()), json.size(),
                                   kDevice, r.context));
    }
    return r;
}

void seedLegacy(int kind) {
    if (kind) io.disk["productctx"]["payload"] = stringValue(legacyJson(kind));
}

bool present(const char* space) {
    const auto found = io.disk.find(space);
    return found != io.disk.end() && found->second.count("record");
}

fake::Database protectedData() {
    auto disk = io.disk;
    for (const char* space : {"productstate", "productpair"}) {
        const auto found = disk.find(space);
        if (found != disk.end()) {
            found->second.erase("record");
            if (found->second.empty()) disk.erase(found);
        }
    }
    return disk;
}

void noWrites() {
    CHECK(fake::count(Op::OpenRW) == 0);
    CHECK(fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
}

void audit() {
    CHECK(io.handles.empty());
    CHECK(fake::count(Op::Erase) == 0 && fake::count(Op::Init) == 0);
    for (const auto& c : io.calls) {
        if (c.op == Op::Set || c.op == Op::Commit || c.op == Op::OpenRW) {
            CHECK(c.name == "productstate" || c.name == "productpair");
            if (c.op == Op::Set) CHECK(c.key == "record");
        }
    }
}

void verifyState(const CommissioningImport& r) {
    CHECK(present("productstate"));
    const auto& bytes = io.disk.at("productstate").at("record").bytes;
    MotionState expected, actual;
    expected.pairing = r.pairing;
    if (r.hasContext) CHECK(makeMotionContextBarrier(r.context, expected.context));
    CHECK(decodeMotionState(bytes.data(), bytes.size(), actual));
    CHECK(sameMotionState(expected, actual));
    CHECK(actual.slot.kind == MotionSlotKind::Empty && actual.pendingResultCount == 0);
}

void verifyPair(const CommissioningImport& r) {
    CHECK(present("productpair"));
    v4::Pairing actual;
    const auto& bytes = io.disk.at("productpair").at("record").bytes;
    CHECK(decodePairingRecord(bytes.data(), bytes.size(), actual));
    std::array<uint8_t, kPairingRecordMaxSize> expected{};
    const auto n = encodePairingRecord(r.pairing, expected.data(), expected.size());
    CHECK(n == bytes.size() && !std::memcmp(bytes.data(), expected.data(), n));
}

v4::Frame wireRoundtrip(const v4::Frame& frame) {
    std::array<uint8_t, v4::kMaxFrame> bytes{};
    const auto n = v4::encode(frame, bytes.data(), bytes.size());
    CHECK(n);
    v4::Parser parser;
    v4::Frame decoded;
    unsigned completed = 0;
    for (size_t i = 0; i < n; ++i) if (parser.push(bytes[i], currentNow, decoded)) ++completed;
    CHECK(completed == 1);
    CHECK(decoded.kind == frame.kind && decoded.senderBoot == frame.senderBoot &&
          decoded.receiverBoot == frame.receiverBoot && decoded.messageId == frame.messageId &&
          decoded.total == frame.total && decoded.offset == frame.offset && decoded.length == frame.length);
    CHECK(!std::memcmp(decoded.payload, frame.payload, frame.length));
    ++wireFrames;
    return decoded;
}

v4::Frame take(BoardMaintenance& owner) {
    CHECK(owner.outgoing());
    const auto frame = wireRoundtrip(*owner.outgoing());
    owner.queued();
    CHECK(!owner.outgoing());
    return frame;
}

void deliver(BoardInstall& receiver, const v4::Message& message) {
    for (size_t offset = 0; offset < message.length;) {
        v4::Frame frame;
        CHECK(v4::fragment(message, offset, frame));
        receiver.receive(wireRoundtrip(frame), currentNow);
        offset += frame.length;
    }
}

struct Evidence final : BoardMaintenanceTarget {
    bool acquireSafe = true, releaseSafe = true;
    bool safeToAcquire() const override { return acquireSafe; }
    bool safeToRelease() const override { return releaseSafe; }
};

struct Session {
    Evidence evidence;
    BoardMaintenance leaseBrain, leaseMotion;
    // Test-owned RX buffers outlive each channel and are never shared by peers.
    v4::Assembler brainRxAssembler{}, motionRxAssembler{};
    v4::Message brainRxMessage{}, motionRxMessage{};
    BoardInstall brain, motion;
    BoardDiscovery discoveryBrain, discoveryMotion;
    MotionStateStore store;
    MotionInstallTarget target;
    uint64_t brainBoot, motionBoot;
    explicit Session(uint64_t brainId = kBrainBoot, uint64_t motionId = kMotionBoot,
                     bool (*safe)() = safeNow, uint32_t (*clock)() = nowMs)
        : target(store, leaseMotion, safe, clock), brainBoot(brainId), motionBoot(motionId) {
        CHECK(extra::mac == (std::array<uint8_t, 6>{{0x01, 0x23, 0x45, 0xab, 0xcd, 0xef}}));
        // The shared fake backend represents one MCU at a time. Verify both
        // channels' IDs through the actual MAC reader, then restore Motion's.
        const auto motionMac = extra::mac;
        auto brainPair = request().pairing;
        brainPair.role = v4::Role::Brain;
        std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
        extra::mac = {{0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54}};
        const auto brainHardware = verifyBoardPairing(v4::Role::Brain, brainPair);
        extra::mac = motionMac;
        CHECK(brainHardware == PairingLoad::Ready);
        CHECK(verifyBoardPairing(v4::Role::Motion, request().pairing) == PairingLoad::Ready);
        CHECK(brain.bindReceiveBuffers(brainRxAssembler, brainRxMessage));
        CHECK(motion.bindReceiveBuffers(motionRxAssembler, motionRxMessage));
        CHECK(brain.begin(v4::Role::Brain, kBrain, brainBoot));
        CHECK(motion.begin(v4::Role::Motion, kMotion, motionBoot));
        CHECK(motion.setTarget(&target));
        CHECK(leaseBrain.begin(v4::Role::Brain, kBrain, brainBoot));
        CHECK(leaseMotion.begin(v4::Role::Motion, kMotion, motionBoot));
        CHECK(leaseMotion.setTarget(&evidence));
        CHECK(discoveryBrain.begin(v4::Role::Brain, kBrain, brainBoot, DiscoveryPairState::Missing));
        CHECK(discoveryMotion.begin(v4::Role::Motion, kMotion, motionBoot, DiscoveryPairState::Missing));
    }
    void acquire(const char* nonce = kNonce) {
        CHECK(discoveryBrain.request(kDevice, currentNow));
        CHECK(discoveryBrain.outgoing());
        const auto discover = wireRoundtrip(*discoveryBrain.outgoing());
        discoveryBrain.queued();
        discoveryMotion.receive(discover, currentNow);
        CHECK(discoveryMotion.outgoing());
        const auto found = wireRoundtrip(*discoveryMotion.outgoing());
        discoveryMotion.queued();
        discoveryBrain.receive(found, currentNow);
        const auto& peer = discoveryBrain.result();
        CHECK(peer.state == DiscoveryState::Found && peer.peerBoot == motionBoot);
        CHECK(!std::strcmp(peer.physicalId, kMotion));
        CHECK(leaseBrain.request(kDevice, peer, nonce, currentNow));
        const auto acquire = take(leaseBrain);
        CHECK(acquire.senderBoot == brainBoot && acquire.receiverBoot == motionBoot);
        CHECK(!std::memcmp(acquire.payload + 2 + acquire.payload[1], kBrain, 12));
        leaseMotion.receive(acquire, currentNow);
        leaseBrain.receive(take(leaseMotion), currentNow);
        CHECK(leaseBrain.state() == BoardMaintenanceState::Active);
        CHECK(leaseMotion.owns(kDevice, nonce, brainBoot, kBrain));
        CHECK(!leaseMotion.owns(kDevice, nonce, brainBoot, kForeign));
        CHECK(!leaseMotion.owns(kDevice, nonce, brainBoot + 1, kBrain));
        CHECK(!leaseMotion.owns(kDevice, kFresh, brainBoot, kBrain) || !std::strcmp(nonce, kFresh));
    }
    v4::Message make(const CommissioningImport& r, const char* nonce = kNonce) {
        CHECK(brain.request(r, nonce, motionBoot, currentNow));
        CHECK(brain.outgoing());
        const auto message = *brain.outgoing();
        brain.queued();
        CHECK(!brain.outgoing());
        CHECK(message.senderBoot == brainBoot && message.receiverBoot == motionBoot);
        return message;
    }
    Result response() {
        CHECK(motion.outgoing());
        const auto reply = *motion.outgoing();
        const auto result = motion.result();
        motion.queued();
        deliver(brain, reply);
        CHECK(brain.state() == BoardInstallState::Complete && brain.result() == result);
        audit();
        return result;
    }
    Result run(const CommissioningImport& r, const char* nonce = kNonce) {
        deliver(motion, make(r, nonce));
        return response();
    }
};

void scenario(const std::string& name, const std::function<void()>& body) {
    fake::reset();
    extra::reset();
    fake_product_crypto::reset();
    currentNow = 100;
    stationary = true;
    safeChecks = 0;
    for (const char* space : {"wifi-cfg", "actuatorcfg", "sensorcfg", "outbox"})
        io.disk[space]["payload"] = {{0xff, 0, 1}, fake::Type::Blob};
    for (const char* space : {"productpair", "productstate", "brainstate", "formulaevt", "productctx"})
        io.disk[space]["unrelated"] = {{1, 2, 3, 4}, fake::Type::U32};
    const auto old = protectedData();
    ++scenarios;
    try {
        body();
        audit();
        fake::verifyFaults();
        // Seeding legacy evidence is intentional; every test checks its own
        // full snapshot after seeding, plus unrelated namespaces here.
        for (const auto& space : old) for (const auto& value : space.second)
            CHECK(io.disk.at(space.first).at(value.first) == value.second);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        fake::reboot();
    }
}

void success() {
    for (int kind : {0, 1, 2}) for (bool early : {false, true}) scenario("state first, pair last", [=] {
        io.durableOnSet = early;
        seedLegacy(kind);
        const auto r = request(kind);
        const auto old = protectedData();
        Session s;
        s.acquire();
        noWrites();
        bool pairObserved = false;
        io.before = [&](const fake::Call& c) {
            if (c.op == Op::Set && c.name == "productpair") {
                CHECK(!pairObserved);
                pairObserved = true;
                CHECK(fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 1);
                verifyState(r);
                CHECK(!present("productpair"));
                CHECK(std::any_of(io.calls.begin(), io.calls.end(), [](const fake::Call& x) {
                    return x.op == Op::Read && x.name == "productstate";
                }));
            }
        };
        CHECK(s.run(r) == Result::Installed);
        CHECK(pairObserved && !s.target.faulted());
        CHECK(fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 2);
        verifyState(r);
        verifyPair(r);
        CHECK(protectedData() == old && !present("brainstate"));
        CHECK(s.store.ready() && s.store.state().slot.kind == MotionSlotKind::Empty);
        if (kind == 1) {
            CHECK(std::strlen(r.context.babyName) == 320 && std::strlen(r.context.formulaBrand) == 480);
            CHECK(s.store.state().context.profileVersion == UINT32_C(2147483647));
        }
        io.before = {};
        const auto disk = io.disk;
        const auto writes = fake::count(Op::Set);
        CHECK(s.run(r) == Result::AlreadyInstalled);
        CHECK(fake::count(Op::Set) == writes && io.disk == disk);
    });
    scenario("duplicate complete message and final frame never write twice", [] {
        seedLegacy(1);
        Session s;
        s.acquire();
        const auto message = s.make(request());
        deliver(s.motion, message);
        CHECK(s.response() == Result::Installed);
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        deliver(s.motion, message);
        CHECK(s.response() == Result::Installed);
        CHECK(io.calls.size() == calls && io.disk == disk);
        v4::Frame last;
        CHECK(v4::fragment(message, ((message.length - 1) / v4::kMaxFragment) * v4::kMaxFragment, last));
        s.motion.receive(wireRoundtrip(last), currentNow);
        CHECK(!s.motion.outgoing() && io.calls.size() == calls && io.disk == disk);
    });
    scenario("single-frame duplicate after response is cached", [] {
        Session s;
        s.acquire();
        const auto message = s.make(request(0));
        CHECK(message.length <= v4::kMaxFragment);
        deliver(s.motion, message);
        CHECK(s.response() == Result::Installed);
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        deliver(s.motion, message);
        CHECK(s.response() == Result::Installed);
        CHECK(io.calls.size() == calls && io.disk == disk);
    });
    scenario("decoded prior context cannot leak into absent-context request", [] {
        Session s;
        s.acquire();
        CHECK(s.run(request()) == Result::Conflict);
        noWrites();
        CHECK(s.run(request(0)) == Result::Installed);
        verifyState(request(0));
        CHECK(!s.store.state().context.present && !present("brainstate"));
    });
}

void ownerFence() {
    for (uint32_t attackId : {UINT32_MAX, UINT32_C(2)})
        scenario("foreign Brain MAC with same boot cannot claim owner ID " + std::to_string(attackId), [=] {
        seedLegacy(1);
        const auto r = request();
        Session s;
        s.acquire();
        const auto first = s.make(r);
        CHECK(first.messageId == 1);
        deliver(s.motion, first);
        CHECK(s.response() == Result::Installed);
        const auto disk = io.disk;
        const auto writes = fake::count(Op::Set), commits = fake::count(Op::Commit);

        v4::Assembler foreignRxAssembler{};
        v4::Message foreignRxMessage{};
        BoardInstall foreign;
        CHECK(foreign.bindReceiveBuffers(foreignRxAssembler, foreignRxMessage));
        CHECK(foreign.begin(v4::Role::Brain, kForeign, s.brainBoot));
        auto wrongOwner = r;
        std::strcpy(wrongOwner.pairing.peerPhysicalId, kForeign);
        CHECK(foreign.request(wrongOwner, kNonce, s.motionBoot, currentNow));
        CHECK(foreign.outgoing());
        auto attack = *foreign.outgoing();
        foreign.queued();
        attack.messageId = attackId;
        CHECK(attack.senderBoot == first.senderBoot);
        CHECK(s.leaseMotion.owns(kDevice, kNonce, s.brainBoot, kBrain));
        CHECK(!s.leaseMotion.owns(kDevice, kNonce, s.brainBoot, kForeign));
        deliver(s.motion, attack);
        CHECK(s.motion.outgoing() && s.motion.result() == Result::Unsafe);
        CHECK(s.motion.outgoing()->messageId == attackId);
        s.motion.queued();
        CHECK(io.disk == disk && fake::count(Op::Set) == writes && fake::count(Op::Commit) == commits);

        // An exact rejected retry may be cached, but must not become a
        // monotone watermark for this boot's legitimate maintenance owner.
        const auto calls = io.calls.size();
        const auto checks = safeChecks;
        deliver(s.motion, attack);
        CHECK(s.motion.outgoing() && s.motion.result() == Result::Unsafe);
        s.motion.queued();
        CHECK(io.calls.size() == calls && safeChecks == checks && io.disk == disk);
        CHECK(!s.target.faulted());
        CHECK(s.target.install(r, kNonce, s.brainBoot) == Result::AlreadyInstalled);

        const auto legitimate = s.make(r);
        CHECK(legitimate.messageId == 2 && legitimate.senderBoot == attack.senderBoot);
        CHECK(legitimate.length == attack.length &&
              std::memcmp(legitimate.payload, attack.payload, legitimate.length));
        const auto beforeLegitimate = safeChecks;
        deliver(s.motion, legitimate);
        CHECK(s.response() == Result::AlreadyInstalled);
        CHECK(safeChecks > beforeLegitimate);
        CHECK(fake::count(Op::Set) == writes && fake::count(Op::Commit) == commits && io.disk == disk);
        verifyState(r);
        verifyPair(r);
        CHECK(s.leaseMotion.owns(kDevice, kNonce, s.brainBoot, kBrain));
    });
}

void guards() {
    for (unsigned gate = 0; gate < 9; ++gate) scenario("lease and hardware evidence gate " + std::to_string(gate), [=] {
        seedLegacy(1);
        Session s(kBrainBoot, kMotionBoot, gate == 7 ? nullptr : safeNow, gate == 8 ? nullptr : nowMs);
        if (gate != 0) s.acquire();
        auto r = request();
        const auto disk = io.disk;
        if (gate == 3) stationary = false;
        if (gate == 4) extra::mac[0] ^= 0x10;
        if (gate == 5) currentNow += BoardMaintenance::kLeaseMs;
        if (gate == 6) {
            CHECK(s.leaseBrain.release(currentNow));
            s.leaseMotion.receive(take(s.leaseBrain), currentNow);
            s.leaseBrain.receive(take(s.leaseMotion), currentNow);
            CHECK(!s.leaseMotion.owns(kDevice, kNonce, kBrainBoot, kBrain));
        }
        if (gate == 2) {
            // Brain.request correctly refuses a peer MAC not matching its own
            // identity. A foreign Brain channel can serialize it, not own it.
            std::strcpy(r.pairing.peerPhysicalId, kForeign);
            CHECK(!s.brain.request(r, kNonce, kMotionBoot, currentNow));
            v4::Assembler foreignRxAssembler{};
            v4::Message foreignRxMessage{};
            BoardInstall foreign;
            CHECK(foreign.bindReceiveBuffers(foreignRxAssembler, foreignRxMessage));
            CHECK(foreign.begin(v4::Role::Brain, kForeign, kBrainBoot));
            CHECK(foreign.request(r, kNonce, kMotionBoot, currentNow));
            const auto message = *foreign.outgoing();
            foreign.queued();
            deliver(s.motion, message);
            CHECK(s.motion.result() == Result::Unsafe && s.motion.outgoing());
            deliver(foreign, *s.motion.outgoing());
            s.motion.queued();
            CHECK(foreign.state() == BoardInstallState::Complete && foreign.result() == Result::Unsafe);
        } else CHECK(s.run(r, gate == 1 ? kFresh : kNonce) ==
                     (gate == 4 ? Result::IdentityMismatch : Result::Unsafe));
        noWrites();
        CHECK(io.disk == disk && !s.target.faulted());
    });
    scenario("lease is bound to requesting Brain boot", [] {
        seedLegacy(1);
        Session s;
        s.acquire();
        auto message = s.make(request());
        ++message.senderBoot;
        deliver(s.motion, message);
        CHECK(s.motion.outgoing() && s.motion.result() == Result::Unsafe);
        s.motion.queued();
        noWrites();
        CHECK(!s.target.faulted());
    });
    scenario("stationary evidence is rechecked before state write", [] {
        seedLegacy(1);
        Session s;
        s.acquire();
        bool changed = false;
        io.before = [&](const fake::Call& c) {
            if (c.op == Op::OpenRO && c.name == "productstate") {
                changed = true;
                stationary = false;
            }
        };
        const auto disk = io.disk;
        CHECK(s.run(request()) == Result::Unsafe);
        CHECK(changed && safeChecks == 2 && io.disk == disk);
        noWrites();
    });
}

size_t lengthAt(const v4::Message& m, size_t at) {
    return size_t(m.payload[at]) | (size_t(m.payload[at + 1]) << 8);
}

void malformed() {
    for (unsigned mutation = 0; mutation < 23; ++mutation) scenario("raw malformed install " + std::to_string(mutation), [=] {
        seedLegacy(1);
        Session s;
        s.acquire();
        auto message = s.make(request());
        const size_t pair = lengthAt(message, 33), flag = 35 + pair, context = flag + 3;
        if (mutation == 0) message.payload[0] = 3;
        if (mutation == 1) message.payload[1] = 'G';
        if (mutation == 2) std::memset(message.payload + 1, '0', 32);
        if (mutation == 3) { message.payload[33] = 0xff; message.payload[34] = 0xff; }
        if (mutation == 4) message.payload[35] ^= 0x80;
        if (mutation == 5) message.payload[35 + pair - 1] ^= 0x01;
        if (mutation == 6) message.payload[flag] = 2;
        if (mutation == 7) message.payload[flag] = 0;
        if (mutation == 8) { message.payload[flag + 1] = 0xff; message.payload[flag + 2] = 0xff; }
        if (mutation == 9) { message.payload[flag + 1] = 0; message.payload[flag + 2] = 0; }
        if (mutation == 10) message.payload[context] = 2;
        if (mutation == 11) message.payload[context + 1] = 2;
        if (mutation == 12) message.payload[context + 4] = '!';
        if (mutation == 13) message.payload[message.length - 1] = 0;
        if (mutation == 14) --message.length;
        if (mutation == 15) message.payload[message.length++] = 1;
        if (mutation == 16) message.length = 38;
        if (mutation == 17) ++message.receiverBoot;
        if (mutation == 18) message.kind = v4::Kind::Command;
        if (mutation == 19) {
            // Invalid UTF-8 in the complete baby name, with envelope intact.
            const size_t nameLength = context + 4 + std::strlen(kDevice) + 4 + 2 + 96;
            message.payload[nameLength + 2] = 0xff;
        }
        if (mutation == 20) {
            auto p = request().pairing;
            p.role = v4::Role::Brain;
            CHECK(encodePairingRecord(p, message.payload + 35, pair) == pair);
        }
        if (mutation == 21) {
            auto p = request().pairing;
            std::strcpy(p.localPhysicalId, kForeign);
            CHECK(encodePairingRecord(p, message.payload + 35, pair) == pair);
        }
        if (mutation == 22) { message.senderBoot = kMotionBoot; }
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        deliver(s.motion, message);
        CHECK(!s.motion.outgoing() && safeChecks == 0 && io.calls.size() == calls);
        noWrites();
        CHECK(io.disk == disk && !s.target.faulted());
    });
    scenario("CRC-damaged and truncated bytes never reach NVS", [] {
        Session s;
        s.acquire();
        const auto message = s.make(request(0));
        v4::Frame frame;
        CHECK(v4::fragment(message, 0, frame));
        std::array<uint8_t, v4::kMaxFrame> bytes{};
        const auto n = v4::encode(frame, bytes.data(), bytes.size());
        CHECK(n);
        const auto disk = io.disk;
        const auto calls = io.calls.size();
        for (size_t at = 0; at < n; ++at) {
            auto bad = bytes;
            bad[at] ^= 1;
            v4::Parser parser;
            v4::Frame decoded;
            for (size_t i = 0; i < n; ++i)
                if (parser.push(bad[i], currentNow, decoded)) s.motion.receive(decoded, currentNow);
            CHECK(!s.motion.outgoing() && safeChecks == 0 && io.calls.size() == calls);
        }
        for (size_t cut = 0; cut < n; ++cut) {
            v4::Parser parser;
            v4::Frame decoded;
            for (size_t i = 0; i < cut; ++i)
                if (parser.push(bytes[i], currentNow, decoded)) s.motion.receive(decoded, currentNow);
            CHECK(!s.motion.outgoing() && safeChecks == 0 && io.calls.size() == calls);
        }
        noWrites();
        CHECK(io.disk == disk);
    });
    scenario("incomplete multi-fragment install never writes", [] {
        seedLegacy(1);
        Session s;
        s.acquire();
        const auto message = s.make(request());
        v4::Frame first;
        CHECK(v4::fragment(message, 0, first) && first.total > first.length);
        s.motion.receive(wireRoundtrip(first), currentNow);
        noWrites();
        CHECK(safeChecks == 0);
        currentNow += v4::kMessageTimeoutMs;
        s.motion.poll(currentNow);
        for (size_t offset = first.length; offset < message.length;) {
            v4::Frame fragment;
            CHECK(v4::fragment(message, offset, fragment));
            s.motion.receive(wireRoundtrip(fragment), currentNow);
            offset += fragment.length;
        }
        noWrites();
        CHECK(!s.motion.outgoing() && safeChecks == 0);
    });
}

void legacy() {
    for (const char* payload : {"{pending-event}", "{malformed-json", "{}"}) scenario("legacy event stays owned", [=] {
        seedLegacy(1);
        io.disk["formulaevt"]["payload"] = stringValue(payload);
        const auto disk = io.disk;
        Session s;
        s.acquire();
        CHECK(s.run(request()) == Result::LegacyPending);
        noWrites();
        CHECK(io.disk == disk && !s.target.faulted());
    });
    for (const auto& event : {fake::Value{{0}, fake::Type::String},
                             fake::Value{{1, 2, 3}, fake::Type::Blob}})
        scenario("unreadable legacy event is retained, not an absent slot", [&] {
            seedLegacy(1);
            io.disk["formulaevt"]["payload"] = event;
            const auto disk = io.disk;
            Session s;
            s.acquire();
            CHECK(s.run(request()) == Result::StorageFault);
            noWrites();
            CHECK(s.target.faulted() && io.disk == disk);
        });
    for (unsigned field = 0; field < 8; ++field) scenario("full legacy match, not version alone", [=] {
        seedLegacy(1);
        auto r = request();
        if (field == 0) r.context.babyName[319] = 'C';
        if (field == 1) r.context.formulaBrand[479] = char(0xb7);
        if (field == 2) --r.context.profileVersion;
        if (field == 3) ++r.context.waterMl;
        if (field == 4) ++r.context.temperatureC;
        if (field == 5) r.context.powderGPer100Ml += 0.5f;
        if (field == 6) r.context.babyId[95] = 'c';
        if (field == 7) r = request(0);
        CHECK(!r.hasContext || validProductContext(r.context));
        const auto disk = io.disk;
        Session s;
        s.acquire();
        CHECK(s.run(r) == Result::Conflict);
        noWrites();
        CHECK(io.disk == disk);
    });
    for (int wanted : {0, 1}) scenario("tombstone never converted to absent or active", [=] {
        seedLegacy(2);
        Session s;
        s.acquire();
        CHECK(s.run(request(wanted)) == Result::Conflict);
        noWrites();
    });
    scenario("missing legacy is not evidence for importing invented context", [] {
        Session s;
        s.acquire();
        CHECK(s.run(request()) == Result::Conflict);
        noWrites();
        CHECK(!present("brainstate") && !present("productstate"));
    });
}

ProductRequest command(const CommissioningImport& r, bool cloud, bool prepare, uint64_t seq = 1) {
    ProductRequest q;
    q.command = prepare ? ProductCommand::Prepare : ProductCommand::Clean;
    q.sequence = seq;
    q.source = cloud ? v4::Source::CloudCommand : v4::Source::LocalTouch;
    std::strcpy(q.deviceId, r.pairing.deviceId);
    if (cloud) std::strcpy(q.commandId, ("cloud-test-" + std::to_string(seq)).c_str());
    else {
        auto brain = r.pairing;
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        CHECK(makeLocalCommandId(brain, seq, q.commandId));
    }
    if (prepare) {
        std::strcpy(q.babyId, r.context.babyId);
        q.profileVersion = r.context.profileVersion;
        q.waterMl = r.context.waterMl;
        q.temperatureC = r.context.temperatureC;
        q.powderGPer100Ml = r.context.powderGPer100Ml;
    }
    CHECK(validProductRequest(q));
    return q;
}

void reuse() {
    for (bool paired : {false, true}) for (unsigned used = 0; used < 7; ++used)
        scenario("used state cannot be reset " + std::to_string(used), [=] {
            seedLegacy(1);
            const auto r = request();
            {
                MotionStateStore existing;
                auto imported = r.context;
                if (used == 0) --imported.profileVersion;
                CHECK(existing.installInitial(r.pairing, &imported) == MotionWrite::Stored);
                if (used == 0) {
                    CHECK(existing.state().context.profileVersion != r.context.profileVersion);
                } else if (used < 4) {
                    const auto q = command(r, used == 2, false);
                    CHECK(existing.recordDecision(q, used == 3, used == 3 ? "accepted" : "busy",
                                                  used == 3 ? kExecution : nullptr) == MotionWrite::Stored);
                } else {
                    const unsigned count = used == 6 ? kMotionResultQueueCapacity : 1;
                    for (unsigned i = 1; i <= count; ++i) {
                        auto execution = std::string(kExecution);
                        execution.back() = char('0' + i);
                        const auto q = command(r, true, true, i);
                        CHECK(existing.recordDecision(q, true, "accepted", execution.c_str()) == MotionWrite::Stored);
                        CHECK(existing.finishFeeding(execution.c_str(), true, "", "", 123) == MotionWrite::Stored);
                        if (used != 4) CHECK(existing.archiveFeeding(true) == MotionWrite::Stored);
                    }
                }
                if (paired) {
                    PairingInstaller installer;
                    CHECK(installer.installFirst(v4::Role::Motion, r.pairing) == PairingInstall::Installed);
                }
            }
            fake::reboot();
            const auto disk = io.disk;
            Session s;
            s.acquire();
            CHECK(s.run(r) == Result::Conflict);
            noWrites();
            CHECK(io.disk == disk);
        });
    scenario("existing pair with a different epoch refuses reuse", [] {
        seedLegacy(1);
        const auto r = request();
        {
            Session initial;
            initial.acquire();
            CHECK(initial.run(r) == Result::Installed);
        }
        fake::reboot();
        const auto disk = io.disk;
        auto changed = r;
        changed.pairing.epoch[0] = 'f';
        Session s;
        s.acquire();
        CHECK(s.run(changed) == Result::Conflict);
        noWrites();
        CHECK(io.disk == disk);
    });
}

void expiry() {
    for (int kind : {0, 1, 2}) for (bool releaseSafe : {false, true})
        scenario("lease expires after durable state, before pair", [=] {
        seedLegacy(kind);
        const auto r = request(kind);
        const auto old = protectedData();
        fake::Value orphan;
        {
            Session s;
            s.acquire();
            s.evidence.releaseSafe = releaseSafe;
            ++currentNow;
            bool expired = false;
            io.before = [&](const fake::Call& c) {
                if (c.op == Op::Read && c.name == "productstate" && fake::count(Op::Commit) == 1) {
                    expired = true;
                    currentNow += BoardMaintenance::kLeaseMs - 1;
                }
            };
            // The transport received this request before the I/O hook advanced
            // the real guard's clock. Unsafe is not an atomic-rollback promise.
            CHECK(s.run(r) == Result::Unsafe);
            CHECK(expired && !s.target.faulted());
            CHECK(!s.leaseMotion.owns(kDevice, kNonce, kBrainBoot, kBrain));
            CHECK(s.leaseMotion.active() == !releaseSafe);
            CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            CHECK(!present("productpair"));
            verifyState(r);
            orphan = io.disk.at("productstate").at("record");
            CHECK(protectedData() == old);
        }
        io.before = {};
        fake::reboot();
        currentNow = 100;
        Session resumed(kBrainBoot + 1, kMotionBoot + 1);
        resumed.acquire(kFresh);
        CHECK(resumed.run(r, kFresh) == Result::Installed);
        CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
        CHECK(io.disk.at("productstate").at("record") == orphan);
        verifyState(r);
        verifyPair(r);
        CHECK(protectedData() == old);
    });
    scenario("new epoch cannot claim orphan state after explicit reboot", [] {
        seedLegacy(1);
        auto r = request();
        {
            Session s;
            s.acquire();
            ++currentNow;
            io.before = [&](const fake::Call& c) {
                if (c.op == Op::Read && c.name == "productstate" && fake::count(Op::Commit) == 1)
                    currentNow += BoardMaintenance::kLeaseMs - 1;
            };
            CHECK(s.run(r) == Result::Unsafe);
        }
        fake::reboot();
        currentNow = 100;
        const auto disk = io.disk;
        r.pairing.epoch[0] = 'f';
        Session s(kBrainBoot + 1, kMotionBoot + 1);
        s.acquire(kFresh);
        // Existing store reports an identity mismatch as a latched storage
        // fault, unlike a differing already-written pair (Conflict).
        CHECK(s.run(r, kFresh) == Result::StorageFault);
        CHECK(s.target.faulted() && io.disk == disk);
        noWrites();
    });
}

void faults() {
    std::vector<fake::Call> trace;
    scenario("capture real import I/O boundaries", [&] {
        seedLegacy(1);
        Session s;
        s.acquire();
        CHECK(s.run(request()) == Result::Installed);
        trace = io.calls;
    });
    for (const auto& c : trace) {
        if (c.op != Op::Set && c.op != Op::Commit && c.op != Op::Read) continue;
        for (bool early : {false, true}) for (bool apply : {false, true}) {
            if (apply && c.op == Op::Read) continue;
            scenario("real NVS failure " + c.name + " op=" + std::to_string(int(c.op)) +
                     " n=" + std::to_string(c.occurrence) + " early=" + std::to_string(early) +
                     " apply=" + std::to_string(apply), [=] {
                seedLegacy(1);
                io.durableOnSet = early;
                const auto r = request();
                const auto old = protectedData();
                {
                    Session s;
                    s.acquire();
                    fake::fail(c.op, c.occurrence, ESP_FAIL, apply);
                    CHECK(s.run(r) == Result::StorageFault);
                    fake::verifyFaults();
                    CHECK(s.target.faulted() && protectedData() == old);
                    const auto disk = io.disk;
                    const auto calls = io.calls.size();
                    CHECK(s.run(r) == Result::StorageFault);
                    CHECK(io.calls.size() == calls && io.disk == disk);
                    CHECK(s.leaseMotion.owns(kDevice, kNonce, kBrainBoot, kBrain));
                    // Only the importer latches; the independent pre-write lease
                    // still processes its real release/acquire protocol.
                    CHECK(s.leaseBrain.release(currentNow));
                    s.leaseMotion.receive(take(s.leaseBrain), currentNow);
                    s.leaseBrain.receive(take(s.leaseMotion), currentNow);
                    CHECK(s.leaseBrain.state() == BoardMaintenanceState::Released);
                    CHECK(!s.leaseMotion.active());
                    s.acquire(kFresh);
                    CHECK(s.run(r, kFresh) == Result::StorageFault);
                    CHECK(io.calls.size() == calls && io.disk == disk && protectedData() == old);
                    if (present("productpair")) { verifyState(r); verifyPair(r); }
                }
                const bool hadState = present("productstate"), hadPair = present("productpair");
                const auto state = hadState ? io.disk.at("productstate").at("record") : fake::Value{};
                fake::reboot();
                Session rebooted(kBrainBoot + 1, kMotionBoot + 1);
                rebooted.acquire(kFresh);
                CHECK(rebooted.run(r, kFresh) == (hadPair ? Result::AlreadyInstalled : Result::Installed));
                CHECK(fake::count(Op::Set) == unsigned(!hadState) + unsigned(!hadPair));
                if (hadState) CHECK(io.disk.at("productstate").at("record") == state);
                verifyState(r);
                verifyPair(r);
                CHECK(protectedData() == old);
            });
        }
    }
}

struct PowerCut {};

void powerCuts() {
    for (int kind : {0, 1, 2}) {
        fake::Database start;
        std::vector<fake::Call> trace;
        scenario("abrupt-cut baseline context=" + std::to_string(kind), [&] {
            seedLegacy(kind);
            start = io.disk;
            Session s;
            s.acquire();
            CHECK(s.run(request(kind)) == Result::Installed);
            trace = io.calls;
            CHECK(!trace.empty() && trace.size() <= 128);
        });
        std::printf("cuts context=%d: %zu NVS I/O boundaries, two durability modes\n", kind, trace.size());
        for (size_t cut = 1; cut <= trace.size() + 1; ++cut) for (bool early : {false, true}) {
            scenario("abrupt cut context=" + std::to_string(kind) + " boundary=" + std::to_string(cut) +
                     " early=" + std::to_string(early), [=, &start, &trace] {
                io.disk = start;
                io.durableOnSet = early;
                const auto old = protectedData();
                const auto r = request(kind);
                bool hit = false;
                io.before = [&](const fake::Call& c) {
                    if (io.calls.size() == cut) {
                        CHECK(cut <= trace.size());
                        const auto& baseline = trace[cut - 1];
                        CHECK(c.op == baseline.op && c.occurrence == baseline.occurrence &&
                              c.name == baseline.name && c.key == baseline.key);
                        hit = true;
                        throw PowerCut{};
                    }
                };
                try {
                    Session interrupted;
                    interrupted.acquire();
                    CHECK(interrupted.run(r) == Result::Installed);
                    CHECK(cut == trace.size() + 1);
                } catch (const PowerCut&) { CHECK(hit); }
                CHECK(hit == (cut <= trace.size()));
                CHECK(fake::count(Op::Erase) == 0 && fake::count(Op::Init) == 0);
                CHECK(protectedData() == old);
                const bool hadState = present("productstate"), hadPair = present("productpair");
                CHECK(!hadPair || hadState);
                if (hadState) verifyState(r);
                if (hadPair) verifyPair(r);
                const auto state = hadState ? io.disk.at("productstate").at("record") : fake::Value{};
                const auto pair = hadPair ? io.disk.at("productpair").at("record") : fake::Value{};

                // Abrupt cuts can leak live handles and staged writes. Only
                // reboot drops them; do not turn this into a normal I/O error.
                fake::reboot();
                CHECK(io.handles.empty() && !io.before && io.calls.empty());
                currentNow = 100;
                Session resumed(kBrainBoot + 1, kMotionBoot + 1);
                resumed.acquire(kFresh);
                CHECK(resumed.run(r, kFresh) == (hadPair ? Result::AlreadyInstalled : Result::Installed));
                CHECK(fake::count(Op::Set) == unsigned(!hadState) + unsigned(!hadPair));
                CHECK(fake::count(Op::Commit) == unsigned(!hadState) + unsigned(!hadPair));
                if (hadState) CHECK(io.disk.at("productstate").at("record") == state);
                if (hadPair) CHECK(io.disk.at("productpair").at("record") == pair);
                verifyState(r);
                verifyPair(r);
                CHECK(protectedData() == old && !present("brainstate"));
            });
        }
    }

    // Reject recovery of an orphan created by an actual cut, not a handcrafted
    // blob. Used records below are mutated only through the real Motion store.
    for (int kind : {0, 1, 2}) for (unsigned mutation = 0; mutation < (kind == 1 ? 4u : 3u); ++mutation) {
        scenario("abrupt orphan refuses changed/used state context=" + std::to_string(kind) +
                 " mutation=" + std::to_string(mutation), [=] {
            seedLegacy(kind);
            const auto r = request(kind);
            const auto old = protectedData();
            bool hit = false;
            io.before = [&](const fake::Call& c) {
                if (c.op == Op::Close && c.name == "productstate" && fake::count(Op::Commit) == 1) {
                    hit = true;
                    throw PowerCut{};
                }
            };
            try {
                Session interrupted;
                interrupted.acquire();
                interrupted.run(r);
                CHECK(false);
            } catch (const PowerCut&) { CHECK(hit); }
            CHECK(hit && present("productstate") && !present("productpair"));
            verifyState(r);
            CHECK(protectedData() == old && fake::count(Op::Erase) == 0);
            fake::reboot();
            auto supplied = r;
            if (mutation == 0) supplied.pairing.epoch[0] = 'f';
            else {
                MotionStateStore used;
                CHECK(used.load(r.pairing) == MotionLoad::Ready);
                const bool queued = mutation == 3;
                const auto q = command(r, queued, queued);
                const bool accepted = mutation != 1;
                CHECK(used.recordDecision(q, accepted, accepted ? "accepted" : "busy",
                                          accepted ? kExecution : nullptr) == MotionWrite::Stored);
                if (queued) {
                    CHECK(used.finishFeeding(kExecution, true, "", "", 123) == MotionWrite::Stored);
                    CHECK(used.archiveFeeding(true) == MotionWrite::Stored);
                    CHECK(used.state().pendingResultCount == 1);
                }
            }
            CHECK(protectedData() == old);
            fake::reboot();
            const auto disk = io.disk;
            Session refused(kBrainBoot + 1, kMotionBoot + 1);
            refused.acquire(kFresh);
            CHECK(refused.run(supplied, kFresh) == (mutation == 0 ? Result::StorageFault : Result::Conflict));
            noWrites();
            CHECK(io.disk == disk && protectedData() == old && !present("productpair"));
        });
    }
}

void boundaries() {
    scenario("successful Motion packet does not create empty Brain evidence", [] {
        Session s;
        s.acquire();
        const auto brainBefore = io.disk.at("brainstate");
        CHECK(s.run(request(0)) == Result::Installed);
        CHECK(io.disk.at("brainstate") == brainBefore && !present("brainstate"));
        CHECK(s.store.state().slot.kind == MotionSlotKind::Empty);
        CHECK(s.store.state().cloudSequence == 0 && s.store.state().localSequence == 0);
        CHECK(s.store.state().pendingResultCount == 0);
    });
    scenario("pair without state is never repaired with invented empty data", [] {
        const auto r = request(0);
        PairingInstaller installer;
        CHECK(installer.installFirst(v4::Role::Motion, r.pairing) == PairingInstall::Installed);
        fake::reboot();
        const auto disk = io.disk;
        Session s;
        s.acquire();
        CHECK(s.run(r) == Result::StateMissing);
        CHECK(s.target.faulted() && io.disk == disk);
        noWrites();
        CHECK(!present("brainstate") && !present("productstate"));
    });
    scenario("Motion packet cannot overwrite a used Brain state", [] {
        seedLegacy(1);
        const auto r = request();
        auto brainPair = r.pairing;
        brainPair.role = v4::Role::Brain;
        std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
        {
            BrainStateStore existing;
            CHECK(existing.installInitial(brainPair, &r.context) == BrainWrite::Stored);
            CHECK(existing.reserveLocal(command(r, false, false)) == BrainWrite::Stored);
        }
        fake::reboot();
        const auto before = io.disk.at("brainstate");
        const auto old = protectedData();
        Session s;
        s.acquire();
        CHECK(s.run(r) == Result::Installed);
        CHECK(io.disk.at("brainstate") == before && protectedData() == old);
        CHECK(s.store.state().slot.kind == MotionSlotKind::Empty);
        CHECK(s.store.state().localSequence == 0 && s.store.state().cloudSequence == 0);
    });
}
}

int main(int argc, char** argv) {
    if (argc > 2) { std::fprintf(stderr, "Usage: motion_install [group]\n"); return 2; }
    const std::vector<std::pair<const char*, void (*)()>> groups = {
        {"success", success}, {"owner_fence", ownerFence}, {"guards", guards},
        {"malformed", malformed}, {"legacy", legacy},
        {"reuse", reuse}, {"expiry", expiry}, {"faults", faults},
        {"cuts", powerCuts}, {"boundaries", boundaries}
    };
    bool selected = false;
    for (const auto& group : groups) {
        if (argc == 2 && std::strcmp(group.first, argv[1])) continue;
        selected = true;
        const auto before = scenarios;
        group.second();
        std::printf("%s: %u scenarios\n", group.first, scenarios - before);
    }
    if (!selected) { std::fprintf(stderr, "Unknown Motion install group\n"); return 2; }
    std::printf("Motion install integration: %u scenarios, %u failures, %u wire frames; "
                "real channel/target/lease/stores, fake NVS; no hardware or activation proof\n",
                scenarios, failures, wireFrames);
    return failures ? 1 : 0;
}
