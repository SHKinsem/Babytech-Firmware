#include "brain_context_sync.h"
#include "MotionProductRuntime.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace nvs = fake_brain;
using nvs::Op;
namespace {
unsigned scenarios = 0, failures = 0, integrationScenarios = 0;
unsigned wireContexts = 0, wireResults = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + \
    std::to_string(__LINE__) + ": " #x); } while (false)

v4::Pairing pairing(v4::Role role = v4::Role::Brain) {
    v4::Pairing p;
    p.role = role;
    std::strcpy(p.deviceId, "Babytech_context-test");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, role == v4::Role::Brain ? "112233445566" : "aabbccddeeff");
    std::strcpy(p.peerPhysicalId, role == v4::Role::Brain ? "aabbccddeeff" : "112233445566");
    return p;
}
ProductContext context(uint32_t version = 10, bool cleared = false, bool full = false) {
    ProductContext c;
    std::strcpy(c.deviceId, pairing().deviceId);
    c.profileVersion = version; c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, "baby-original");
        std::strcpy(c.babyName, "Full baby name beyond display capacity");
        std::strcpy(c.formulaBrand, "Full formula brand beyond display capacity");
        if (full) {
            const std::string glyph = "\xe5\xae\x9d";
            std::string id, name, brand;
            for (unsigned i = 0; i < 32; ++i) id += glyph;
            for (unsigned i = 0; i < 100; ++i) name += glyph;
            for (unsigned i = 0; i < 160; ++i) brand += glyph;
            name += std::string(20, 'n');
            std::strcpy(c.babyId, id.c_str()); std::strcpy(c.babyName, name.c_str());
            std::strcpy(c.formulaBrand, brand.c_str());
        }
        c.waterMl = 180; c.temperatureC = 45; c.powderGPer100Ml = 13.123456f;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(const ProductContext& c, uint64_t sequence = 1,
                       ProductCommand command = ProductCommand::Prepare) {
    ProductRequest r;
    r.command = command; r.sequence = sequence;
    std::strcpy(r.deviceId, c.deviceId);
    CHECK(makeLocalCommandId(pairing(), sequence, r.commandId));
    if (command == ProductCommand::Prepare) {
        r.profileVersion = c.profileVersion;
        std::strcpy(r.babyId, c.babyId);
        r.waterMl = c.waterMl; r.temperatureC = c.temperatureC; r.powderGPer100Ml = c.powderGPer100Ml;
    }
    CHECK(validProductRequest(r));
    return r;
}
BrainState diskBrain() {
    BrainState saved;
    const auto& bytes = nvs::io.disk.at("brainstate").at("record").bytes;
    CHECK(decodeBrainState(bytes.data(), bytes.size(), saved));
    return saved;
}
nvs::Database protectedData() {
    auto disk = nvs::io.disk;
    disk["brainstate"].erase("record"); disk["productstate"].erase("record");
    return disk;
}
void scenario(const std::string& name, const std::function<void()>& run, bool integration = false) {
    ++scenarios;
    if (integration) ++integrationScenarios;
    nvs::reset(); fake_product_crypto::reset();
    for (const char* space : {"brainstate", "productstate", "productpair", "productctx",
                              "outbox", "wifi-cfg", "actuatorcfg", "sensorcfg"})
        nvs::io.disk[space]["unrelated"] = {{0, 0xff, 0x7f}, nvs::Type::Blob};
    const auto protectedBefore = protectedData();
    try {
        run(); nvs::verifyFaults();
        CHECK(nvs::io.handles.empty() && protectedData() == protectedBefore);
        CHECK(!nvs::count(Op::Erase) && !nvs::count(Op::Init));
        for (const auto& call : nvs::io.calls) {
            CHECK(call.name == "brainstate" || (integration && call.name == "productstate"));
            CHECK(call.key.empty() || call.key == "record");
        }
        std::printf("PASS %s\n", name.c_str());
    } catch (const std::exception& error) {
        ++failures; std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
    }
    nvs::io.before = {};
}

struct FakeLink {
    bool board = true, available = true, accept = true;
    ContextSendState state = ContextSendState::Idle;
    StopSendState stop = StopSendState::Idle;
    ContextResult reply;
    unsigned attempts = 0, cancellations = 0;
    std::vector<ProductContext> contexts;
    std::function<void(const ProductContext&)> beforeSend;
    bool connected(uint32_t) const { return board; }
    StopSendState stopSendState() const { return stop; }
    bool commandAvailable(uint32_t) const { return board && available && state != ContextSendState::Pending; }
    bool requestContext(const ProductContext& c, uint32_t now) {
        ++attempts;
        if (beforeSend) beforeSend(c);
        if (!commandAvailable(now) || !accept) return false;
        v4::Message wire; ProductContext decoded;
        CHECK(encodeContextMessage(c, wire) && decodeContextMessage(wire, c.deviceId, decoded));
        contexts.push_back(decoded);
        reply = ContextResult{};
        reply.replyTo = attempts; reply.profileVersion = c.profileVersion; reply.cleared = c.cleared;
        std::strcpy(reply.deviceId, c.deviceId);
        CHECK(contextDigest(c, reply.digest));
        state = ContextSendState::Pending;
        return true;
    }
    ContextSendState contextSendState() const { return state; }
    const ContextResult& contextResponse() const { return reply; }
    void cancelContext() { ++cancellations; state = ContextSendState::Cancelled; }
    void complete(ContextStatus status = ContextStatus::Stored) {
        reply.status = status;
        v4::Message wire;
        CHECK(encodeContextResult(reply, wire) && decodeContextResult(wire, reply));
        state = ContextSendState::Complete;
    }
};
using Sync = babytech::brain::BrainContextSync<FakeLink>;
static_assert(!std::is_copy_constructible<Sync>::value && !std::is_copy_assignable<Sync>::value,
              "single owner cannot be copied");
struct Rig {
    BrainStateStore store;
    FakeLink link;
    Sync sync{link, store};
    explicit Rig(unsigned cache = 1, bool pending = false) {
        const auto c = context(10, cache == 2);
        CHECK(store.installInitial(pairing(), cache ? &c : nullptr) == BrainWrite::Stored);
        if (pending) CHECK(store.reserveLocal(request(c)) == BrainWrite::Stored);
        link.beforeSend = [this](const ProductContext& sent) {
            CHECK(store.ready() && sameProductContext(sent, store.state().context));
            CHECK(sameBrainState(diskBrain(), store.state()));
        };
    }
    void confirm(uint32_t now = 100) {
        sync.poll(now); CHECK(link.state == ContextSendState::Pending && !sync.canPrepare());
        link.complete(ContextStatus::Unchanged); sync.poll(now + 1);
        CHECK(sync.canPrepare() == !store.state().context.cleared);
    }
};
void pendingUnchanged(const BrainState& old, const BrainState& saved) {
    CHECK(old.localSequence == saved.localSequence && old.pending == saved.pending);
    CHECK(sameProductRequest(old.pendingRequest, saved.pendingRequest));
    CHECK(!std::memcmp(old.pendingDigest, saved.pendingDigest, sizeof(old.pendingDigest)));
}

