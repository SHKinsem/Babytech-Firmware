#include "BoardDiscovery.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace babytech::boardlink;
using namespace babytech::v4;
using Bytes = std::vector<uint8_t>;

namespace {
// Explicit instantiation permits private member names for these two narrow
// fault-injection hooks. Exhaustion/encode failure otherwise need billions of
// requests or invalidation of an immutable snapshot. No production API changes.
template <typename Tag, typename Tag::Type Member> struct TestAccess {
    friend typename Tag::Type member(Tag) { return Member; }
};
struct NextId {
    using Type = uint32_t BoardDiscovery::*;
    friend Type member(NextId);
};
struct SavedPair {
    using Type = Pairing BoardDiscovery::*;
    friend Type member(SavedPair);
};
template struct TestAccess<NextId, &BoardDiscovery::nextId_>;
template struct TestAccess<SavedPair, &BoardDiscovery::pairing_>;

constexpr char kBrain[] = "012345abcdef", kMotion[] = "fedcba987654";
constexpr char kForeign[] = "a123456789ab", kDevice[] = "device-A_9";
constexpr uint64_t kBrainBoot = UINT64_C(0x0123456789abcdef);
constexpr uint64_t kMotionBoot = UINT64_C(0xfedcba9876543210);
size_t rejected = 0, roundtrips = 0;

Pairing pairing(Role role) {
    Pairing value{};
    value.role = role;
    std::strcpy(value.deviceId, kDevice);
    std::strcpy(value.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(value.localPhysicalId, role == Role::Brain ? kBrain : kMotion);
    std::strcpy(value.peerPhysicalId, role == Role::Brain ? kMotion : kBrain);
    assert(validPairing(value));
    return value;
}

void samePair(const Pairing& a, const Pairing& b) {
    assert(a.role == b.role);
    assert(!std::strcmp(a.deviceId, b.deviceId));
    assert(!std::strcmp(a.epoch, b.epoch));
    assert(!std::strcmp(a.localPhysicalId, b.localPhysicalId));
    assert(!std::strcmp(a.peerPhysicalId, b.peerPhysicalId));
}

void sameResult(const DiscoveryResult& a, const DiscoveryResult& b) {
    assert(a.state == b.state && a.peerBoot == b.peerBoot && a.pairingState == b.pairingState);
    assert(!std::strcmp(a.physicalId, b.physicalId));
    samePair(a.pairing, b.pairing);
}

template <typename T> std::array<uint8_t, sizeof(T)> snapshot(const T& value) {
    std::array<uint8_t, sizeof(T)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

void ignored(BoardDiscovery& core, const Frame& frame, uint32_t now = 10) {
    const auto before = snapshot(core);
    core.receive(frame, now);
    assert(snapshot(core) == before);
    ++rejected;
}

void rejectedRequest(BoardDiscovery& core, const char* device, uint32_t now = 10) {
    const auto before = snapshot(core);
    assert(!core.request(device, now));
    assert(snapshot(core) == before);
    ++rejected;
}

void rejectedBegin(BoardDiscovery& core, Role role, const char* physical, uint64_t boot,
                   DiscoveryPairState state, const Pairing* pair = nullptr) {
    const auto before = snapshot(core);
    assert(!core.begin(role, physical, boot, state, pair));
    assert(snapshot(core) == before);
    ++rejected;
}

BoardDiscovery brain(DiscoveryPairState state = DiscoveryPairState::Missing,
                     const Pairing* pair = nullptr) {
    BoardDiscovery core;
    assert(core.begin(Role::Brain, kBrain, kBrainBoot, state, pair));
    return core;
}

BoardDiscovery motion(DiscoveryPairState state = DiscoveryPairState::Missing,
                      const Pairing* pair = nullptr) {
    BoardDiscovery core;
    assert(core.begin(Role::Motion, kMotion, kMotionBoot, state, pair));
    return core;
}

Frame query(const char* device = kDevice) {
    const size_t length = std::strlen(device);
    assert(length > 0 && length <= 64);
    Frame frame{};
    frame.kind = Kind::Discovery;
    frame.senderBoot = kBrainBoot;
    frame.messageId = 1;
    frame.total = frame.length = uint16_t(14 + length);
    frame.payload[0] = 1;
    frame.payload[1] = uint8_t(length);
    std::memcpy(frame.payload + 2, device, length);
    std::memcpy(frame.payload + 2 + length, kBrain, 12);
    return frame;
}

Frame reply(DiscoveryPairState state = DiscoveryPairState::Missing,
            const Pairing* pair = nullptr, const char* physical = kMotion) {
    Frame frame{};
    frame.kind = Kind::Discovery;
    frame.senderBoot = kMotionBoot;
    frame.receiverBoot = kBrainBoot;
    frame.messageId = 1;
    frame.payload[0] = 2;
    std::memcpy(frame.payload + 1, physical, 12);
    frame.payload[13] = uint8_t(state);
    size_t length = 0;
    if (pair) {
        length = encodePairingRecord(*pair, frame.payload + 15, 134);
        assert(length != 0);
    }
    frame.payload[14] = uint8_t(length);
    frame.total = frame.length = uint16_t(15 + length);
    return frame;
}

Bytes wire(const Frame& frame) {
    Bytes bytes(kMaxFrame, 0xa5);
    const size_t length = encode(frame, bytes.data(), bytes.size());
    assert(length == kHeaderSize + frame.length + 2);
    for (size_t i = length; i < bytes.size(); ++i) assert(bytes[i] == 0xa5);
    bytes.resize(length);
    return bytes;
}

size_t feed(Parser& parser, BoardDiscovery& receiver, const Bytes& bytes, uint32_t now) {
    size_t frames = 0;
    Frame output{};
    for (uint8_t byte : bytes) {
        const auto before = snapshot(output);
        if (parser.push(byte, now, output)) {
            receiver.receive(output, now);
            ++frames;
        } else assert(snapshot(output) == before);
    }
    return frames;
}

void transfer(BoardDiscovery& sender, BoardDiscovery& receiver, uint32_t now) {
    assert(sender.outgoing());
    const Frame frame = *sender.outgoing();
    assert(frame.offset == 0 && frame.total == frame.length && frame.length <= 160);
    for (size_t i = frame.length; i < 160; ++i) assert(frame.payload[i] == 0);
    Parser parser;
    assert(feed(parser, receiver, wire(frame), now) == 1);
    const DiscoveryResult before = sender.result();
    sender.queued();
    assert(!sender.outgoing());
    sameResult(before, sender.result());
}

void repairPairCrc(Frame& frame) {
    uint8_t* record = frame.payload + 15;
    const size_t length = frame.payload[14];
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < length; ++i) {
        if (i >= 8 && i < 12) continue;
        crc ^= record[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc ^= 0xffffffff;
    for (size_t i = 0; i < 4; ++i) record[8 + i] = uint8_t(crc >> (8 * i));
}

void lifecycleAndInitialization() {
    static_assert(BoardDiscovery::kTimeoutMs == 1000, "Discovery deadline");
    static_assert(uint8_t(DiscoveryPairState::Missing) == 0 &&
                  uint8_t(DiscoveryPairState::Ready) == 1 &&
                  uint8_t(DiscoveryPairState::Corrupt) == 2 &&
                  uint8_t(DiscoveryPairState::IoError) == 3 &&
                  uint8_t(DiscoveryPairState::IdentityMismatch) == 4, "Wire states");
    BoardDiscovery core;
    const DiscoveryResult empty{};
    sameResult(core.result(), empty);
    assert(!core.outgoing());
    rejectedRequest(core, kDevice);
    ignored(core, query());
    core.poll(5000);
    core.queued();
    sameResult(core.result(), empty);
    for (unsigned value = 0; value <= 255; ++value) {
        if (value != 1 && value != 2)
            rejectedBegin(core, Role(value), kBrain, kBrainBoot, DiscoveryPairState::Missing);
        if (value > 4)
            rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState(value));
    }
    const char* invalid[] = {nullptr, "", "012345abcde", "012345abcdef0", "000000000000",
                             "012345abcdeF", "012345abcdeg", "012345abcde-", "012345abcde\xff"};
    for (const char* id : invalid)
        rejectedBegin(core, Role::Brain, id, kBrainBoot, DiscoveryPairState::Missing);
    rejectedBegin(core, Role::Brain, kBrain, 0, DiscoveryPairState::Missing);
    Pairing saved = pairing(Role::Brain);
    rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready);
    for (DiscoveryPairState state : {DiscoveryPairState::Missing, DiscoveryPairState::Corrupt,
                                    DiscoveryPairState::IoError, DiscoveryPairState::IdentityMismatch})
        rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, state, &saved);
    Pairing bad = saved;
    bad.role = Role::Motion;
    rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready, &bad);
    bad = saved; std::strcpy(bad.localPhysicalId, kForeign);
    rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready, &bad);
    bad = saved; bad.epoch[0] = 'G';
    rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready, &bad);
    bad = saved; std::memset(bad.deviceId, 'a', sizeof(bad.deviceId));
    rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready, &bad);
    assert(core.begin(Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready, &saved));
    rejectedBegin(core, Role::Motion, kMotion, kMotionBoot, DiscoveryPairState::Missing);
    rejectedRequest(core, "different-device");
    assert(core.request(kDevice, 0));
    rejectedBegin(core, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Ready, &saved);
    rejectedRequest(core, kDevice);
    core.queued();
    assert(core.result().state == DiscoveryState::Pending);
    rejectedRequest(core, kDevice);
    BoardDiscovery other = motion();
    rejectedRequest(other, kDevice);
}

