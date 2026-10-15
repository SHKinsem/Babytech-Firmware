#include "BoardInstall.h"
#include "BoardMaintenance.h"
#include "BoardTransmitV4.h"
#include "FakeBrainNvs.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech;
using namespace babytech::boardlink;
namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition); } while (0)
constexpr char brainMac[] = "010203040506", motionMac[] = "0a0b0c0d0e0f";
constexpr char nonce[] = "1234567890abcdef1234567890abcdef";
constexpr uint64_t brainBoot = 11, motionBoot = 22;
unsigned scenarios = 0, failures = 0;

struct Target : BoardInstallTarget {
    CommissioningImport received;
    CommissioningResult answer = CommissioningResult::Installed;
    unsigned calls = 0;
    uint64_t requesterBoot = 0;
    char receivedNonce[33]{};
    bool authorized = true;
    CommissioningResult install(const CommissioningImport& request, const char* challenge,
                                uint64_t boot) override {
        ++calls;
        received = request;
        requesterBoot = boot;
        std::strcpy(receivedNonce, challenge);
        if (!request.hasContext) {
            const auto& c = request.context;
            CHECK(!c.deviceId[0] && !c.profileVersion && !c.cleared && !c.babyId[0] &&
                  !c.babyName[0] && !c.formulaBrand[0] && !c.waterMl && !c.temperatureC &&
                  c.powderGPer100Ml == 0 && !std::signbit(c.powderGPer100Ml));
        }
        return authorized ? answer : CommissioningResult::Unsafe;
    }
};

void makeImport(CommissioningImport& request, unsigned kind) {
    request.pairing.role = v4::Role::Motion;
    std::memset(request.pairing.deviceId, 'd', 64);
    request.pairing.deviceId[64] = 0;
    std::strcpy(request.pairing.epoch, "abcdef1234567890abcdef1234567890");
    std::strcpy(request.pairing.localPhysicalId, motionMac);
    std::strcpy(request.pairing.peerPhysicalId, brainMac);
    request.hasContext = kind != 0;
    if (!request.hasContext) return;
    std::strcpy(request.context.deviceId, request.pairing.deviceId);
    request.context.profileVersion = INT32_MAX;
    request.context.cleared = kind == 2;
    if (request.context.cleared) return;
    std::memset(request.context.babyId, 'b', 96);
    request.context.babyId[96] = 0;
    for (size_t i = 0; i < 320; i += 4)
        std::memcpy(request.context.babyName + i, "\xf0\x9f\x98\x80", 4);
    request.context.babyName[320] = 0;
    for (size_t i = 0; i < 480; i += 3)
        std::memcpy(request.context.formulaBrand + i, "\xe5\xa5\xb6", 3);
    request.context.formulaBrand[480] = 0;
    request.context.waterMl = 237;
    request.context.temperatureC = 43;
    request.context.powderGPer100Ml = 13.123456f;
}

void deliver(BoardInstall& receiver, const v4::Message& message, uint32_t now) {
    v4::Parser parser;
    for (size_t offset = 0; offset < message.length; offset += v4::kMaxFragment) {
        v4::Frame fragment, parsed;
        CHECK(v4::fragment(message, offset, fragment));
        uint8_t wire[v4::kMaxFrame];
        const auto length = v4::encode(fragment, wire, sizeof(wire));
        CHECK(length != 0);
        bool seen = false;
        for (size_t i = 0; i < length; ++i) if (parser.push(wire[i], now, parsed)) {
            CHECK(!seen);
            seen = true;
            receiver.receive(parsed, now);
        }
        CHECK(seen);
    }
}

struct ReceiveBuffers {
    v4::Assembler assembler;
    v4::Message scratch{};
};

struct Rig {
    ReceiveBuffers brainReceive, motionReceive;
    BoardInstall brain, motion;
    Target target;
    CommissioningImport request;
    v4::Message original{}, response{}, altered{};
    explicit Rig(unsigned kind = 1) {
        makeImport(request, kind);
        CHECK(brain.bindReceiveBuffers(brainReceive.assembler, brainReceive.scratch));
        CHECK(motion.bindReceiveBuffers(motionReceive.assembler, motionReceive.scratch));
        CHECK(brain.begin(v4::Role::Brain, brainMac, brainBoot));
        CHECK(motion.begin(v4::Role::Motion, motionMac, motionBoot));
        CHECK(motion.setTarget(&target));
    }
    void start(uint32_t now = 0) {
        CHECK(brain.request(request, nonce, motionBoot, now));
        CHECK(brain.outgoing());
        original = *brain.outgoing();
        brain.queued();
    }
    void install(uint32_t now = 1) {
        deliver(motion, original, now);
        CHECK(motion.outgoing());
        response = *motion.outgoing();
        motion.queued();
    }
    void finish(uint32_t now = 2) {
        deliver(brain, response, now);
        CHECK(brain.state() == BoardInstallState::Complete);
    }
    size_t pairLength() const { return size_t(original.payload[33]) | (size_t(original.payload[34]) << 8); }
    size_t flag() const { return 35 + pairLength(); }
    size_t context() const { return flag() + 3; }
    void rewritePair(v4::Message& message, const v4::Pairing& pairing) {
        CHECK(encodePairingRecord(pairing, message.payload + 35, pairLength()) == pairLength());
    }
};

void scenario(const std::string& name, const std::function<void()>& body) {
    ++scenarios;
    fake_brain::reset();
    fake_commissioning::reset();
    fake_product_crypto::reset();
    try {
        body();
        CHECK(fake_brain::io.calls.empty());
        CHECK(fake_brain::io.disk.empty());
        CHECK(fake_commissioning::macCalls == 0);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
    }
}

