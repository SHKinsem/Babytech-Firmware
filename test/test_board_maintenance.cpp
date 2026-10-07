#include "BoardMaintenance.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech::boardlink;
using namespace babytech::v4;

namespace {
#define CHECK(expression) do { if (!(expression)) \
    throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #expression); } while (false)

constexpr char kBrain[] = "012345abcdef", kMotion[] = "fedcba987654";
constexpr char kForeign[] = "a123456789ab", kDevice[] = "device-A_9";
constexpr char kNonce[] = "0123456789abcdef0123456789abcdef";
constexpr char kFresh[] = "fedcba9876543210fedcba9876543210";
constexpr char kThird[] = "a123456789abcdefa123456789abcdef";
constexpr uint64_t kBrainBoot = UINT64_C(0x0123456789abcdef);
constexpr uint64_t kMotionBoot = UINT64_C(0xfedcba9876543210);
constexpr uint8_t kAcquire = 1, kRenew = 2, kRelease = 3, kReply = 4;
constexpr uint8_t kInactive = 0, kActive = 1, kUnsafe = 2, kBusy = 3;
size_t roundtrips = 0, rejections = 0;

// Evidence only: no fake lease decisions, readiness, NVS, actions or timers.
struct Evidence final : BoardMaintenanceTarget {
    bool acquireSafe = true, releaseSafe = true;
    mutable unsigned acquireChecks = 0, releaseChecks = 0;
    bool safeToAcquire() const override { ++acquireChecks; return acquireSafe; }
    bool safeToRelease() const override { ++releaseChecks; return releaseSafe; }
};

Pairing pair(Role role, const char* device = kDevice) {
    Pairing result{};
    result.role = role;
    std::strcpy(result.deviceId, device);
    std::strcpy(result.epoch, kNonce);
    std::strcpy(result.localPhysicalId, role == Role::Brain ? kBrain : kMotion);
    std::strcpy(result.peerPhysicalId, role == Role::Brain ? kMotion : kBrain);
    CHECK(validPairing(result));
    return result;
}

DiscoveryResult found(bool ready = false, const char* device = kDevice) {
    DiscoveryResult result{};
    result.state = DiscoveryState::Found;
    result.peerBoot = kMotionBoot;
    std::strcpy(result.physicalId, kMotion);
    result.pairingState = ready ? DiscoveryPairState::Ready : DiscoveryPairState::Missing;
    if (ready) result.pairing = pair(Role::Motion, device);
    return result;
}

bool sameFrame(const Frame& a, const Frame& b) {
    return a.kind == b.kind && a.senderBoot == b.senderBoot && a.receiverBoot == b.receiverBoot &&
        a.messageId == b.messageId && a.total == b.total && a.offset == b.offset &&
        a.length == b.length && !std::memcmp(a.payload, b.payload, kMaxFragment);
}

std::vector<uint8_t> wire(const Frame& frame) {
    std::vector<uint8_t> bytes(kMaxFrame, 0xa5);
    const auto length = encode(frame, bytes.data(), bytes.size());
    CHECK(length == kHeaderSize + frame.length + 2);
    for (size_t i = length; i < bytes.size(); ++i) CHECK(bytes[i] == 0xa5);
    bytes.resize(length);
    return bytes;
}

Frame roundtrip(const Frame& frame, uint32_t now = 0) {
    Parser parser;
    Frame decoded{};
    unsigned count = 0;
    for (const auto byte : wire(frame)) if (parser.push(byte, now, decoded)) ++count;
    CHECK(count == 1 && sameFrame(frame, decoded));
    ++roundtrips;
    return decoded;
}

Frame take(BoardMaintenance& sender, bool queued = true) {
    CHECK(sender.outgoing());
    const Frame frame = roundtrip(*sender.outgoing());
    CHECK(frame.kind == Kind::MigrationMaintenance && frame.offset == 0 && frame.total == frame.length);
    for (size_t i = frame.length; i < kMaxFragment; ++i) CHECK(frame.payload[i] == 0);
    if (queued) { sender.queued(); CHECK(!sender.outgoing()); }
    return frame;
}

void deliver(BoardMaintenance& target, const Frame& frame, uint32_t now) {
    target.receive(roundtrip(frame, now), now);
}

Frame operation(Frame frame, uint8_t op, uint32_t id, const char* nonce = kNonce) {
    frame.payload[0] = op;
    frame.messageId = id;
    std::memcpy(frame.payload + 14 + frame.payload[1], nonce, 32);
    return frame;
}

Frame response(const Frame& request, uint8_t result = kActive) {
    Frame frame{};
    frame.kind = Kind::MigrationMaintenance;
    frame.senderBoot = request.receiverBoot;
    frame.receiverBoot = request.senderBoot;
    frame.messageId = request.messageId;
    frame.total = frame.length = 34;
    frame.payload[0] = kReply;
    frame.payload[1] = result;
    std::memcpy(frame.payload + 2, request.payload + 14 + request.payload[1], 32);
    return frame;
}

void expectReply(BoardMaintenance& motion, const Frame& request, uint8_t status, uint32_t now) {
    deliver(motion, request, now);
    const Frame reply = take(motion);
    CHECK(sameFrame(reply, response(request, status)));
}

void ignored(BoardMaintenance& core, const Frame& frame, uint32_t now, Evidence* target = nullptr) {
    const auto state = core.state();
    const bool active = core.active(), output = core.outgoing() != nullptr;
    const Frame before = output ? *core.outgoing() : Frame{};
    const unsigned acquire = target ? target->acquireChecks : 0;
    const unsigned release = target ? target->releaseChecks : 0;
    core.receive(frame, now);
    CHECK(core.state() == state && core.active() == active);
    CHECK((core.outgoing() != nullptr) == output);
    if (output) CHECK(sameFrame(before, *core.outgoing()));
    if (target) CHECK(target->acquireChecks == acquire && target->releaseChecks == release);
    ++rejections;
}

struct Session {
    BoardMaintenance brain, motion;
    Evidence evidence;
    Pairing brainPair, motionPair;
    DiscoveryResult peer;
    explicit Session(bool brainReady = false, bool motionReady = false, const char* device = kDevice)
        : brainPair(pair(Role::Brain, device)), motionPair(pair(Role::Motion, device)),
          peer(found(motionReady, device)) {
        CHECK(brain.begin(Role::Brain, kBrain, kBrainBoot, brainReady ? &brainPair : nullptr));
        CHECK(motion.begin(Role::Motion, kMotion, kMotionBoot, motionReady ? &motionPair : nullptr));
        CHECK(motion.setTarget(&evidence));
    }
    Frame request(uint32_t now = 0, const char* nonce = kNonce, const char* device = kDevice) {
        CHECK(brain.request(device, peer, nonce, now));
        return take(brain);
    }
    Frame acquire(uint32_t now = 0, const char* device = kDevice) {
        const Frame frame = request(now, kNonce, device);
        deliver(motion, frame, now);
        CHECK(motion.active() && motion.owns(device, kNonce, kBrainBoot));
        CHECK(evidence.acquireChecks == 1 && evidence.releaseChecks == 0);
        deliver(brain, take(motion), now);
        CHECK(brain.state() == BoardMaintenanceState::Active && !brain.active());
        CHECK(!brain.owns(device, kNonce, kBrainBoot));
        return frame;
    }
};

void defaultAndExplicitOnly() {
    static_assert(uint8_t(Kind::MigrationMaintenance) == 19, "Kind19 wire contract");
    static_assert(BoardMaintenance::kResponseMs == 1000 && BoardMaintenance::kLeaseMs == 3000 &&
                  BoardMaintenance::kRenewMs == 500, "Approved maintenance deadlines");
    BoardMaintenance empty;
    Evidence evidence;
    CHECK(!empty.setTarget(&evidence));
    CHECK(!empty.request(kDevice, found(), kNonce, 0) && !empty.release(0));
    empty.poll(UINT32_MAX);
    empty.queued();
    CHECK(empty.state() == BoardMaintenanceState::Idle && !empty.active() && !empty.outgoing());
    for (bool ready : {false, true}) {
        Session s(ready, ready);
        CHECK(!s.brain.setTarget(&evidence) && !s.motion.request(kDevice, s.peer, kNonce, 0));
        CHECK(!s.motion.release(0) && !s.brain.release(0));
        for (uint32_t now : {0u, 1000u, 3000u, UINT32_MAX}) {
            s.brain.poll(now); s.motion.poll(now);
            CHECK(s.brain.state() == BoardMaintenanceState::Idle && !s.brain.outgoing());
            CHECK(!s.motion.active() && !s.motion.outgoing());
        }
        CHECK(s.evidence.acquireChecks == 0 && s.evidence.releaseChecks == 0);
        Frame ordinary{};
        ordinary.kind = Kind::Heartbeat;
        ordinary.senderBoot = kBrainBoot; ordinary.receiverBoot = kMotionBoot; ordinary.messageId = 1;
        ignored(s.motion, roundtrip(ordinary), 0, &s.evidence);
    }
}

void initializationValidation() {
    for (unsigned role = 0; role <= 255; ++role) {
        if (role == 1 || role == 2) continue;
        BoardMaintenance core;
        CHECK(!core.begin(Role(role), kBrain, kBrainBoot));
        CHECK(core.state() == BoardMaintenanceState::Idle && !core.outgoing());
    }
    for (const char* id : {static_cast<const char*>(nullptr), "", "012345abcde", "012345abcdef0",
                           "000000000000", "012345abcdeF", "012345abcdeg"}) {
        BoardMaintenance core;
        CHECK(!core.begin(Role::Brain, id, kBrainBoot));
    }
    for (Role role : {Role::Brain, Role::Motion}) {
        const char* physical = role == Role::Brain ? kBrain : kMotion;
        BoardMaintenance core;
        CHECK(!core.begin(role, physical, 0));
        for (unsigned field = 0; field < 5; ++field) {
            Pairing bad = pair(role);
            if (field == 0) bad.role = role == Role::Brain ? Role::Motion : Role::Brain;
            if (field == 1) std::strcpy(bad.localPhysicalId, kForeign);
            if (field == 2) bad.epoch[0] = 'G';
            if (field == 3) std::memset(bad.deviceId, 'a', sizeof(bad.deviceId));
            if (field == 4) std::strcpy(bad.peerPhysicalId, physical);
            CHECK(!core.begin(role, physical, kBrainBoot, &bad));
        }
        const Pairing good = pair(role);
        CHECK(core.begin(role, physical, kBrainBoot, &good));
        CHECK(!core.begin(role, physical, kBrainBoot, &good));
        CHECK(!core.active() && !core.outgoing());
    }
}

void codecIdentityAndRelease() {
    for (size_t length = 1; length <= 64; ++length) {
        std::string device(length, 'a');
        constexpr char alphabet[] = "aZ09_-";
        for (size_t i = 0; i < length; ++i) device[i] = alphabet[i % 6];
        // Missing Brain may discover a matching Ready Motion; Ready Brain must
        // see matching Ready evidence before any acquisition is emitted.
        for (unsigned mode = 0; mode < 3; ++mode) {
            Session s(mode == 2, mode != 0, device.c_str());
            const Frame request = s.acquire(0, device.c_str());
            CHECK(request.senderBoot == kBrainBoot && request.receiverBoot == kMotionBoot);
            CHECK(request.messageId == 1 && request.length == 46 + length);
            CHECK(request.payload[0] == kAcquire && request.payload[1] == length);
            CHECK(!std::memcmp(request.payload + 2, device.data(), length));
            CHECK(!std::memcmp(request.payload + 2 + length, kBrain, 12));
            CHECK(!std::memcmp(request.payload + 14 + length, kNonce, 32));
            CHECK(!s.motion.owns(nullptr, kNonce, kBrainBoot));
            CHECK(!s.motion.owns(device.c_str(), nullptr, kBrainBoot));
            CHECK(!s.motion.owns("different", kNonce, kBrainBoot));
            CHECK(!s.motion.owns(device.c_str(), kFresh, kBrainBoot));
            CHECK(!s.motion.owns(device.c_str(), kNonce, kBrainBoot + 1));
            CHECK(!s.motion.setTarget(nullptr));
            CHECK(s.brain.release(10));
            CHECK(s.brain.state() == BoardMaintenanceState::Releasing);
            const Frame end = take(s.brain);
            CHECK(end.messageId == 2 && end.payload[0] == kRelease);
            deliver(s.motion, end, 10);
            CHECK(!s.motion.active() && !s.motion.owns(device.c_str(), kNonce, kBrainBoot));
            deliver(s.brain, take(s.motion), 10);
            CHECK(s.brain.state() == BoardMaintenanceState::Released);
            CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 1);
            s.brain.poll(5000); s.motion.poll(5000);
            CHECK(!s.brain.outgoing() && !s.motion.active());
            CHECK(!s.brain.release(5000) && s.motion.setTarget(nullptr));
        }
    }
}

