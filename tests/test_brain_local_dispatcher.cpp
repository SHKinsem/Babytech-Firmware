#include "brain_local_dispatcher.h"
#include "brain_pending_recovery.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace display = babytech::display;
namespace nvs = fake_brain;
namespace crypto = fake_product_crypto;
using nvs::Op;

namespace {
uint32_t nowMs = 100;
unsigned scenarios = 0, failures = 0;
std::function<void()> clockHook;
std::function<void(const ProductRequest&, uint32_t)> acceptanceHook;
constexpr char execution[] = "11111111111111111111111111111111";

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    "line " + std::to_string(__LINE__) + ": " #condition); } while (false)

uint32_t clockNow() { if (clockHook) clockHook(); return nowMs; }
void observeAcceptance(const ProductRequest& request, uint32_t at) {
    CHECK(acceptanceHook); acceptanceHook(request, at);
}
v4::Pairing pairing(v4::Role role = v4::Role::Brain) {
    v4::Pairing p;
    p.role = role;
    std::strcpy(p.deviceId, "Babytech_local-test");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, role == v4::Role::Brain ? "112233445566" : "aabbccddeeff");
    std::strcpy(p.peerPhysicalId, role == v4::Role::Brain ? "aabbccddeeff" : "112233445566");
    return p;
}
ProductContext context(uint32_t version = 10, bool cleared = false) {
    ProductContext c;
    std::strcpy(c.deviceId, pairing().deviceId);
    c.profileVersion = version;
    c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, "original-baby");
        std::strcpy(c.babyName, "Full original baby name, beyond display capacity");
        std::strcpy(c.formulaBrand, "Full original formula brand, beyond display capacity");
        c.waterMl = 180; c.temperatureC = 45; c.powderGPer100Ml = 13.5f;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(ProductCommand command, uint64_t sequence, const ProductContext& c = context()) {
    ProductRequest r;
    r.command = command; r.sequence = sequence;
    std::strcpy(r.deviceId, pairing().deviceId);
    CHECK(makeLocalCommandId(pairing(), sequence, r.commandId));
    if (command == ProductCommand::Prepare) {
        std::strcpy(r.babyId, c.babyId);
        r.profileVersion = c.profileVersion;
        r.waterMl = c.waterMl; r.temperatureC = c.temperatureC; r.powderGPer100Ml = c.powderGPer100Ml;
    }
    CHECK(validProductRequest(r));
    return r;
}
CommandResult result(const ProductRequest& r, bool accepted = true, const char* reason = "accepted") {
    CommandResult reply;
    reply.source = r.source; reply.sequence = r.sequence; reply.accepted = accepted;
    std::strcpy(reply.commandId, r.commandId); std::strcpy(reply.reason, reason);
    return reply;
}
nvs::Database protectedData(bool motion) {
    auto disk = nvs::io.disk;
    disk["brainstate"].erase("record");
    if (motion) disk["productstate"].erase("record");
    return disk;
}
void scenario(const std::string& name, const std::function<void()>& run, bool motion = false) {
    ++scenarios; nvs::reset(); crypto::reset(); nowMs = 100; clockHook = {}; acceptanceHook = {};
    for (const char* space : {"brainstate", "productstate", "productpair", "productctx",
                              "outbox", "wifi-cfg", "actuatorcfg", "sensorcfg"})
        nvs::io.disk[space]["unrelated"] = {{0, 0xff, 0x7f}, nvs::Type::Blob};
    const auto protectedBefore = protectedData(motion);
    try {
        run();
        nvs::verifyFaults();
        CHECK(nvs::io.handles.empty() && protectedData(motion) == protectedBefore);
        CHECK(!nvs::count(Op::Erase) && !nvs::count(Op::Init));
        for (const auto& call : nvs::io.calls) {
            CHECK(call.name == "brainstate" || (motion && call.name == "productstate"));
            CHECK(call.key.empty() || call.key == "record");
        }
        std::printf("PASS %s\n", name.c_str());
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
    }
    clockHook = {}; acceptanceHook = {}; nvs::io.before = {};
}

// Fault injection replaces only transport boundaries; valid traffic crosses codecs.
// No Cloud dependency or query API: local recovery has a separate owner.
struct FakeLink {
    bool board = true, telemetry = true, preflight = true, allowSend = true;
    uint32_t receivedAt = 100;
    Status status;
    CommandSendState send = CommandSendState::Idle;
    CommandResult reply;
    unsigned attempts = 0, cancellations = 0;
    std::vector<CommandMessage> commands;
    std::vector<uint32_t> sentAt;
    std::function<void()> beforePreflight;
    std::function<void(const CommandMessage&, uint32_t)> beforeRequest;

    bool connected(uint32_t) const { return board; }
    const Status* lastTelemetry() const { return telemetry ? &status : nullptr; }
    uint32_t lastTelemetryReceivedAtMs() const { return receivedAt; }
    bool commandAvailable(uint32_t at) const {
        if (beforePreflight) beforePreflight();
        return connected(at) && preflight && send != CommandSendState::Pending;
    }
    bool requestCommand(const CommandMessage& c, uint32_t at) {
        ++attempts;
        if (beforeRequest) beforeRequest(c, at);
        if (!commandAvailable(at) || !allowSend) return false;
        v4::Message wire; CommandMessage decoded;
        CHECK(encodeCommand(c, wire) && decodeCommand(wire, decoded));
        commands.push_back(decoded); sentAt.push_back(at); send = CommandSendState::Pending;
        return true;
    }
    CommandSendState commandSendState() const { return send; }
    const CommandResult& commandResponse() const { return reply; }
    void cancelCommand() { ++cancellations; send = CommandSendState::Cancelled; }
    void complete(const CommandResult& r) {
        v4::Message wire;
        CHECK(encodeCommandResult(r, wire) && decodeCommandResult(wire, reply));
        send = CommandSendState::Complete;
    }
};
using Dispatcher = babytech::brain::BrainLocalDispatcher<FakeLink>;
static_assert(!std::is_copy_constructible<Dispatcher>::value, "one direct-send owner");
static_assert(sizeof(Dispatcher) <= sizeof(ProductRequest) + 128, "no Store/Link/context copy");