void roundtripBoundsAndReset() {
    for (size_t length = 1; length <= 64; ++length) {
        std::string device(length, 'a');
        const char alphabet[] = "aZ09_-";
        for (size_t i = 0; i < length; ++i) device[i] = alphabet[i % 6];
        for (bool saved : {false, true}) {
            Pairing local = pairing(Role::Brain), peer = pairing(Role::Motion);
            std::strcpy(local.deviceId, device.c_str());
            std::strcpy(peer.deviceId, device.c_str());
            BoardDiscovery b = saved ? brain(DiscoveryPairState::Ready, &local) : brain();
            BoardDiscovery m = saved ? motion(DiscoveryPairState::Ready, &peer) : motion();
            assert(b.request(device.c_str(), 0));
            assert(b.outgoing()->length == 14 + length);
            assert(b.outgoing()->receiverBoot == 0 && b.outgoing()->senderBoot == kBrainBoot);
            assert(b.outgoing()->payload[1] == length && b.outgoing()->messageId == 1);
            assert(!std::memcmp(b.outgoing()->payload + 2, device.data(), length));
            assert(!std::memcmp(b.outgoing()->payload + 2 + length, kBrain, 12));
            transfer(b, m, 1);
            assert(b.result().state == DiscoveryState::Pending);
            assert(m.outgoing()->length == (saved ? 85 + length : 15));
            assert(m.outgoing()->senderBoot == kMotionBoot && m.outgoing()->receiverBoot == kBrainBoot);
            assert(m.outgoing()->messageId == 1 && m.outgoing()->payload[0] == 2);
            const Frame response = *m.outgoing();
            transfer(m, b, 999);
            assert(b.result().state == DiscoveryState::Found && b.result().peerBoot == kMotionBoot);
            assert(!std::strcmp(b.result().physicalId, kMotion));
            assert(b.result().pairingState == (saved ? DiscoveryPairState::Ready : DiscoveryPairState::Missing));
            if (saved) samePair(b.result().pairing, peer);
            else samePair(b.result().pairing, Pairing{});
            assert(m.result().state == DiscoveryState::Idle);
            ignored(b, response);
            b.poll(5000);
            assert(b.result().state == DiscoveryState::Found);
            // Neither request nor Found installs or mutates pairing.
            samePair(b.*member(SavedPair{}), saved ? local : Pairing{});
            samePair(m.*member(SavedPair{}), saved ? peer : Pairing{});
            assert(b.request(device.c_str(), 5000));
            assert(b.outgoing()->messageId == 2);
            assert(b.result().peerBoot == 0 && b.result().physicalId[0] == 0);
            assert(b.result().pairingState == DiscoveryPairState::Missing);
            samePair(b.result().pairing, Pairing{});
            ignored(b, response, 5001);
            ++roundtrips;
        }
    }
    // Saved snapshots and input strings are copied, not retained by address.
    Pairing saved = pairing(Role::Motion);
    BoardDiscovery m = motion(DiscoveryPairState::Ready, &saved);
    std::strcpy(saved.deviceId, "different");
    BoardDiscovery b = brain();
    char device[65]; std::strcpy(device, kDevice);
    assert(b.request(device, 0));
    std::strcpy(device, "different");
    transfer(b, m, 0);
    transfer(m, b, 0);
    assert(b.result().state == DiscoveryState::Found);
    samePair(b.result().pairing, pairing(Role::Motion));
}