void discoveryOwnershipValidation() {
    const auto rejected = [](BoardMaintenance& brain, const DiscoveryResult& peer,
                             const char* device = kDevice, const char* nonce = kNonce) {
        CHECK(!brain.request(device, peer, nonce, 0));
        CHECK(brain.state() == BoardMaintenanceState::Idle && !brain.outgoing());
        ++rejections;
    };
    Session missing;
    for (const char* device : {static_cast<const char*>(nullptr), "", "_a", "a.b", "a b", "a\n"})
        rejected(missing.brain, missing.peer, device);
    rejected(missing.brain, missing.peer, std::string(65, 'a').c_str());
    for (const char* nonce : {static_cast<const char*>(nullptr), "", "0123",
                             "00000000000000000000000000000000", "0123456789abcdef0123456789abcdeF",
                             "0123456789abcdef0123456789abcdef0"})
        rejected(missing.brain, missing.peer, kDevice, nonce);
    for (DiscoveryState state : {DiscoveryState::Idle, DiscoveryState::Pending, DiscoveryState::Conflict,
                                 DiscoveryState::Unavailable, DiscoveryState::TimedOut}) {
        auto peer = missing.peer; peer.state = state; rejected(missing.brain, peer);
    }
    for (DiscoveryPairState state : {DiscoveryPairState::Corrupt, DiscoveryPairState::IoError,
                                     DiscoveryPairState::IdentityMismatch, DiscoveryPairState(255)}) {
        auto peer = missing.peer; peer.pairingState = state; rejected(missing.brain, peer);
    }
    for (uint64_t boot : {UINT64_C(0), kBrainBoot}) {
        auto peer = missing.peer; peer.peerBoot = boot; rejected(missing.brain, peer);
    }
    for (const char* physical : {kBrain, "000000000000", "fedcba98765F"}) {
        auto peer = missing.peer; std::strcpy(peer.physicalId, physical); rejected(missing.brain, peer);
    }
    for (bool readyBrain : {false, true}) {
        Session s(readyBrain, true);
        for (unsigned field = 0; field < 6; ++field) {
            auto bad = s.peer;
            if (field == 0) bad.pairing.role = Role::Brain;
            if (field == 1) std::strcpy(bad.pairing.deviceId, "different");
            if (field == 2) std::strcpy(bad.pairing.peerPhysicalId, kForeign);
            if (field == 3) std::strcpy(bad.pairing.localPhysicalId, kForeign);
            if (field == 4) bad.pairing.epoch[0] = 'G';
            if (field == 5) std::strcpy(bad.physicalId, kForeign);
            rejected(s.brain, bad);
        }
    }
    Session ready(true, true);
    rejected(ready.brain, found());
    auto otherEpoch = ready.peer; otherEpoch.pairing.epoch[0] = 'f';
    rejected(ready.brain, otherEpoch);
    auto otherMac = ready.peer;
    std::strcpy(otherMac.physicalId, kForeign);
    std::strcpy(otherMac.pairing.localPhysicalId, kForeign);
    rejected(ready.brain, otherMac);
    Session noSavedBrain(false, true);
    CHECK(noSavedBrain.brain.request(kDevice, otherEpoch, kNonce, 0));
    // A missing Brain has no local epoch to match; it does not claim Ready.
    CHECK(noSavedBrain.brain.state() == BoardMaintenanceState::Pending);
    const auto first = *noSavedBrain.brain.outgoing();
    CHECK(!noSavedBrain.brain.request(kDevice, otherEpoch, kFresh, 1));
    CHECK(sameFrame(first, *noSavedBrain.brain.outgoing()));
}