void statusFor(FakeLink& link, const ProductContext& c, bool initialize = false) {
    link.receivedAt = nowMs;
    link.status.stationary = true;
    link.status.snapshot.stage = initialize ? display::DisplayStage::NotReady : display::DisplayStage::Ready;
    link.status.snapshot.startEnabled = !initialize;
    link.status.snapshot.cloudConnected = false;
    // Display-only shortened/rounded values are not the authoritative recipe.
    link.status.snapshot.waterMl = 30; link.status.snapshot.temperatureC = 1;
    std::strcpy(link.status.snapshot.babyName.data(), "short UI name");
    std::strcpy(link.status.snapshot.formulaBrand.data(), "short UI brand");
    link.status.contextVersion = c.profileVersion;
    link.status.feedingContextConfigured = !c.cleared;
    std::strcpy(link.status.babyId, c.babyId);
}
struct Rig {
    BrainStateStore store;
    FakeLink link;
    Dispatcher d;
    explicit Rig(bool initialize = false, unsigned cache = 1)
        : d(link, store, clockNow) {
        const auto c = context(10, cache == 2);
        CHECK(store.installInitial(pairing(), cache ? &c : nullptr) == BrainWrite::Stored);
        statusFor(link, c, initialize);
        link.beforeRequest = [this](const CommandMessage& message, uint32_t) {
            CHECK(store.ready() && store.state().pending);
            CHECK(sameProductRequest(store.state().pendingRequest, message.request));
            CHECK(nvs::count(Op::Commit) >= 2);
            BrainState persisted;
            const auto& bytes = nvs::io.disk.at("brainstate").at("record").bytes;
            CHECK(decodeBrainState(bytes.data(), bytes.size(), persisted));
            CHECK(sameBrainState(persisted, store.state()));
        };
    }
    bool start(bool initialize = false, bool blocked = false) {
        return d.dispatch(initialize ? display::DisplayIntent::Initialize : display::DisplayIntent::StartFeeding,
                          nowMs, blocked);
    }
    void poll(uint32_t at) { nowMs = at; d.poll(at); }
};
void reason(const Dispatcher& d, const char* expected) { CHECK(!std::strcmp(d.reason(), expected)); }
void unchanged(Rig& rig, const std::function<void()>& action) {
    const auto before = rig.store.state(); const auto disk = nvs::io.disk;
    const auto calls = nvs::io.calls.size(); const auto sends = rig.link.attempts;
    action();
    CHECK(sameBrainState(rig.store.state(), before) && nvs::io.disk == disk);
    CHECK(nvs::io.calls.size() == calls && rig.link.attempts == sends);
}
void rebootPending(const BrainState& expected) {
    nvs::reboot(); crypto::reset();
    BrainStateStore boot;
    CHECK(boot.load(pairing()) == BrainLoad::Ready);
    CHECK(sameBrainState(boot.state(), expected) && boot.state().pending);
    FakeLink link; statusFor(link, context());
    // Even a matching stale response cannot confer ownership to a new boot.
    link.complete(result(expected.pendingRequest));
    Dispatcher fresh(link, boot, clockNow);
    const auto calls = nvs::io.calls.size(); const auto disk = nvs::io.disk;
    for (uint32_t tick = 0; tick < 32; ++tick) fresh.poll(nowMs + tick * 1000u);
    CHECK(!fresh.busy() && !fresh.canStart(nowMs) && !fresh.canInitialize(nowMs));
    CHECK(!fresh.dispatch(display::DisplayIntent::StartFeeding, nowMs));
    CHECK(!link.attempts && !link.cancellations && nvs::io.calls.size() == calls);
    CHECK(nvs::io.disk == disk && sameBrainState(boot.state(), expected));
}

void basics() {
    scenario("unconfirmed configuration prevents local Prepare without reserving evidence", [] {
        Rig rig;
        rig.d.setPrepareReadyHandler(+[] { return false; });
        unchanged(rig, [&] { CHECK(!rig.d.canStart(nowMs) && !rig.start()); });
        rig.d.setPrepareReadyHandler(+[] { return true; });
        CHECK(rig.d.canStart(nowMs) && rig.start());
    });
    scenario("local Initialize does not require configuration transfer proof", [] {
        Rig rig(true);
        rig.d.setPrepareReadyHandler(+[] { return false; });
        CHECK(rig.d.canInitialize(nowMs) && rig.start(true));
    });
    for (bool initialize : {false, true}) scenario(initialize ? "offline Initialize once" : "offline Prepare once", [=] {
        Rig rig(initialize);
        CHECK(initialize ? rig.d.canInitialize(nowMs) : rig.d.canStart(nowMs));
        CHECK(rig.start(initialize) && rig.d.busy()); reason(rig.d, "sending");
        const auto expected = request(initialize ? ProductCommand::Initialize : ProductCommand::Prepare, 1);
        CHECK(rig.link.commands.size() == 1 && sameProductRequest(rig.link.commands[0].request, expected));
        CHECK(rig.link.commands[0].remainingTtlMs == 5000 && rig.link.sentAt[0] == nowMs);
        CHECK(rig.store.state().localSequence == 1 && rig.store.state().pending);
        unchanged(rig, [&] {
            for (unsigned tick = 0; tick < 32; ++tick) {
                CHECK(!rig.start(initialize)); rig.d.poll(nowMs);
            }
        });
        CHECK(rig.link.commands.size() == 1 && rig.d.busy());
    });
    for (unsigned cache : {0u, 2u}) for (auto stage : {display::DisplayStage::NotReady, display::DisplayStage::Error})
        scenario("Initialize missing/tombstoned context and stage / " + std::to_string(cache) + "/" +
                 std::to_string(unsigned(stage)), [=] {
            Rig rig(true, cache); rig.link.status.snapshot.stage = stage;
            rig.link.status.contextVersion = 0; rig.link.status.feedingContextConfigured = false;
            rig.link.status.babyId[0] = 0;
            rig.link.status.eventPending = true;
            rig.link.status.lowWaterValid = rig.link.status.powderValid = false;
            rig.link.status.actuatorPositionReferenced = rig.link.status.executionAuthorized = false;
            // Unreferenced idle hardware can recover through Initialize; Motion
            // owns final mechanical admission, not Brain's stationary evidence.
            rig.link.status.stationary = false;
            CHECK(rig.d.canInitialize(nowMs) && rig.start(true));
            CHECK(sameProductRequest(rig.link.commands[0].request, request(ProductCommand::Initialize, 1)));
        });
    scenario("unsupported intents leave store and transport untouched", [] {
        Rig rig;
        unchanged(rig, [&] {
            CHECK(!rig.d.dispatch(display::DisplayIntent::None, nowMs));
            CHECK(!rig.d.dispatch(static_cast<display::DisplayIntent>(255), nowMs));
        });
        reason(rig.d, "invalid_parameter");
    });
}