void success() {
    for (unsigned kind : {0u, 1u, 2u}) scenario("absent/full UTF8/tombstone", [=] {
        auto r = std::make_unique<Rig>(kind);
        CHECK(r->brain.boot() == brainBoot && !std::strcmp(r->brain.physicalId(), brainMac));
        r->start();
        CHECK(r->original.kind == v4::Kind::MigrationInstall);
        CHECK(!v4::isControl(r->original.kind));
        CHECK(r->original.payload[r->original.length - 1] == 1);
        if (kind == 1) CHECK(r->original.length == 39 + 134 + kContextIdentityMaxSize);
        r->install();
        CHECK(r->target.calls == 1 && r->target.requesterBoot == brainBoot);
        CHECK(!std::strcmp(r->target.receivedNonce, nonce));
        CHECK(r->target.received.hasContext == r->request.hasContext);
        CHECK(!std::memcmp(&r->target.received.pairing, &r->request.pairing, sizeof(v4::Pairing)));
        if (kind) CHECK(sameProductContext(r->target.received.context, r->request.context));
        r->finish();
        CHECK(r->brain.result() == CommissioningResult::Installed);
        CHECK(!r->brain.outgoing());
    });
    const CommissioningResult results[] = {CommissioningResult::Installed,
        CommissioningResult::AlreadyInstalled, CommissioningResult::Invalid,
        CommissioningResult::IdentityMismatch, CommissioningResult::Conflict,
        CommissioningResult::Unsafe, CommissioningResult::LegacyPending,
        CommissioningResult::StorageFault, CommissioningResult::StateMissing};
    for (unsigned i = 0; i < 9; ++i) scenario("explicit result " + std::to_string(i), [=] {
        auto r = std::make_unique<Rig>();
        r->target.answer = results[i];
        r->start(); r->install();
        CHECK(r->response.length == 34 && r->response.payload[0] == 2);
        CHECK(r->response.payload[33] == i);
        r->finish();
        CHECK(r->brain.result() == results[i]);
    });
    scenario("no target and malformed target enum", [] {
        auto r = std::make_unique<Rig>();
        CHECK(r->motion.setTarget(nullptr));
        r->start(); r->install(); r->finish();
        CHECK(r->motion.state() == BoardInstallState::Unavailable);
        CHECK(r->brain.result() == CommissioningResult::StateMissing && r->target.calls == 0);
        CHECK(r->motion.setTarget(&r->target));
        r->target.answer = static_cast<CommissioningResult>(255);
        r->start(5); r->install(6); r->finish(7);
        CHECK(r->brain.result() == CommissioningResult::Invalid);
    });
    scenario("handoff flag is not authorization and Unsafe permits explicit retry", [] {
        auto r = std::make_unique<Rig>();
        r->target.authorized = false;
        r->start(); r->install(); r->finish();
        CHECK(r->brain.result() == CommissioningResult::Unsafe);
        const auto firstId = r->original.messageId;
        deliver(r->motion, r->original, 3);
        CHECK(r->target.calls == 1 && r->motion.outgoing()->payload[33] == 5);
        r->motion.queued();
        r->target.authorized = true;
        r->start(4);
        CHECK(r->original.messageId > firstId);
        r->install(5); r->finish(6);
        CHECK(r->target.calls == 2 && r->brain.result() == CommissioningResult::Installed);
    });
    scenario("absent after present clears reusable context scratch", [] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install(); r->finish();
        r->request.hasContext = false;
        CHECK(!r->brain.request(r->request, nonce, motionBoot, 3));
        r->request.context = ProductContext{};
        r->start(4); r->install(5); r->finish(6);
        CHECK(!r->target.received.hasContext && !r->target.received.context.deviceId[0]);
    });
}