void deadlinesAndQueues() {
    for (uint32_t start : {uint32_t(0), uint32_t(UINT32_MAX - 500)}) {
        BoardDiscovery b = brain(), m = motion();
        assert(b.request(kDevice, start));
        b.poll(start + uint32_t(999));
        assert(b.outgoing() && b.result().state == DiscoveryState::Pending);
        b.poll(start + uint32_t(1000));
        assert(!b.outgoing() && b.result().state == DiscoveryState::TimedOut);
        ignored(b, reply(), start + uint32_t(1001));
        b.poll(start + uint32_t(5000));
        assert(!b.outgoing() && b.result().state == DiscoveryState::TimedOut);
        assert(b.request(kDevice, start + uint32_t(5000)));
        assert(b.outgoing()->messageId == 2);
        b.queued();
        b.poll(start + uint32_t(5999));
        assert(b.result().state == DiscoveryState::Pending);
        b.poll(start + uint32_t(6000));
        assert(b.result().state == DiscoveryState::TimedOut);

        BoardDiscovery late = brain();
        assert(late.request(kDevice, start));
        late.receive(reply(), start + uint32_t(1000));
        assert(late.result().state == DiscoveryState::TimedOut && !late.outgoing());
        BoardDiscovery onTime = brain();
        assert(onTime.request(kDevice, start));
        onTime.receive(reply(), start + uint32_t(999));
        assert(onTime.result().state == DiscoveryState::Found && !onTime.outgoing());

        m.receive(query(), start);
        assert(m.outgoing());
        Frame next = query(); next.messageId = 2;
        const auto queued = snapshot(*m.outgoing());
        m.receive(next, start + uint32_t(999));
        assert(snapshot(*m.outgoing()) == queued);
        m.poll(start + uint32_t(1000));
        assert(!m.outgoing() && m.result().state == DiscoveryState::Idle);
        m.receive(next, start + uint32_t(1001));
        assert(m.outgoing()->messageId == 2);
        m.queued();
        assert(m.result().state == DiscoveryState::Idle);
        m.receive(query(), start + uint32_t(2000));
        // A later legal request can replace an expired, never-queued reply.
        m.receive(next, start + uint32_t(3000));
        assert(m.outgoing()->messageId == 2);
    }
    BoardDiscovery exhausted = brain();
    exhausted.*member(NextId{}) = UINT32_MAX;
    assert(exhausted.request(kDevice, 0));
    assert(exhausted.outgoing()->messageId == UINT32_MAX);
    Frame last = reply(); last.messageId = UINT32_MAX;
    exhausted.receive(last, 1);
    assert(exhausted.result().state == DiscoveryState::Found);
    assert(exhausted.*member(NextId{}) == 0);
    rejectedRequest(exhausted, kDevice);
    rejectedBegin(exhausted, Role::Brain, kBrain, kBrainBoot, DiscoveryPairState::Missing);
}