void gates() {
    for (unsigned fault = 0; fault < 10; ++fault) for (bool initialize : {false, true}) {
        if (fault == 2 && initialize) continue;
        scenario("shared admission gate / " + std::to_string(fault) + "/" + std::to_string(initialize), [=] {
            Rig rig(initialize);
            if (fault == 0) rig.link.board = false;
            if (fault == 1) rig.link.telemetry = false;
            if (fault == 2) rig.link.status.stationary = false;
            if (fault == 3) rig.link.status.motionBusy = true;
            if (fault == 4) std::strcpy(rig.link.status.activeExecutionId, execution);
            if (fault == 5) rig.link.receivedAt = nowMs - 1500u;
            if (fault == 6) rig.link.receivedAt = nowMs - 100000u;
            if (fault == 7) rig.link.preflight = false;
            if (fault == 8) rig.link.send = CommandSendState::Pending; // Another direct owner.
            unchanged(rig, [&] {
                if (fault < 7 || fault == 9)
                    CHECK(!(initialize ? rig.d.canInitialize(nowMs, fault == 9) : rig.d.canStart(nowMs, fault == 9)));
                CHECK(!rig.start(initialize, fault == 9));
            });
            CHECK(!rig.d.busy() && !rig.store.state().pending && !rig.link.cancellations);
        });
    }
    for (unsigned cache : {0u, 2u}) scenario("Start needs active local cache / " + std::to_string(cache), [=] {
        Rig rig(false, cache);
        // A positive Motion flag does not manufacture an absent/revoked Brain cache.
        rig.link.status.feedingContextConfigured = true; rig.link.status.snapshot.startEnabled = true;
        unchanged(rig, [&] { CHECK(!rig.d.canStart(nowMs) && !rig.start()); });
    });
    for (unsigned fault = 0; fault < 4; ++fault) scenario("Start cache and Motion must match / " + std::to_string(fault), [=] {
        Rig rig;
        if (fault == 0) rig.link.status.feedingContextConfigured = false;
        if (fault == 1) ++rig.link.status.contextVersion;
        if (fault == 2) std::strcpy(rig.link.status.babyId, "another-baby");
        if (fault == 3) rig.link.status.snapshot.startEnabled = false;
        unchanged(rig, [&] { CHECK(!rig.d.canStart(nowMs) && !rig.start()); });
    });
    for (unsigned fault = 0; fault < 4; ++fault) scenario("Initialize preserves display helper gates / " + std::to_string(fault), [=] {
        Rig rig(true);
        if (fault == 0) rig.link.status.snapshot.startEnabled = true;
        if (fault == 1) rig.link.status.snapshot.primaryCondition = display::DisplayCondition::LowWater;
        if (fault == 2) rig.link.status.snapshot.stage = display::DisplayStage::Ready;
        if (fault == 3) rig.link.status.snapshot.stage = display::DisplayStage::Mixing;
        unchanged(rig, [&] { CHECK(!rig.d.canInitialize(nowMs) && !rig.start(true)); });
    });
    scenario("unloaded store is neither implicitly installed nor loaded", [] {
        BrainStateStore store; FakeLink link; statusFor(link, context());
        Dispatcher d(link, store, clockNow);
        const auto calls = nvs::io.calls.size();
        CHECK(!d.canStart(nowMs) && !d.canInitialize(nowMs));
        CHECK(!d.dispatch(display::DisplayIntent::StartFeeding, nowMs));
        CHECK(!link.attempts && nvs::io.calls.size() == calls && !store.ready());
    });
}