void sourceEvidenceAndUsbOverlap() {
    for (bool localUsbOrUnsafe : {false, true}) {
        Session s;
        if (!localUsbOrUnsafe) CHECK(s.motion.setTarget(nullptr));
        else s.evidence.acquireSafe = false; // Existing owner/safety evidence, not a fake gate.
        const Frame request = s.request();
        deliver(s.motion, request, 0);
        CHECK(!s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
        const Frame reply = take(s.motion);
        CHECK(sameFrame(reply, response(request, kUnsafe)));
        deliver(s.brain, reply, 1);
        CHECK(s.brain.state() == BoardMaintenanceState::Unsafe);
        CHECK(s.evidence.acquireChecks == (localUsbOrUnsafe ? 1u : 0u));
        CHECK(s.evidence.releaseChecks == 0);
        CHECK(s.motion.setTarget(&s.evidence));
        s.evidence.acquireSafe = true;
        const Frame retry = s.request(2, kFresh);
        expectReply(s.motion, retry, kActive, 2);
        CHECK(s.motion.active() && s.motion.owns(kDevice, kFresh, kBrainBoot));
    }
    Session s;
    const auto acquire = s.acquire();
    s.evidence.acquireSafe = false;
    expectReply(s.motion, operation(acquire, kRenew, 2), kActive, 500);
    CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 0);
    CHECK(!s.motion.setTarget(nullptr));
    // Pre-write release needs no Ready/config/sensor acquisition evidence.
    expectReply(s.motion, operation(acquire, kRelease, 3), kInactive, 501);
    CHECK(!s.motion.active() && s.evidence.releaseChecks == 1);
}