void validation() {
    for (unsigned mutation = 0; mutation < 15; ++mutation) scenario("request API validation", [=] {
        auto r = std::make_unique<Rig>();
        const char* challenge = nonce;
        uint64_t peer = motionBoot;
        switch (mutation) {
            case 0: challenge = nullptr; break;
            case 1: challenge = "00000000000000000000000000000000"; break;
            case 2: challenge = "ABCDEF1234567890abcdef1234567890"; break;
            case 3: challenge = "1234"; break;
            case 4: challenge = "1234567890abcdef1234567890abcdef0"; break;
            case 5: peer = 0; break;
            case 6: peer = brainBoot; break;
            case 7: r->request.pairing.role = v4::Role::Brain; break;
            case 8: r->request.pairing.peerPhysicalId[0] = 'f'; break;
            case 9: std::strcpy(r->request.pairing.localPhysicalId, brainMac); break;
            case 10: r->request.pairing.epoch[0] = 'z'; break;
            case 11: r->request.context.deviceId[0] = 'x'; break;
            case 12: r->request.context.babyName[0] = char(0xff); break;
            case 13: r->request.context.waterMl = 0; break;
            case 14: r->request.pairing.localPhysicalId[12] = 'a'; break;
        }
        CHECK(!r->brain.request(r->request, challenge, peer, 0));
        CHECK(r->brain.state() == BoardInstallState::Idle && !r->brain.outgoing());
        CHECK(!r->motion.request(r->request, nonce, brainBoot, 0));
    });
    for (unsigned mutation = 0; mutation < 7; ++mutation) scenario("begin validation/reset", [=] {
        auto receive = std::make_unique<ReceiveBuffers>();
        auto channel = std::make_unique<BoardInstall>();
        CHECK(channel->bindReceiveBuffers(receive->assembler, receive->scratch));
        const char* mac = brainMac;
        uint64_t boot = brainBoot;
        v4::Role role = v4::Role::Brain;
        if (mutation == 0) mac = nullptr;
        if (mutation == 1) mac = "000000000000";
        if (mutation == 2) mac = "01020304050F";
        if (mutation == 3) mac = "01020304050";
        if (mutation == 4) mac = "010203040506a";
        if (mutation == 5) boot = 0;
        if (mutation == 6) role = static_cast<v4::Role>(0);
        CHECK(!channel->begin(role, mac, boot));
        CHECK(channel->begin(v4::Role::Brain, brainMac, brainBoot));
        CHECK(!channel->begin(v4::Role::Brain, brainMac, brainBoot));
        CHECK(!channel->setTarget(nullptr));
        channel->reset();
        CHECK(channel->boot() == 0 && !channel->physicalId()[0] && !channel->outgoing());
        CHECK(channel->state() == BoardInstallState::Idle);
        CHECK(channel->begin(v4::Role::Motion, motionMac, motionBoot));
    });
    scenario("unbound initialization refused, null reset/poll/receive safe", [] {
        auto r = std::make_unique<Rig>(); r->start();
        auto receive = std::make_unique<ReceiveBuffers>();
        auto channel = std::make_unique<BoardInstall>();
        v4::Frame frame;
        CHECK(v4::fragment(r->original, 0, frame));
        CHECK(!channel->begin(v4::Role::Motion, motionMac, motionBoot));
        CHECK(!channel->request(r->request, nonce, brainBoot, 0));
        CHECK(!channel->setTarget(&r->target));
        channel->receive(frame, 1); channel->poll(3000); channel->reset();
        CHECK(channel->bindReceiveBuffers(receive->assembler, receive->scratch));
        CHECK(channel->begin(v4::Role::Motion, motionMac, motionBoot));
        CHECK(channel->bindReceiveBuffers(receive->assembler, receive->scratch));
        CHECK(!channel->bindReceiveBuffers(r->brainReceive.assembler, r->brainReceive.scratch));
        CHECK(channel->setTarget(&r->target));
        deliver(*channel, r->original, 3001);
        CHECK(r->target.calls == 1 && channel->outgoing());
        channel->reset();
        CHECK(!receive->assembler.active() && receive->scratch.length == 0);
        for (uint8_t byte : receive->scratch.payload) CHECK(byte == 0);
        CHECK(channel->begin(v4::Role::Motion, motionMac, motionBoot));
        CHECK(channel->setTarget(&r->target));
        deliver(*channel, r->original, 3002);
        CHECK(r->target.calls == 2);
    });
    for (unsigned field = 0; field < 10; ++field) scenario("nonempty absent context API rejected", [=] {
        auto r = std::make_unique<Rig>(0);
        auto& c = r->request.context;
        if (field == 0) c.deviceId[0] = 'x';
        if (field == 1) c.profileVersion = 1;
        if (field == 2) c.cleared = true;
        if (field == 3) c.babyId[0] = 'x';
        if (field == 4) c.babyName[0] = 'x';
        if (field == 5) c.formulaBrand[0] = 'x';
        if (field == 6) c.waterMl = 1;
        if (field == 7) c.temperatureC = 1;
        if (field == 8) c.powderGPer100Ml = 1;
        if (field == 9) c.powderGPer100Ml = -0.0f;
        CHECK(!r->brain.request(r->request, nonce, motionBoot, 0));
        CHECK(!r->brain.outgoing());
    });
}

void malformed() {
    // Each complete malformed message is followed by the valid same-ID request:
    // malformed traffic must neither execute nor consume the replay fence.
    for (unsigned mutation = 0; mutation < 24; ++mutation) scenario("malformed wire", [=] {
        auto r = std::make_unique<Rig>();
        r->start();
        r->altered = r->original;
        auto& m = r->altered;
        const auto flag = r->flag(), ctx = r->context();
        switch (mutation) {
            case 0: m.payload[0] = 2; break;
            case 1: m.payload[1] = 'z'; break;
            case 2: std::memset(m.payload + 1, '0', 32); break;
            case 3: m.payload[1] = 0; break;
            case 4: m.payload[1] = 'A'; break;
            case 5: m.payload[33] = m.payload[34] = 0; break;
            case 6: m.payload[33] = m.payload[34] = 255; break;
            case 7: --m.payload[33]; break;
            case 8: m.payload[35] ^= 1; break;
            case 9: m.payload[43] ^= 1; break;
            case 10: m.payload[flag] = 2; break;
            case 11: m.payload[flag] = 0; break;
            case 12: m.payload[flag + 1] = m.payload[flag + 2] = 0; break;
            case 13: m.payload[flag + 1] = m.payload[flag + 2] = 255; break;
            case 14: --m.payload[flag + 1]; break;
            case 15: m.payload[ctx] = 2; break;
            case 16: m.payload[ctx + 1] = 2; break;
            case 17: m.payload[ctx + 4] = 'x'; break;
            case 18: m.payload[ctx + 180] = 255; break;
            case 19: m.payload[m.length - 1] = 0; break;
            case 20: m.payload[m.length - 1] = 2; break;
            case 21: m.payload[m.length++] = 1; break;
            case 22: --m.length; break;
            case 23: m.payload[m.length - 2] = 127; break;
        }
        deliver(r->motion, m, 1);
        CHECK(r->target.calls == 0 && !r->motion.outgoing());
        r->install(2);
        CHECK(r->target.calls == 1);
    });
    scenario("every truncated payload", [] {
        auto r = std::make_unique<Rig>();
        r->start();
        for (uint16_t length = 1; length < r->original.length; ++length) {
            r->altered = r->original;
            r->altered.length = length;
            deliver(r->motion, r->altered, 1);
            CHECK(r->target.calls == 0 && !r->motion.outgoing());
        }
        r->install(2);
        CHECK(r->target.calls == 1);
    });
    for (unsigned mutation = 0; mutation < 4; ++mutation) scenario("canonical wrong MAC/role pairing", [=] {
        auto r = std::make_unique<Rig>();
        r->start(); r->altered = r->original;
        auto pair = r->request.pairing;
        if (mutation == 0) pair.localPhysicalId[0] = 'f';
        if (mutation == 1) pair.role = v4::Role::Brain;
        if (mutation == 2) pair.peerPhysicalId[0] = 'f';
        if (mutation == 3) {
            pair.deviceId[0] = 'x';
        }
        r->rewritePair(r->altered, pair);
        // The peer MAC is authorized by the target, not falsely inferred from
        // an unauthenticated UART header or a saved pairing on this channel.
        if (mutation == 2) r->target.authorized = false;
        deliver(r->motion, r->altered, 1);
        CHECK(r->target.calls == unsigned(mutation == 2));
        if (mutation == 2) {
            CHECK(r->motion.outgoing()->payload[33] == 5);
            r->motion.queued();
        } else CHECK(!r->motion.outgoing());
    });
    for (unsigned mutation = 0; mutation < 4; ++mutation) scenario("absent context flags", [=] {
        auto r = std::make_unique<Rig>(0);
        r->start(); r->altered = r->original;
        if (mutation == 0) r->altered.payload[r->flag()] = 1;
        if (mutation == 1) r->altered.payload[r->flag() + 1] = 1;
        if (mutation == 2) r->altered.payload[r->flag()] = 255;
        if (mutation == 3) --r->altered.length;
        deliver(r->motion, r->altered, 1);
        CHECK(!r->motion.outgoing() && r->target.calls == 0);
    });
}