void identity() {
    scenario("full UTF-8 identity and frozen recipe, not shortened display data", [] {
        auto full = context();
        const std::string glyph = "\xe5\xae\x9d";
        std::string id, name, brand;
        for (unsigned i = 0; i < 32; ++i) id += glyph;
        for (unsigned i = 0; i < 100; ++i) name += glyph;
        for (unsigned i = 0; i < 160; ++i) brand += glyph;
        name += std::string(20, 'n');
        std::strcpy(full.babyId, id.c_str()); std::strcpy(full.babyName, name.c_str());
        std::strcpy(full.formulaBrand, brand.c_str()); full.profileVersion = 11;
        CHECK(validProductContext(full));
        Rig rig; CHECK(rig.store.saveContext(full) == BrainWrite::Stored); statusFor(rig.link, full);
        CHECK(rig.start());
        const auto frozen = rig.link.commands[0].request;
        CHECK(sameProductRequest(frozen, request(ProductCommand::Prepare, 1, full)));
        CHECK(std::strlen(frozen.babyId) == 96 && sameProductContext(rig.store.state().context, full));
        auto newer = context(12); newer.waterMl = 90; newer.temperatureC = 38; newer.powderGPer100Ml = 20;
        CHECK(rig.store.saveContext(newer) == BrainWrite::Stored);
        CHECK(sameProductRequest(rig.store.state().pendingRequest, frozen));
        CHECK(sameProductRequest(rig.link.commands[0].request, frozen));
        rig.link.complete(result(frozen)); rig.d.poll(nowMs);
        CHECK(!rig.store.state().pending && sameProductContext(rig.store.state().context, newer));
    });
    for (bool exhausted : {false, true}) scenario(exhausted ? "sequence max blocks without wrap" : "last positive sequence uses canonical ID", [=] {
        Rig original;
        auto state = original.store.state(); state.localSequence = v4::kMaxSequence - (exhausted ? 0 : 1);
        uint8_t bytes[kBrainStateMaxSize]; const auto length = encodeBrainState(state, bytes, sizeof(bytes));
        CHECK(length);
        // Seed a valid serialized near-limit boot, never mutate a live Store's RAM.
        nvs::io.disk["brainstate"]["record"] = {{bytes, bytes + length}, nvs::Type::Blob};
        nvs::reboot(); BrainStateStore loaded; CHECK(loaded.load(pairing()) == BrainLoad::Ready);
        FakeLink link; statusFor(link, context()); Dispatcher d(link, loaded, clockNow);
        const auto calls = nvs::io.calls.size();
        CHECK(d.canStart(nowMs) != exhausted);
        CHECK(d.dispatch(display::DisplayIntent::StartFeeding, nowMs) != exhausted);
        if (exhausted) {
            CHECK(!d.canInitialize(nowMs) && !link.attempts && nvs::io.calls.size() == calls);
        } else {
            CHECK(link.commands.size() == 1);
            CHECK(sameProductRequest(link.commands[0].request, request(ProductCommand::Prepare, v4::kMaxSequence)));
            link.complete(result(link.commands[0].request)); d.poll(nowMs);
            CHECK(!loaded.state().pending && loaded.state().localSequence == v4::kMaxSequence);
            CHECK(!d.canStart(nowMs) && !d.canInitialize(nowMs));
        }
    });
}

void timing() {
    for (bool wrap : {false, true}) for (uint32_t age : {1499u, 1500u})
        scenario("receipt freshness boundary / " + std::to_string(wrap) + "/" + std::to_string(age), [=] {
            Rig rig; nowMs = wrap ? 5u : 10000u; rig.link.receivedAt = nowMs - age;
            // Peer sample clock and Cloud connectivity are not receipt freshness.
            rig.link.status.sampleUptimeMs = UINT32_MAX;
            CHECK(rig.d.canStart(nowMs) == (age < 1500));
            if (age < 1500) CHECK(rig.start());
            else unchanged(rig, [&] { CHECK(!rig.start()); });
        });
    for (bool wrap : {false, true}) for (uint32_t elapsed : {0u, 120u, 4949u, 4950u, 5000u, 5001u})
        scenario("Flash consumes original TTL / " + std::to_string(wrap) + "/" + std::to_string(elapsed), [=] {
            Rig rig; nowMs = wrap ? UINT32_MAX - 10u : 100u; rig.link.receivedAt = nowMs;
            const uint32_t started = nowMs; const unsigned commit = nvs::count(Op::Commit);
            nvs::io.before = [&](const nvs::Call& call) {
                if (call.op == Op::Commit && call.occurrence == commit + 1) {
                    nowMs = started + elapsed;
                    rig.link.receivedAt = nowMs;
                }
            };
            CHECK(rig.start() == (elapsed < 4950)); nvs::io.before = {};
            CHECK(rig.store.ready() && rig.store.state().pending && rig.store.state().localSequence == 1);
            const auto pending = rig.store.state();
            if (elapsed < 4950) {
                CHECK(rig.link.commands.size() == 1 && rig.link.sentAt[0] == uint32_t(started + elapsed));
                CHECK(rig.link.commands[0].remainingTtlMs == 5000 - elapsed);
            } else {
                CHECK(!rig.link.attempts && !rig.d.busy()); reason(rig.d, "result_unknown");
                rebootPending(pending);
            }
        });
    scenario("SHA elapsed is included, with no extra TTL after reservation", [] {
        Rig rig; const auto before = crypto::calls; const uint32_t started = nowMs;
        clockHook = [&] { if (crypto::calls > before) nowMs = started + 75; };
        CHECK(rig.start()); clockHook = {};
        CHECK(rig.link.sentAt[0] == started + 75 && rig.link.commands[0].remainingTtlMs == 4925);
    });
    scenario("TTL originates at click, including dry preflight time", [] {
        Rig rig; const uint32_t clicked = nowMs;
        rig.link.beforePreflight = [&] { nowMs = clicked + 123; };
        CHECK(rig.start()); rig.link.beforePreflight = {};
        CHECK(rig.link.sentAt[0] == clicked + 123 && rig.link.commands[0].remainingTtlMs == 4877);
    });
    scenario("link disappears during reserve: no send, durable query-only evidence", [] {
        Rig rig;
        nvs::io.before = [&](const nvs::Call& call) { if (call.op == Op::Commit) rig.link.board = false; };
        CHECK(!rig.start()); nvs::io.before = {};
        CHECK(rig.link.attempts == 1 && rig.link.commands.empty() && !rig.d.busy());
        reason(rig.d, "result_unknown");
        rebootPending(rig.store.state());
    });
}