void cacheTests() {
    scenario("Brain simulation cache-only saves context without Motion or pending mutation", [] {
        Rig rig(1, true);
        rig.link.board = false;
        const auto before = rig.store.state();
        CHECK(rig.sync.hasUsableCache() && !rig.sync.canPrepare());
        const auto newer = context(11, false, true);
        CHECK(rig.sync.receive(newer) && !rig.sync.hasUsableCache());
        rig.sync.poll(100, false, false, false);
        CHECK(rig.sync.hasUsableCache() && !rig.sync.canPrepare());
        CHECK(sameProductContext(diskBrain().context, newer));
        pendingUnchanged(before, diskBrain());
        CHECK(rig.link.attempts == 0 && rig.link.contexts.empty());
        auto conflict = newer;
        ++conflict.waterMl;
        CHECK(!rig.sync.receive(conflict) && !rig.sync.hasUsableCache());
        CHECK(rig.sync.receive(context(12, true)));
        rig.sync.poll(200, false, false, false);
        CHECK(!rig.sync.hasUsableCache() && rig.store.state().context.cleared);
        CHECK(rig.sync.receive(context(13)));
        rig.sync.poll(300, false, false, false);
        CHECK(rig.sync.hasUsableCache() && rig.link.attempts == 0);
        rig.link.board = true;
        rig.sync.poll(301);
        CHECK(rig.link.attempts == 1 && rig.link.contexts.back().profileVersion == 13);
        CHECK(!rig.sync.canPrepare());
        rig.link.complete();
        rig.sync.poll(302);
        CHECK(rig.sync.canPrepare());
        pendingUnchanged(before, diskBrain());
    });
    for (unsigned cache : {0u, 1u, 2u}) scenario("cold boot persisted cache / " + std::to_string(cache), [=] {
        Rig seed(cache); const auto expected = seed.store.state();
        nvs::reboot(); BrainStateStore store;
        CHECK(store.load(pairing()) == BrainLoad::Ready);
        FakeLink link; Sync sync(link, store);
        const auto sets = nvs::count(Op::Set), commits = nvs::count(Op::Commit);
        CHECK(!sync.canPrepare()); sync.poll(100);
        CHECK(link.contexts.size() == (cache ? 1u : 0u));
        if (cache) { link.complete(ContextStatus::Unchanged); sync.poll(101); }
        CHECK(sync.canPrepare() == (cache == 1));
        CHECK(sameBrainState(store.state(), expected));
        CHECK(nvs::count(Op::Set) == sets && nvs::count(Op::Commit) == commits);
    });
    scenario("unloaded store is never implicitly installed", [] {
        BrainStateStore store; FakeLink link; Sync sync(link, store);
        CHECK(!sync.receive(context()) && !sync.canPrepare()); sync.poll(100);
        CHECK(nvs::io.calls.empty() && !link.attempts && !store.ready());
    });
    for (bool cleared : {false, true}) scenario("callback copies newest; poll persists; pending frozen / " + std::to_string(cleared), [=] {
        Rig rig(1, true); rig.confirm();
        const auto old = rig.store.state(), before = diskBrain();
        const auto calls = nvs::io.calls.size(), sends = rig.link.contexts.size();
        auto newest = context(12, cleared, !cleared);
        CHECK(rig.sync.receive(context(11)) && !rig.sync.canPrepare());
        CHECK(rig.sync.receive(newest) && !rig.sync.canPrepare());
        newest.profileVersion = 99; // Caller memory is not the pending cache.
        CHECK(nvs::io.calls.size() == calls && sameBrainState(before, diskBrain()));
        CHECK(rig.link.contexts.size() == sends);
        rig.sync.poll(200, false, true);
        CHECK(sameProductContext(rig.store.state().context, context(12, cleared, !cleared)));
        pendingUnchanged(old, rig.store.state()); pendingUnchanged(old, diskBrain());
        CHECK(rig.link.contexts.size() == sends && !rig.sync.canPrepare());
        rig.sync.poll(201); CHECK(rig.link.contexts.size() == sends + 1);
        rig.link.complete(); rig.sync.poll(202);
        CHECK(rig.sync.canPrepare() == !cleared);
    });
    for (bool incoming : {false, true}) for (unsigned mutation = 0; mutation < 8; ++mutation)
        scenario("old/same/invalid context does not replace legal cache / " + std::to_string(incoming) + "/" + std::to_string(mutation), [=] {
            Rig rig; rig.confirm();
            const auto latest = context(incoming ? 12 : 10);
            if (incoming) CHECK(rig.sync.receive(latest));
            auto rejected = latest;
            if (mutation == 0) --rejected.profileVersion;
            if (mutation == 1) ++rejected.waterMl;
            if (mutation == 2) std::strcpy(rejected.babyName, "conflict name");
            if (mutation == 3) rejected = context(latest.profileVersion, true);
            if (mutation == 4) std::strcpy(rejected.deviceId, "wrong-device");
            if (mutation == 5) rejected.profileVersion = 0;
            if (mutation == 6) rejected.powderGPer100Ml = 0;
            const auto calls = nvs::io.calls.size();
            CHECK(rig.sync.receive(rejected) == (mutation == 7));
            CHECK(nvs::io.calls.size() == calls);
            const bool conflict = mutation >= 1 && mutation <= 3;
            CHECK(rig.sync.canPrepare() == (!incoming && !conflict));
            const auto sends = rig.link.contexts.size();
            rig.sync.poll(200);
            CHECK(sameProductContext(rig.store.state().context, latest));
            if (conflict) CHECK(rig.link.contexts.size() == sends);
            else if (incoming) { rig.link.complete(); rig.sync.poll(201); }
            CHECK(rig.sync.canPrepare() == !conflict);
        });
    scenario("new context supersedes a pending transfer, never sends stale retry", [] {
        Rig rig; rig.sync.poll(100);
        CHECK(rig.sync.receive(context(11))); rig.sync.poll(101);
        CHECK(rig.link.cancellations == 1 && rig.link.contexts.size() == 2);
        CHECK(rig.link.contexts.back().profileVersion == 11 && !rig.sync.canPrepare());
        rig.link.complete(); rig.sync.poll(102); CHECK(rig.sync.canPrepare());
    });
    scenario("maximum profile version is not sequence rollover", [] {
        Rig rig; CHECK(rig.sync.receive(context(INT32_MAX))); rig.sync.poll(100);
        rig.link.complete(); rig.sync.poll(101); CHECK(rig.sync.canPrepare());
        CHECK(!rig.sync.receive(context(1)) && !rig.sync.receive(context(INT32_MAX - 1)));
        CHECK(rig.store.state().context.profileVersion == INT32_MAX);
    });
}

void conflicts() {
    for (bool incoming : {false, true}) for (bool tombstone : {false, true})
        for (auto status : {ContextStatus::Stored, ContextStatus::Unchanged, ContextStatus::Busy,
                            ContextStatus::Conflict, ContextStatus::StorageFault})
            scenario("same version conflict requires higher durable proof / " + std::to_string(incoming) + "/" +
                     std::to_string(tombstone) + "/" + std::to_string(unsigned(status)), [=] {
                Rig rig(1, true); rig.confirm(); const auto frozen = rig.store.state();
                const auto a = context(incoming ? 11 : 10);
                if (incoming) CHECK(rig.sync.receive(a));
                auto b = a;
                if (tombstone) b = context(a.profileVersion, true);
                else std::strcpy(b.formulaBrand, "same version conflicting full brand");
                const auto calls = nvs::io.calls.size();
                CHECK(!rig.sync.receive(b) && !rig.sync.canPrepare());
                CHECK(nvs::io.calls.size() == calls);
                CHECK(rig.sync.receive(a) && !rig.sync.canPrepare());
                CHECK(!rig.sync.receive(context(a.profileVersion - 1)) && !rig.sync.canPrepare());
                const auto sends = rig.link.contexts.size();
                for (unsigned i = 0; i < 3; ++i) rig.sync.poll(200 + i * 1000);
                CHECK(rig.link.contexts.size() == sends && !rig.sync.canPrepare());
                CHECK(sameProductContext(rig.store.state().context, a));
                pendingUnchanged(frozen, rig.store.state());
                CHECK(rig.sync.receive(context(a.profileVersion + 1)) && !rig.sync.canPrepare());
                rig.sync.poll(4000);
                CHECK(rig.link.contexts.size() == sends + 1 && !rig.sync.canPrepare());
                rig.link.complete(status); rig.sync.poll(4001);
                const bool proof = status == ContextStatus::Stored || status == ContextStatus::Unchanged;
                CHECK(rig.sync.canPrepare() == proof);
                pendingUnchanged(frozen, diskBrain());
                if (!proof) {
                    // A late old A or same-version conflicting B cannot relax the barrier.
                    CHECK(!rig.sync.receive(a) && !rig.sync.receive(b) && !rig.sync.canPrepare());
                    rig.sync.poll(5001); rig.link.complete(ContextStatus::Unchanged); rig.sync.poll(5002);
                    CHECK(rig.sync.canPrepare());
                }
            });
    scenario("conflict cancels an in-flight version without altering durable request", [] {
        Rig rig(1, true); const auto before = rig.store.state(); rig.sync.poll(100);
        auto b = context(); ++b.waterMl;
        CHECK(!rig.sync.receive(b)); rig.sync.poll(101);
        CHECK(rig.link.cancellations == 1 && !rig.sync.canPrepare());
        CHECK(sameBrainState(before, rig.store.state()) && sameBrainState(before, diskBrain()));
        CHECK(rig.sync.receive(context())); rig.sync.poll(2000);
        CHECK(rig.link.contexts.size() == 1 && !rig.sync.canPrepare());
    });
}

void replies() {
    for (auto status : {ContextStatus::Stored, ContextStatus::Unchanged, ContextStatus::Busy,
                        ContextStatus::Conflict, ContextStatus::StorageFault})
        scenario("only stored/unchanged prove persistence / " + std::to_string(unsigned(status)), [=] {
            Rig rig; rig.sync.poll(100); const auto disk = nvs::io.disk;
            rig.link.complete(status); rig.sync.poll(101);
            const bool proof = status == ContextStatus::Stored || status == ContextStatus::Unchanged;
            CHECK(rig.sync.canPrepare() == proof && nvs::io.disk == disk);
            rig.sync.poll(1100); CHECK(rig.link.contexts.size() == 1);
            rig.sync.poll(1101); CHECK(rig.link.contexts.size() == (proof ? 1u : 2u));
            if (!proof) CHECK(sameProductContext(rig.link.contexts[0], rig.link.contexts[1]));
        });
    for (unsigned mutation = 0; mutation < 4; ++mutation)
        scenario("FakeLink unmatched completed reply never proves save / " + std::to_string(mutation), [=] {
            Rig rig; rig.sync.poll(100);
            if (mutation == 0) std::strcpy(rig.link.reply.deviceId, "wrong-device");
            if (mutation == 1) ++rig.link.reply.profileVersion;
            if (mutation == 2) rig.link.reply.cleared = true;
            if (mutation == 3) rig.link.reply.digest[0] ^= 1;
            rig.link.complete(); rig.sync.poll(101); CHECK(!rig.sync.canPrepare());
            rig.sync.poll(1101); CHECK(rig.link.contexts.size() == 2);
        });
    for (auto state : {ContextSendState::Pending, ContextSendState::TimedOut,
                       ContextSendState::Unavailable, ContextSendState::Cancelled, ContextSendState::Idle})
        scenario("noncomplete transport state is not application proof / " + std::to_string(unsigned(state)), [=] {
            Rig rig; rig.sync.poll(100); rig.link.state = state;
            rig.sync.poll(101); CHECK(!rig.sync.canPrepare());
            rig.sync.poll(1101);
            CHECK(rig.link.contexts.size() == (state == ContextSendState::Pending ? 1u : 2u));
        });
    for (unsigned mutation = 0; mutation < 7; ++mutation)
        scenario("changed/retired proof immediately denies Prepare / " + std::to_string(mutation), [=] {
            Rig rig; rig.confirm();
            if (mutation == 0) ++rig.link.reply.replyTo;
            if (mutation == 1) ++rig.link.reply.profileVersion;
            if (mutation == 2) rig.link.reply.cleared = true;
            if (mutation == 3) rig.link.reply.digest[31] ^= 1;
            if (mutation == 4) std::strcpy(rig.link.reply.deviceId, "wrong-device");
            if (mutation == 5) rig.link.reply.status = ContextStatus::Busy;
            if (mutation == 6) rig.link.state = ContextSendState::Unavailable;
            CHECK(!rig.sync.canPrepare()); rig.sync.poll(102);
            CHECK(rig.link.contexts.size() == 2 && !rig.sync.canPrepare());
            rig.link.complete(ContextStatus::Unchanged); rig.sync.poll(103); CHECK(rig.sync.canPrepare());
        });
    scenario("new owner cannot inherit a stale completed reply", [] {
        Rig rig; rig.confirm(); Sync fresh(rig.link, rig.store);
        CHECK(!fresh.canPrepare()); fresh.poll(200);
        CHECK(rig.link.contexts.size() == 2 && !fresh.canPrepare());
        rig.link.complete(); fresh.poll(201); CHECK(fresh.canPrepare());
    });
}