void duplicates() {
    scenario("exact duplicates cached, lower IDs ignored", [] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install();
        r->target.answer = CommissioningResult::StorageFault;
        deliver(r->motion, r->original, 2);
        CHECK(r->target.calls == 1 && r->motion.outgoing());
        CHECK(r->motion.outgoing()->payload[33] == 0);
        r->motion.queued();
        r->finish(3);
        const auto old = std::make_unique<v4::Message>(r->original);
        r->target.answer = CommissioningResult::AlreadyInstalled;
        r->start(4); r->install(5);
        CHECK(r->target.calls == 2);
        deliver(r->motion, *old, 6);
        CHECK(r->target.calls == 2 && !r->motion.outgoing());
    });
    for (unsigned mutation = 0; mutation < 4; ++mutation) scenario("changed duplicate ignored not old response", [=] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install();
        r->altered = r->original;
        if (mutation == 0) r->altered.payload[1] = 'f';
        if (mutation == 1) {
            --r->request.context.profileVersion;
            CHECK(encodeContextIdentity(r->request.context, r->altered.payload + r->context(),
                                        kContextIdentityMaxSize) == kContextIdentityMaxSize);
        }
        if (mutation == 2) {
            auto pair = r->request.pairing;
            pair.epoch[0] = 'f';
            r->rewritePair(r->altered, pair);
        }
        if (mutation == 3) --r->altered.length;
        deliver(r->motion, r->altered, 2);
        CHECK(r->target.calls == 1 && !r->motion.outgoing());
        deliver(r->motion, r->original, 3);
        CHECK(r->target.calls == 1 && r->motion.outgoing()->payload[33] == 0);
    });
    scenario("foreign malformed boot cannot poison accepted fence", [] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install();
        r->altered = r->original;
        r->altered.senderBoot = 99;
        r->altered.messageId = UINT32_MAX;
        r->altered.payload[r->altered.length - 1] = 0;
        deliver(r->motion, r->altered, 2);
        CHECK(!r->motion.outgoing() && r->target.calls == 1);
        deliver(r->motion, r->original, 3);
        CHECK(r->motion.outgoing()->payload[33] == 0 && r->target.calls == 1);
        r->motion.queued(); r->finish(4);
        r->start(5); r->install(6);
        CHECK(r->target.calls == 2);
    });
    scenario("valid new boot remains target authorized", [] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install();
        r->altered = r->original;
        r->altered.senderBoot = 99;
        r->target.authorized = false;
        deliver(r->motion, r->altered, 2);
        CHECK(r->target.calls == 2 && r->target.requesterBoot == 99);
        CHECK(r->motion.outgoing()->payload[33] == 5);
    });
    for (CommissioningResult denied : {CommissioningResult::Unsafe, CommissioningResult::StateMissing,
                                      CommissioningResult::StorageFault})
        scenario("foreign denial cannot evict original owner fence", [=] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install();
        r->altered = r->original; r->altered.senderBoot = 99;
        r->altered.messageId = UINT32_MAX;
        r->target.answer = denied;
        deliver(r->motion, r->altered, 2);
        CHECK(r->target.calls == 2 && r->motion.outgoing());
        r->motion.queued();
        deliver(r->motion, r->altered, 3);
        CHECK(r->target.calls == 2 && r->motion.outgoing());
        r->motion.queued();
        r->altered = r->original; r->altered.payload[1] = 'f';
        deliver(r->motion, r->altered, 4);
        CHECK(r->target.calls == 2 && !r->motion.outgoing());
        deliver(r->motion, r->original, 5);
        CHECK(r->target.calls == 2 && r->motion.outgoing()->payload[33] == 0);
        CHECK(r->motion.result() == CommissioningResult::Installed);
        r->motion.queued();
        r->finish(6);
        r->target.answer = CommissioningResult::Installed;
        r->start(7); r->install(8); r->finish(9);
        CHECK(r->target.calls == 3);
    });
    scenario("SHA failure never executes or advances fence", [] {
        auto r = std::make_unique<Rig>(); r->start();
        fake_product_crypto::fail = true;
        deliver(r->motion, r->original, 1);
        CHECK(r->target.calls == 0 && !r->motion.outgoing());
        fake_product_crypto::fail = false;
        r->install(2);
        CHECK(r->target.calls == 1);
        fake_product_crypto::fail = true;
        deliver(r->motion, r->original, 3);
        CHECK(r->target.calls == 1 && !r->motion.outgoing());
        fake_product_crypto::fail = false;
        deliver(r->motion, r->original, 4);
        CHECK(r->target.calls == 1 && r->motion.outgoing()->payload[33] == 0);
    });
    for (bool installed : {false, true}) scenario("same boot denied high ID cannot poison owner domain", [=] {
        auto r = std::make_unique<Rig>();
        r->start();
        if (installed) { r->install(); r->finish(); }
        r->altered = r->original;
        r->altered.messageId = UINT32_MAX;
        auto wrongPair = r->request.pairing;
        wrongPair.peerPhysicalId[0] = 'f';
        r->rewritePair(r->altered, wrongPair);
        r->target.authorized = false;
        deliver(r->motion, r->altered, 3);
        const auto deniedCalls = r->target.calls;
        CHECK(deniedCalls == (installed ? 2u : 1u));
        CHECK(r->motion.outgoing() && r->motion.outgoing()->payload[33] == 5);
        r->motion.queued();
        deliver(r->motion, r->altered, 4);
        CHECK(r->target.calls == deniedCalls && r->motion.outgoing()->payload[33] == 5);
        r->motion.queued();
        // A changed rejected tuple reaches the guard again, never borrowing
        // either a cached response or an authoritative ID fence from Unsafe.
        r->altered.payload[1] = 'f';
        deliver(r->motion, r->altered, 5);
        CHECK(r->target.calls == deniedCalls + 1 && r->motion.outgoing()->payload[33] == 5);
        r->motion.queued();
        r->target.authorized = true;
        if (installed) {
            r->start(6);
            CHECK(r->original.messageId == 2);
        } else CHECK(r->original.messageId == 1);
        r->install(7); r->finish(8);
        CHECK(r->target.calls == deniedCalls + 2 && r->brain.result() == CommissioningResult::Installed);
    });
    for (bool installed : {false, true}) scenario("wrong MAC denied tuple cannot block legitimate same boot and ID", [=] {
        auto r = std::make_unique<Rig>();
        r->start();
        if (installed) { r->install(); r->finish(); r->start(4); }
        CHECK(r->original.messageId == (installed ? 2u : 1u));
        r->altered = r->original;
        auto wrongPair = r->request.pairing;
        wrongPair.peerPhysicalId[0] = 'f';
        r->rewritePair(r->altered, wrongPair);
        r->target.authorized = false;
        deliver(r->motion, r->altered, 5);
        const unsigned deniedCalls = installed ? 2 : 1;
        CHECK(r->target.calls == deniedCalls && r->motion.outgoing()->payload[33] == 5);
        r->motion.queued();
        deliver(r->motion, r->altered, 6);
        CHECK(r->target.calls == deniedCalls && r->motion.outgoing()->payload[33] == 5);
        r->motion.queued();
        r->target.authorized = true;
        r->install(7); r->finish(8);
        CHECK(r->target.calls == deniedCalls + 1 && r->brain.result() == CommissioningResult::Installed);
        CHECK(!std::strcmp(r->target.received.pairing.peerPhysicalId, brainMac));
        r->altered = r->original; r->altered.payload[1] = 'f';
        deliver(r->motion, r->altered, 9);
        CHECK(r->target.calls == deniedCalls + 1 && !r->motion.outgoing());
        deliver(r->motion, r->original, 10);
        CHECK(r->target.calls == deniedCalls + 1 && r->motion.outgoing()->payload[33] == 0);
    });
    for (CommissioningResult denied : {CommissioningResult::Unsafe, CommissioningResult::StateMissing,
                                      CommissioningResult::StorageFault, CommissioningResult::Conflict})
        scenario("same boot denied result never advances success watermark", [=] {
        auto r = std::make_unique<Rig>(); r->start(); r->install(); r->finish();
        r->altered = r->original; r->altered.messageId = UINT32_MAX;
        r->target.answer = denied;
        deliver(r->motion, r->altered, 3);
        CHECK(r->target.calls == 2 && r->motion.outgoing());
        r->motion.queued();
        r->target.answer = CommissioningResult::AlreadyInstalled;
        r->start(4); r->install(5); r->finish(6);
        CHECK(r->original.messageId == 2 && r->target.calls == 3);
        CHECK(r->brain.result() == CommissioningResult::AlreadyInstalled);
        r->altered = r->original; r->altered.payload[1] = 'f';
        deliver(r->motion, r->altered, 7);
        CHECK(r->target.calls == 3 && !r->motion.outgoing());
    });
}