void storage() {
    const struct { Op op; unsigned occurrence; } faults[] = {
        {Op::OpenRO, 1}, {Op::Query, 1}, {Op::Read, 1}, {Op::OpenRW, 1},
        {Op::Query, 2}, {Op::Read, 2}, {Op::Set, 1}, {Op::Commit, 1},
        {Op::OpenRO, 2}, {Op::Query, 3}, {Op::Read, 3}};
    for (bool early : {false, true}) for (const auto& fault : faults) for (bool apply : {false, true}) {
        if (apply && fault.op != Op::Set && fault.op != Op::Commit) continue;
        scenario("reserve fault / " + std::to_string(unsigned(fault.op)) + "/" +
                 std::to_string(fault.occurrence) + "/" + std::to_string(early) + "/" + std::to_string(apply), [=] {
            Rig rig; nvs::io.durableOnSet = early; const auto before = rig.store.state();
            nvs::fail(fault.op, nvs::count(fault.op) + fault.occurrence, ESP_FAIL, apply);
            CHECK(!rig.start()); nvs::verifyFaults();
            CHECK(!rig.link.attempts && !rig.d.busy() && rig.store.faulted()); reason(rig.d, "storage_fault");
            CHECK(sameBrainState(rig.store.state(), before));
            unchanged(rig, [&] { for (unsigned tick = 0; tick < 32; ++tick) { rig.d.poll(nowMs); CHECK(!rig.start()); } });
            nvs::reboot(); BrainStateStore fresh; CHECK(fresh.load(pairing()) == BrainLoad::Ready);
            const bool readback = (fault.op == Op::OpenRO && fault.occurrence == 2) ||
                ((fault.op == Op::Query || fault.op == Op::Read) && fault.occurrence == 3);
            const bool durable = readback || apply || (early && fault.op == Op::Commit);
            CHECK(fresh.state().pending == durable && fresh.state().localSequence == (durable ? 1u : 0u));
            if (durable) rebootPending(fresh.state());
        });
    }
    scenario("reserve SHA failure: storage fault without sends", [] {
        Rig rig; crypto::fail = true;
        CHECK(!rig.start() && !rig.link.attempts && !rig.d.busy()); reason(rig.d, "storage_fault");
        CHECK(rig.store.faulted()); crypto::reset();
    });
    for (auto op : {Op::Set, Op::Commit, Op::Read}) scenario("definitive result clear fails closed / " + std::to_string(unsigned(op)), [=] {
        Rig rig; CHECK(rig.start()); const auto pending = rig.store.state();
        rig.link.complete(result(pending.pendingRequest));
        // Read occurrence 3 is post-write readback; all failures preserve last verified RAM.
        nvs::fail(op, nvs::count(op) + (op == Op::Read ? 3 : 1));
        rig.d.poll(nowMs); nvs::verifyFaults();
        CHECK(!rig.d.busy() && rig.store.faulted() && sameBrainState(rig.store.state(), pending));
        reason(rig.d, "storage_fault");
        unchanged(rig, [&] { for (unsigned tick = 0; tick < 32; ++tick) rig.d.poll(nowMs + tick); });
        CHECK(rig.link.commands.size() == 1 && rig.link.cancellations == 1);
    });
}

void acceptanceCallbacks() {
    for (bool initialize : {false, true}) for (bool accepted : {false, true})
        scenario(std::string("local acceptance callback before pending clear / ") +
                 (initialize ? "initialize" : "prepare") + (accepted ? " accepted" : " rejected"), [=] {
            Rig rig(initialize); CHECK(rig.start(initialize));
            const auto original = rig.store.state().pendingRequest;
            CHECK(rig.store.saveContext(context(11, true)) == BrainWrite::Stored);
            const auto disk = nvs::io.disk; const auto writes = nvs::count(Op::Set);
            unsigned calls = 0;
            rig.d.setAcceptanceHandler(observeAcceptance);
            acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
                ++calls; CHECK(sameProductRequest(request, original) && at == 123);
                CHECK(rig.store.state().pending && sameProductRequest(rig.store.state().pendingRequest, original));
                CHECK(nvs::io.disk == disk && nvs::count(Op::Set) == writes && !rig.link.cancellations);
            };
            rig.link.complete(result(original, accepted, accepted ? "accepted" : "not_ready"));
            rig.poll(123);
            CHECK(calls == unsigned(accepted) && !rig.store.state().pending && !rig.d.busy());
            rig.poll(124); rig.poll(1000); CHECK(calls == unsigned(accepted));
        });
    for (unsigned fault = 0; fault < 5; ++fault)
        scenario("local acceptance callback excludes wrong/late owner / " + std::to_string(fault), [=] {
            Rig rig; CHECK(rig.start()); const auto original = rig.store.state().pendingRequest;
            unsigned calls = 0;
            rig.d.setAcceptanceHandler(observeAcceptance);
            acceptanceHook = [&](const ProductRequest&, uint32_t) { ++calls; };
            auto reply = result(original);
            if (fault == 0) reply.source = v4::Source::CloudCommand;
            if (fault == 1) ++reply.sequence;
            if (fault == 2) std::strcpy(reply.commandId, "other-command");
            if (fault == 3) {
                CHECK(rig.store.clearPending(original) == BrainWrite::Stored);
                CHECK(rig.store.reserveLocal(request(ProductCommand::Initialize, 2)) == BrainWrite::Stored);
                reply = result(rig.store.state().pendingRequest);
            }
            if (fault == 4) { rig.link.send = CommandSendState::TimedOut; rig.poll(122); }
            const auto before = rig.store.state(); const auto disk = nvs::io.disk;
            rig.link.complete(reply); rig.poll(123); rig.poll(124);
            CHECK(!calls && sameBrainState(rig.store.state(), before) && nvs::io.disk == disk);
        });
    scenario("local acceptance callback survives pending-clear storage failure", [] {
        Rig rig; CHECK(rig.start()); const auto original = rig.store.state().pendingRequest;
        unsigned calls = 0;
        rig.d.setAcceptanceHandler(observeAcceptance);
        acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
            ++calls; CHECK(sameProductRequest(request, original) && at == 123);
            CHECK(rig.store.state().pending && !rig.link.cancellations);
        };
        nvs::fail(Op::Set, nvs::count(Op::Set) + 1);
        rig.link.complete(result(original)); rig.poll(123); rig.poll(124);
        CHECK(calls == 1 && rig.store.state().pending); reason(rig.d, "storage_fault");
    });
}