void wrongRequestsAndPairedMotion() {
    Session s;
    const Frame good = s.request();
    for (unsigned field = 0; field < 16; ++field) {
        Frame bad = good;
        if (field == 0) bad.kind = Kind::Command;
        if (field == 1) bad.senderBoot = 0;
        if (field == 2) bad.senderBoot = kMotionBoot;
        if (field == 3) bad.receiverBoot = 0;
        if (field == 4) ++bad.receiverBoot;
        if (field == 5) bad.messageId = 0;
        if (field == 6) ++bad.total;
        if (field == 7) { ++bad.total; ++bad.offset; }
        if (field == 8) bad.payload[1] = 0;
        if (field == 9) bad.payload[1] = 65;
        if (field == 10) bad.payload[2] = '_';
        if (field == 11) bad.payload[3] = 0;
        if (field == 12) bad.payload[2 + bad.payload[1]] = 'G';
        if (field == 13) std::memcpy(bad.payload + 2 + bad.payload[1], kMotion, 12);
        if (field == 14) bad.payload[14 + bad.payload[1]] = 'G';
        if (field == 15) std::memset(bad.payload + 14 + bad.payload[1], '0', 32);
        ignored(s.motion, bad, 0, &s.evidence);
    }
    for (unsigned op = 0; op <= 255; ++op) {
        if (op >= kAcquire && op <= kRelease) continue;
        Frame bad = good; bad.payload[0] = uint8_t(op); ignored(s.motion, bad, 0, &s.evidence);
    }
    for (size_t length = 0; length < good.length; ++length) {
        Frame bad = good; bad.length = bad.total = uint16_t(length); ignored(s.motion, bad, 0, &s.evidence);
    }
    Frame bad = good; ++bad.length; ++bad.total; ignored(s.motion, bad, 0, &s.evidence);
    bad = good; bad.length = bad.total = 161; ignored(s.motion, bad, 0, &s.evidence);
    bad = good; std::memset(bad.payload + 2 + bad.payload[1], '0', 12);
    ignored(s.motion, bad, 0, &s.evidence);
    Session saved(true, true);
    const Frame owned = saved.request();
    bad = owned; std::memcpy(bad.payload + 2 + bad.payload[1], kForeign, 12);
    ignored(saved.motion, bad, 0, &saved.evidence);
    bad = owned; bad.payload[2] = 'x'; ignored(saved.motion, bad, 0, &saved.evidence);
    expectReply(saved.motion, owned, kActive, 0);
    CHECK(saved.evidence.acquireChecks == 1);
    CHECK(!s.motion.active() && s.evidence.acquireChecks == 0 && s.evidence.releaseChecks == 0);
}

void duplicateAndStaleLease() {
    for (uint8_t op : {kAcquire, kRenew, kRelease}) {
        Session s;
        const Frame first = s.acquire();
        expectReply(s.motion, operation(first, op, 1), kBusy, 2999);
        CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 0);
        CHECK(s.motion.owns(kDevice, kNonce, kBrainBoot));
        s.motion.poll(3000);
        CHECK(!s.motion.active() && s.evidence.releaseChecks == 1);
    }
    Session s;
    const Frame first = s.acquire();
    const Frame renew = operation(first, kRenew, 2);
    expectReply(s.motion, renew, kActive, 500);
    for (uint8_t op : {kAcquire, kRenew, kRelease}) {
        expectReply(s.motion, operation(first, op, 1), kBusy, 1000);
        expectReply(s.motion, operation(first, op, 2), kBusy, 1000);
    }
    CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 0);
    s.motion.poll(3499); CHECK(s.motion.active());
    s.motion.poll(3500); CHECK(!s.motion.active() && s.evidence.releaseChecks == 1);
}

void retainedClosedFence() {
    for (bool expiry : {false, true}) {
        Session s;
        const Frame first = s.acquire();
        const uint32_t closed = expiry ? 3000 : 10;
        if (expiry) s.motion.poll(closed);
        else expectReply(s.motion, operation(first, kRelease, 2), kInactive, closed);
        CHECK(!s.motion.active() && s.evidence.releaseChecks == 1);
        expectReply(s.motion, operation(first, kAcquire, 100), kBusy, closed);
        expectReply(s.motion, operation(first, kRenew, 101), kInactive, closed);
        expectReply(s.motion, operation(first, kRelease, 102), kInactive, closed);
        for (uint8_t op : {kAcquire, kRenew, kRelease})
            expectReply(s.motion, operation(first, op, 102), kBusy, closed);
        expectReply(s.motion, operation(first, kAcquire, 103), kBusy, closed);
        CHECK(!s.motion.active() && s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 1);
        expectReply(s.motion, operation(first, kAcquire, 104, kFresh), kActive, closed + 1);
        CHECK(s.motion.owns(kDevice, kFresh, kBrainBoot));
        CHECK(!s.motion.owns(kDevice, kNonce, kBrainBoot));
        CHECK(s.evidence.acquireChecks == 2 && s.evidence.releaseChecks == 1);
    }
}

void explicitNewSessionAfterClose() {
    for (bool expiry : {false, true}) {
        for (bool ready : {false, true}) {
            Session s(ready, ready);
            s.acquire();
            const uint32_t now = expiry ? 3000 : 10;
            if (expiry) {
                s.brain.poll(500);
                take(s.brain); // Renewal lost: no automatic acquisition retry.
                s.brain.poll(1500);
                CHECK(s.brain.state() == BoardMaintenanceState::TimedOut);
                s.motion.poll(now);
            } else {
                CHECK(s.brain.release(now));
                deliver(s.motion, take(s.brain), now);
                deliver(s.brain, take(s.motion), now);
                CHECK(s.brain.state() == BoardMaintenanceState::Released);
            }
            CHECK(!s.motion.active());
            const Frame staleNonce = s.request(now + 1);
            deliver(s.motion, staleNonce, now + 1);
            deliver(s.brain, take(s.motion), now + 1);
            CHECK(s.brain.state() == BoardMaintenanceState::Busy && !s.motion.active());
            const Frame fresh = s.request(now + 2, kFresh);
            deliver(s.motion, fresh, now + 2);
            deliver(s.brain, take(s.motion), now + 2);
            CHECK(s.brain.state() == BoardMaintenanceState::Active);
            CHECK(s.motion.owns(kDevice, kFresh, kBrainBoot));
            CHECK(s.evidence.acquireChecks == 2 && s.evidence.releaseChecks == 1);
        }
    }
}