void savedIdentityChecks() {
    const Pairing local = pairing(Role::Brain), peer = pairing(Role::Motion);
    const auto check = [&](DiscoveryPairState localState, const Pairing* own,
                           DiscoveryPairState peerState, const Pairing* other,
                           DiscoveryState expected, const char* physical = kMotion) {
        BoardDiscovery b = brain(localState, own);
        assert(b.request(kDevice, 0));
        const Frame response = reply(peerState, other, physical);
        b.receive(response, 1);
        assert(b.result().state == expected && !b.outgoing());
        assert(b.result().pairingState == peerState);
        ignored(b, response, 2);
    };
    check(DiscoveryPairState::Ready, &local, DiscoveryPairState::Ready, &peer, DiscoveryState::Found);
    check(DiscoveryPairState::Missing, nullptr, DiscoveryPairState::Ready, &peer, DiscoveryState::Found);
    check(DiscoveryPairState::Ready, &local, DiscoveryPairState::Missing, nullptr, DiscoveryState::Conflict);
    for (unsigned field = 0; field < 4; ++field) {
        Pairing different = peer;
        const char* physical = kMotion;
        if (field == 0) std::strcpy(different.deviceId, "foreign-device");
        if (field == 1) different.epoch[0] = 'f';
        if (field == 2) { std::strcpy(different.localPhysicalId, kForeign); physical = kForeign; }
        if (field == 3) std::strcpy(different.peerPhysicalId, kForeign);
        check(DiscoveryPairState::Ready, &local, DiscoveryPairState::Ready, &different,
              DiscoveryState::Conflict, physical);
        check(DiscoveryPairState::Missing, nullptr, DiscoveryPairState::Ready, &different,
              (field == 0 || field == 3) ? DiscoveryState::Conflict : DiscoveryState::Found, physical);
    }
    for (DiscoveryPairState state : {DiscoveryPairState::Corrupt, DiscoveryPairState::IoError,
                                    DiscoveryPairState::IdentityMismatch}) {
        check(DiscoveryPairState::Missing, nullptr, state, nullptr, DiscoveryState::Unavailable);
        check(DiscoveryPairState::Ready, &local, state, nullptr, DiscoveryState::Unavailable);
        check(state, nullptr, DiscoveryPairState::Missing, nullptr, DiscoveryState::Unavailable);
        check(state, nullptr, DiscoveryPairState::Ready, &peer, DiscoveryState::Unavailable);
        // The responder honestly reports errors, never represents them as Missing.
        BoardDiscovery b = brain(), m = motion(state);
        assert(b.request(kDevice, 0));
        transfer(b, m, 0);
        assert(m.outgoing()->length == 15 && m.outgoing()->payload[13] == uint8_t(state));
        assert(m.outgoing()->payload[14] == 0);
        transfer(m, b, 1);
        assert(b.result().state == DiscoveryState::Unavailable);
    }
    // Motion reports its snapshot even to a foreign querying Brain; it never
    // adopts the request's device/physical identity or changes persistent data.
    BoardDiscovery m = motion(DiscoveryPairState::Ready, &peer);
    Frame foreign = query("foreign-device");
    std::memcpy(foreign.payload + 2 + foreign.payload[1], kForeign, 12);
    m.receive(foreign, 0);
    assert(m.outgoing());
    Pairing decoded{};
    assert(decodePairingRecord(m.outgoing()->payload + 15, m.outgoing()->payload[14], decoded));
    samePair(decoded, peer);
    samePair(m.*member(SavedPair{}), peer);
}