void responses() {
    for (bool accepted : {false, true}) scenario(accepted ? "exact acceptance clears pending" : "exact rejection clears pending", [=] {
        Rig rig; CHECK(rig.start()); const auto frozen = rig.store.state().pendingRequest;
        const char* text = accepted ? "already_clear" : "motion_not_ready";
        rig.link.complete(result(frozen, accepted, text)); rig.d.poll(nowMs);
        CHECK(!rig.d.busy() && !rig.store.state().pending && rig.store.state().localSequence == 1);
        reason(rig.d, text);
        std::strcpy(rig.link.reply.reason, "overwritten_shared_link_buffer"); reason(rig.d, text);
        unchanged(rig, [&] { for (unsigned tick = 0; tick < 32; ++tick) rig.d.poll(nowMs + tick); });
        CHECK(rig.link.cancellations == 1 && rig.link.commands.size() == 1);
        nvs::reboot(); BrainStateStore fresh; CHECK(fresh.load(pairing()) == BrainLoad::Ready);
        CHECK(!fresh.state().pending && fresh.state().localSequence == 1);
    });
    for (unsigned wrong = 0; wrong < 3; ++wrong) scenario("wrong response identity retains pending / " + std::to_string(wrong), [=] {
        Rig rig; CHECK(rig.start()); const auto pending = rig.store.state();
        auto reply = result(pending.pendingRequest);
        if (wrong == 0) reply.source = v4::Source::CloudCommand;
        if (wrong == 1) ++reply.sequence;
        if (wrong == 2) std::strcpy(reply.commandId, "another-command");
        rig.link.complete(reply);
        unchanged(rig, [&] { rig.d.poll(nowMs); });
        CHECK(!rig.d.busy() && rig.link.cancellations == 1); reason(rig.d, "result_unknown");
        rebootPending(pending);
    });
    for (auto terminal : {CommandSendState::Idle, CommandSendState::TimedOut,
                          CommandSendState::Unavailable, CommandSendState::Cancelled})
        scenario("timeout/unavailable/Stop cancellation never clears or retries / " + std::to_string(unsigned(terminal)), [=] {
            Rig rig; CHECK(rig.start()); const auto pending = rig.store.state();
            rig.link.send = terminal;
            unchanged(rig, [&] { rig.d.poll(nowMs + 5001); });
            CHECK(!rig.d.busy() && rig.link.cancellations == 1); reason(rig.d, "result_unknown");
            rig.link.complete(result(pending.pendingRequest)); // Late completion after loss of ownership.
            unchanged(rig, [&] { for (unsigned tick = 0; tick < 32; ++tick) rig.d.poll(nowMs + tick); });
            rebootPending(pending);
        });
    scenario("send failure after preflight retains reserved pending without retry", [] {
        Rig rig; rig.link.allowSend = false;
        CHECK(!rig.start() && !rig.d.busy() && rig.link.attempts == 1 && rig.store.state().pending);
        reason(rig.d, "result_unknown"); const auto pending = rig.store.state();
        unchanged(rig, [&] { for (unsigned tick = 0; tick < 32; ++tick) { rig.d.poll(nowMs); CHECK(!rig.start()); } });
        CHECK(!rig.link.cancellations); rebootPending(pending);
    });
    scenario("new same-boot dispatcher cannot steal direct-send ownership", [] {
        Rig rig; CHECK(rig.start()); Dispatcher competitor(rig.link, rig.store, clockNow);
        rig.link.complete(result(rig.store.state().pendingRequest));
        unchanged(rig, [&] {
            competitor.poll(nowMs); CHECK(!competitor.busy());
            CHECK(!competitor.dispatch(display::DisplayIntent::StartFeeding, nowMs));
        });
        CHECK(rig.d.busy() && rig.store.state().pending && !rig.link.cancellations);
        rig.d.poll(nowMs); CHECK(!rig.store.state().pending);
    });
    for (bool newReply : {false, true}) scenario("replaced pending is not old direct owner's result / " + std::to_string(newReply), [=] {
        Rig rig; CHECK(rig.start()); const auto original = rig.store.state().pendingRequest;
        CHECK(rig.store.clearPending(original) == BrainWrite::Stored);
        CHECK(rig.store.reserveLocal(request(ProductCommand::Initialize, 2)) == BrainWrite::Stored);
        const auto replacement = rig.store.state();
        rig.link.complete(result(newReply ? replacement.pendingRequest : original));
        unchanged(rig, [&] { rig.d.poll(nowMs); });
        CHECK(!rig.d.busy() && rig.store.state().pending); reason(rig.d, "result_unknown");
    });
}