void scheduling() {
    for (auto settled : {StopSendState::Received, StopSendState::TimedOut})
        scenario("Stop pending copies but never writes/sends, receipt or timeout resumes / " +
                 std::to_string(unsigned(settled)), [=] {
            Rig rig(1, true); rig.confirm(); const auto before = rig.store.state();
            rig.link.stop = StopSendState::Pending;
            const auto disk = nvs::io.disk;
            const auto calls = nvs::io.calls.size();
            const auto attempts = rig.link.attempts, cancellations = rig.link.cancellations;
            CHECK(rig.sync.receive(context(11)) && rig.sync.receive(context(12)));
            CHECK(!rig.sync.canPrepare());
            for (unsigned i = 0; i < 10; ++i) rig.sync.poll(200 + i * 1000);
            CHECK(nvs::io.disk == disk && nvs::io.calls.size() == calls);
            CHECK(rig.link.attempts == attempts && rig.link.cancellations == cancellations);
            CHECK(rig.link.stop == StopSendState::Pending && sameBrainState(rig.store.state(), before));
            rig.link.stop = settled; rig.sync.poll(10200);
            CHECK(rig.store.state().context.profileVersion == 12 && rig.link.attempts == attempts + 1);
            pendingUnchanged(before, rig.store.state());
            CHECK(!rig.sync.canPrepare());
            rig.link.complete(); rig.sync.poll(10201); CHECK(rig.sync.canPrepare());
        });
    for (unsigned gate = 0; gate < 3; ++gate) scenario("ordinary busy/offline/transport owner only defer transfer / " + std::to_string(gate), [=] {
        Rig rig;
        if (gate == 1) rig.link.board = false;
        if (gate == 2) rig.link.available = false;
        CHECK(rig.sync.receive(context(11))); rig.sync.poll(100, false, gate == 0);
        CHECK(!rig.link.attempts && rig.store.state().context.profileVersion == 11);
        rig.link.board = rig.link.available = true; rig.sync.poll(101);
        CHECK(rig.link.contexts.size() == 1);
    });
    scenario("maintenance defers callback cache write and cancels only current configuration", [] {
        Rig rig(1, true); rig.sync.poll(100); const auto before = rig.store.state();
        CHECK(rig.sync.receive(context(11))); const auto calls = nvs::io.calls.size();
        rig.sync.poll(101, true); CHECK(rig.link.cancellations == 1);
        rig.sync.poll(5000, true); CHECK(rig.link.cancellations == 1);
        CHECK(nvs::io.calls.size() == calls && sameBrainState(rig.store.state(), before));
        rig.sync.poll(5001); CHECK(rig.link.contexts.size() == 2);
        pendingUnchanged(before, rig.store.state());
        CHECK(rig.link.contexts.back().profileVersion == 11);
    });
    scenario("yield retains cache, pending and sequence; same config retries at one second", [] {
        Rig rig(1, true); const auto before = rig.store.state(); rig.sync.poll(100);
        rig.sync.yield(101); CHECK(!rig.sync.canPrepare());
        rig.sync.yield(102); CHECK(rig.link.cancellations == 1);
        rig.sync.poll(1100); CHECK(rig.link.contexts.size() == 1);
        rig.sync.poll(1101); CHECK(rig.link.contexts.size() == 2);
        CHECK(sameProductContext(rig.link.contexts[0], rig.link.contexts[1]));
        CHECK(sameBrainState(before, rig.store.state()) && sameBrainState(before, diskBrain()));
    });
    scenario("request refusal retry is bounded across uint32 clock rollover", [] {
        Rig rig; rig.link.accept = false; constexpr uint32_t start = UINT32_MAX - 499;
        rig.sync.poll(start); CHECK(rig.link.attempts == 1 && rig.link.contexts.empty());
        rig.link.accept = true;
        rig.sync.poll(start + 999u); CHECK(rig.link.attempts == 1);
        rig.sync.poll(start + 1000u); CHECK(rig.link.attempts == 2 && rig.link.contexts.size() == 1);
        rig.link.state = ContextSendState::TimedOut; rig.sync.poll(start + 1001u);
        rig.sync.poll(start + 2000u); CHECK(rig.link.contexts.size() == 1);
        rig.sync.poll(start + 2001u); CHECK(rig.link.contexts.size() == 2);
    });
}

void storage() {
    std::vector<nvs::Call> writes;
    scenario("capture real Brain context NVS path", [&] {
        Rig rig(1, true); nvs::reboot();
        CHECK(rig.sync.receive(context(11))); rig.sync.poll(100);
        writes = nvs::io.calls;
    });
    for (const auto& call : writes) {
        if (call.op == Op::Close) continue;
        const unsigned variants = call.op == Op::Set || call.op == Op::Commit ? 2 : 1;
        for (unsigned apply = 0; apply < variants; ++apply)
            scenario("Brain save failure never sends / " + std::to_string(unsigned(call.op)) + "/" +
                     std::to_string(call.occurrence) + "/" + std::to_string(apply), [=] {
                Rig rig(1, true); rig.confirm(); const auto before = rig.store.state();
                nvs::reboot();
                nvs::fail(call.op, call.occurrence, ESP_FAIL, apply != 0);
                CHECK(rig.sync.receive(context(11))); const auto attempts = rig.link.attempts;
                rig.sync.poll(200);
                CHECK(rig.store.faulted() && !rig.store.ready() && !rig.sync.canPrepare());
                CHECK(rig.link.attempts == attempts && sameBrainState(rig.store.state(), before));
                for (unsigned i = 0; i < 3; ++i) rig.sync.poll(1200 + 1000 * i);
                CHECK(rig.link.attempts == attempts);
                nvs::verifyFaults(); const auto durable = diskBrain(); pendingUnchanged(before, durable);
                const bool committed = apply || (call.op == Op::OpenRO && call.occurrence == 2) ||
                    ((call.op == Op::Query || call.op == Op::Read) && call.occurrence == 3);
                CHECK(durable.context.profileVersion == (committed ? 11u : 10u));
                nvs::reboot(); BrainStateStore loaded; CHECK(loaded.load(pairing()) == BrainLoad::Ready);
                FakeLink link; Sync owner(link, loaded); owner.poll(5000);
                CHECK(link.contexts.size() == 1 && sameProductContext(link.contexts[0], durable.context));
                CHECK(sameBrainState(loaded.state(), durable) && loaded.state().pending);
                CHECK(!nvs::count(Op::Set) && !nvs::count(Op::Commit));
            });
    }
    scenario("SHA failure during save never sends nor publishes a new snapshot", [] {
        Rig rig(1, true); rig.confirm(); const auto before = rig.store.state(); const auto disk = nvs::io.disk;
        CHECK(rig.sync.receive(context(11))); const auto sends = rig.link.attempts;
        fake_product_crypto::fail = true; rig.sync.poll(200); fake_product_crypto::fail = false;
        CHECK(!rig.sync.canPrepare() && rig.store.faulted() && rig.link.attempts == sends);
        CHECK(sameBrainState(before, rig.store.state()) && nvs::io.disk == disk);
    });
}