void invalidRequestsAndQueries() {
    BoardDiscovery b = brain(), m = motion();
    for (const char* id : {static_cast<const char*>(nullptr), "", "_a", "-a", "a.b", "a b",
                           "a/b", "a\xff", "a\n", "a\xc3\xa9"})
        rejectedRequest(b, id);
    rejectedRequest(b, std::string(65, 'a').c_str());
    const Frame good = query();
    for (size_t length = 0; length < good.length; ++length) {
        Frame bad = good; bad.total = bad.length = uint16_t(length);
        ignored(m, bad);
    }
    Frame bad = good; ++bad.total; ignored(m, bad);
    bad = good; ++bad.total; ++bad.offset; ignored(m, bad);
    bad = good; ++bad.total; ++bad.length; ignored(m, bad);
    bad = good; bad.total = bad.length = 161; ignored(m, bad);
    for (unsigned length = 0; length <= 255; ++length) {
        if (length == good.payload[1]) continue;
        bad = good; bad.payload[1] = uint8_t(length); ignored(m, bad);
    }
    for (unsigned op = 0; op <= 255; ++op) {
        if (op == 1) continue;
        bad = good; bad.payload[0] = uint8_t(op); ignored(m, bad);
    }
    for (size_t at : {size_t(2), size_t(3)}) {
        for (unsigned value = 0; value <= 255; ++value) {
            const bool alnum = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                               (value >= '0' && value <= '9');
            if (alnum || (at > 2 && (value == '_' || value == '-'))) continue;
            bad = good; bad.payload[at] = uint8_t(value); ignored(m, bad);
            std::string device = kDevice; device[at - 2] = char(value);
            if (value || at == 2) rejectedRequest(b, device.c_str());
        }
    }
    for (size_t at = good.length - 12; at < good.length; ++at) {
        for (unsigned value : {0u, unsigned('F'), unsigned('g'), unsigned('-'), 0xffu}) {
            bad = good; bad.payload[at] = uint8_t(value); ignored(m, bad);
        }
    }
    bad = good; std::memset(bad.payload + good.length - 12, '0', 12); ignored(m, bad);
    bad = good; std::memcpy(bad.payload + good.length - 12, kMotion, 12); ignored(m, bad);
    bad = good; bad.receiverBoot = kMotionBoot; ignored(m, bad);
    bad = good; bad.senderBoot = kMotionBoot; ignored(m, bad);
    bad = good; bad.senderBoot = 0; ignored(m, bad);
    bad = good; bad.messageId = 0; ignored(m, bad);
    bad = good; bad.kind = Kind::Hello; ignored(m, bad);
    ignored(m, reply());
    ignored(b, good);
    m.receive(good, 0);
    assert(m.outgoing());
    for (unsigned i = 0; i < 10; ++i) ignored(m, good, 999);
    bad = good; bad.payload[1] = 0;
    ignored(m, bad, 2000);  // Bad decode cannot even expire an existing reply.

    // Inject an invalid saved snapshot to force production encode failure while
    // a valid reply is queued. Construction failure must not erase that reply.
    Pairing saved = pairing(Role::Motion);
    BoardDiscovery corrupt = motion(DiscoveryPairState::Ready, &saved);
    corrupt.receive(good, 0);
    (corrupt.*member(SavedPair{})).epoch[0] = 'G';
    ignored(corrupt, good, 2000);
}