// Real protocol/store integration, but deliberately not the actual MotionRuntime.
// The handler persists one scripted accept/reject decision, with no actuator API.
MotionStateStore* motionStore = nullptr;
bool motionAccepts = true;
unsigned commandCalls = 0, queryCalls = 0, stopCalls = 0;
CommandMessage receivedCommand;
bool commandHandler(const CommandMessage& incoming, uint32_t, CommandResult& reply) {
    ++commandCalls; CHECK(motionStore); receivedCommand = incoming;
    CHECK(motionStore->recordDecision(incoming.request, motionAccepts,
        motionAccepts ? "accepted" : "not_ready", motionAccepts ? execution : nullptr) == MotionWrite::Stored);
    reply = result(incoming.request, motionAccepts, motionAccepts ? "accepted" : "not_ready");
    return true;
}
bool queryHandler(const ResultQuery& query, QueriedResult& reply) {
    ++queryCalls; CHECK(motionStore); return queryMotionResult(*motionStore, query, reply);
}
bool stopHandler(const v4::StopRequest&, uint32_t) { ++stopCalls; return true; }
struct TelemetryLink : ReadOnlyLink {
    const Status* lastTelemetry() const { return freshStatus(nowMs) ? &peerStatus() : nullptr; }
    uint32_t lastTelemetryReceivedAtMs() const { return peerStatusReceivedAtMs(); }
};
struct ShortWire : v4::ByteSink {
    std::vector<uint8_t> bytes;
    v4::Parser observer;
    unsigned writes = 0, shorts = 0, zeros = 0, commands = 0, results = 0, queries = 0, stops = 0;
    bool idle() const override { return bytes.empty(); }
    size_t available() const override { return 128 - bytes.size(); }
    size_t write(const uint8_t* data, size_t length) override {
        CHECK(length <= available());
        const size_t take = (++writes % 5) ? std::min(length, size_t(17)) : 0;
        if (take < length) ++shorts;
        if (!take) ++zeros;
        bytes.insert(bytes.end(), data, data + take); return take;
    }
    void deliver(ReadOnlyLink& peer, bool drop = false) {
        for (const auto byte : bytes) {
            v4::Frame frame;
            if (observer.push(byte, nowMs, frame) && !frame.offset) {
                if (frame.kind == v4::Kind::Command) ++commands;
                if (frame.kind == v4::Kind::CommandResult || frame.kind == v4::Kind::Result) ++results;
                if (frame.kind == v4::Kind::ResultQuery) ++queries;
                if (frame.kind == v4::Kind::Stop) ++stops;
            }
            if (!drop) peer.receive(byte, nowMs);
        }
        bytes.clear();
    }
};
void production() {
    for (unsigned mode = 0; mode < 5; ++mode) scenario("real Brain/Motion links and stores / " + std::to_string(mode), [=] {
        commandCalls = queryCalls = stopCalls = 0; motionAccepts = mode != 1;
        const bool initialize = mode == 2;
        const auto c = context();
        BrainStateStore brainStore; MotionStateStore store;
        CHECK(brainStore.installInitial(pairing(), initialize ? nullptr : &c) == BrainWrite::Stored);
        CHECK(store.installInitial(pairing(v4::Role::Motion), initialize ? nullptr : &c) == MotionWrite::Stored);
        motionStore = &store;
        TelemetryLink brain; ReadOnlyLink motion; ShortWire toMotion, toBrain;
        CHECK(brain.begin(pairing(), 101) && motion.begin(pairing(v4::Role::Motion), 202));
        CHECK(motion.setCommandHandler(commandHandler) && motion.setResultQueryHandler(queryHandler));
        CHECK(motion.setStopHandler(stopHandler));
        FakeLink source; statusFor(source, c, initialize); Status status = source.status;
        if (initialize) {
            status.contextVersion = 0; status.babyId[0] = 0; status.feedingContextConfigured = false;
            status.stationary = false;
        }
        auto step = [&](bool drop = false) {
            status.sampleUptimeMs = nowMs;
            brain.poll(nowMs, toMotion); motion.poll(nowMs, toBrain, &status);
            toMotion.deliver(motion); toBrain.deliver(brain, drop); ++nowMs;
        };
        for (unsigned tick = 0; tick < 400; ++tick) step();
        CHECK(brain.connected(nowMs) && motion.connected(nowMs) && brain.lastTelemetry());
        CHECK(brain.lastTelemetry()->snapshot.startEnabled == !initialize);
        CHECK(brain.lastTelemetry()->snapshot.stage == status.snapshot.stage);
        babytech::brain::BrainLocalDispatcher<TelemetryLink> d(brain, brainStore, clockNow);
        const auto intent = initialize ? display::DisplayIntent::Initialize : display::DisplayIntent::StartFeeding;
        // Let the current ordinary heartbeat frame drain before the explicit click.
        for (unsigned tick = 0; tick < 1000 && !brain.commandAvailable(nowMs); ++tick) step();
        CHECK(brain.commandAvailable(nowMs));
        if (mode == 0) {
            ResultQuery competing;
            const auto other = request(ProductCommand::Initialize, 99);
            competing.source = other.source; competing.sequence = other.sequence;
            std::strcpy(competing.deviceId, other.deviceId); std::strcpy(competing.commandId, other.commandId);
            CHECK(brain.requestResult(competing, nowMs));
            const auto calls = nvs::io.calls.size(); const auto disk = nvs::io.disk;
            CHECK(!d.dispatch(intent, nowMs) && !d.busy() && !brainStore.state().pending);
            CHECK(brain.resultLookupState() == ResultLookupState::Pending);
            CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
            brain.cancelResultQuery();
            for (unsigned tick = 0; tick < 1000 && !brain.commandAvailable(nowMs); ++tick) step();
            CHECK(brain.commandAvailable(nowMs));
        }
        CHECK(d.dispatch(intent, nowMs)); CHECK(d.busy() && brainStore.state().pending);
        const auto reserved = brainStore.state();
        if (mode == 3) {
            // Real binary Stop cancels the ordinary sender before any command frame.
            v4::StopRequest stop; stop.scope = v4::StopScope::Idle;
            CHECK(brain.requestStop(stop, nowMs)); d.poll(nowMs);
            CHECK(!d.busy() && brainStore.state().pending);
            for (unsigned tick = 0; tick < 400; ++tick) { step(); d.poll(nowMs); }
            CHECK(!commandCalls && !toMotion.commands && stopCalls == 1 && toMotion.stops == 1);
            rebootPending(reserved);
        } else if (mode == 4) {
            babytech::brain::BrainPendingRecovery<TelemetryLink> pausedRecovery(brain, brainStore);
            for (unsigned tick = 0; tick < 1500 && d.busy(); ++tick) {
                pausedRecovery.poll(nowMs, d.busy());
                CHECK(pausedRecovery.state() == babytech::brain::BrainPendingRecoveryState::Paused);
                step(true); d.poll(nowMs);
            }
            CHECK(!d.busy() && brainStore.state().pending && commandCalls == 1 && !queryCalls);
            CHECK(toMotion.commands == 1 && toBrain.results == 1);
            nvs::reboot(); BrainStateStore loadedBrain; MotionStateStore loadedMotion;
            CHECK(loadedBrain.load(pairing()) == BrainLoad::Ready && loadedBrain.state().pending);
            CHECK(loadedMotion.load(pairing(v4::Role::Motion)) == MotionLoad::Ready);
            CHECK(sameBrainState(loadedBrain.state(), reserved)); motionStore = &loadedMotion;
            TelemetryLink newBrain; ShortWire newToMotion, newToBrain;
            CHECK(newBrain.begin(pairing(), 303) && motion.begin(pairing(v4::Role::Motion), 404));
            CHECK(motion.setResultQueryHandler(queryHandler) && motion.setCommandHandler(commandHandler));
            auto recoveredStep = [&] {
                status.sampleUptimeMs = nowMs;
                newBrain.poll(nowMs, newToMotion); motion.poll(nowMs, newToBrain, &status);
                newToMotion.deliver(motion); newToBrain.deliver(newBrain); ++nowMs;
            };
            for (unsigned tick = 0; tick < 400; ++tick) recoveredStep();
            CHECK(newBrain.connected(nowMs) && newBrain.lastTelemetry());
            babytech::brain::BrainLocalDispatcher<TelemetryLink> newOwner(newBrain, loadedBrain, clockNow);
            newOwner.poll(nowMs);
            CHECK(!newOwner.busy() && !newOwner.dispatch(intent, nowMs) && loadedBrain.state().pending);
            const auto motionBefore = loadedMotion.state();
            const auto motionDisk = nvs::io.disk.at("productstate");
            babytech::brain::BrainPendingRecovery<TelemetryLink> recovery(newBrain, loadedBrain);
            for (unsigned tick = 0; tick < 2000 && loadedBrain.state().pending; ++tick) {
                recovery.poll(nowMs); recoveredStep(); newOwner.poll(nowMs);
            }
            CHECK(recovery.state() == babytech::brain::BrainPendingRecoveryState::Resolved);
            CHECK(!loadedBrain.state().pending && loadedBrain.state().localSequence == 1);
            CHECK(commandCalls == 1 && queryCalls == 1 && !newToMotion.commands && newToMotion.queries == 1);
            CHECK(sameMotionState(loadedMotion.state(), motionBefore));
            CHECK(nvs::io.disk.at("productstate") == motionDisk && !newOwner.busy());
            std::printf("  reboot: zero replay frames, one read-only query resolved by existing recovery owner\n");
        } else {
            for (unsigned tick = 0; tick < 1500 && d.busy(); ++tick) { step(); d.poll(nowMs); }
            CHECK(!d.busy() && !brainStore.state().pending && commandCalls == 1 && toMotion.commands == 1);
            CHECK(sameProductRequest(receivedCommand.request, reserved.pendingRequest));
            CHECK(!std::strcmp(d.reason(), motionAccepts ? "accepted" : "not_ready"));
            CHECK(store.state().localSequence == 1 && !queryCalls);
            CHECK(store.state().localResult.kind == MotionResultKind::Ordinary);
            CHECK(store.state().localResult.accepted == motionAccepts);
            CHECK(sameProductRequest(store.state().localResult.request, reserved.pendingRequest));
            CHECK(store.state().slot.kind == (motionAccepts ? MotionSlotKind::Intent : MotionSlotKind::Empty));
            const auto motionBefore = store.state();
            for (unsigned tick = 0; tick < 200; ++tick) { step(); d.poll(nowMs); }
            CHECK(commandCalls == 1 && toBrain.results == 1);
            CHECK(sameMotionState(store.state(), motionBefore));
            nvs::reboot(); BrainStateStore loadedBrain; MotionStateStore loadedMotion;
            CHECK(loadedBrain.load(pairing()) == BrainLoad::Ready && !loadedBrain.state().pending);
            CHECK(loadedMotion.load(pairing(v4::Role::Motion)) == MotionLoad::Ready);
            CHECK(loadedBrain.state().localSequence == 1 && loadedMotion.state().localSequence == 1);
        }
        CHECK(toMotion.shorts && toBrain.shorts && toMotion.zeros && toBrain.zeros);
        std::printf("  real link frames: commands=%u results=%u queries=%u stops=%u (test handler, no MotionRuntime)\n",
                    toMotion.commands, toBrain.results, toMotion.queries, toMotion.stops);
        motionStore = nullptr;
    }, true);
}
}  // namespace

int main(int argc, char** argv) {
    const std::string selected = argc == 2 ? argv[1] : "all";
    const struct { const char* name; void (*run)(); } groups[] = {
        {"basics", basics}, {"gates", gates}, {"identity", identity}, {"timing", timing},
        {"storage", storage}, {"responses", responses}, {"production", production},
        {"acceptance", acceptanceCallbacks}};
    bool found = selected == "all";
    for (const auto& group : groups) if (selected == "all" || selected == group.name) {
        found = true; group.run();
    }
    if (!found || argc > 2) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("Brain local dispatcher: %u scenarios, %u failures; SDK SHA major %d; owner %zu bytes\n",
                scenarios, failures, MBEDTLS_VERSION_MAJOR, sizeof(Dispatcher));
    return failures ? 1 : 0;
}