void responses() {
    for (unsigned mutation = 0; mutation < 10; ++mutation) scenario("foreign/invalid response ignored", [=] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install(); r->altered = r->response;
        switch (mutation) {
            case 0: r->altered.senderBoot = 99; break;
            case 1: r->altered.receiverBoot = 99; break;
            case 2: ++r->altered.messageId; break;
            case 3: r->altered.payload[0] = 1; break;
            case 4: r->altered.payload[1] = 'f'; break;
            case 5: r->altered.payload[33] = 9; break;
            case 6: r->altered.payload[33] = 255; break;
            case 7: --r->altered.length; break;
            case 8: r->altered.payload[r->altered.length++] = 0; break;
            case 9: r->altered.kind = v4::Kind::CommandResult; break;
        }
        deliver(r->brain, r->altered, 2);
        CHECK(r->brain.state() == BoardInstallState::Pending);
        r->finish(3);
        deliver(r->brain, r->response, 4);
        CHECK(r->brain.result() == CommissioningResult::Installed);
    });
    scenario("old response cannot complete new explicit request", [] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install(); r->finish();
        r->altered = r->response;
        r->start(4);
        deliver(r->brain, r->altered, 5);
        CHECK(r->brain.state() == BoardInstallState::Pending);
        r->install(6); r->finish(7);
    });
}