void invalidReplies() {
    BoardDiscovery b = brain();
    assert(b.request(kDevice, 0));
    const Pairing saved = pairing(Role::Motion);
    const Frame good = reply(DiscoveryPairState::Ready, &saved);
    for (size_t length = 0; length < good.length; ++length) {
        Frame bad = good; bad.total = bad.length = uint16_t(length); ignored(b, bad, 2000);
    }
    Frame bad = good; ++bad.total; ++bad.length; ignored(b, bad);
    bad = good; ++bad.total; ignored(b, bad);
    bad = good; ++bad.total; ++bad.offset; ignored(b, bad);
    bad = good; bad.total = bad.length = 161; ignored(b, bad);
    bad = good; bad.receiverBoot = 0; ignored(b, bad);
    bad = good; bad.receiverBoot = kBrainBoot + 1; ignored(b, bad);
    bad = good; bad.senderBoot = 0; ignored(b, bad);
    bad = good; bad.senderBoot = kBrainBoot; ignored(b, bad);
    bad = good; bad.messageId = 0; ignored(b, bad);
    bad = good; bad.messageId = 2; ignored(b, bad);
    bad = good; bad.kind = Kind::HelloAck; ignored(b, bad);
    ignored(b, query());
    for (unsigned value = 0; value <= 255; ++value) {
        if (value != 2) { bad = good; bad.payload[0] = uint8_t(value); ignored(b, bad); }
        if (value > 4) { bad = good; bad.payload[13] = uint8_t(value); ignored(b, bad); }
        if (value != good.payload[14]) { bad = good; bad.payload[14] = uint8_t(value); ignored(b, bad); }
    }
    for (DiscoveryPairState state : {DiscoveryPairState::Missing, DiscoveryPairState::Corrupt,
                                    DiscoveryPairState::IoError, DiscoveryPairState::IdentityMismatch}) {
        bad = good; bad.payload[13] = uint8_t(state); ignored(b, bad);
    }
    bad = reply(DiscoveryPairState::Ready); ignored(b, bad);
    for (size_t at = 1; at <= 12; ++at) {
        for (unsigned value : {0u, unsigned('F'), unsigned('g'), unsigned('-'), 0x80u, 0xffu}) {
            bad = good; bad.payload[at] = uint8_t(value); ignored(b, bad);
        }
    }
    bad = good; std::memset(bad.payload + 1, '0', 12); ignored(b, bad);
    bad = reply(DiscoveryPairState::Missing, nullptr, kBrain); ignored(b, bad);
    bad = good; std::memcpy(bad.payload + 1, kForeign, 12); ignored(b, bad);
    Pairing wrongRole = saved; wrongRole.role = Role::Brain;
    bad = reply(DiscoveryPairState::Ready, &wrongRole); ignored(b, bad);
    for (size_t at = 15; at < good.length; ++at) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            bad = good; bad.payload[at] ^= uint8_t(1u << bit); ignored(b, bad);
        }
    }
    // Recompute CRC to exercise semantic record validation, not just its CRC gate.
    for (size_t at : {size_t(12), size_t(13), size_t(14), size_t(46), size_t(58), size_t(70)}) {
        bad = good; bad.payload[15 + at] = 0; repairPairCrc(bad); ignored(b, bad);
    }
    bad = good; bad.payload[15 + 12] = 1; repairPairCrc(bad); ignored(b, bad);
    bad = good; std::memcpy(bad.payload + 15 + 46, kForeign, 12);
    repairPairCrc(bad); ignored(b, bad);
    bad = good; ++bad.total; ++bad.length; ++bad.payload[14]; repairPairCrc(bad); ignored(b, bad);
    assert(b.result().state == DiscoveryState::Pending && b.outgoing());
    b.receive(good, 999);
    assert(b.result().state == DiscoveryState::Found);
    ignored(b, good);
    // A response for the old Brain boot is irrelevant after local reboot.
    BoardDiscovery rebooted;
    assert(rebooted.begin(Role::Brain, kBrain, kBrainBoot + 1, DiscoveryPairState::Missing));
    assert(rebooted.request(kDevice, 0));
    ignored(rebooted, good);
}