// Small test boundary helpers follow test_motion_product_runtime.cpp. No
// product admission, context projection, persistence or protocol logic is copied.
struct Executor : motion::DemoExecutor {
    motion::DemoEvidence sample{true, true, false, 0};
    motion::DemoExecution state = motion::DemoExecution::Running;
    unsigned starts = 0, stops = 0;
    bool healthy() const override { return true; }
    bool available() const override { return true; }
    motion::DemoEvidence evidence(uint8_t id) const override { CHECK(id == 1); return sample; }
    bool start(const motion::DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts; state = motion::DemoExecution::Running; sample.stationary = false; return true;
    }
    motion::DemoExecution execution() const override { return state; }
    babytech::display::DisplayError failureError() const override { return babytech::display::DisplayError::CanFault; }
    bool stop() override { ++stops; sample.fresh = sample.stationary = false; return true; }
    bool reset() override { return true; }
};
struct Hardware : motion::MotionProductHardware {
    Executor& executor;
    uint32_t clock = 1000;
    const char* owner = nullptr;
    unsigned generated = 0;
    explicit Hardware(Executor& value) : executor(value) {}
    uint32_t nowMs() const override { return clock; }
    const char* unavailable() const override { return owner; }
    bool newExecution(char (&id)[33]) override {
        std::snprintf(id, sizeof(id), "%032x", ++generated); return true;
    }
    bool stationary() const override { return executor.sample.fresh && executor.sample.stationary && !executor.sample.fault; }
};
struct MotionFixture {
    MotionStateStore store;
    Executor executor;
    motion::DemoFlowController flow{executor};
    motion::ProductSession product{flow};
    Hardware hardware{executor};
    motion::MotionProductRuntime runtime{store, product, flow, hardware};
    explicit MotionFixture(const ProductContext& c, bool restored = false, bool ready = false) {
        if (restored) CHECK(store.load(pairing(v4::Role::Motion)) == MotionLoad::Ready);
        else CHECK(store.installInitial(pairing(v4::Role::Motion), &c) == MotionWrite::Stored);
        motion::DemoConfig config;
        config.configured = true; config.axes.push_back({1, 10, 0, true});
        config.initialization.commands = {"initialize"};
        for (auto& stage : config.stages) stage.commands = {"stage"};
        CHECK(flow.apply(config));
        product.setExecutionAuthorized(true); product.resources(true, false, true, 300, 1000);
        if (ready) {
            ContextResult reply;
            CHECK(runtime.context(c, 1000, reply) && reply.status == ContextStatus::Unchanged);
            CHECK(product.initialize(900)); flow.tick(901);
            executor.state = motion::DemoExecution::Done; executor.sample = {true, true, false, 0};
            flow.tick(902); product.tick(902); CHECK(product.canStart()); executor.starts = 0;
        }
    }
    void tick(uint32_t now) { flow.tick(now); product.tick(now); runtime.poll(now); }
};
MotionFixture* target = nullptr;
std::vector<ProductContext> delivered;
unsigned commands = 0, stops = 0;
bool routeContext(const ProductContext& c, uint32_t now, ContextResult& reply) {
    CHECK(target); delivered.push_back(c); return target->runtime.context(c, now, reply);
}
bool routeCommand(const CommandMessage& c, uint32_t now, CommandResult& reply) {
    CHECK(target); ++commands; return target->runtime.command(c, now, reply);
}
bool routeStop(const v4::StopRequest& r, uint32_t now) { CHECK(target); ++stops; return target->runtime.stop(r, now); }
bool routeReady(CommandResult& r) { CHECK(target); return target->runtime.resultReady(r); }
struct PeerWire : v4::ByteSink {
    static constexpr size_t capacity = 23;
    std::array<uint8_t, capacity> bytes{};
    size_t used = 0, writeLimit = 7;
    unsigned calls = 0, shorts = 0, zeros = 0;
    bool drop = false;
    v4::Parser observer;
    v4::Assembler assembler;
    std::vector<v4::Frame> frames;
    std::vector<uint32_t> frameTimes;
    std::vector<v4::Message> messages;
    bool idle() const override { return used == 0; }
    size_t available() const override { return capacity - used; }
    size_t write(const uint8_t* data, size_t length) override {
        CHECK(length && length <= available());
        if (++calls % 13 == 0) { ++zeros; return 0; }
        const auto take = std::min(length, writeLimit);
        if (take < length) ++shorts;
        std::memcpy(bytes.data() + used, data, take); used += take; return take;
    }
    void deliver(ReadOnlyLink& peer, uint32_t now) {
        for (size_t i = 0; i < used; ++i) {
            v4::Frame frame;
            if (observer.push(bytes[i], now, frame)) {
                frames.push_back(frame); frameTimes.push_back(now); v4::Message message;
                if (assembler.accept(frame, now, message) == v4::AssemblyResult::Complete) messages.push_back(message);
            }
            if (!drop) peer.receive(bytes[i], now);
        }
        used = 0;
    }
    unsigned count(v4::Kind kind) const {
        return unsigned(std::count_if(messages.begin(), messages.end(), [=](const v4::Message& m) { return m.kind == kind; }));
    }
    unsigned fragments(v4::Kind kind) const {
        return unsigned(std::count_if(frames.begin(), frames.end(), [=](const v4::Frame& f) { return f.kind == kind; }));
    }
};
using ProductionSync = babytech::brain::BrainContextSync<ReadOnlyLink>;
struct Integration {
    std::unique_ptr<BrainStateStore> store{new BrainStateStore};
    std::unique_ptr<MotionFixture> device;
    ReadOnlyLink brain, motionLink;
    PeerWire toMotion, toBrain;
    std::unique_ptr<ProductionSync> sync;
    uint32_t now = 1000;
    uint64_t brainBoot = 11, motionBoot = 22;
    explicit Integration(bool clear = false, bool ready = false, uint64_t localSequence = 0) {
        CHECK(!target); const auto c = context(10, clear);
        CHECK(store->installInitial(pairing(), &c) == BrainWrite::Stored);
        if (localSequence) {
            auto seeded = store->state(); seeded.localSequence = localSequence;
            std::array<uint8_t, kBrainStateMaxSize> bytes{};
            const auto length = encodeBrainState(seeded, bytes.data(), bytes.size()); CHECK(length);
            nvs::io.disk["brainstate"]["record"] = {{bytes.begin(), bytes.begin() + length}, nvs::Type::Blob};
            store.reset(new BrainStateStore); CHECK(store->load(pairing()) == BrainLoad::Ready);
        }
        device.reset(new MotionFixture(c, false, ready)); target = device.get();
        delivered.clear(); commands = stops = 0;
        CHECK(brain.begin(pairing(), brainBoot)); beginMotion();
        sync.reset(new ProductionSync(brain, *store));
        run(1000, false); CHECK(brain.connected(now) && motionLink.connected(now));
        available();
    }
    ~Integration() {
        wireContexts += toMotion.count(v4::Kind::Context); wireResults += toBrain.count(v4::Kind::ContextResult);
        target = nullptr;
    }
    void beginMotion() {
        CHECK(motionLink.begin(pairing(v4::Role::Motion), motionBoot));
        CHECK(motionLink.setContextHandler(routeContext) && motionLink.setCommandHandler(routeCommand));
        CHECK(motionLink.setStopHandler(routeStop) && motionLink.setCommandReadyHandler(routeReady));
    }
    void step(bool owner = true, bool maintenance = false, bool busy = false) {
        device->hardware.clock = now;
        if (owner) sync->poll(now, maintenance, busy);
        brain.poll(now, toMotion); motionLink.poll(now, toBrain);
        toMotion.deliver(motionLink, now); toBrain.deliver(brain, now);
        device->tick(now); ++now;
    }
    void run(unsigned ticks, bool owner = true, bool maintenance = false, bool busy = false) {
        while (ticks--) step(owner, maintenance, busy);
    }
    void available() {
        for (unsigned i = 0; i < 1000 && !brain.commandAvailable(now); ++i) step(false);
        CHECK(brain.commandAvailable(now));
    }
    void await(ContextStatus status, bool cleared = false) {
        const auto before = delivered.size();
        for (unsigned i = 0; i < 5000; ++i) {
            step();
            if (brain.contextSendState() == ContextSendState::Complete &&
                brain.contextResponse().status == status && delivered.size() > before) {
                sync->poll(now);
                CHECK(matchesContextResult(brain.contextResponse(), store->state().context));
                const bool persisted = status == ContextStatus::Stored || status == ContextStatus::Unchanged;
                CHECK(sync->canPrepare() == (persisted && !cleared)); return;
            }
        }
        CHECK(false);
    }
    void confirm() { await(ContextStatus::Unchanged, store->state().context.cleared); }
    void reboot(bool restartBrain, bool restartMotion) {
        if (restartBrain) {
            sync.reset(); store.reset(new BrainStateStore);
            CHECK(store->load(pairing()) == BrainLoad::Ready);
            CHECK(brain.begin(pairing(), ++brainBoot));
            sync.reset(new ProductionSync(brain, *store));
        }
        if (restartMotion) {
            target = nullptr; device.reset(new MotionFixture(context(), true)); target = device.get();
            ++motionBoot; beginMotion();
        }
        run(1000, false); CHECK(brain.connected(now) && motionLink.connected(now));
        CHECK(!sync->canPrepare()); available();
    }
    void noActions() const {
        CHECK(!commands && !device->executor.starts && !device->executor.stops && !device->hardware.generated);
        CHECK(!toMotion.count(v4::Kind::Command) && !toMotion.count(v4::Kind::ResultQuery));
    }
    void inject(v4::Message message, bool oldSender = false, bool oldReceiver = false) {
        message.senderBoot = motionBoot + (oldSender ? 1 : 0);
        message.receiverBoot = brainBoot + (oldReceiver ? 1 : 0); message.messageId = 900;
        size_t offset = 0;
        while (offset < message.length) {
            v4::Frame frame; CHECK(v4::fragment(message, offset, frame));
            brain.receiveFrame(frame, now); offset += frame.length;
        }
    }
    void injectMotionFrame(const v4::Frame& frame, const std::function<void()>& beforeCrc) {
        std::array<uint8_t, v4::kMaxFrame> bytes{};
        const auto length = v4::encode(frame, bytes.data(), bytes.size()); CHECK(length);
        // Exercise the real parser, including its commit boundary, not receiveFrame.
        for (size_t i = 0; i + 1 < length; ++i) motionLink.receive(bytes[i], now);
        beforeCrc();
        motionLink.receive(bytes[length - 1], now);
    }
    void fullWire(const ProductContext& expected) const {
        MotionState persisted;
        const auto& bytes = nvs::io.disk.at("productstate").at("record").bytes;
        CHECK(decodeMotionState(bytes.data(), bytes.size(), persisted));
        CHECK(sameMotionState(persisted, device->store.state()));
        MotionContextBarrier barrier;
        CHECK(makeMotionContextBarrier(expected, barrier) && sameMotionContextBarrier(persisted.context, barrier));
        CHECK(sameProductContext(diskBrain().context, expected));
        CHECK(toMotion.count(v4::Kind::Context) && toBrain.count(v4::Kind::ContextResult));
        bool foundContext = false, foundReply = false;
        for (const auto& m : toMotion.messages) if (m.kind == v4::Kind::Context) {
            ProductContext c; CHECK(decodeContextMessage(m, expected.deviceId, c));
            if (sameProductContext(c, expected)) foundContext = true;
        }
        for (const auto& m : toBrain.messages) if (m.kind == v4::Kind::ContextResult) {
            ContextResult r; CHECK(decodeContextResult(m, r));
            if (matchesContextResult(r, expected) && r.replyTo == brain.contextResponse().replyTo) foundReply = true;
        }
        CHECK(foundContext && foundReply && toMotion.shorts && toBrain.shorts && toMotion.zeros && toBrain.zeros);
    }
};