void deadlines() {
    for (uint32_t began : {0u, UINT32_MAX - 1000}) scenario("drop response/deadline wrap/explicit retry", [=] {
        auto r = std::make_unique<Rig>();
        r->start(began); r->install(began + 1);
        r->brain.poll(began + 2999);
        CHECK(r->brain.state() == BoardInstallState::Pending);
        r->brain.poll(began + 3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut && !r->brain.outgoing());
        CHECK(r->target.calls == 1);
        deliver(r->brain, r->response, began + 3001);
        CHECK(r->brain.state() == BoardInstallState::TimedOut);
        r->brain.poll(began + 10000);
        CHECK(!r->brain.outgoing() && r->target.calls == 1);
        const auto id = r->original.messageId;
        r->target.answer = CommissioningResult::AlreadyInstalled;
        r->start(began + 10001);
        CHECK(r->original.messageId > id);
        r->install(began + 10002); r->finish(began + 10003);
        CHECK(r->brain.result() == CommissioningResult::AlreadyInstalled);
    });
    scenario("entire request dropped and outgoing timeout withdrawn", [] {
        auto r = std::make_unique<Rig>();
        CHECK(r->brain.request(r->request, nonce, motionBoot, 0));
        r->original = *r->brain.outgoing();
        r->brain.poll(3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut);
        CHECK(!r->brain.outgoing());
        r->brain.queued();
        r->brain.poll(9000);
        CHECK(r->target.calls == 0 && !r->brain.outgoing());
        r->start(9001); r->install(9002); r->finish(9003);
    });
    scenario("response at exact deadline ignored", [] {
        auto r = std::make_unique<Rig>();
        r->start(); r->install();
        deliver(r->brain, r->response, 3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut);
    });
}

