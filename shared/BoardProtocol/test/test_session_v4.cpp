#include "BoardSessionV4.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace babytech::v4;

namespace {

const uint64_t kLocalBoot = UINT64_C(0xfedcba9876543210);
const uint64_t kPeerBoot = UINT64_C(0x0123456789abcdef);
const uint64_t kRestartBoot = UINT64_C(0x1123456789abcdef);

Pairing pairing(Role role = Role::Brain) {
    Pairing value;
    value.role = role;
    std::strcpy(value.deviceId, "Babytech_01-device");
    std::strcpy(value.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(value.localPhysicalId, "012345abcdef");
    std::strcpy(value.peerPhysicalId, "fedcba543210");
    return value;
}

Hello peerHello(const Pairing& value = pairing()) {
    Hello hello;
    hello.role = value.role == Role::Brain ? Role::Motion : Role::Brain;
    std::memcpy(hello.deviceId, value.deviceId, sizeof(hello.deviceId));
    std::memcpy(hello.epoch, value.epoch, sizeof(hello.epoch));
    std::memcpy(hello.physicalId, value.peerPhysicalId, sizeof(hello.physicalId));
    return hello;
}

Hello peerAck(uint32_t probeId, const Pairing& value = pairing()) {
    Hello hello = peerHello(value);
    hello.replyTo = probeId;
    return hello;
}

Frame envelope(Kind kind = Kind::Hello, uint32_t id = 1,
               uint64_t boot = kPeerBoot) {
    Frame value;
    value.kind = kind;
    value.senderBoot = boot;
    value.receiverBoot = kind == Kind::Hello ? 0 : kLocalBoot;
    value.messageId = id;
    if (kind != Kind::Heartbeat) {
        value.total = value.length = 1;
        value.payload[0] = 'x';
    }
    assert(validFrame(value));
    return value;
}

Message helloEnvelope(Kind kind = Kind::Hello, uint32_t id = 1,
                      uint64_t boot = kPeerBoot) {
    Message value;
    value.kind = kind;
    value.senderBoot = boot;
    value.receiverBoot = kind == Kind::Hello ? 0 : kLocalBoot;
    value.messageId = id;
    // Session receives reassembled bytes plus a decoded Hello; no JSON here.
    value.length = 1;
    value.payload[0] = 'x';
    return value;
}

Message statusMessage(uint32_t id = 20, uint64_t boot = kPeerBoot) {
    Message value;
    value.kind = Kind::Status;
    value.senderBoot = boot;
    value.receiverBoot = kLocalBoot;
    value.messageId = id;
    value.length = 1;
    value.payload[0] = 'x';
    return value;
}

void handshake(Session& session, uint32_t now = 100,
               uint64_t boot = kPeerBoot, uint32_t probeId = 1) {
    assert(session.expectHelloAck(probeId, now));
    assert(session.awaitingHelloAck(now));
    assert(session.hello(helloEnvelope(Kind::HelloAck, 1, boot), peerAck(probeId), now) ==
           HelloResult::Accepted);
    assert(!session.awaitingHelloAck(now));
    assert(session.canExchange());
    assert(session.peerBoot() == boot);
    assert(session.matches(boot, kLocalBoot));
}

void connect(Session& session, uint32_t now = 100) {
    assert(session.begin(pairing(), kLocalBoot));
    handshake(session, now);
    assert(session.heartbeat(envelope(Kind::Heartbeat, 10), now));
    assert(session.connected(now));
    assert(!session.freshStatus(now));
    assert(session.takeFailure() == LinkFailure::None);
}

void rejectPairing(const Pairing& value) {
    assert(!validPairing(value));
    Session session;
    assert(!session.begin(value, kLocalBoot));
    assert(!session.canExchange());
    assert(!session.connected(0));
    assert(!session.freshStatus(0));
}

void testPairingDeviceId() {
    assert(validPairing(pairing()));
    assert(validPairing(pairing(Role::Motion)));
    Pairing value = pairing();
    value.role = static_cast<Role>(0);
    rejectPairing(value);
    value.role = static_cast<Role>(3);
    rejectPairing(value);
    value = pairing();
    value.deviceId[0] = '\0';
    rejectPairing(value);
    std::memset(value.deviceId, 'a', sizeof(value.deviceId));
    rejectPairing(value);
    value.deviceId[64] = '\0';
    assert(validPairing(value));

    // Check bytes explicitly rather than inheriting the process locale's rules.
    for (unsigned c = 1; c <= 255; ++c) {
        const bool alnum = (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        value = pairing();
        value.deviceId[0] = static_cast<char>(c);
        value.deviceId[1] = '\0';
        assert(validPairing(value) == alnum);
        assert(validHello(peerHello(value)) == alnum);
        value.deviceId[0] = 'A';
        value.deviceId[1] = static_cast<char>(c);
        value.deviceId[2] = '\0';
        const bool allowed = alnum || c == '_' || c == '-';
        assert(validPairing(value) == allowed);
        assert(validHello(peerHello(value)) == allowed);
    }
}

void testPairingHexFields() {
    for (unsigned field = 0; field < 3; ++field) {
        Pairing value = pairing();
        char* text = field == 0 ? value.epoch :
                     field == 1 ? value.localPhysicalId : value.peerPhysicalId;
        const size_t width = field == 0 ? 32 : 12;
        std::memset(text, '0', width);
        rejectPairing(value);
        text[width - 1] = '1';
        assert(validPairing(value));
        text[width - 1] = '\0';
        rejectPairing(value);
        std::memset(text, '1', width + 1);
        rejectPairing(value);
        text[width] = '\0';
        for (unsigned c = 1; c <= 255; ++c) {
            text[0] = static_cast<char>(c);
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            assert(validPairing(value) == hex);
        }
    }
    Pairing value = pairing();
    std::strcpy(value.peerPhysicalId, value.localPhysicalId);
    rejectPairing(value);

    for (unsigned field = 0; field < 2; ++field) {
        Hello hello = peerHello();
        char* text = field == 0 ? hello.epoch : hello.physicalId;
        const size_t width = field == 0 ? 32 : 12;
        std::memset(text, '0', width);
        assert(!validHello(hello));
        text[width - 1] = '1';
        assert(validHello(hello));
        text[width - 1] = '\0';
        assert(!validHello(hello));
        std::memset(text, '1', width + 1);
        assert(!validHello(hello));
        text[width] = '\0';
        for (unsigned c = 1; c <= 255; ++c) {
            text[0] = static_cast<char>(c);
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            assert(validHello(hello) == hex);
        }
    }
    Hello hello = peerHello();
    hello.role = static_cast<Role>(0);
    assert(!validHello(hello));
    hello = peerHello();
    hello.deviceId[0] = '\0';
    assert(!validHello(hello));
    std::memset(hello.deviceId, 'a', sizeof(hello.deviceId));
    assert(!validHello(hello));
    hello.deviceId[64] = '\0';
    assert(validHello(hello));
}

void testBeginAndLocalHello() {
    Session session;
    assert(!session.canExchange());
    assert(!session.connected(0));
    assert(!session.freshStatus(0));
    assert(!session.matches(kPeerBoot, kLocalBoot));
    assert(session.takeFailure() == LinkFailure::None);
    assert(!session.begin(pairing(), 0));
    assert(!session.canExchange());
    for (Role role : {Role::Brain, Role::Motion}) {
        const Pairing value = pairing(role);
        assert(session.begin(value, kLocalBoot));
        assert(session.localBoot() == kLocalBoot);
        assert(!session.canExchange());
        assert(!session.connected(0));
        assert(!session.freshStatus(0));
        assert(!session.heartbeat(envelope(Kind::Heartbeat, 10), 0));
        assert(!session.status(statusMessage(), 0));
        session.poll(1500);
        session.poll(3000);
        assert(session.takeFailure() == LinkFailure::None);
        const Hello hello = session.localHello();
        assert(validHello(hello));
        assert(hello.protocol == 4 && hello.role == role);
        assert(hello.replyTo == 0);
        assert((hello.capabilities & UINT32_C(1)) != 0);
        assert(std::strcmp(hello.deviceId, value.deviceId) == 0);
        assert(std::strcmp(hello.epoch, value.epoch) == 0);
        assert(std::strcmp(hello.physicalId, value.localPhysicalId) == 0);
        assert(session.hello(helloEnvelope(), peerHello(value), 3000) == HelloResult::Accepted);
        assert(session.peerBoot() == 0);
        assert(!session.canExchange());
        assert(!session.connected(3000));
        assert(session.takeFailure() == LinkFailure::None);
        assert(!session.freshStatus(3000));
        assert(session.expectHelloAck(1, 3000));
        assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(1, value), 3000) ==
               HelloResult::Accepted);
        assert(session.matches(kPeerBoot, kLocalBoot));
        assert(!session.awaitingHelloAck(3000));
        assert(session.takeFailure() == LinkFailure::None);
    }
}

void rejectHelloWithoutPollution(const Message& frame, const Hello& hello) {
    Session fresh;
    assert(fresh.begin(pairing(), kLocalBoot));
    assert(fresh.expectHelloAck(42, 100));
    assert(fresh.hello(frame, hello, 100) != HelloResult::Accepted);
    assert(fresh.awaitingHelloAck(100));
    assert(!fresh.canExchange());
    assert(!fresh.connected(100));
    assert(!fresh.freshStatus(100));
    assert(fresh.takeFailure() == LinkFailure::None);
    assert(fresh.hello(helloEnvelope(Kind::HelloAck), peerAck(42), 100) ==
           HelloResult::Accepted);
    assert(!fresh.awaitingHelloAck(100));

    Session active;
    connect(active);
    assert(active.status(statusMessage(), 100));
    assert(active.expectHelloAck(42, 900));
    assert(active.hello(frame, hello, 1000) != HelloResult::Accepted);
    assert(active.awaitingHelloAck(1000));
    assert(active.canExchange());
    assert(active.peerBoot() == kPeerBoot);
    assert(active.matches(kPeerBoot, kLocalBoot));
    assert(active.connected(1000));
    assert(active.freshStatus(1000));
    assert(active.takeFailure() == LinkFailure::None);
    assert(!active.connected(1600));
    assert(!active.freshStatus(1600));
    active.poll(1600);
    assert(active.takeFailure() == LinkFailure::HeartbeatExpired);
    assert(active.takeFailure() == LinkFailure::None);
}

void testHelloIdentity() {
    // Invalid identities arrive with a different boot to catch premature reset.
    for (unsigned fault = 0; fault < 18; ++fault) {
        Hello hello = peerHello();
        switch (fault) {
        case 0: hello.role = Role::Brain; break;
        case 1: hello.role = static_cast<Role>(255); break;
        case 2: std::strcpy(hello.deviceId, "Other-device"); break;
        case 3: hello.deviceId[0] = '_'; break;
        case 4: hello.epoch[0] = '1'; break;
        case 5: std::strcpy(hello.physicalId, pairing().localPhysicalId); break;
        case 6: std::strcpy(hello.physicalId, "112345abcdef"); break;
        case 7: hello.protocol = 3; break;
        case 8: hello.protocol = 5; break;
        case 9: hello.capabilities = 0; break;
        case 10: hello.capabilities = UINT32_C(0xfffffffe); break;
        case 11: std::memset(hello.deviceId, 'a', sizeof(hello.deviceId)); break;
        case 12: std::memset(hello.epoch, '0', 32); break;
        case 13: std::memset(hello.physicalId, '0', 12); break;
        case 14: hello.epoch[31] = '\0'; break;
        case 15: hello.physicalId[11] = '\0'; break;
        case 16: hello.epoch[0] = 'A'; break;
        case 17: hello.physicalId[0] = 'A'; break;
        }
        rejectHelloWithoutPollution(helloEnvelope(Kind::Hello, 1, kRestartBoot), hello);
        hello.replyTo = 42;
        rejectHelloWithoutPollution(helloEnvelope(Kind::HelloAck, 1, kRestartBoot), hello);
    }
    Session session;
    assert(session.begin(pairing(), kLocalBoot));
    Hello hello = peerHello();
    hello.capabilities = UINT32_C(0xffffffff);
    assert(validHello(hello));
    assert(session.hello(helloEnvelope(), hello, 100) == HelloResult::Accepted);
    assert(!session.canExchange());
    assert(session.expectHelloAck(1, 100));
    hello.replyTo = 1;
    assert(session.hello(helloEnvelope(Kind::HelloAck), hello, 100) == HelloResult::Accepted);
    assert(session.takeFailure() == LinkFailure::None);
}

void testHelloProtocolMismatchPriority() {
    for (unsigned fault = 0; fault < 4; ++fault) {
        Hello hello = peerHello();
        switch (fault) {
        case 0: hello.protocol = 3; break;
        case 1: hello.protocol = 5; break;
        case 2: hello.capabilities = 0; break;
        case 3: hello.capabilities = UINT32_C(0xfffffffe); break;
        }
        assert(!validHello(hello));
        for (bool badIdentity : {false, true}) {
            if (badIdentity) {
                hello.role = Role::Brain;
                hello.deviceId[0] = '\0';
            }
            Session session;
            assert(session.begin(pairing(), kLocalBoot));
            assert(session.hello(helloEnvelope(), hello, 100) ==
                   HelloResult::ProtocolMismatch);
            assert(!session.canExchange());
        }
    }
}

void testHelloEnvelope() {
    Session unpaired;
    assert(unpaired.hello(helloEnvelope(), peerHello(), 0) != HelloResult::Accepted);
    assert(!unpaired.canExchange());

    for (Kind kind : {Kind::Hello, Kind::HelloAck}) {
        Message good = helloEnvelope(kind);
        good.receiverBoot = kLocalBoot;
        const Hello peer = kind == Kind::Hello ? peerHello() : peerAck(42);
        Session session;
        assert(session.begin(pairing(), kLocalBoot));
        assert(session.expectHelloAck(42, 100));
        assert(session.hello(good, peer, 100) == HelloResult::Accepted);
        assert(session.matches(kPeerBoot, kLocalBoot) == (kind == Kind::HelloAck));
        assert(session.peerBoot() == (kind == Kind::HelloAck ? kPeerBoot : 0));
        assert(session.awaitingHelloAck(100) == (kind == Kind::Hello));
        assert(session.takeFailure() == LinkFailure::None);
        Message bad = good;
        bad.receiverBoot = kLocalBoot - 1;
        rejectHelloWithoutPollution(bad, peer);
        for (unsigned fault = 0; fault < 4; ++fault) {
            bad = good;
            switch (fault) {
            case 0: bad.senderBoot = 0; break;
            case 1: bad.messageId = 0; break;
            case 2: bad.length = 0; break;
            case 3: bad.length = static_cast<uint16_t>(kMaxMessage + 1); break;
            }
            rejectHelloWithoutPollution(bad, peer);
        }
        for (uint16_t length : {uint16_t(160), uint16_t(161), uint16_t(2047)}) {
            Session fragmented;
            assert(fragmented.begin(pairing(), kLocalBoot));
            assert(fragmented.expectHelloAck(42, 100));
            good.length = length;
            std::memset(good.payload, 'x', length);
            assert(fragmented.hello(good, peer, 100) == HelloResult::Accepted);
            assert(fragmented.canExchange() == (kind == Kind::HelloAck));
            assert(fragmented.awaitingHelloAck(100) == (kind == Kind::Hello));
        }
    }
    Message bad = helloEnvelope(Kind::HelloAck);
    bad.receiverBoot = 0;
    rejectHelloWithoutPollution(bad, peerAck(42));
    rejectHelloWithoutPollution(helloEnvelope(Kind::Heartbeat), peerHello());
    rejectHelloWithoutPollution(helloEnvelope(Kind::Status), peerHello());
}

void testHelloOnlyDiscovers() {
    Session session;
    assert(session.begin(pairing(), kLocalBoot));
    assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(1), 100) !=
           HelloResult::Accepted);
    for (uint32_t now : {UINT32_C(100), UINT32_C(1400), UINT32_C(3000)}) {
        assert(session.hello(helloEnvelope(), peerHello(), now) == HelloResult::Accepted);
        assert(session.peerBoot() == 0);
        assert(!session.canExchange());
        assert(!session.matches(kPeerBoot, kLocalBoot));
        assert(!session.heartbeat(envelope(Kind::Heartbeat, 10), now));
        assert(!session.status(statusMessage(), now));
        assert(!session.connected(now));
        assert(!session.freshStatus(now));
        assert(!session.awaitingHelloAck(now));
        session.poll(now);
        assert(session.takeFailure() == LinkFailure::None);
    }
    assert(session.expectHelloAck(42, 3000));
    assert(session.hello(helloEnvelope(), peerHello(), 3001) == HelloResult::Accepted);
    assert(session.awaitingHelloAck(3001));
    assert(session.peerBoot() == 0);
    assert(!session.canExchange());
    rejectHelloWithoutPollution(helloEnvelope(), peerAck(42));
}