void foreignCannotPoisonClosedFence() {
    for (bool expiry : {false, true}) {
        for (unsigned identity = 0; identity < 4; ++identity) {
            for (uint8_t op : {kRenew, kRelease}) {
                // A newer END from the same owner legitimately fences a new
                // nonce, unlike a foreign owner or unsolicited idle RENEW.
                if (identity == 3 && op == kRelease) continue;
                Session s;
                const Frame first = s.acquire();
                const uint32_t now = expiry ? 3000 : 10;
                if (expiry) s.motion.poll(now);
                else expectReply(s.motion, operation(first, kRelease, 2), kInactive, now);
                Frame foreign = operation(first, op, UINT32_MAX, kFresh);
                if (identity == 0) ++foreign.senderBoot;
                if (identity == 1) std::memcpy(foreign.payload + 2 + foreign.payload[1], kForeign, 12);
                if (identity == 2) foreign.payload[2] = 'x';
                expectReply(s.motion, foreign, kInactive, now);
                expectReply(s.motion, operation(first, kAcquire, 3), kBusy, now);
                // A foreign maximal ID must not move the original owner's fence.
                expectReply(s.motion, operation(first, kAcquire, 4, kFresh), kActive, now + 1);
                CHECK(s.motion.owns(kDevice, kFresh, kBrainBoot));
                CHECK(s.evidence.acquireChecks == 2 && s.evidence.releaseChecks == 1);
            }
        }
    }
}

void activeOwnerIsolation() {
    Session s;
    const Frame first = s.acquire();
    for (unsigned field = 0; field < 4; ++field) {
        for (uint8_t op : {kAcquire, kRenew, kRelease}) {
            Frame foreign = operation(first, op, UINT32_MAX);
            if (field == 0) ++foreign.senderBoot;
            if (field == 1) foreign.payload[2] = 'x';
            if (field == 2) std::memcpy(foreign.payload + 2 + foreign.payload[1], kForeign, 12);
            if (field == 3) std::memcpy(foreign.payload + 14 + foreign.payload[1], kFresh, 32);
            expectReply(s.motion, foreign, kBusy, 2999);
            CHECK(s.motion.owns(kDevice, kNonce, kBrainBoot));
        }
    }
    CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 0);
    s.motion.poll(3000);
    CHECK(!s.motion.active() && s.evidence.releaseChecks == 1);
}

void responseDeadlineAndWrap() {
    for (uint32_t start : {0u, uint32_t(UINT32_MAX - 499)}) {
        for (bool queued : {false, true}) {
            Session s;
            CHECK(s.brain.request(kDevice, s.peer, kNonce, start));
            const Frame first = take(s.brain, queued);
            s.brain.poll(start + 999);
            CHECK(s.brain.state() == BoardMaintenanceState::Pending);
            CHECK((s.brain.outgoing() != nullptr) == !queued);
            s.brain.poll(start + 1000);
            CHECK(s.brain.state() == BoardMaintenanceState::TimedOut && !s.brain.outgoing());
            deliver(s.brain, response(first), start + 1000);
            CHECK(s.brain.state() == BoardMaintenanceState::TimedOut);
            s.brain.poll(start + 9000); CHECK(!s.brain.outgoing());
            const Frame next = s.request(start + 9000, kFresh);
            CHECK(next.messageId == 2);
            deliver(s.brain, response(first), start + 9001);
            CHECK(s.brain.state() == BoardMaintenanceState::Pending);
            deliver(s.brain, response(next), start + 9999);
            CHECK(s.brain.state() == BoardMaintenanceState::Active);
        }
        for (uint32_t delay : {999u, 1000u, 1001u}) {
            Session s;
            const Frame first = s.request(start);
            deliver(s.brain, response(first), start + delay);
            CHECK(s.brain.state() == (delay < 1000 ? BoardMaintenanceState::Active :
                                                    BoardMaintenanceState::TimedOut));
        }
    }
}

void leaseExactExpiryAndWrap() {
    for (uint32_t start : {0u, uint32_t(UINT32_MAX - 1499)}) {
        for (uint8_t op : {kAcquire, kRenew, kRelease}) {
            Session s;
            const Frame first = s.acquire(start);
            s.motion.poll(start + 2999);
            CHECK(s.motion.owns(kDevice, kNonce, kBrainBoot) && s.evidence.releaseChecks == 0);
            expectReply(s.motion, operation(first, op, 2), op == kAcquire ? kBusy : kInactive, start + 3000);
            CHECK(!s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
            CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 1);
            s.motion.poll(start + 3001); CHECK(s.evidence.releaseChecks == 1);
            expectReply(s.motion, operation(first, kAcquire, 3, kFresh), kActive, start + 3001);
            CHECK(s.motion.owns(kDevice, kFresh, kBrainBoot));
        }
        Session timely;
        const Frame first = timely.acquire(start);
        expectReply(timely.motion, operation(first, kRenew, 2), kActive, start + 2999);
        timely.motion.poll(start + 5998); CHECK(timely.motion.active());
        timely.motion.poll(start + 5999); CHECK(!timely.motion.active());
    }
}

void implicitRenewal() {
    for (uint32_t start : {0u, uint32_t(UINT32_MAX - 249)}) {
        Session s;
        s.acquire(start);
        for (uint32_t cycle = 1; cycle <= 8; ++cycle) {
            const uint32_t now = start + cycle * 500;
            s.brain.poll(now - 1); CHECK(!s.brain.outgoing());
            s.brain.poll(now);
            CHECK(s.brain.state() == BoardMaintenanceState::Active && s.brain.outgoing());
            CHECK(!s.brain.request(kDevice, s.peer, kFresh, now));
            const Frame renewal = take(s.brain);
            CHECK(renewal.payload[0] == kRenew && renewal.messageId == cycle + 1);
            s.brain.poll(now); CHECK(!s.brain.outgoing());
            deliver(s.motion, renewal, now);
            deliver(s.brain, take(s.motion), now);
            CHECK(s.brain.state() == BoardMaintenanceState::Active);
            CHECK(s.motion.owns(kDevice, kNonce, kBrainBoot));
        }
        CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 0);
        s.motion.poll(start + 6999); CHECK(s.motion.active());
        s.motion.poll(start + 7000); CHECK(!s.motion.active());
    }
}