void fragments() {
    scenario("borrowed core buffers reused by ordinary and install RX", [] {
        auto r = std::make_unique<Rig>(); r->start();
        r->altered = r->original;
        r->altered.kind = v4::Kind::Context;
        r->altered.messageId = r->original.messageId;
        v4::Frame frame;
        CHECK(v4::fragment(r->altered, 0, frame));
        CHECK(r->motionReceive.assembler.accept(frame, 1, r->motionReceive.scratch) ==
              v4::AssemblyResult::Incomplete);
        CHECK(v4::fragment(r->original, 0, frame));
        r->motion.receive(frame, 2);
        CHECK(r->target.calls == 0 && !r->motion.outgoing());
        for (size_t offset = 160; offset < r->altered.length; offset += 160) {
            CHECK(v4::fragment(r->altered, offset, frame));
            const auto result = r->motionReceive.assembler.accept(frame, 3, r->motionReceive.scratch);
            CHECK(result == (offset + frame.length == r->altered.length
                            ? v4::AssemblyResult::Complete : v4::AssemblyResult::Incomplete));
            r->motion.receive(frame, 3);
        }
        CHECK(r->target.calls == 0 && r->motionReceive.scratch.kind == v4::Kind::Context);
        deliver(r->motion, r->original, 4);
        CHECK(r->motion.outgoing());
        r->response = *r->motion.outgoing();
        CHECK(r->target.calls == 1 && r->motionReceive.scratch.kind == v4::Kind::MigrationInstall);
        // A subsequent ordinary receive overwrites the borrowed RX scratch,
        // but target import staging and the copied response remain intact.
        r->altered.length = 1;
        CHECK(v4::fragment(r->altered, 0, frame));
        CHECK(r->motionReceive.assembler.accept(frame, 5, r->motionReceive.scratch) ==
              v4::AssemblyResult::Complete);
        CHECK(sameProductContext(r->target.received.context, r->request.context));
        CHECK(!std::memcmp(r->motion.outgoing(), &r->response, sizeof(v4::Message)));
        r->motion.queued();
        r->finish(6);
    });
    scenario("timeout preserves unrelated borrowed partial RX, reset preserves binding", [] {
        auto r = std::make_unique<Rig>(); r->start();
        r->altered = r->original; r->altered.kind = v4::Kind::Context;
        r->altered.receiverBoot = brainBoot; r->altered.senderBoot = motionBoot;
        v4::Frame frame;
        CHECK(v4::fragment(r->altered, 0, frame));
        CHECK(r->brainReceive.assembler.accept(frame, 2999, r->brainReceive.scratch) ==
              v4::AssemblyResult::Incomplete);
        r->brain.poll(3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut && r->brainReceive.assembler.active());
        for (size_t offset = 160; offset < r->altered.length; offset += 160) {
            CHECK(v4::fragment(r->altered, offset, frame));
            const auto result = r->brainReceive.assembler.accept(frame, 3001, r->brainReceive.scratch);
            CHECK(result == (offset + frame.length == r->altered.length
                            ? v4::AssemblyResult::Complete : v4::AssemblyResult::Incomplete));
        }
        CHECK(r->brainReceive.scratch.kind == v4::Kind::Context);
        r->start(3002); r->install(3003); r->finish(3004);
        r->brain.reset();
        CHECK(r->brainReceive.scratch.length == 0 && !r->brainReceive.assembler.active());
        CHECK(r->brain.bindReceiveBuffers(r->brainReceive.assembler, r->brainReceive.scratch));
        CHECK(r->brain.begin(v4::Role::Brain, brainMac, brainBoot));
    });
    scenario("timeout cancels only the matching incomplete install response", [] {
        auto r = std::make_unique<Rig>(); r->start(); r->install(1);
        v4::Frame first;
        CHECK(v4::fragment(r->response, 0, first));
        first.length = 17;
        r->brain.receive(first, 2999);
        CHECK(r->brainReceive.assembler.active());
        r->brain.poll(3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut && !r->brainReceive.assembler.active());
    });
    scenario("ordinary same-ID traffic does not cancel in-flight installation", [] {
        auto r = std::make_unique<Rig>(); r->start();
        v4::Frame first;
        CHECK(v4::fragment(r->original, 0, first));
        r->motion.receive(first, 1);
        r->altered = r->original; r->altered.kind = v4::Kind::Context;
        CHECK(v4::fragment(r->altered, 0, first));
        CHECK(r->motionReceive.assembler.accept(first, 2, r->motionReceive.scratch) ==
              v4::AssemblyResult::Busy);
        CHECK(r->motionReceive.assembler.active());
        for (size_t offset = 160; offset < r->original.length; offset += 160) {
            CHECK(v4::fragment(r->original, offset, first));
            r->motion.receive(first, 3);
        }
        CHECK(r->target.calls == 1 && r->motion.outgoing());
    });
    for (unsigned mutation = 0; mutation < 7; ++mutation) scenario("wrong boot/invalid frame no side effect", [=] {
        auto r = std::make_unique<Rig>(); r->start();
        v4::Frame f;
        CHECK(v4::fragment(r->original, 0, f));
        if (mutation == 0) f.receiverBoot = 99;
        if (mutation == 1) f.senderBoot = motionBoot;
        if (mutation == 2) f.senderBoot = 0;
        if (mutation == 3) f.receiverBoot = 0;
        if (mutation == 4) f.messageId = 0;
        if (mutation == 5) f.total = v4::kMaxMessage + 1;
        if (mutation == 6) f.kind = v4::Kind::MigrationMaintenance;
        r->motion.receive(f, 1);
        r->install(2);
        CHECK(r->target.calls == 1);
    });
    for (uint32_t began : {0u, UINT32_MAX - 500}) scenario("dropped middle fragment expires with wrap", [=] {
        auto r = std::make_unique<Rig>(); r->start(began);
        v4::Frame first, third;
        CHECK(v4::fragment(r->original, 0, first));
        CHECK(v4::fragment(r->original, 320, third));
        r->motion.receive(first, began);
        CHECK(!r->motion.setTarget(nullptr));
        r->motion.receive(first, began + 1);
        r->motion.receive(third, began + 2);
        CHECK(r->target.calls == 0 && !r->motion.outgoing());
        r->motion.receive(first, began + 3);
        r->motion.poll(began + 1003);
        CHECK(r->motion.setTarget(&r->target));
        r->install(began + 1004);
        CHECK(r->target.calls == 1);
    });
    scenario("foreign fragments do not displace in-flight valid assembly", [] {
        auto r = std::make_unique<Rig>(); r->start();
        v4::Frame f, foreign;
        CHECK(v4::fragment(r->original, 0, f));
        r->motion.receive(f, 1);
        foreign = f;
        foreign.senderBoot = 99; foreign.messageId = UINT32_MAX;
        r->motion.receive(foreign, 2);
        for (size_t offset = 160; offset < r->original.length; offset += 160) {
            CHECK(v4::fragment(r->original, offset, f));
            r->motion.receive(f, 3);
        }
        CHECK(r->target.calls == 1 && r->motion.outgoing());
    });
    scenario("pending response cannot be overwritten or replaced", [] {
        auto r = std::make_unique<Rig>(); r->start();
        deliver(r->motion, r->original, 1);
        CHECK(r->motion.outgoing()); r->response = *r->motion.outgoing();
        r->altered = r->original; ++r->altered.messageId;
        deliver(r->motion, r->altered, 2);
        r->motion.poll(9000);
        CHECK(r->target.calls == 1);
        CHECK(!r->motion.setTarget(nullptr));
        CHECK(!std::memcmp(r->motion.outgoing(), &r->response, sizeof(v4::Message)));
        r->motion.queued();
        deliver(r->motion, r->altered, 9001);
        CHECK(r->target.calls == 2);
    });
    scenario("reset clears target/outgoing/assembly and begins again in place", [] {
        auto r = std::make_unique<Rig>(); r->start();
        v4::Frame first;
        CHECK(v4::fragment(r->original, 0, first));
        r->motion.receive(first, 1);
        r->motion.reset(); r->brain.reset();
        CHECK(r->motion.state() == BoardInstallState::Idle && !r->motion.outgoing());
        CHECK(r->motion.boot() == 0 && !r->motion.physicalId()[0]);
        CHECK(r->motion.begin(v4::Role::Motion, motionMac, motionBoot));
        CHECK(r->brain.begin(v4::Role::Brain, brainMac, brainBoot));
        r->start(3); r->install(4); r->finish(5);
        CHECK(r->target.calls == 0 && r->brain.result() == CommissioningResult::StateMissing);
        CHECK(r->motion.setTarget(&r->target));
        r->start(6); r->install(7); r->finish(8);
        CHECK(r->target.calls == 1);
        r->motion.reset();
        CHECK(!r->motion.outgoing() && r->motion.result() == CommissioningResult::Invalid);
    });
}

struct Sink : v4::ByteSink {
    std::vector<uint8_t> bytes;
    bool idle() const override { return true; }
    size_t available() const override { return v4::kMaxFrame; }
    size_t write(const uint8_t* data, size_t size) override {
        bytes.insert(bytes.end(), data, data + size);
        return size;
    }
};
struct LeaseTarget : BoardMaintenanceTarget {
    bool safeToAcquire() const override { return true; }
    bool safeToRelease() const override { return true; }
};