void testProbeLifecycle() {
    Session session;
    assert(!session.expectHelloAck(1, 0));
    assert(!session.awaitingHelloAck(0));
    assert(session.begin(pairing(), kLocalBoot));
    assert(!session.expectHelloAck(0, 100));
    assert(!session.awaitingHelloAck(100));
    assert(session.expectHelloAck(10, 100));
    assert(!session.canExchange());
    for (uint32_t id : {UINT32_C(0), UINT32_C(9), UINT32_C(10), UINT32_C(11), UINT32_MAX}) {
        assert(!session.expectHelloAck(id, 1000));
        assert(session.awaitingHelloAck(1000));
    }
    for (uint32_t reply : {UINT32_C(0), UINT32_C(9), UINT32_C(11)}) {
        assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(reply), 1000) !=
               HelloResult::Accepted);
        assert(session.awaitingHelloAck(1000));
        assert(session.peerBoot() == 0);
        assert(!session.canExchange());
    }
    // Failed registrations and invalid traffic cannot move the original deadline.
    assert(session.awaitingHelloAck(1599));
    assert(!session.awaitingHelloAck(1600));
    assert(!session.expectHelloAck(9, 1600));
    assert(!session.expectHelloAck(10, 1600));
    assert(session.expectHelloAck(11, 1600));
    assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(10), 1600) !=
           HelloResult::Accepted);
    assert(session.awaitingHelloAck(1600));
    // Correlation uses replyTo, not the peer's independent envelope message ID.
    const Message ack = helloEnvelope(Kind::HelloAck, 777);
    assert(session.hello(ack, peerAck(11), 1601) == HelloResult::Accepted);
    assert(!session.awaitingHelloAck(1601));
    assert(session.canExchange());
    assert(!session.connected(1601));
    assert(session.takeFailure() == LinkFailure::None);
    assert(session.hello(ack, peerAck(11), 1602) != HelloResult::Accepted);
    assert(!session.awaitingHelloAck(1602));
    assert(!session.expectHelloAck(11, 1602));
    assert(session.expectHelloAck(12, 1602));
    assert(session.hello(ack, peerAck(11), 1603) != HelloResult::Accepted);
    assert(session.awaitingHelloAck(1603));
    assert(session.hello(ack, peerAck(12), 1603) == HelloResult::Accepted);
    assert(!session.awaitingHelloAck(1603));

    Session exhausted;
    assert(exhausted.begin(pairing(), kLocalBoot));
    assert(exhausted.expectHelloAck(UINT32_MAX, 0));
    assert(exhausted.hello(ack, peerAck(UINT32_MAX), 0) == HelloResult::Accepted);
    exhausted.poll(1500);
    assert(exhausted.takeFailure() == LinkFailure::HeartbeatExpired);
    for (uint32_t id : {UINT32_C(0), UINT32_C(1), UINT32_MAX - 1, UINT32_MAX}) {
        assert(!exhausted.expectHelloAck(id, 1500));
        assert(!exhausted.awaitingHelloAck(1500));
    }
    assert(exhausted.hello(ack, peerAck(UINT32_MAX), 1500) != HelloResult::Accepted);
    assert(!exhausted.canExchange());
}