void preemption() {
    for (unsigned phase = 0; phase < 4; ++phase)
        for (auto operation : {ProductCommand::Initialize, ProductCommand::Clean})
            scenario("real yielded config -> immediate multi-fragment Initialize/Clean / " +
                     std::to_string(phase) + "/" + std::to_string(unsigned(operation)), [=] {
                // A valid near-limit boot makes even Clean's canonical local ID
                // and decimal sequence long enough for multiple real fragments.
                constexpr uint64_t sequence = v4::kMaxSequence - 1;
                Integration rig(false, operation == ProductCommand::Clean, sequence - 1);
                const auto c = context(11, false, true);
                CHECK(rig.sync->receive(c)); rig.sync->poll(rig.now);
                const auto contextId = rig.brain.contextResponse().replyTo;
                CHECK(contextId && rig.brain.contextSendState() == ContextSendState::Pending);
                if (phase == 1) {
                    rig.toMotion.writeLimit = 1; rig.run(10, false);
                    CHECK(!rig.toMotion.fragments(v4::Kind::Context) && !rig.motionLink.installAssembler().active());
                } else if (phase == 2) {
                    for (unsigned i = 0; i < 800 && !rig.toMotion.fragments(v4::Kind::Context); ++i) rig.step(false);
                    CHECK(rig.toMotion.fragments(v4::Kind::Context) == 1 && delivered.empty());
                    CHECK(rig.motionLink.installAssembler().active());
                } else if (phase == 3) {
                    rig.toBrain.drop = true;
                    for (unsigned i = 0; i < 800 && delivered.empty(); ++i) rig.step(false);
                    CHECK(delivered.size() == 1 && rig.device->store.state().context.profileVersion == 11);
                }
                CHECK(rig.brain.contextSendState() == ContextSendState::Pending);
                const auto motionContext = rig.device->store.state().context;
                const auto committedBrain = diskBrain();
                const uint32_t yieldedAt = rig.now;
                rig.sync->yield(rig.now);
                CHECK(rig.brain.contextSendState() == ContextSendState::Cancelled && !rig.sync->canPrepare());
                rig.toMotion.writeLimit = 7; rig.toBrain.drop = false;
                // A partial poisoned TX frame must drain; it is not an assembler
                // timeout or permission to wait for/replay an ordinary action.
                for (unsigned i = 0; i < 50 && !rig.brain.commandAvailable(rig.now); ++i) rig.step(false);
                CHECK(rig.brain.commandAvailable(rig.now) && uint32_t(rig.now - yieldedAt) < 50);
                const auto r = request(c, sequence, operation);
                CommandMessage command; command.request = r; command.remainingTtlMs = 5000;
                v4::Message encoded; CHECK(encodeCommand(command, encoded));
                CHECK(encoded.length > v4::kMaxFragment);
                CHECK(rig.store->reserveLocal(r) == BrainWrite::Stored);
                CHECK(rig.brain.requestCommand(command, rig.now));
                for (unsigned i = 0; i < 500 && rig.brain.commandSendState() == CommandSendState::Pending; ++i) rig.step(false);
                CHECK(uint32_t(rig.now - yieldedAt) < v4::kMessageTimeoutMs);
                CHECK(rig.brain.commandSendState() == CommandSendState::Complete);
                const auto& reply = rig.brain.commandResponse();
                CHECK(reply.accepted && reply.source == r.source && reply.sequence == r.sequence &&
                      !std::strcmp(reply.commandId, r.commandId) && !std::strcmp(reply.reason, "accepted"));
                CHECK(commands == 1 && !stops && rig.device->store.state().localSequence == sequence);
                CHECK(sameProductRequest(rig.device->store.state().localResult.request, r));
                CHECK(rig.device->store.state().localResult.accepted);
                CHECK(sameMotionContextBarrier(rig.device->store.state().context, motionContext));
                CHECK(sameProductContext(diskBrain().context, committedBrain.context));
                CHECK(delivered.size() == (phase == 3 ? 1u : 0u));
                CHECK(!rig.sync->canPrepare());
                if (operation == ProductCommand::Initialize) CHECK(rig.device->executor.starts == 1);
                else CHECK(rig.device->product.ownsMotion() && !rig.device->executor.starts);

                // Reassemble only the observed COMMAND bytes with the production
                // assembler, independently of the spectator's old Context slot.
                v4::Assembler commandAssembly; v4::Message complete;
                unsigned fragments = 0; uint32_t commandId = 0;
                bool assembled = false;
                for (size_t i = 0; i < rig.toMotion.frames.size(); ++i) {
                    const auto& frame = rig.toMotion.frames[i];
                    if (frame.kind != v4::Kind::Command) continue;
                    CHECK(frame.total > v4::kMaxFragment && frame.messageId > contextId);
                    if (!fragments) { CHECK(frame.offset == 0); commandId = frame.messageId; }
                    CHECK(frame.messageId == commandId);
                    ++fragments;
                    if (commandAssembly.accept(frame, rig.toMotion.frameTimes[i], complete) == v4::AssemblyResult::Complete)
                        assembled = true;
                }
                CHECK(fragments >= 2 && assembled);
                CommandMessage decoded; CHECK(decodeCommand(complete, decoded) && sameProductRequest(decoded.request, r));
                CHECK(rig.store->clearPending(r) == BrainWrite::Stored);
                CHECK(!rig.store->state().pending && rig.store->state().localSequence == sequence);
                std::printf("  preemption phase=%u command=%u fragments=%u completion=%u ms\n",
                            phase, unsigned(operation), fragments, uint32_t(rig.now - yieldedAt));
            }, true);
}