void cancelBeforeAcquireAck() {
    for (bool delivered : {false, true}) {
        Session s;
        CHECK(s.brain.request(kDevice, s.peer, kNonce, 0));
        const Frame acquire = take(s.brain, delivered);
        Frame ack = response(acquire);
        if (delivered) { deliver(s.motion, acquire, 0); ack = take(s.motion); }
        CHECK(s.brain.release(1));
        const Frame end = take(s.brain);
        CHECK(end.messageId == 2 && end.payload[0] == kRelease);
        CHECK(s.brain.state() == BoardMaintenanceState::Releasing);
        ignored(s.brain, ack, 2);
        deliver(s.motion, end, 3);
        deliver(s.brain, take(s.motion), 3);
        CHECK(s.brain.state() == BoardMaintenanceState::Released && !s.motion.active());
        ignored(s.brain, ack, 4);
        s.brain.poll(5000); CHECK(!s.brain.outgoing());
        CHECK(s.evidence.acquireChecks == (delivered ? 1u : 0u));
        CHECK(s.evidence.releaseChecks == (delivered ? 1u : 0u));
    }
    Session renewing;
    renewing.acquire();
    renewing.brain.poll(500);
    const Frame renew = take(renewing.brain, false);
    CHECK(renewing.brain.release(501));
    const Frame end = take(renewing.brain);
    CHECK(end.messageId > renew.messageId);
    ignored(renewing.brain, response(renew), 502);
    deliver(renewing.motion, end, 503);
    deliver(renewing.brain, take(renewing.motion), 503);
    CHECK(renewing.brain.state() == BoardMaintenanceState::Released && !renewing.motion.active());
}

void releaseTimeoutAndReplyStatus() {
    for (uint8_t result : {kInactive, kActive, kUnsafe, kBusy}) {
        Session s;
        const Frame first = s.request();
        deliver(s.brain, response(first, result), 1);
        const auto expected = result == kInactive ? BoardMaintenanceState::Released :
            result == kActive ? BoardMaintenanceState::Active :
            result == kUnsafe ? BoardMaintenanceState::Unsafe : BoardMaintenanceState::Busy;
        CHECK(s.brain.state() == expected);
        if (result != kActive) { s.brain.poll(5000); CHECK(!s.brain.outgoing()); }
    }
    for (uint8_t result : {kInactive, kActive, kUnsafe, kBusy}) {
        Session s;
        s.request();
        CHECK(s.brain.release(1));
        const Frame end = take(s.brain);
        deliver(s.brain, response(end, result), 2);
        const auto expected = result == kInactive ? BoardMaintenanceState::Released :
            result == kActive ? BoardMaintenanceState::Unavailable :
            result == kUnsafe ? BoardMaintenanceState::Unsafe : BoardMaintenanceState::Busy;
        CHECK(s.brain.state() == expected);
        s.brain.poll(5000); CHECK(!s.brain.outgoing());
    }
    for (uint32_t start : {0u, uint32_t(UINT32_MAX - 499)}) {
        Session s;
        s.acquire(start);
        CHECK(s.brain.release(start + 1));
        const Frame end = take(s.brain, false);
        s.brain.poll(start + 1000); CHECK(s.brain.state() == BoardMaintenanceState::Releasing);
        deliver(s.brain, response(end, kInactive), start + 1001);
        CHECK(s.brain.state() == BoardMaintenanceState::TimedOut && !s.brain.outgoing());
        s.motion.poll(start + 3000); CHECK(!s.motion.active());
    }
}

void invalidAndLateReplies() {
    Session s;
    const Frame request = s.request();
    const Frame good = response(request);
    for (unsigned field = 0; field < 12; ++field) {
        Frame bad = good;
        if (field == 0) ++bad.senderBoot;
        if (field == 1) bad.senderBoot = kBrainBoot;
        if (field == 2) bad.senderBoot = 0;
        if (field == 3) ++bad.receiverBoot;
        if (field == 4) bad.receiverBoot = 0;
        if (field == 5) bad.messageId = 0;
        if (field == 6) ++bad.messageId;
        if (field == 7) bad.kind = Kind::MigrationRead;
        if (field == 8) ++bad.total;
        if (field == 9) { ++bad.total; ++bad.offset; }
        if (field == 10) bad.payload[2] ^= 1;
        if (field == 11) { ++bad.total; ++bad.length; }
        ignored(s.brain, bad, 1);
    }
    for (size_t length = 0; length < 34; ++length) {
        Frame bad = good; bad.total = bad.length = uint16_t(length); ignored(s.brain, bad, 1);
    }
    for (unsigned value = 0; value <= 255; ++value) {
        if (value != kReply) { Frame bad = good; bad.payload[0] = uint8_t(value); ignored(s.brain, bad, 1); }
        if (value > kBusy) { Frame bad = good; bad.payload[1] = uint8_t(value); ignored(s.brain, bad, 1); }
    }
    deliver(s.brain, good, 499);
    ignored(s.brain, good, 500);
    s.brain.poll(999);
    const Frame renew = take(s.brain);
    ignored(s.brain, good, 1000);
    deliver(s.brain, response(renew), 1999);
    CHECK(s.brain.state() == BoardMaintenanceState::TimedOut && !s.brain.outgoing());
    deliver(s.brain, response(renew), 2000);
    CHECK(s.brain.state() == BoardMaintenanceState::TimedOut);
    CHECK(s.evidence.acquireChecks == 0 && s.evidence.releaseChecks == 0);
}