void testProbeDeadlineAndWraparound() {
    for (uint32_t start : {UINT32_C(0), UINT32_C(100), UINT32_MAX - 500}) {
        Session justInTime;
        assert(justInTime.begin(pairing(), kLocalBoot));
        assert(justInTime.expectHelloAck(1, start));
        assert(justInTime.awaitingHelloAck(start + UINT32_C(1499)));
        assert(justInTime.hello(helloEnvelope(Kind::HelloAck), peerAck(1),
                               start + UINT32_C(1499)) == HelloResult::Accepted);
        assert(!justInTime.awaitingHelloAck(start + UINT32_C(1499)));
        assert(justInTime.canExchange());
        assert(!justInTime.connected(start + UINT32_C(1499)));
        assert(justInTime.heartbeat(envelope(Kind::Heartbeat, 10), start + UINT32_C(1499)));
        assert(justInTime.connected(start + UINT32_C(1500)));

        for (bool pollFirst : {false, true}) {
            Session expired;
            assert(expired.begin(pairing(), kLocalBoot));
            assert(expired.expectHelloAck(1, start));
            expired.poll(start + UINT32_C(1499));
            assert(expired.awaitingHelloAck(start + UINT32_C(1499)));
            if (pollFirst) expired.poll(start + UINT32_C(1500));
            assert(!expired.awaitingHelloAck(start + UINT32_C(1500)));
            assert(expired.hello(helloEnvelope(Kind::HelloAck), peerAck(1),
                                 start + UINT32_C(1500)) != HelloResult::Accepted);
            assert(expired.hello(helloEnvelope(Kind::HelloAck), peerAck(1),
                                 start + UINT32_C(1501)) != HelloResult::Accepted);
            assert(!expired.canExchange());
            assert(expired.peerBoot() == 0);
            assert(!expired.connected(start + UINT32_C(1501)));
            assert(!expired.freshStatus(start + UINT32_C(1501)));
            expired.poll(start + UINT32_C(1501));
            assert(expired.takeFailure() == LinkFailure::None);
            assert(!expired.expectHelloAck(1, start + UINT32_C(1501)));
            handshake(expired, start + UINT32_C(1501), kPeerBoot, 2);
            assert(expired.takeFailure() == LinkFailure::None);
        }
    }
}