void interleaving() {
    scenario("transmitter backpressure timeout cannot enqueue late install", [] {
        auto r = std::make_unique<Rig>();
        auto tx = std::make_unique<v4::Transmitter>();
        Sink sink;
        r->altered.kind = v4::Kind::Context;
        r->altered.senderBoot = brainBoot; r->altered.receiverBoot = motionBoot;
        r->altered.messageId = 77; r->altered.length = 1; r->altered.payload[0] = 1;
        CHECK(tx->enqueue(r->altered));
        CHECK(r->brain.request(r->request, nonce, motionBoot, 0));
        CHECK(!tx->enqueue(*r->brain.outgoing()));
        r->brain.poll(2999);
        CHECK(r->brain.outgoing());
        r->brain.poll(3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut && !r->brain.outgoing());
        CHECK(tx->pump(sink));
        v4::Parser parser;
        v4::Frame frame;
        for (uint8_t byte : sink.bytes) if (parser.push(byte, 3001, frame)) r->motion.receive(frame, 3001);
        CHECK(!tx->pending() && r->target.calls == 0 && !r->motion.outgoing());
        r->brain.poll(6000);
        CHECK(!r->brain.outgoing() && r->target.calls == 0);
    });
    scenario("already copied TX may execute after timeout, no rollback assumption", [] {
        auto r = std::make_unique<Rig>();
        auto tx = std::make_unique<v4::Transmitter>();
        Sink sink;
        CHECK(r->brain.request(r->request, nonce, motionBoot, 0));
        CHECK(tx->enqueue(*r->brain.outgoing()));
        r->brain.queued(); r->brain.poll(3000);
        CHECK(r->brain.state() == BoardInstallState::TimedOut);
        v4::Parser parser;
        v4::Frame frame;
        size_t consumed = 0;
        while (tx->pending()) {
            CHECK(tx->pump(sink));
            while (consumed < sink.bytes.size())
                if (parser.push(sink.bytes[consumed++], 3001, frame)) r->motion.receive(frame, 3001);
        }
        CHECK(r->target.calls == 1 && r->motion.outgoing());
        deliver(r->brain, *r->motion.outgoing(), 3002);
        CHECK(r->brain.state() == BoardInstallState::TimedOut && !r->brain.outgoing());
    });
    scenario("sole transmitter copies message and lease renew interleaves fragments", [] {
        auto r = std::make_unique<Rig>(); r->start();
        auto tx = std::make_unique<v4::Transmitter>();
        Sink sink;
        CHECK(tx->enqueue(r->original));
        CHECK(!tx->enqueue(r->original));
        CHECK(tx->pump(sink));
        BoardMaintenance lease;
        LeaseTarget target;
        CHECK(lease.begin(v4::Role::Motion, motionMac, motionBoot));
        CHECK(lease.setTarget(&target));
        v4::Frame acquire;
        acquire.kind = v4::Kind::MigrationMaintenance;
        acquire.senderBoot = brainBoot; acquire.receiverBoot = motionBoot;
        acquire.messageId = 1;
        acquire.payload[0] = 1; acquire.payload[1] = 64;
        std::memcpy(acquire.payload + 2, r->request.pairing.deviceId, 64);
        std::memcpy(acquire.payload + 66, brainMac, 12);
        std::memcpy(acquire.payload + 78, nonce, 32);
        acquire.length = acquire.total = 110;
        lease.receive(acquire, 0);
        CHECK(lease.owns(r->request.pairing.deviceId, nonce, brainBoot));
        lease.queued();
        // Maintenance is ordinary as well: the adapter must schedule its frame
        // through the same transmitter, not introduce a second UART writer.
        const auto renewal = std::make_unique<v4::Message>();
        renewal->kind = acquire.kind; renewal->senderBoot = brainBoot;
        renewal->receiverBoot = motionBoot; renewal->messageId = 2;
        renewal->length = acquire.length;
        std::memcpy(renewal->payload, acquire.payload, acquire.length);
        renewal->payload[0] = 2;
        CHECK(!tx->enqueue(*renewal));
        v4::Parser parser;
        v4::Frame frame;
        size_t consumed = 0;
        auto parse = [&](uint32_t now) {
            while (consumed < sink.bytes.size()) if (parser.push(sink.bytes[consumed++], now, frame)) {
                r->motion.receive(frame, now);
                lease.receive(frame, now);
            }
        };
        parse(1);
        // Incoming renewals can arrive between install fragments without
        // disturbing the shared receive assembler or the lease owner.
        acquire.messageId = 2; acquire.payload[0] = 2;
        r->motion.receive(acquire, 500); lease.receive(acquire, 500);
        lease.queued();
        while (tx->pending()) { CHECK(tx->pump(sink)); parse(501); }
        CHECK(r->target.calls == 1 && r->motion.outgoing());
        CHECK(lease.owns(r->request.pairing.deviceId, nonce, brainBoot));
        CHECK(tx->enqueue(*renewal));
        CHECK(tx->pump(sink)); parse(1000);
        CHECK(lease.owns(r->request.pairing.deviceId, nonce, brainBoot));
        CHECK(tx->healthy());
    });
}
}

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, void (*)()>> groups = {
        {"success", success}, {"validation", validation}, {"malformed", malformed},
        {"duplicates", duplicates}, {"responses", responses}, {"deadlines", deadlines},
        {"fragments", fragments}, {"interleaving", interleaving}
    };
    bool selected = false;
    for (const auto& group : groups) {
        if (argc > 1 && group.first != std::string(argv[1])) continue;
        selected = true;
        const auto before = scenarios;
        group.second();
        std::printf("%s: %u scenarios\n", group.first, scenarios - before);
    }
    if (!selected) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("BoardInstall: %u scenarios, %u failures; sizeof=%zu bytes\n",
                scenarios, failures, sizeof(BoardInstall));
    return failures ? 1 : 0;
}