void streamingCrcAndDisconnection() {
    for (bool isQuery : {true, false}) {
        const Frame frame = isQuery ? query() : reply();
        const Bytes bytes = wire(frame);
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            BoardDiscovery receiver = isQuery ? motion() : brain();
            if (!isQuery) assert(receiver.request(kDevice, 0));
            Parser parser;
            const auto before = snapshot(receiver);
            assert(feed(parser, receiver, Bytes(bytes.begin(), bytes.begin() + cut), 0) == 0);
            assert(snapshot(receiver) == before);
            assert(feed(parser, receiver, Bytes(bytes.begin() + cut, bytes.end()), 99) == 1);
            assert(isQuery ? receiver.outgoing() != nullptr : receiver.result().state == DiscoveryState::Found);
        }
        for (size_t at = 0; at < bytes.size(); ++at) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                BoardDiscovery receiver = isQuery ? motion() : brain();
                if (!isQuery) assert(receiver.request(kDevice, 0));
                const auto before = snapshot(receiver);
                Bytes corrupt = bytes; corrupt[at] ^= uint8_t(1u << bit);
                Parser parser;
                assert(feed(parser, receiver, corrupt, 0) == 0);
                assert(snapshot(receiver) == before);
                assert(feed(parser, receiver, bytes, 101) == 1);
                ++rejected;
            }
        }
    }
    BoardDiscovery b = brain(), m = motion();
    assert(b.request(kDevice, 0));
    transfer(b, m, 0);
    const Bytes bytes = wire(*m.outgoing());
    m.queued();
    Parser parser;
    const size_t cut = bytes.size() / 2;
    assert(feed(parser, b, Bytes(bytes.begin(), bytes.begin() + cut), 0) == 0);
    assert(feed(parser, b, Bytes(bytes.begin() + cut, bytes.end()), 100) == 0);
    b.poll(1000);
    assert(b.result().state == DiscoveryState::TimedOut && !b.outgoing());
    const auto before = snapshot(b);
    assert(feed(parser, b, bytes, 1001) == 1);
    assert(snapshot(b) == before);
    assert(b.request(kDevice, 2000));
    // Complete old-ID responses and exact duplicate bytes cannot finish a new request.
    const auto pending = snapshot(b);
    assert(feed(parser, b, bytes, 2001) == 1);
    assert(snapshot(b) == pending);
    transfer(b, m, 2001);
    transfer(m, b, 2002);
    assert(b.result().state == DiscoveryState::Found);
}
}  // namespace

int main() {
    lifecycleAndInitialization();
    roundtripBoundsAndReset();
    deadlinesAndQueues();
    savedIdentityChecks();
    invalidRequestsAndQueries();
    invalidReplies();
    streamingCrcAndDisconnection();
    std::printf("Board discovery: %zu production-codec roundtrips, %zu atomic rejections; "
                "identity, queue/deadline/wrap, exhaustion, CRC and disconnect tests passed; no NVS\n",
                roundtrips, rejected);
}