void replayOldBoot(Session& session, uint32_t now) {
    assert(session.hello(helloEnvelope(), peerHello(), now) == HelloResult::Accepted);
    assert(session.peerBoot() == kRestartBoot);
    assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(1), now) !=
           HelloResult::Accepted);
    assert(!session.heartbeat(envelope(Kind::Heartbeat, 10), now));
    assert(!session.status(statusMessage(20), now));
    assert(session.peerBoot() == kRestartBoot);
    assert(!session.matches(kPeerBoot, kLocalBoot));
    assert(!session.freshStatus(now));
    assert(session.takeFailure() == LinkFailure::None);
}

void testOldBootCannotRestoreReady() {
    Session session;
    connect(session);
    assert(session.status(statusMessage(20), 100));
    assert(session.freshStatus(100));
    assert(session.expectHelloAck(2, 200));
    assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(1), 200) !=
           HelloResult::Accepted);
    assert(session.awaitingHelloAck(200));
    assert(session.peerBoot() == kPeerBoot);
    const Message newAck = helloEnvelope(Kind::HelloAck, 1, kRestartBoot);
    assert(session.hello(newAck, peerAck(2), 200) == HelloResult::Accepted);
    assert(!session.awaitingHelloAck(200));
    assert(session.takeFailure() == LinkFailure::PeerRestarted);
    assert(!session.connected(200));
    replayOldBoot(session, 201);
    assert(!session.connected(201));

    assert(session.heartbeat(envelope(Kind::Heartbeat, 2, kRestartBoot), 210));
    assert(session.status(statusMessage(3, kRestartBoot), 210));
    assert(session.freshStatus(210));
    assert(session.heartbeat(envelope(Kind::Heartbeat, 4, kRestartBoot), 1600));
    assert(session.connected(1710));
    assert(!session.freshStatus(1710));
    replayOldBoot(session, 1710);
    assert(session.connected(1710));

    session.poll(3100);
    assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
    replayOldBoot(session, 3100);
    assert(!session.canExchange());
    assert(session.hello(newAck, peerAck(2), 3100) != HelloResult::Accepted);
    assert(!session.canExchange());
    assert(session.expectHelloAck(3, 3200));
    replayOldBoot(session, 3200);
    assert(session.awaitingHelloAck(3200));
    assert(session.hello(newAck, peerAck(2), 3200) != HelloResult::Accepted);
    assert(session.awaitingHelloAck(3200));
    assert(!session.canExchange());
    assert(session.hello(newAck, peerAck(3), 3200) == HelloResult::Accepted);
    assert(!session.awaitingHelloAck(3200));
    assert(!session.freshStatus(3200));
    assert(!session.heartbeat(envelope(Kind::Heartbeat, 4, kRestartBoot), 3200));
    assert(session.heartbeat(envelope(Kind::Heartbeat, 5, kRestartBoot), 3200));
    assert(!session.status(statusMessage(3, kRestartBoot), 3200));
    assert(session.status(statusMessage(6, kRestartBoot), 3200));
    assert(session.freshStatus(3200));
    assert(session.takeFailure() == LinkFailure::None);
}