void preemptionRejections() {
    const char* cases[] = {"old-id", "first-offset-1", "first-offset-fragment",
                          "first-offset-last", "old-sender-boot", "future-sender-boot",
                          "old-receiver-boot", "future-receiver-boot"};
    for (unsigned mutation = 0; mutation < sizeof(cases) / sizeof(cases[0]); ++mutation)
        for (auto operation : {ProductCommand::Initialize, ProductCommand::Clean})
            scenario("real unfinished Context survives Command / " + std::string(cases[mutation]) + "/" +
                     std::to_string(unsigned(operation)), [=] {
                constexpr uint64_t sequence = v4::kMaxSequence - 1;
                Integration rig(false, operation == ProductCommand::Clean, sequence - 1);
                const auto c = context(11, false, true);
                const auto r = request(c, sequence, operation);
                CHECK(rig.store->reserveLocal(r) == BrainWrite::Stored);
                CHECK(rig.sync->receive(c)); rig.sync->poll(rig.now);
                const auto contextId = rig.brain.contextResponse().replyTo;
                CHECK(contextId > 1);
                for (unsigned i = 0; i < 800 && !rig.toMotion.fragments(v4::Kind::Context); ++i) rig.step(false);
                CHECK(rig.toMotion.fragments(v4::Kind::Context) == 1 && delivered.empty());
                CHECK(rig.motionLink.installAssembler().active());
                const auto beganAt = rig.now;
                const auto brainBefore = rig.store->state();
                const auto motionBefore = rig.device->store.state();
                const auto disk = nvs::io.disk; const auto calls = nvs::io.calls.size();
                const auto sets = nvs::count(Op::Set), commits = nvs::count(Op::Commit);
                const auto projected = rig.device->product.context();
                const auto unchanged = [&] {
                    CHECK(rig.brain.contextSendState() == ContextSendState::Pending);
                    CHECK(rig.brain.commandSendState() == CommandSendState::Idle);
                    CHECK(delivered.empty() && !stops); rig.noActions();
                    CHECK(!rig.device->runtime.active() && !rig.device->product.ownsMotion());
                    CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
                    CHECK(sameBrainState(rig.store->state(), brainBefore));
                    CHECK(sameMotionState(rig.device->store.state(), motionBefore));
                    CHECK(rig.device->product.context().profileVersion == projected.profileVersion &&
                          rig.device->product.context().babyId == projected.babyId);
                    CHECK(rig.motionLink.installAssembler().active());
                };
                CommandMessage command; command.request = r; command.remainingTtlMs = 5000;
                v4::Message illegal; CHECK(encodeCommand(command, illegal));
                CHECK(illegal.length > v4::kMaxFragment);
                illegal.senderBoot = rig.brainBoot; illegal.receiverBoot = rig.motionBoot;
                illegal.messageId = mutation == 0 ? contextId - 1 : contextId + 1;
                size_t offset = 0;
                if (mutation == 1) offset = 1;
                if (mutation == 2) offset = v4::kMaxFragment;
                if (mutation == 3) offset = illegal.length - 1;
                if (mutation == 4) --illegal.senderBoot;
                if (mutation == 5) ++illegal.senderBoot;
                if (mutation == 6) --illegal.receiverBoot;
                if (mutation == 7) ++illegal.receiverBoot;
                while (offset < illegal.length) {
                    v4::Frame frame; CHECK(v4::fragment(illegal, offset, frame));
                    rig.injectMotionFrame(frame, unchanged); unchanged(); offset += frame.length;
                }
                // Continue the original sender, without yielding, a retry, or RX timeout.
                for (unsigned i = 0; i < 800 && rig.brain.contextSendState() == ContextSendState::Pending; ++i)
                    rig.step(false);
                CHECK(uint32_t(rig.now - beganAt) < v4::kMessageTimeoutMs);
                CHECK(!rig.motionLink.installAssembler().active());
                CHECK(rig.brain.contextSendState() == ContextSendState::Complete &&
                      rig.brain.contextResponse().status == ContextStatus::Stored);
                CHECK(delivered.size() == 1 && sameProductContext(delivered[0], c));
                CHECK(rig.toMotion.count(v4::Kind::Context) == 1 && rig.toBrain.count(v4::Kind::ContextResult) == 1);
                rig.sync->poll(rig.now); CHECK(rig.sync->canPrepare());
                rig.fullWire(c); rig.noActions(); CHECK(!stops);
                CHECK(sameBrainState(rig.store->state(), brainBefore) && nvs::io.disk.at("brainstate") == disk.at("brainstate"));
                auto expected = motionBefore; CHECK(makeMotionContextBarrier(c, expected.context));
                CHECK(sameMotionState(rig.device->store.state(), expected));
                CHECK(nvs::count(Op::Set) == sets + 1 && nvs::count(Op::Commit) == commits + 1);
            }, true);

    for (auto operation : {ProductCommand::Initialize, ProductCommand::Clean})
        scenario("real equal-ID kind collision aborts Context; timeout retry recovers / " +
                 std::to_string(unsigned(operation)), [=] {
            constexpr uint64_t sequence = v4::kMaxSequence - 1;
            Integration rig(false, operation == ProductCommand::Clean, sequence - 1);
            const auto c = context(11, false, true); const auto r = request(c, sequence, operation);
            CHECK(rig.store->reserveLocal(r) == BrainWrite::Stored);
            CHECK(rig.sync->receive(c)); rig.sync->poll(rig.now);
            const auto sentAt = rig.now, contextId = rig.brain.contextResponse().replyTo;
            for (unsigned i = 0; i < 800 && !rig.toMotion.fragments(v4::Kind::Context); ++i) rig.step(false);
            CHECK(rig.toMotion.fragments(v4::Kind::Context) == 1 && rig.motionLink.installAssembler().active());
            const auto brainBefore = rig.store->state(); const auto motionBefore = rig.device->store.state();
            const auto disk = nvs::io.disk; const auto calls = nvs::io.calls.size();
            const auto sets = nvs::count(Op::Set), commits = nvs::count(Op::Commit);
            const auto untouched = [&] {
                CHECK(delivered.empty() && !stops && !rig.sync->canPrepare()); rig.noActions();
                CHECK(!rig.device->runtime.active() && !rig.device->product.ownsMotion());
                CHECK(sameBrainState(rig.store->state(), brainBefore));
                CHECK(sameMotionState(rig.device->store.state(), motionBefore));
                CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
            };
            CommandMessage command; command.request = r; command.remainingTtlMs = 5000;
            v4::Message collision; CHECK(encodeCommand(command, collision));
            CHECK(collision.length > v4::kMaxFragment);
            collision.senderBoot = rig.brainBoot; collision.receiverBoot = rig.motionBoot; collision.messageId = contextId;
            size_t offset = 0;
            while (offset < collision.length) {
                v4::Frame frame; CHECK(v4::fragment(collision, offset, frame));
                rig.injectMotionFrame(frame, [&] {
                    untouched(); CHECK(rig.motionLink.installAssembler().active() == (offset == 0));
                });
                // Same key but a different kind is malformed-message Invalid,
                // not a newer Command preemption. Its tails cannot start a command.
                CHECK(!rig.motionLink.installAssembler().active()); untouched(); offset += frame.length;
            }
            for (unsigned i = 0; i < 800 && !rig.toMotion.count(v4::Kind::Context); ++i) {
                rig.step(false); untouched(); CHECK(!rig.motionLink.installAssembler().active());
            }
            CHECK(rig.toMotion.count(v4::Kind::Context) == 1 && rig.toBrain.count(v4::Kind::ContextResult) == 0);
            CHECK(uint32_t(rig.now - sentAt) < v4::kMessageTimeoutMs &&
                  rig.brain.contextSendState() == ContextSendState::Pending);
            for (unsigned i = 0; i < 1200 && rig.brain.contextSendState() == ContextSendState::Pending; ++i) {
                rig.step(false); untouched(); CHECK(!rig.motionLink.installAssembler().active());
            }
            CHECK(rig.brain.contextSendState() == ContextSendState::TimedOut &&
                  uint32_t(rig.now - sentAt) >= v4::kMessageTimeoutMs);
            rig.sync->poll(rig.now); rig.run(1000); untouched();
            CHECK(rig.brain.contextSendState() == ContextSendState::TimedOut && rig.toMotion.count(v4::Kind::Context) == 1);
            rig.await(ContextStatus::Stored); rig.fullWire(c); rig.noActions(); CHECK(!stops);
            CHECK(rig.brain.contextResponse().replyTo > contextId && !rig.motionLink.installAssembler().active());
            CHECK(delivered.size() == 1 && sameProductContext(delivered[0], c));
            CHECK(rig.toMotion.count(v4::Kind::Context) == 2 && rig.toBrain.count(v4::Kind::ContextResult) == 1);
            CHECK(sameBrainState(rig.store->state(), brainBefore) && nvs::io.disk.at("brainstate") == disk.at("brainstate"));
            auto expected = motionBefore; CHECK(makeMotionContextBarrier(c, expected.context));
            CHECK(sameMotionState(rig.device->store.state(), expected));
            CHECK(nvs::count(Op::Set) == sets + 1 && nvs::count(Op::Commit) == commits + 1);
        }, true);

    for (bool continuation : {false, true})
        for (auto operation : {ProductCommand::Initialize, ProductCommand::Clean})
            scenario("real existing Command RX survives newer Command " + std::string(continuation ? "tail" : "head") +
                     "/" + std::to_string(unsigned(operation)), [=] {
                constexpr uint64_t sequence = v4::kMaxSequence - 1;
                Integration rig(false, operation == ProductCommand::Clean, sequence - 1); rig.confirm(); rig.available();
                // A completed high-ID Context leaves its marker behind. Starting
                // a lower-ID Command keeps that marker, exercising kind-specific cancellation.
                constexpr uint32_t contextId = 900;
                v4::Message prior; CHECK(encodeContextMessage(context(), prior));
                prior.senderBoot = rig.brainBoot; prior.receiverBoot = rig.motionBoot; prior.messageId = contextId;
                const auto setupDisk = nvs::io.disk;
                const auto setupSets = nvs::count(Op::Set), setupCommits = nvs::count(Op::Commit);
                const auto replies = rig.toBrain.count(v4::Kind::ContextResult);
                size_t offset = 0;
                while (offset < prior.length) {
                    v4::Frame frame; CHECK(v4::fragment(prior, offset, frame));
                    rig.injectMotionFrame(frame, [] {}); offset += frame.length;
                }
                CHECK(!rig.motionLink.installAssembler().active() && delivered.size() == 2);
                for (unsigned i = 0; i < 300 && rig.toBrain.count(v4::Kind::ContextResult) == replies; ++i) rig.step(false);
                CHECK(rig.toBrain.count(v4::Kind::ContextResult) == replies + 1);
                CHECK(nvs::io.disk == setupDisk && nvs::count(Op::Set) == setupSets &&
                      nvs::count(Op::Commit) == setupCommits); rig.noActions();
                const auto r = request(context(), sequence, operation);
                CHECK(rig.store->reserveLocal(r) == BrainWrite::Stored);
                CommandMessage command; command.request = r; command.remainingTtlMs = 5000;
                CHECK(rig.brain.requestCommand(command, rig.now));
                for (unsigned i = 0; i < 100 && !rig.toMotion.fragments(v4::Kind::Command); ++i) rig.step(false);
                CHECK(rig.toMotion.fragments(v4::Kind::Command) == 1 && rig.motionLink.installAssembler().active());
                const auto& first = rig.toMotion.frames.back();
                CHECK(first.kind == v4::Kind::Command && !first.offset && first.messageId < contextId && first.total > first.length);
                const auto beganAt = rig.now;
                const auto brainBefore = rig.store->state(); const auto motionBefore = rig.device->store.state();
                const auto disk = nvs::io.disk; const auto calls = nvs::io.calls.size();
                const auto sets = nvs::count(Op::Set), commits = nvs::count(Op::Commit);
                const auto unchanged = [&] {
                    CHECK(rig.motionLink.installAssembler().active() && commands == 0 && stops == 0);
                    CHECK(rig.brain.commandSendState() == CommandSendState::Pending);
                    CHECK(!rig.device->executor.starts && !rig.device->executor.stops && !rig.device->hardware.generated);
                    CHECK(!rig.device->runtime.active() && !rig.device->product.ownsMotion());
                    CHECK(delivered.size() == 2 && nvs::io.calls.size() == calls && nvs::io.disk == disk);
                    CHECK(sameBrainState(rig.store->state(), brainBefore));
                    CHECK(sameMotionState(rig.device->store.state(), motionBefore));
                };
                auto intruder = command; intruder.request = request(context(), sequence - 1, operation);
                v4::Message illegal; CHECK(encodeCommand(intruder, illegal));
                CHECK(illegal.length > v4::kMaxFragment);
                illegal.senderBoot = rig.brainBoot; illegal.receiverBoot = rig.motionBoot; illegal.messageId = contextId + 1;
                offset = continuation ? v4::kMaxFragment : 0;
                while (offset < illegal.length) {
                    v4::Frame frame; CHECK(v4::fragment(illegal, offset, frame));
                    rig.injectMotionFrame(frame, unchanged); unchanged(); offset += frame.length;
                }
                for (unsigned i = 0; i < 500 && rig.brain.commandSendState() == CommandSendState::Pending; ++i) rig.step(false);
                CHECK(uint32_t(rig.now - beganAt) < v4::kMessageTimeoutMs);
                CHECK(!rig.motionLink.installAssembler().active() && rig.brain.commandSendState() == CommandSendState::Complete);
                const auto& reply = rig.brain.commandResponse();
                CHECK(reply.accepted && reply.source == r.source && reply.sequence == r.sequence &&
                      !std::strcmp(reply.commandId, r.commandId) && !std::strcmp(reply.reason, "accepted"));
                CHECK(commands == 1 && !stops && delivered.size() == 2 && rig.device->hardware.generated == 1);
                CHECK(rig.toMotion.count(v4::Kind::Command) == 1 && rig.toBrain.count(v4::Kind::CommandResult) == 1);
                CHECK(rig.device->store.state().localSequence == sequence && rig.device->store.state().localResult.accepted);
                CHECK(sameProductRequest(rig.device->store.state().localResult.request, r));
                CHECK(sameMotionContextBarrier(rig.device->store.state().context, motionBefore.context));
                CHECK(sameBrainState(rig.store->state(), brainBefore) && nvs::io.disk.at("brainstate") == disk.at("brainstate"));
                CHECK(nvs::count(Op::Set) == sets + 1 && nvs::count(Op::Commit) == commits + 1);
                if (operation == ProductCommand::Initialize)
                    CHECK(rig.device->executor.starts == 1 && !rig.device->executor.stops);
                else CHECK(rig.device->product.ownsMotion() && !rig.device->executor.starts && rig.device->executor.stops == 1);
            }, true);
}