void packetLossAndMissingTxCallback() {
    for (unsigned loss = 0; loss < 4; ++loss) {
        Session s;
        const Frame first = s.request();
        if (loss == 0) {
            s.brain.poll(1000);
            CHECK(s.brain.state() == BoardMaintenanceState::TimedOut && !s.motion.active());
            CHECK(s.evidence.acquireChecks == 0 && s.evidence.releaseChecks == 0);
            continue;
        }
        deliver(s.motion, first, 0);
        const Frame ack = take(s.motion, loss != 2);
        if (loss == 3) {
            deliver(s.brain, ack, 0);
            s.brain.poll(500);
            const Frame renewal = take(s.brain, false);
            CHECK(renewal.payload[0] == kRenew);
            s.brain.poll(1499); CHECK(s.brain.state() == BoardMaintenanceState::Active);
            s.brain.poll(1500);
        } else s.brain.poll(1000);
        CHECK(s.brain.state() == BoardMaintenanceState::TimedOut && !s.brain.outgoing());
        s.evidence.acquireSafe = false;
        s.motion.poll(2999); CHECK(s.motion.active());
        s.motion.poll(3000); CHECK(!s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
        CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 1);
        // Absent TX completion cannot keep debug reserved. Copying a late reply
        // later cannot resurrect either side's expired ownership.
        if (loss == 2) { CHECK(s.motion.outgoing()); s.motion.queued(); }
        deliver(s.brain, ack, 3001);
        CHECK(s.brain.state() == BoardMaintenanceState::TimedOut && !s.motion.active());
    }
    Session blockedTx;
    const Frame first = blockedTx.request();
    deliver(blockedTx.motion, first, 0);
    const Frame ack = *blockedTx.motion.outgoing();
    ignored(blockedTx.motion, operation(first, kRelease, 2), 1, &blockedTx.evidence);
    CHECK(sameFrame(ack, *blockedTx.motion.outgoing()));
    blockedTx.motion.poll(3000);
    CHECK(!blockedTx.motion.active() && blockedTx.evidence.releaseChecks == 1);
    blockedTx.motion.queued();
    expectReply(blockedTx.motion, operation(first, kAcquire, 2), kBusy, 3001);
    expectReply(blockedTx.motion, operation(first, kAcquire, 3, kFresh), kActive, 3001);
}

void futureUnsafeReleaseReservation() {
    // Future importer uncertainty only: never treat held reservation as write
    // authorization after expiry. Real pre-write release evidence stays true.
    for (uint32_t start : {0u, uint32_t(UINT32_MAX - 1499)}) {
        Session s;
        const Frame first = s.acquire(start);
        s.evidence.releaseSafe = false;
        s.motion.poll(start + 2999);
        CHECK(s.motion.owns(kDevice, kNonce, kBrainBoot) && s.evidence.releaseChecks == 0);
        s.motion.poll(start + 3000);
        CHECK(s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
        CHECK(!s.motion.setTarget(nullptr));
        for (uint8_t op : {kAcquire, kRenew}) {
            expectReply(s.motion, operation(first, op, 2), kUnsafe, start + 3000);
            CHECK(s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
        }
        expectReply(s.motion, operation(first, kAcquire, 3, kFresh), kBusy, start + 3000);
        expectReply(s.motion, operation(first, kRelease, 4), kUnsafe, start + 3000);
        CHECK(s.evidence.acquireChecks == 1 && !s.motion.owns(kDevice, kNonce, kBrainBoot));
        s.evidence.releaseSafe = true;
        expectReply(s.motion, operation(first, kRelease, 5), kInactive, start + 3001);
        CHECK(!s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
        expectReply(s.motion, operation(first, kAcquire, 6), kBusy, start + 3001);
        expectReply(s.motion, operation(first, kAcquire, 7, kFresh), kActive, start + 3001);
        CHECK(s.motion.owns(kDevice, kFresh, kBrainBoot) && s.evidence.acquireChecks == 2);
    }
    Session retryByPoll;
    retryByPoll.acquire();
    retryByPoll.evidence.releaseSafe = false;
    retryByPoll.motion.poll(3000);
    CHECK(retryByPoll.motion.active() && !retryByPoll.motion.owns(kDevice, kNonce, kBrainBoot));
    retryByPoll.evidence.releaseSafe = true;
    retryByPoll.motion.poll(3001);
    CHECK(!retryByPoll.motion.active());
    for (uint8_t op : {kAcquire, kRenew}) {
        Session inlineExpiry;
        const Frame first = inlineExpiry.acquire();
        inlineExpiry.evidence.releaseSafe = false;
        expectReply(inlineExpiry.motion, operation(first, op, 2), kUnsafe, 3000);
        CHECK(inlineExpiry.motion.active() && !inlineExpiry.motion.owns(kDevice, kNonce, kBrainBoot));
        CHECK(inlineExpiry.evidence.acquireChecks == 1 && inlineExpiry.evidence.releaseChecks == 1);
        inlineExpiry.evidence.releaseSafe = true;
        inlineExpiry.motion.poll(3001); // Rejected Begin/Renew did not extend the lease.
        CHECK(!inlineExpiry.motion.active() && inlineExpiry.evidence.releaseChecks == 2);
    }
    Session beforeExpiry;
    const Frame first = beforeExpiry.acquire();
    beforeExpiry.evidence.releaseSafe = false;
    expectReply(beforeExpiry.motion, operation(first, kRelease, 2), kUnsafe, 1);
    CHECK(beforeExpiry.motion.active() && beforeExpiry.motion.owns(kDevice, kNonce, kBrainBoot));
    expectReply(beforeExpiry.motion, operation(first, kAcquire, 3, kFresh), kBusy, 2);
    CHECK(beforeExpiry.evidence.acquireChecks == 1 && beforeExpiry.evidence.releaseChecks == 1);
    beforeExpiry.motion.poll(3000);
    CHECK(beforeExpiry.motion.active() && !beforeExpiry.motion.owns(kDevice, kNonce, kBrainBoot));
}

void crcAndStreamingLoss() {
    for (bool isRequest : {true, false}) {
        Session producer;
        const Frame request = producer.request();
        const Frame frame = isRequest ? request : response(request);
        const auto bytes = wire(frame);
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            Session s;
            if (!isRequest) s.request();
            auto& receiver = isRequest ? s.motion : s.brain;
            Parser parser;
            Frame decoded{};
            unsigned count = 0;
            for (size_t i = 0; i < cut; ++i) if (parser.push(bytes[i], 0, decoded)) ++count;
            CHECK(count == 0 && !s.motion.active());
            for (size_t i = cut; i < bytes.size(); ++i) {
                if (parser.push(bytes[i], 99, decoded)) { receiver.receive(decoded, 99); ++count; }
            }
            CHECK(count == 1);
            CHECK(isRequest ? s.motion.active() : s.brain.state() == BoardMaintenanceState::Active);
        }
        for (size_t at = 0; at < bytes.size(); ++at) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                Session s;
                if (!isRequest) s.request();
                auto& receiver = isRequest ? s.motion : s.brain;
                auto bad = bytes; bad[at] ^= uint8_t(1u << bit);
                Parser parser;
                Frame decoded{};
                unsigned count = 0;
                for (auto byte : bad) if (parser.push(byte, 0, decoded)) { receiver.receive(decoded, 0); ++count; }
                CHECK(count == 0 && !s.motion.active() && s.evidence.acquireChecks == 0);
                for (auto byte : bytes) if (parser.push(byte, 101, decoded)) { receiver.receive(decoded, 101); ++count; }
                CHECK(count == 1);
                ++rejections;
            }
        }
        Session s;
        if (!isRequest) s.request();
        auto& receiver = isRequest ? s.motion : s.brain;
        Parser parser;
        Frame decoded{};
        unsigned count = 0;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (parser.push(bytes[i], i < bytes.size() / 2 ? 0 : 100, decoded)) {
                receiver.receive(decoded, 100); ++count;
            }
        }
        CHECK(count == 0 && !s.motion.active() && s.evidence.acquireChecks == 0);
        s.brain.poll(1000);
        CHECK(isRequest ? s.brain.state() == BoardMaintenanceState::Idle :
                          s.brain.state() == BoardMaintenanceState::TimedOut);
    }
}