void testHelloDoesNotRefreshHeartbeat() {
    Session session;
    connect(session);
    assert(session.status(statusMessage(), 100));
    assert(session.hello(helloEnvelope(Kind::Hello, 30), peerHello(), 1400) == HelloResult::Accepted);
    assert(session.connected(1599));
    assert(!session.connected(1600));
    assert(!session.freshStatus(1600));
    session.poll(1600);
    assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
    session.poll(1601);
    session.poll(3100);
    assert(session.takeFailure() == LinkFailure::None);
    assert(!session.canExchange());

    Session noHeartbeat;
    assert(noHeartbeat.begin(pairing(), kLocalBoot));
    handshake(noHeartbeat, 100);
    assert(!noHeartbeat.connected(100));
    assert(!noHeartbeat.freshStatus(100));
    assert(!noHeartbeat.status(statusMessage(), 100));
    assert(noHeartbeat.expectHelloAck(2, 1400));
    assert(noHeartbeat.hello(helloEnvelope(Kind::HelloAck, 2), peerAck(2), 1400) ==
           HelloResult::Accepted);
    noHeartbeat.poll(1599);
    assert(noHeartbeat.takeFailure() == LinkFailure::None);
    noHeartbeat.poll(1600);
    assert(!noHeartbeat.connected(1600));
    assert(!noHeartbeat.canExchange());
    assert(noHeartbeat.takeFailure() == LinkFailure::HeartbeatExpired);
    noHeartbeat.poll(4000);
    assert(noHeartbeat.takeFailure() == LinkFailure::None);
}