void production() {
    for (auto operation : {ProductCommand::Initialize, ProductCommand::Clean})
        scenario("real context conflict does not gate explicit Initialize/Clean / " +
                 std::to_string(unsigned(operation)), [=] {
            Integration rig(false, operation == ProductCommand::Clean); rig.confirm();
            auto conflict = context(); ++conflict.waterMl;
            CHECK(!rig.sync->receive(conflict)); rig.sync->poll(rig.now); CHECK(!rig.sync->canPrepare());
            rig.available(); const auto r = request(context(), 1, operation);
            CHECK(rig.store->reserveLocal(r) == BrainWrite::Stored);
            CommandMessage command; command.request = r; command.remainingTtlMs = 5000;
            CHECK(rig.brain.requestCommand(command, rig.now));
            for (unsigned i = 0; i < 1000 && rig.brain.commandSendState() == CommandSendState::Pending; ++i) rig.step(false);
            CHECK(rig.brain.commandSendState() == CommandSendState::Complete && rig.brain.commandResponse().accepted);
            CHECK(commands == 1 && !stops && !rig.sync->canPrepare());
            CHECK(sameProductRequest(rig.store->state().pendingRequest, r));
            CHECK(rig.device->store.state().localSequence == 1);
            if (operation == ProductCommand::Initialize) CHECK(rig.device->executor.starts == 1);
            else CHECK(rig.device->product.ownsMotion() && !rig.device->executor.starts);
        }, true);
    for (auto op : {Op::Set, Op::Commit}) for (bool apply : {false, true})
        scenario("real Motion storage failure is not proof; reboot checks durable bytes / " +
                 std::to_string(unsigned(op)) + "/" + std::to_string(apply), [=] {
            Integration rig; rig.confirm();
            bool injected = false;
            nvs::io.before = [&](const nvs::Call& call) {
                if (!injected && call.name == "productstate" && call.op == op) {
                    injected = true; nvs::fail(op, call.occurrence, ESP_FAIL, apply);
                }
            };
            CHECK(rig.sync->receive(context(11))); rig.await(ContextStatus::StorageFault);
            CHECK(injected && rig.device->store.faulted() && !rig.sync->canPrepare());
            CHECK(rig.store->ready() && rig.store->state().context.profileVersion == 11);
            nvs::io.before = {}; nvs::verifyFaults();
            const auto& bytes = nvs::io.disk.at("productstate").at("record").bytes;
            MotionState saved; CHECK(decodeMotionState(bytes.data(), bytes.size(), saved));
            CHECK(saved.context.profileVersion == (apply ? 11u : 10u));
            rig.reboot(false, true);
            rig.await(apply ? ContextStatus::Unchanged : ContextStatus::Stored);
            rig.noActions(); rig.fullWire(context(11));
        }, true);
    scenario("real Motion higher barrier returns Conflict, higher Brain proof recovers", [] {
        Integration rig;
        CHECK(rig.device->store.saveContext(context(12)) == MotionWrite::Stored);
        rig.await(ContextStatus::Conflict); CHECK(!rig.sync->canPrepare());
        CHECK(rig.sync->receive(context(13))); rig.await(ContextStatus::Stored);
        rig.noActions(); rig.fullWire(context(13));
    }, true);
    scenario("real tombstone retains monotonic barrier; only higher active context restores", [] {
        Integration rig; rig.confirm(); CHECK(rig.sync->receive(context(11, true)));
        rig.await(ContextStatus::Stored, true);
        const auto calls = nvs::io.calls.size();
        CHECK(!rig.sync->receive(context(10)) && !rig.sync->canPrepare());
        CHECK(nvs::io.calls.size() == calls && rig.device->store.state().context.cleared);
        CHECK(rig.sync->receive(context(12, false, true)) && !rig.sync->canPrepare());
        rig.await(ContextStatus::Stored); rig.noActions(); rig.fullWire(context(12, false, true));
    }, true);
    scenario("real running Prepare snapshot is isolated from higher cache and conflict", [] {
        Integration rig(false, true); rig.confirm(); rig.available();
        const auto r = request(context());
        CHECK(rig.store->reserveLocal(r) == BrainWrite::Stored);
        CommandMessage command; command.request = r; command.remainingTtlMs = 5000;
        CHECK(rig.brain.requestCommand(command, rig.now));
        for (unsigned i = 0; i < 1000 && rig.brain.commandSendState() == CommandSendState::Pending; ++i) rig.step(false);
        CHECK(rig.brain.commandSendState() == CommandSendState::Complete && rig.brain.commandResponse().accepted);
        CHECK(rig.device->runtime.active() && rig.device->product.active() && commands == 1);
        const auto before = rig.store->state(); const auto motionDisk = nvs::io.disk.at("productstate");
        const auto run = rig.device->product.activeRun(); const auto contexts = delivered.size();
        CHECK(rig.sync->receive(context(11)) && !rig.sync->canPrepare());
        auto conflict = context(11); ++conflict.waterMl;
        CHECK(!rig.sync->receive(conflict)); rig.run(1200, true, false, true);
        CHECK(rig.store->state().context.profileVersion == 11 && !rig.sync->canPrepare());
        pendingUnchanged(before, rig.store->state()); pendingUnchanged(before, diskBrain());
        CHECK(nvs::io.disk.at("productstate") == motionDisk && delivered.size() == contexts);
        const auto& running = rig.device->product.activeRun();
        CHECK(running.commandId == run.commandId && running.babyId == run.babyId && running.profileVersion == 10);
        CHECK(running.recipe.waterMl == run.recipe.waterMl && running.recipe.temperatureC == run.recipe.temperatureC &&
              running.recipe.powderGPer100Ml == run.recipe.powderGPer100Ml);
        CHECK(commands == 1 && !stops && !rig.device->executor.stops);
    }, true);
    for (unsigned mutation = 0; mutation < 7; ++mutation)
        scenario("real core rejects nonmatching content/request/boot reply / " + std::to_string(mutation), [=] {
            Integration rig; rig.sync->poll(rig.now);
            auto reply = rig.brain.contextResponse(); reply.status = ContextStatus::Stored;
            if (mutation == 0) ++reply.replyTo;
            if (mutation == 1) ++reply.profileVersion;
            if (mutation == 2) reply.cleared = true;
            if (mutation == 3) std::strcpy(reply.deviceId, "wrong-device");
            if (mutation == 4) reply.digest[0] ^= 1;
            v4::Message wire; CHECK(encodeContextResult(reply, wire));
            rig.inject(wire, mutation == 5, mutation == 6);
            CHECK(rig.brain.contextSendState() == ContextSendState::Pending && !rig.sync->canPrepare());
            rig.confirm(); rig.noActions(); rig.fullWire(context());
        }, true);
    scenario("real LINK_ACK and matching STATUS version are never save proof", [] {
        Integration rig; rig.sync->poll(rig.now);
        v4::Message ack; ack.kind = v4::Kind::LinkAck;
        const auto id = rig.brain.contextResponse().replyTo;
        const int length = std::snprintf(reinterpret_cast<char*>(ack.payload), sizeof(ack.payload),
                                        "{\"message_id\":%u}", id);
        CHECK(length > 0); ack.length = uint16_t(length); rig.inject(ack);
        Status status; status.sampleUptimeMs = rig.now; status.contextVersion = 10;
        status.snapshot.startEnabled = true; status.feedingContextConfigured = true;
        std::strcpy(status.babyId, context().babyId);
        v4::Message wire; CHECK(encodeStatus(status, wire)); rig.inject(wire);
        rig.sync->poll(rig.now);
        CHECK(rig.brain.freshStatus(rig.now) && rig.brain.peerStatus().contextVersion == 10);
        CHECK(rig.brain.contextSendState() == ContextSendState::Pending && !rig.sync->canPrepare());
        CHECK(delivered.empty()); rig.confirm(); rig.noActions();
    }, true);
    for (bool incoming : {false, true}) for (bool tombstone : {false, true})
        scenario("real conflict isolates new Prepare until higher precise proof / " + std::to_string(incoming) + "/" +
                 std::to_string(tombstone), [=] {
            Integration rig; rig.confirm(); const auto a = context(incoming ? 11 : 10);
            if (incoming) CHECK(rig.sync->receive(a));
            auto b = a;
            if (tombstone) b = context(a.profileVersion, true); else ++b.waterMl;
            CHECK(!rig.sync->receive(b) && !rig.sync->canPrepare());
            CHECK(rig.sync->receive(a) && !rig.sync->canPrepare());
            CHECK(!rig.sync->receive(context(a.profileVersion - 1)));
            const auto transfers = delivered.size(); rig.run(1200);
            CHECK(delivered.size() == transfers && !rig.sync->canPrepare());
            CHECK(sameProductContext(rig.store->state().context, a));
            rig.device->hardware.owner = "debug_busy";
            const auto c = context(a.profileVersion + 1);
            CHECK(rig.sync->receive(c)); rig.await(ContextStatus::Busy);
            CHECK(!rig.sync->canPrepare()); rig.device->hardware.owner = nullptr;
            rig.await(ContextStatus::Stored); rig.noActions(); rig.fullWire(c);
        }, true);
    for (bool timeout : {false, true})
        scenario("real backpressured Stop pauses Flash until receipt/timeout / " + std::to_string(timeout), [=] {
            Integration rig; rig.confirm(); rig.available();
            v4::StopRequest stop; stop.scope = v4::StopScope::Idle;
            CHECK(rig.brain.requestStop(stop, rig.now)); rig.toMotion.writeLimit = 1;
            rig.toBrain.drop = timeout;
            const auto old = rig.store->state(); const auto disk = nvs::io.disk;
            const auto calls = nvs::io.calls.size(), contexts = delivered.size();
            CHECK(rig.sync->receive(context(11)) && !rig.sync->canPrepare());
            for (unsigned i = 0; i < 1600 && rig.brain.stopSendState() == StopSendState::Pending; ++i) {
                rig.step();
                CHECK(nvs::io.calls.size() == calls && nvs::io.disk == disk);
                CHECK(delivered.size() == contexts && sameBrainState(rig.store->state(), old));
            }
            CHECK(rig.brain.stopSendState() == (timeout ? StopSendState::TimedOut : StopSendState::Received));
            CHECK(stops == 1); rig.toBrain.drop = false; rig.toMotion.writeLimit = 7;
            // Neither receipt nor timeout creates stationary evidence for Motion.
            rig.device->executor.sample.stationary = false;
            rig.await(ContextStatus::Busy);
            CHECK(rig.store->state().context.profileVersion == 11 && !rig.sync->canPrepare());
            CHECK(rig.device->store.state().context.profileVersion == 10);
            rig.device->executor.sample = {true, true, false, 0};
            rig.await(ContextStatus::Stored); rig.noActions();
        }, true);
    for (bool clear : {false, true}) scenario("real cores + Runtime offline full cache/tombstone / " + std::to_string(clear), [=] {
        Integration rig; const auto newer = context(11, clear, !clear);
        const auto calls = nvs::io.calls.size();
        CHECK(rig.sync->receive(newer) && nvs::io.calls.size() == calls);
        rig.await(ContextStatus::Stored, clear); rig.fullWire(newer); rig.noActions();
        const auto transfers = delivered.size(), steadyCalls = nvs::io.calls.size();
        rig.run(2500);
        CHECK(delivered.size() == transfers && nvs::io.calls.size() == steadyCalls);
        CHECK(rig.sync->canPrepare() == !clear);
        CHECK(sameProductContext(diskBrain().context, newer));
        CHECK(rig.device->store.state().context.profileVersion == newer.profileVersion);
        CHECK(rig.device->product.hasContext() == !clear);
        if (!clear) {
            CHECK(rig.device->product.context().babyName == newer.babyName);
            CHECK(rig.device->product.context().formulaBrand == newer.formulaBrand);
            CHECK(rig.device->product.context().recipe.powderGPer100Ml == newer.powderGPer100Ml);
            CHECK(!rig.device->product.canStart()); // Config proof is not mechanical Ready.
        }
    }, true);
    for (unsigned boards = 1; boards < 4; ++boards) for (bool clear : {false, true})
        scenario("real persisted proof revalidated after board reboot / " + std::to_string(boards) + "/" + std::to_string(clear), [=] {
            Integration rig(clear); rig.confirm(); const auto disk = nvs::io.disk;
            rig.reboot(boards & 1, boards & 2);
            const auto commits = nvs::count(Op::Commit), sets = nvs::count(Op::Set);
            rig.confirm(); rig.noActions(); rig.fullWire(context(10, clear));
            CHECK(nvs::io.disk == disk && nvs::count(Op::Commit) == commits && nvs::count(Op::Set) == sets);
            CHECK(rig.device->product.context().profileVersion == 10);
            CHECK(rig.device->product.hasContext() == !clear);
        }, true);
    scenario("real lost reply retries same saved config, never an action", [] {
        Integration rig; const auto c = context(11); CHECK(rig.sync->receive(c));
        rig.toBrain.drop = true;
        for (unsigned i = 0; i < 1000 && delivered.empty(); ++i) rig.step();
        CHECK(delivered.size() == 1 && rig.device->store.state().context.profileVersion == 11);
        for (unsigned i = 0; i < 1200 && rig.brain.contextSendState() == ContextSendState::Pending; ++i) rig.step(false);
        CHECK(rig.brain.contextSendState() == ContextSendState::TimedOut && !rig.sync->canPrepare());
        rig.sync->poll(rig.now); rig.toBrain.drop = false;
        const auto commits = nvs::count(Op::Commit); rig.await(ContextStatus::Unchanged);
        CHECK(delivered.size() == 2 && sameProductContext(delivered[0], delivered[1]));
        CHECK(nvs::count(Op::Commit) == commits); rig.noActions(); rig.fullWire(c);
    }, true);
    for (unsigned busy = 0; busy < 2; ++busy) scenario("real Motion busy is observation not persistence / " + std::to_string(busy), [=] {
        Integration rig; rig.confirm(); const auto oldMotion = nvs::io.disk.at("productstate");
        if (busy == 0) rig.device->hardware.owner = "debug_busy";
        else rig.device->executor.sample.stationary = false;
        CHECK(rig.sync->receive(context(11))); rig.await(ContextStatus::Busy, true);
        CHECK(nvs::io.disk.at("productstate") == oldMotion && !rig.sync->canPrepare());
        rig.device->hardware.owner = nullptr; rig.device->executor.sample = {true, true, false, 0};
        rig.await(ContextStatus::Stored); rig.noActions();
    }, true);
    for (unsigned phase = 0; phase < 4; ++phase)
        scenario("real Stop preempts unsent/partial/fragment/applied config / " + std::to_string(phase), [=] {
            Integration rig; const auto c = context(11, false, true);
            CHECK(rig.sync->receive(c)); rig.sync->poll(rig.now);
            CHECK(rig.brain.contextSendState() == ContextSendState::Pending);
            if (phase == 1) { rig.toMotion.writeLimit = 1; rig.run(10, false); CHECK(delivered.empty()); }
            if (phase == 2) {
                for (unsigned i = 0; i < 800 && !rig.toMotion.fragments(v4::Kind::Context); ++i) rig.step(false);
                CHECK(rig.toMotion.fragments(v4::Kind::Context) && delivered.empty());
            }
            if (phase == 3) {
                rig.toBrain.drop = true;
                for (unsigned i = 0; i < 800 && delivered.empty(); ++i) rig.step(false);
                CHECK(delivered.size() == 1 && rig.device->store.state().context.profileVersion == 11);
            }
            v4::StopRequest stop; stop.scope = v4::StopScope::Idle;
            CHECK(rig.brain.requestStop(stop, rig.now));
            CHECK(rig.brain.contextSendState() == ContextSendState::Cancelled);
            rig.sync->yield(rig.now); CHECK(!rig.sync->canPrepare());
            rig.toMotion.writeLimit = 7; rig.toBrain.drop = false;
            for (unsigned i = 0; i < 700 && rig.brain.stopSendState() == StopSendState::Pending; ++i) rig.step(false);
            CHECK(rig.brain.stopSendState() == StopSendState::Received && stops == 1);
            CHECK(sameProductContext(rig.store->state().context, c));
            rig.await(phase == 3 ? ContextStatus::Unchanged : ContextStatus::Stored);
            rig.noActions(); rig.fullWire(c);
        }, true);
    scenario("real maintenance yields transfer but preserves both cache records", [] {
        Integration rig; CHECK(rig.sync->receive(context(11))); rig.sync->poll(rig.now);
        const auto brainDisk = nvs::io.disk.at("brainstate");
        const auto motionDisk = nvs::io.disk.at("productstate");
        rig.run(1500, true, true);
        CHECK(!rig.sync->canPrepare() && delivered.empty());
        CHECK(nvs::io.disk.at("brainstate") == brainDisk && nvs::io.disk.at("productstate") == motionDisk);
        rig.await(ContextStatus::Stored); rig.noActions();
    }, true);
    scenario("real loss retires proof; same boot reconnect requires application reply", [] {
        Integration rig; rig.confirm(); const auto disk = nvs::io.disk;
        rig.toBrain.drop = true; rig.run(2500, false);
        CHECK(!rig.brain.connected(rig.now) && !rig.sync->canPrepare());
        rig.toBrain.drop = false; rig.run(1000, false);
        CHECK(rig.brain.connected(rig.now) && !rig.sync->canPrepare());
        rig.confirm(); CHECK(nvs::io.disk == disk); rig.noActions();
    }, true);
    scenario("real clock rollover preserves request and retry proof", [] {
        Integration rig; rig.now = UINT32_MAX - 1200;
        // Re-establish time-local heartbeats, not arbitrary stale peer evidence.
        CHECK(rig.brain.begin(pairing(), ++rig.brainBoot)); ++rig.motionBoot; rig.beginMotion();
        rig.run(1000, false); rig.available();
        CHECK(rig.sync->receive(context(11, false, true))); rig.await(ContextStatus::Stored);
        CHECK(rig.now < 5000); rig.noActions(); rig.fullWire(context(11, false, true));
    }, true);
}
} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc == 2 ? argv[1] : "all";
    const struct { const char* name; void (*run)(); } groups[] = {
        {"cache", cacheTests}, {"conflicts", conflicts}, {"replies", replies}, {"scheduling", scheduling},
        {"storage", storage}, {"production", production}, {"preemption", preemption},
        {"preemption-rejections", preemptionRejections}};
    bool found = selected == "all";
    for (const auto& group : groups) if (selected == "all" || selected == group.name) { found = true; group.run(); }
    if (!found || argc > 2) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("Brain context sync: %u scenarios (%u real dual-core/Runtime), %u failures; "
                "complete observed CONTEXT=%u CONTEXT_RESULT=%u; SHA major %d\n",
                scenarios, integrationScenarios, failures, wireContexts, wireResults, MBEDTLS_VERSION_MAJOR);
    return failures ? 1 : 0;
}