// End arriving before its already-copied Acquire must not allow that older
// packet to reacquire later; --case cancel_reordered is the minimal reproducer.
void cancelReordered() {
    Session s;
    const Frame first = s.request();
    CHECK(s.brain.release(1));
    const Frame end = take(s.brain);
    deliver(s.motion, end, 2);
    deliver(s.brain, take(s.motion), 2);
    CHECK(s.brain.state() == BoardMaintenanceState::Released && !s.motion.active());
    deliver(s.motion, first, 3);
    CHECK(!s.motion.active() && !s.motion.owns(kDevice, kNonce, kBrainBoot));
    CHECK(s.evidence.acquireChecks == 0 && s.evidence.releaseChecks == 0);
}

void cancelUnseenSameOwnerSession() {
    Session s;
    s.acquire();
    CHECK(s.brain.release(10));
    deliver(s.motion, take(s.brain), 10);
    deliver(s.brain, take(s.motion), 10);
    CHECK(s.brain.state() == BoardMaintenanceState::Released && !s.motion.active());
    const Frame delayed = s.request(11, kFresh);
    CHECK(s.brain.release(12));
    const Frame end = take(s.brain);
    CHECK(end.messageId > delayed.messageId);
    deliver(s.motion, end, 12);
    deliver(s.brain, take(s.motion), 12);
    CHECK(s.brain.state() == BoardMaintenanceState::Released && !s.motion.active());
    expectReply(s.motion, delayed, kBusy, 13);
    const Frame cancelledNonce = s.request(14, kFresh);
    deliver(s.motion, cancelledNonce, 14);
    deliver(s.brain, take(s.motion), 14);
    CHECK(s.brain.state() == BoardMaintenanceState::Busy && !s.motion.active());
    CHECK(s.evidence.acquireChecks == 1 && s.evidence.releaseChecks == 1);
    const Frame fresh = s.request(15, kThird);
    deliver(s.motion, fresh, 15);
    deliver(s.brain, take(s.motion), 15);
    CHECK(s.brain.state() == BoardMaintenanceState::Active);
    CHECK(s.motion.owns(kDevice, kThird, kBrainBoot));
    CHECK(s.evidence.acquireChecks == 2 && s.evidence.releaseChecks == 1);
}

struct Case { const char* name; void (*run)(); };
const Case cases[] = {
    {"default_explicit_only", defaultAndExplicitOnly},
    {"initialization", initializationValidation},
    {"codec_identity_release", codecIdentityAndRelease},
    {"discovery_ownership", discoveryOwnershipValidation},
    {"source_evidence_usb", sourceEvidenceAndUsbOverlap},
    {"wrong_requests", wrongRequestsAndPairedMotion},
    {"duplicate_stale", duplicateAndStaleLease},
    {"closed_fence", retainedClosedFence},
    {"explicit_new_session", explicitNewSessionAfterClose},
    {"foreign_closed_fence", foreignCannotPoisonClosedFence},
    {"active_owner_isolation", activeOwnerIsolation},
    {"response_boundary_wrap", responseDeadlineAndWrap},
    {"lease_boundary_wrap", leaseExactExpiryAndWrap},
    {"implicit_renew", implicitRenewal},
    {"cancel_before_ack", cancelBeforeAcquireAck},
    {"release_status_timeout", releaseTimeoutAndReplyStatus},
    {"invalid_late_replies", invalidAndLateReplies},
    {"loss_missing_tx_callback", packetLossAndMissingTxCallback},
    {"future_unsafe_release", futureUnsafeReleaseReservation},
    {"crc_streaming_loss", crcAndStreamingLoss},
    {"cancel_reordered", cancelReordered},
    {"cancel_unseen_same_owner", cancelUnseenSameOwnerSession},
};
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) { std::fprintf(stderr, "Usage: maintenance [case_name]\n"); return 2; }
    unsigned ran = 0, failures = 0;
    for (const auto& test : cases) {
        if (argc == 2 && std::strcmp(argv[1], test.name)) continue;
        ++ran;
        try { test.run(); std::printf("PASS %s\n", test.name); }
        catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
        }
    }
    if (!ran) { std::fprintf(stderr, "Unknown maintenance case\n"); return 2; }
    std::printf("Board maintenance: %u cases, %u failures, %zu codec roundtrips, %zu rejections; "
                "pre-write only; no Arduino/NVS/actions; status output only\n",
                ran, failures, roundtrips, rejections);
    return failures ? 1 : 0;
}