void testRepeatedHelloPreservesIds() {
    for (Kind kind : {Kind::Hello, Kind::HelloAck}) {
        Session session;
        connect(session);
        assert(session.status(statusMessage(), 100));
        if (kind == Kind::HelloAck) assert(session.expectHelloAck(2, 1000));
        const Hello peer = kind == Kind::Hello ? peerHello() : peerAck(2);
        assert(session.hello(helloEnvelope(kind, 30), peer, 1000) ==
               HelloResult::Accepted);
        assert(!session.heartbeat(envelope(Kind::Heartbeat, 10), 1000));
        assert(!session.status(statusMessage(), 1000));
        assert(session.freshStatus(1599));
        assert(!session.freshStatus(1600));
        assert(!session.connected(1600));
    }
}

void testHeartbeatValidation() {
    for (unsigned fault = 0; fault < 12; ++fault) {
        Session session;
        connect(session);
        Frame bad = envelope(Kind::Heartbeat, 100);
        switch (fault) {
        case 0: bad.senderBoot = kRestartBoot; break;
        case 1: bad.receiverBoot = kLocalBoot - 1; break;
        case 2: bad.senderBoot = 0; break;
        case 3: bad.receiverBoot = 0; break;
        case 4: bad.messageId = 0; break;
        case 5: bad.messageId = 10; break;
        case 6: bad.messageId = 9; break;
        case 7: bad = envelope(Kind::Hello, 100); break;
        case 8: bad.kind = Kind::StatusQuery; break;
        case 9: bad.total = 1; break;
        case 10: bad.length = 1; break;
        case 11: bad.offset = 1; break;
        }
        assert(!session.heartbeat(bad, 1400));
        assert(session.peerBoot() == kPeerBoot);
        assert(session.connected(1599));
        assert(!session.connected(1600));
        assert(session.takeFailure() == LinkFailure::None);
        session.poll(1600);
        assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
        session.poll(1700);
        assert(session.takeFailure() == LinkFailure::None);

        Session unchangedWatermark;
        connect(unchangedWatermark);
        assert(!unchangedWatermark.heartbeat(bad, 1000));
        assert(unchangedWatermark.heartbeat(envelope(Kind::Heartbeat, 11), 1001));
        assert(unchangedWatermark.connected(1001));
    }
    Session session;
    connect(session);
    assert(session.heartbeat(envelope(Kind::Heartbeat, 11), 1599));
    assert(session.connected(3098));
    assert(!session.connected(3099));
    assert(!session.freshStatus(1599));
    assert(!session.matches(kRestartBoot, kLocalBoot));
    assert(!session.matches(kPeerBoot, 0));
    assert(!session.matches(kPeerBoot, kLocalBoot - 1));
}

void testStatusValidation() {
    for (unsigned fault = 0; fault < 10; ++fault) {
        Session session;
        connect(session);
        assert(session.status(statusMessage(), 100));
        Message bad = statusMessage(100);
        switch (fault) {
        case 0: bad.kind = Kind::Context; break;
        case 1: bad.senderBoot = kRestartBoot; break;
        case 2: bad.receiverBoot = kLocalBoot - 1; break;
        case 3: bad.receiverBoot = 0; break;
        case 4: bad.senderBoot = 0; break;
        case 5: bad.length = 0; break;
        case 6: bad.length = static_cast<uint16_t>(kMaxMessage + 1); break;
        case 7: bad.messageId = 0; break;
        case 8: bad.messageId = 20; break;
        case 9: bad.messageId = 19; break;
        }
        assert(session.heartbeat(envelope(Kind::Heartbeat, 11), 1400));
        assert(!session.status(bad, 1400));
        assert(session.connected(1600));
        assert(session.freshStatus(1599));
        assert(!session.freshStatus(1600));
        // A rejected high ID must not advance the accepted-status watermark.
        assert(session.status(statusMessage(21), 1600));
        assert(session.freshStatus(1600));
        assert(session.takeFailure() == LinkFailure::None);
    }
    Session session;
    connect(session);
    Message maximum = statusMessage();
    maximum.length = static_cast<uint16_t>(kMaxMessage);
    std::memset(maximum.payload, 'x', sizeof(maximum.payload));
    assert(session.status(maximum, 100));
    assert(session.freshStatus(100));
}

void testIndependentStatusFreshness() {
    Session session;
    connect(session);
    assert(session.status(statusMessage(), 100));
    assert(session.heartbeat(envelope(Kind::Heartbeat, 21), 1400));
    assert(session.freshStatus(1599));
    assert(!session.freshStatus(1600));
    assert(session.connected(1600));
    session.poll(1600);
    assert(session.takeFailure() == LinkFailure::None);
    assert(session.heartbeat(envelope(Kind::Heartbeat, 22), 2800));
    assert(!session.freshStatus(2800));
    assert(session.status(statusMessage(23), 2800));
    assert(session.freshStatus(2800));

    Session onlyStatus;
    connect(onlyStatus);
    assert(onlyStatus.status(statusMessage(), 1400));
    assert(onlyStatus.freshStatus(1599));
    assert(!onlyStatus.connected(1600));
    assert(!onlyStatus.freshStatus(1600));
    onlyStatus.poll(1600);
    assert(onlyStatus.takeFailure() == LinkFailure::HeartbeatExpired);
}

void testSameBootReconnect() {
    for (bool pollFirst : {false, true}) {
        Session session;
        connect(session);
        assert(session.status(statusMessage(), 100));
        if (pollFirst) session.poll(1600);
        assert(!session.connected(1600));
        assert(!session.freshStatus(1600));
        assert(!session.heartbeat(envelope(Kind::Heartbeat, 30), 1600));
        assert(!session.status(statusMessage(31), 1600));
        session.poll(1600);
        assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
        assert(session.takeFailure() == LinkFailure::None);
        assert(!session.canExchange());
        assert(!session.matches(kPeerBoot, kLocalBoot));

        assert(session.hello(helloEnvelope(), peerHello(), 1700) == HelloResult::Accepted);
        assert(!session.canExchange());
        assert(session.hello(helloEnvelope(Kind::HelloAck), peerAck(1), 1700) !=
               HelloResult::Accepted);
        assert(!session.canExchange());
        handshake(session, 1700, kPeerBoot, 2);
        assert(session.takeFailure() == LinkFailure::None);
        assert(!session.freshStatus(1700));
        assert(!session.heartbeat(envelope(Kind::Heartbeat, 10), 1700));
        assert(!session.heartbeat(envelope(Kind::Heartbeat, 9), 1700));
        assert(session.heartbeat(envelope(Kind::Heartbeat, 11), 1700));
        assert(session.connected(1700));
        assert(!session.status(statusMessage(20), 1700));
        assert(!session.status(statusMessage(19), 1700));
        assert(!session.freshStatus(1700));
        assert(session.status(statusMessage(21), 1700));
        assert(session.freshStatus(1700));
        session.poll(3200);
        assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
        session.poll(3201);
        assert(session.takeFailure() == LinkFailure::None);
    }
}

void testFreshAckAdvancesBothWatermarks() {
    for (bool pollFirst : {false, true}) {
        Session session;
        connect(session);
        assert(session.status(statusMessage(20), 100));
        // These frames were sent before the new ACK, but never received here.
        const Frame delayedHeartbeat = envelope(Kind::Heartbeat, 30);
        const Message delayedStatus = statusMessage(40);
        if (pollFirst) session.poll(1600);
        assert(!session.connected(1700));
        assert(!session.freshStatus(1700));
        assert(session.expectHelloAck(2, 1700));
        assert(session.hello(helloEnvelope(Kind::HelloAck, 100), peerAck(2), 1700) ==
               HelloResult::Accepted);
        assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
        assert(session.takeFailure() == LinkFailure::None);
        assert(!session.awaitingHelloAck(1700));
        assert(session.matches(kPeerBoot, kLocalBoot));
        assert(!session.connected(1700));
        assert(!session.freshStatus(1700));
        assert(!session.heartbeat(delayedHeartbeat, 1701));
        for (uint32_t id : {UINT32_C(11), UINT32_C(99), UINT32_C(100)})
            assert(!session.heartbeat(envelope(Kind::Heartbeat, id), 1701));
        assert(!session.connected(1701));

        assert(session.heartbeat(envelope(Kind::Heartbeat, 101), 1702));
        assert(session.connected(1702));
        // A live heartbeat must not authorize an unseen pre-ACK status.
        assert(!session.status(delayedStatus, 1702));
        for (uint32_t id : {UINT32_C(21), UINT32_C(99), UINT32_C(100)})
            assert(!session.status(statusMessage(id), 1702));
        assert(!session.freshStatus(1702));
        assert(session.status(statusMessage(102), 1703));
        assert(session.freshStatus(1703));
        assert(session.takeFailure() == LinkFailure::None);
    }
}

void testPeerRestart() {
    Session session;
    connect(session);
    assert(session.status(statusMessage(), 100));
    assert(session.hello(helloEnvelope(Kind::Hello, 1, kRestartBoot), peerHello(), 200) ==
           HelloResult::Accepted);
    assert(session.peerBoot() == kPeerBoot);
    assert(session.freshStatus(200));
    assert(session.takeFailure() == LinkFailure::None);
    handshake(session, 200, kRestartBoot, 2);
    assert(session.peerBoot() == kRestartBoot);
    assert(!session.freshStatus(200));
    assert(!session.matches(kPeerBoot, kLocalBoot));
    assert(session.matches(kRestartBoot, kLocalBoot));
    assert(session.takeFailure() == LinkFailure::PeerRestarted);
    assert(session.takeFailure() == LinkFailure::None);
    assert(!session.heartbeat(envelope(Kind::Heartbeat, 30), 200));
    assert(!session.status(statusMessage(31), 200));
    assert(session.heartbeat(envelope(Kind::Heartbeat, 2, kRestartBoot), 200));
    assert(session.status(statusMessage(3, kRestartBoot), 200));
    assert(session.freshStatus(200));
    assert(session.hello(helloEnvelope(Kind::HelloAck, 4, kRestartBoot), peerAck(2), 300) !=
           HelloResult::Accepted);
    assert(session.takeFailure() == LinkFailure::None);
    assert(session.freshStatus(300));
    session.poll(1700);
    assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
    assert(session.hello(helloEnvelope(Kind::Hello, 1, kPeerBoot), peerHello(), 1800) ==
           HelloResult::Accepted);
    assert(session.peerBoot() == kRestartBoot);
    assert(!session.canExchange());
    assert(session.takeFailure() == LinkFailure::None);
    // A previously seen boot may return only with a response to a new probe.
    handshake(session, 1800, kPeerBoot, 3);
    assert(session.takeFailure() == LinkFailure::PeerRestarted);
    assert(session.takeFailure() == LinkFailure::None);
}

void testIdsDoNotWrap() {
    Session session;
    connect(session);
    assert(session.heartbeat(envelope(Kind::Heartbeat, UINT32_MAX), 200));
    assert(session.status(statusMessage(UINT32_MAX), 200));
    assert(!session.heartbeat(envelope(Kind::Heartbeat, 1), 300));
    assert(!session.status(statusMessage(1), 300));
    session.poll(1700);
    assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
    handshake(session, 1800, kPeerBoot, 2);
    assert(!session.heartbeat(envelope(Kind::Heartbeat, UINT32_MAX), 1800));
    assert(!session.heartbeat(envelope(Kind::Heartbeat, 1), 1800));
    assert(!session.status(statusMessage(UINT32_MAX), 1800));
    assert(!session.status(statusMessage(1), 1800));
    assert(!session.freshStatus(1800));
}

void testTimeWraparound() {
    Session atZero;
    connect(atZero, 0);
    assert(atZero.status(statusMessage(), 0));
    assert(atZero.connected(1499));
    assert(atZero.freshStatus(1499));
    assert(!atZero.connected(1500));
    assert(!atZero.freshStatus(1500));

    const uint32_t start = UINT32_MAX - 500;
    Session session;
    connect(session, start);
    assert(session.status(statusMessage(), start));
    assert(session.connected(start + UINT32_C(1499)));
    assert(session.freshStatus(start + UINT32_C(1499)));
    assert(!session.connected(start + UINT32_C(1500)));
    assert(!session.freshStatus(start + UINT32_C(1500)));
    session.poll(start + UINT32_C(1499));
    assert(session.takeFailure() == LinkFailure::None);
    session.poll(start + UINT32_C(1500));
    assert(session.takeFailure() == LinkFailure::HeartbeatExpired);
    session.poll(start + UINT32_C(1501));
    assert(session.takeFailure() == LinkFailure::None);

    Session active;
    connect(active, start);
    assert(active.status(statusMessage(), start));
    assert(active.heartbeat(envelope(Kind::Heartbeat, 21), start + UINT32_C(1000)));
    assert(active.connected(start + UINT32_C(1500)));
    assert(!active.freshStatus(start + UINT32_C(1500)));
    assert(active.status(statusMessage(22), start + UINT32_C(1500)));
    assert(active.freshStatus(start + UINT32_C(1500)));
    assert(active.connected(start + UINT32_C(2499)));
    assert(!active.connected(start + UINT32_C(2500)));

    Session noHeartbeat;
    assert(noHeartbeat.begin(pairing(), kLocalBoot));
    handshake(noHeartbeat, start);
    noHeartbeat.poll(start + UINT32_C(1499));
    assert(noHeartbeat.takeFailure() == LinkFailure::None);
    noHeartbeat.poll(start + UINT32_C(1500));
    assert(noHeartbeat.takeFailure() == LinkFailure::HeartbeatExpired);
}

}  // namespace

int main() {
    testPairingDeviceId();
    testPairingHexFields();
    testBeginAndLocalHello();
    testHelloIdentity();
    testHelloProtocolMismatchPriority();
    testHelloEnvelope();
    testHelloOnlyDiscovers();
    testProbeLifecycle();
    testProbeDeadlineAndWraparound();
    testOldBootCannotRestoreReady();
    testHelloDoesNotRefreshHeartbeat();
    testRepeatedHelloPreservesIds();
    testHeartbeatValidation();
    testStatusValidation();
    testIndependentStatusFreshness();
    testSameBootReconnect();
    testFreshAckAdvancesBothWatermarks();
    testPeerRestart();
    testIdsDoNotWrap();
    testTimeWraparound();
    std::puts("BoardSessionV4 tests passed");
    return 0;
}
