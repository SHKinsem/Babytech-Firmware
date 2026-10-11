#include "MotionStateStore.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"
#include "RecordBytes.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using fake::Bytes;
using fake::Op;
using fake::io;

namespace {
unsigned scenarios = 0, failures = 0, groupFailures = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr const char* execution = "123456789abcdef0123456789abcdef0";
constexpr const char* otherExecution = "223456789abcdef0123456789abcdef0";
constexpr const char* idleExecution = "00000000000000000000000000000000";
static_assert(kMotionStateMaxSize <= 4096, "contract record budget");

v4::Pairing pairing() {
    v4::Pairing p{};
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, "Babytech_01-test");
    std::strcpy(p.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(p.localPhysicalId, "012345abcdef");
    std::strcpy(p.peerPhysicalId, "fedcba987654");
    return p;
}
ProductContext context(uint32_t version = 10, bool cleared = false) {
    ProductContext c;
    std::strcpy(c.deviceId, pairing().deviceId);
    c.profileVersion = version;
    c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, "baby-original");
        std::strcpy(c.babyName, "Full baby name");
        std::strcpy(c.formulaBrand, "Full formula brand");
        c.waterMl = 120;
        c.temperatureC = 40;
        c.powderGPer100Ml = 13.5f;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t sequence = 8, ProductCommand command = ProductCommand::Prepare,
                       v4::Source source = v4::Source::LocalTouch) {
    ProductRequest r;
    r.sequence = sequence;
    r.command = command;
    r.source = source;
    std::strcpy(r.deviceId, pairing().deviceId);
    if (source == v4::Source::LocalTouch) {
        auto brain = pairing();
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        CHECK(makeLocalCommandId(brain, sequence, r.commandId));
    } else std::snprintf(r.commandId, sizeof(r.commandId), "cloud-%llu",
                         static_cast<unsigned long long>(sequence));
    if (command == ProductCommand::Prepare) {
        const auto c = context();
        std::strcpy(r.babyId, c.babyId);
        r.profileVersion = c.profileVersion;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = c.powderGPer100Ml;
    } else if (command == ProductCommand::SetTargetTemp) r.temperatureC = 45;
    CHECK(validProductRequest(r));
    return r;
}
MotionResult result(const ProductRequest& r, bool accepted = false) {
    MotionResult value;
    value.kind = MotionResultKind::Ordinary;
    value.request = r;
    CHECK(requestDigest(r, value.digest));
    value.accepted = accepted;
    std::strcpy(value.reason, accepted ? "accepted" : "busy");
    return value;
}
MotionState state(bool pending = false, ProductCommand command = ProductCommand::Prepare) {
    MotionState s;
    s.pairing = pairing();
    CHECK(makeMotionContextBarrier(context(), s.context));
    s.localSequence = pending ? 8 : 7;
    s.cloudSequence = 5;
    s.localResult = result(request(s.localSequence, pending ? command : ProductCommand::Clean), pending);
    s.cloudResult = result(request(5, ProductCommand::Clean, v4::Source::CloudCommand));
    if (pending) {
        s.slot.kind = MotionSlotKind::Intent;
        s.slot.request = s.localResult.request;
        std::memcpy(s.slot.digest, s.localResult.digest, sizeof(s.slot.digest));
        std::strcpy(s.slot.executionId, execution);
        if (command == ProductCommand::Prepare) {
            std::strcpy(s.slot.eventId, "evt-0123456789abcdef0123456789abcdef-l-8");
            s.slot.targetPowderG = 24.3f;
        }
    }
    CHECK(validMotionState(s));
    return s;
}
Bytes encode(const MotionState& s) {
    std::array<uint8_t, kMotionStateMaxSize> bytes{};
    const auto n = encodeMotionState(s, bytes.data(), bytes.size());
    CHECK(n);
    return Bytes(bytes.begin(), bytes.begin() + n);
}
Bytes raw(const MotionState& s) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&s);
    return Bytes(bytes, bytes + sizeof(s));
}
Bytes slotBytes(const MotionState& s) {
    // Compare persisted fields, never compiler padding changed by decode/copy.
    std::array<uint8_t, kMotionSlotMaxSize> bytes{};
    detail::ByteWriter writer(bytes.data(), bytes.size());
    const auto& slot = s.slot;
    CHECK(writer.integer(uint8_t(slot.kind), 1));
    if (slot.kind != MotionSlotKind::Empty) {
        uint8_t requestBytes[kRequestIdentityMaxSize];
        const auto n = encodeRequestIdentity(slot.request, requestBytes, sizeof(requestBytes));
        CHECK(n && writer.integer(n, 2) && writer.raw(requestBytes, n));
        uint32_t powderBits;
        std::memcpy(&powderBits, &slot.targetPowderG, sizeof(powderBits));
        CHECK(writer.raw(slot.digest, sizeof(slot.digest)) && writer.text(slot.executionId) &&
              writer.text(slot.eventId) && writer.integer(powderBits, 4) &&
              writer.integer(slot.completed, 1) && writer.integer(slot.uptimeMs, 4) &&
              writer.text(slot.reason) && writer.text(slot.errorCode));
    }
    return Bytes(bytes.begin(), bytes.begin() + writer.size());
}
void seed(const Bytes& bytes, fake::Type type = fake::Type::Blob) {
    io.disk["productstate"]["record"] = {bytes, type};
}
void seed(const MotionState& s) { seed(encode(s)); }
Bytes durable() {
    const auto space = io.disk.find("productstate");
    if (space == io.disk.end() || !space->second.count("record")) return {};
    return space->second.at("record").bytes;
}
fake::Database protectedData() {
    auto disk = io.disk;
    auto found = disk.find("productstate");
    if (found != disk.end()) {
        found->second.erase("record");
        if (found->second.empty()) disk.erase(found);
    }
    return disk;
}
void scenario(const std::string& name, const std::function<void()>& run, bool space = true) {
    fake::reset();
    fake_product_crypto::reset();
    for (const char* ns : {"productpair", "brainstate", "productctx", "outbox", "wifi-cfg",
                           "actuatorcfg", "sensorcfg"}) {
        io.disk[ns]["record"] = {{0, 0xff, 0x7f, 1}, fake::Type::Blob};
        io.disk[ns]["payload"] = {{'o', 'l', 'd', 0}, fake::Type::String};
    }
    if (space) io.disk["productstate"]["unrelated"] = {{1, 2, 3}, fake::Type::U32};
    const auto protectedBefore = protectedData();
    ++scenarios;
    try {
        run();
        fake::verifyFaults();
        CHECK(io.handles.empty());
        CHECK(protectedData() == protectedBefore);
        CHECK(!fake::count(Op::Erase) && !fake::count(Op::Init));
        for (const auto& call : io.calls) {
            CHECK(call.name == "productstate");
            CHECK(call.key.empty() || call.key == "record");
        }
    } catch (const std::exception& error) {
        ++failures;
        if (++groupFailures <= 3) std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        // A failed scenario remains failed; isolate later cases to report groups.
        io.before = {};
        io.handles.clear();
    }
}
void noWrites() {
    CHECK(!fake::count(Op::OpenRW) && !fake::count(Op::Set) && !fake::count(Op::Commit));
}
void auditCalls() {
    CHECK(io.handles.empty());
    CHECK(!fake::count(Op::Erase) && !fake::count(Op::Init));
    for (const auto& call : io.calls) {
        CHECK(call.name == "productstate");
        CHECK(call.key.empty() || call.key == "record");
    }
}
void unchanged(MotionStateStore& store, const std::function<MotionWrite()>& action, MotionWrite expected) {
    const auto bytes = encode(store.state());
    const auto disk = io.disk;
    const auto sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
    CHECK(action() == expected);
    CHECK(store.ready() && !store.faulted());
    CHECK(encode(store.state()) == bytes && io.disk == disk);
    CHECK(fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits);
}
void latched(MotionStateStore& store, const Bytes& before) {
    CHECK(store.faulted() && !store.ready());
    CHECK(raw(store.state()) == before);
    const auto calls = io.calls.size();
    const auto disk = io.disk;
    const auto loaded = store.load(pairing());
    CHECK(loaded != MotionLoad::Ready && loaded != MotionLoad::Missing);
    CHECK(store.load(v4::Pairing{}) == loaded);
    CHECK(store.installInitial(pairing()) == MotionWrite::StorageFault);
    CHECK(store.saveContext(context(99)) == MotionWrite::StorageFault);
    CHECK(store.recordDecision(request(), true, "accepted", execution) == MotionWrite::StorageFault);
    CHECK(store.finishFeeding(execution, false, "reboot_during_feed", "", 12) == MotionWrite::StorageFault);
    CHECK(store.finishOperation(execution, MotionOutcome::Interrupted, true) == MotionWrite::StorageFault);
    CHECK(store.acknowledge("event", true, true) == MotionWrite::StorageFault);
    CHECK(store.recordCloudStop(99, "stop", execution, true, "accepted", true) == MotionWrite::StorageFault);
    CHECK(io.calls.size() == calls && io.disk == disk && raw(store.state()) == before);
}
void recover() {
    fake::verifyFaults();
    auditCalls();
    io.before = {};
    fake::reboot();
    fake_product_crypto::reset();
    const auto disk = io.disk;
    const auto bytes = durable();
    MotionStateStore fresh;
    CHECK(fresh.load(pairing()) == (bytes.empty() ? MotionLoad::Missing : MotionLoad::Ready));
    CHECK(!fresh.faulted());
    if (!bytes.empty()) {
        MotionState persisted;
        CHECK(decodeMotionState(bytes.data(), bytes.size(), persisted));
        CHECK(sameMotionState(fresh.state(), persisted));
        CHECK(fresh.state().slot.kind == persisted.slot.kind);
        CHECK(fresh.load(pairing()) == MotionLoad::Ready);
    }
    CHECK(io.disk == disk);
    noWrites(); // No dispatch/runtime linked: boot must not write/clear/resume intent.
}

void basics() {
    for (bool space : {false, true}) scenario("missing read-only and explicit blank install", [&] {
        MotionStateStore store;
        const auto disk = io.disk;
        CHECK(!store.ready() && !store.faulted());
        for (unsigned n = 0; n < 3; ++n) CHECK(store.load(pairing()) == MotionLoad::Missing);
        CHECK(store.recordDecision(request(), true, "accepted", execution) == MotionWrite::StorageFault);
        CHECK(store.saveContext(context()) == MotionWrite::StorageFault);
        CHECK(store.finishOperation(execution, MotionOutcome::Interrupted, true) == MotionWrite::StorageFault);
        CHECK(store.finishFeeding(execution, true, "", "", 1) == MotionWrite::StorageFault);
        CHECK(store.acknowledge("event", true, true) == MotionWrite::StorageFault);
        CHECK(store.recordCloudStop(1, "stop", execution, true, "accepted", true) == MotionWrite::StorageFault);
        CHECK(io.disk == disk);
        noWrites();
        CHECK(store.installInitial(pairing()) == MotionWrite::Stored);
        CHECK(store.ready() && !store.state().context.present);
        CHECK(!store.state().localSequence && !store.state().cloudSequence);
        CHECK(store.state().slot.kind == MotionSlotKind::Empty);
        unchanged(store, [&] { return store.installInitial(pairing()); }, MotionWrite::Unchanged);
        recover();
    }, space);
    for (bool cleared : {false, true}) scenario("import preserves full context digest/tombstone", [&] {
        const auto c = context(1234, cleared);
        MotionStateStore store;
        CHECK(store.installInitial(pairing(), &c) == MotionWrite::Stored);
        const auto& barrier = store.state().context;
        uint8_t digest[kProductDigestSize];
        CHECK(contextDigest(c, digest));
        CHECK(barrier.present && barrier.profileVersion == 1234 && barrier.cleared == cleared);
        CHECK(!std::memcmp(barrier.digest, digest, sizeof(digest)));
        unchanged(store, [&] { return store.installInitial(pairing(), &c); }, MotionWrite::Unchanged);
        auto other = c;
        other.profileVersion++;
        unchanged(store, [&] { return store.installInitial(pairing(), &other); }, MotionWrite::Conflict);
        auto identity = pairing();
        identity.epoch[0] = 'a';
        unchanged(store, [&] { return store.installInitial(identity, &c); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.installInitial(pairing()); }, MotionWrite::Conflict);
        recover();
    });
    for (unsigned field = 0; field < 5; ++field) scenario("wrong pairing cannot load or overwrite", [&] {
        seed(state());
        auto p = pairing();
        if (field == 0) p.role = v4::Role::Brain;
        if (field == 1) p.deviceId[0] = 'X';
        if (field == 2) p.epoch[0] = 'a';
        if (field == 3) p.localPhysicalId[0] = 'a';
        if (field == 4) p.peerPhysicalId[0] = 'a';
        MotionStateStore store;
        const auto before = raw(store.state());
        CHECK(store.load(p) == MotionLoad::IdentityMismatch);
        latched(store, before);
        MotionStateStore installer;
        CHECK(installer.installInitial(p) == (field == 0 ? MotionWrite::Invalid : MotionWrite::Conflict));
        noWrites();
    });
}

void sequences() {
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("independent watermarks; rejection persists; exact duplicate never dispatches", [&] {
            seed(state());
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            const auto r = request(9, ProductCommand::Clean, source);
            CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
            CHECK(store.state().localSequence == (source == v4::Source::LocalTouch ? 9 : 7));
            CHECK(store.state().cloudSequence == (source == v4::Source::CloudCommand ? 9 : 5));
            unchanged(store, [&] { return store.recordDecision(r, false, "busy"); }, MotionWrite::Unchanged);
            unchanged(store, [&] { return store.recordDecision(r, true, "accepted", execution); }, MotionWrite::Conflict);
            unchanged(store, [&] { return store.recordDecision(r, false, "not_ready"); }, MotionWrite::Conflict);
            auto conflict = r;
            conflict.command = ProductCommand::ResetError;
            unchanged(store, [&] { return store.recordDecision(conflict, false, "busy"); }, MotionWrite::Conflict);
            auto older = request(8, ProductCommand::Clean, source);
            unchanged(store, [&] { return store.recordDecision(older, false, "busy"); }, MotionWrite::Expired);
            auto next = request(10, ProductCommand::Clean, source);
            CHECK(store.recordDecision(next, true, "accepted", execution) == MotionWrite::Stored);
            CHECK(store.state().slot.kind == MotionSlotKind::Intent);
            unchanged(store, [&] { return store.recordDecision(next, true, "accepted", execution); }, MotionWrite::Unchanged);
            CHECK(store.finishOperation(execution, MotionOutcome::Succeeded, true) == MotionWrite::Stored);
            unchanged(store, [&] { return store.recordDecision(next, true, "accepted", execution); }, MotionWrite::Unchanged);
            CHECK(store.state().slot.kind == MotionSlotKind::Empty);
            recover();
        });
    scenario("busy is not a decision; final rejection consumes same request", [] {
        seed(state(true));
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto slot = slotBytes(store.state());
        const auto r = request(9);
        unchanged(store, [&] { return store.recordDecision(r, true, "accepted", otherExecution); }, MotionWrite::Busy);
        CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
        CHECK(slotBytes(store.state()) == slot && store.state().localSequence == 9);
        unchanged(store, [&] { return store.recordDecision(r, true, "accepted", otherExecution); }, MotionWrite::Conflict);
        recover();
    });
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        scenario("maximum sequence is stable and sources independent", [&] {
            seed(state());
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            auto r = request(v4::kMaxSequence, ProductCommand::Clean, source);
            CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
            unchanged(store, [&] { return store.recordDecision(r, false, "busy"); }, MotionWrite::Unchanged);
            r.sequence++;
            unchanged(store, [&] { return store.recordDecision(r, false, "busy"); }, MotionWrite::Invalid);
            r = request(10, ProductCommand::Clean, source == v4::Source::LocalTouch ?
                        v4::Source::CloudCommand : v4::Source::LocalTouch);
            CHECK(store.recordDecision(r, false, "busy") == MotionWrite::Stored);
        });
}

void contexts() {
    for (unsigned kind = 0; kind < 5; ++kind) scenario("prepare requires matching current barrier", [&] {
        auto s = state();
        if (kind == 0) s.context = MotionContextBarrier{};
        if (kind == 1) CHECK(makeMotionContextBarrier(context(10, true), s.context));
        seed(s);
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        auto r = request();
        if (kind == 2) r.profileVersion++;
        if (kind == 3) std::strcpy(r.babyId, "another-baby");
        if (kind == 4) r.powderGPer100Ml += 1;
        unchanged(store, [&] { return store.recordDecision(r, true, "accepted", execution); }, MotionWrite::ContextRequired);
        CHECK(store.recordDecision(r, false, "context_mismatch") == MotionWrite::Stored);
        CHECK(store.state().localSequence == 8 && store.state().slot.kind == MotionSlotKind::Empty);
    });
    scenario("frozen snapshot and event survive context advance, tombstone and later rejection", [] {
        seed(state());
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto r = request();
        CHECK(store.recordDecision(r, true, "accepted", execution) == MotionWrite::Stored);
        CHECK(sameProductRequest(r, store.state().slot.request));
        CHECK(!std::strcmp(store.state().slot.eventId, "evt-0123456789abcdef0123456789abcdef-l-8"));
        uint8_t digest[kProductDigestSize];
        CHECK(requestDigest(r, digest));
        CHECK(!std::memcmp(store.state().slot.digest, digest, sizeof(digest)));
        const auto slot = slotBytes(store.state());
        unchanged(store, [&] { return store.saveContext(context()); }, MotionWrite::Unchanged);
        auto conflict = context();
        std::strcpy(conflict.babyName, "Name changes count in full digest");
        unchanged(store, [&] { return store.saveContext(conflict); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.saveContext(context(9)); }, MotionWrite::Conflict);
        auto newer = context(11);
        std::strcpy(newer.babyId, "new-baby");
        newer.powderGPer100Ml = 16;
        CHECK(store.saveContext(newer) == MotionWrite::Stored);
        CHECK(slotBytes(store.state()) == slot);
        CHECK(store.saveContext(context(12, true)) == MotionWrite::Stored);
        CHECK(slotBytes(store.state()) == slot);
        CHECK(store.recordDecision(request(9), false, "busy") == MotionWrite::Stored);
        CHECK(slotBytes(store.state()) == slot);
        CHECK(store.recordCloudStop(11, "stop-11", execution, true, "accepted", true) == MotionWrite::Stored);
        CHECK(slotBytes(store.state()) == slot);
        recover();
    });
    scenario("target powder follows existing one-decimal product rounding", [] {
        auto c = context();
        c.powderGPer100Ml = 13.57f;
        MotionStateStore store;
        CHECK(store.installInitial(pairing(), &c) == MotionWrite::Stored);
        auto r = request();
        r.waterMl = 123;
        r.powderGPer100Ml = c.powderGPer100Ml;
        CHECK(store.recordDecision(r, true, "accepted", execution) == MotionWrite::Stored);
        CHECK(store.state().slot.targetPowderG == 16.7f);
        recover();
    });
    scenario("cloud event ID uses separate source and frozen request", [] {
        seed(state());
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        auto r = request(8, ProductCommand::Prepare, v4::Source::CloudCommand);
        CHECK(store.recordDecision(r, true, "accepted", execution) == MotionWrite::Stored);
        CHECK(!std::strcmp(store.state().slot.eventId, "evt-0123456789abcdef0123456789abcdef-c-8"));
        CHECK(store.recordCloudStop(9, "stop-9", execution, true, "accepted", true) == MotionWrite::Stored);
        CHECK(sameProductRequest(store.state().slot.request, r));
        recover();
    });
}

void terminals() {
    for (bool completed : {false, true}) scenario("terminal immutable, receipt exact and stationary, keeps watermarks", [&] {
        seed(state(true));
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto before = store.state();
        const std::string event = before.slot.eventId;
        unchanged(store, [&] { return store.acknowledge(event.c_str(), completed, true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.finishFeeding(otherExecution, completed, "", "", 99); }, MotionWrite::Conflict);
        const char* reason = completed ? "" : "reboot_during_feed";
        const char* error = completed ? "" : "stop_unconfirmed";
        CHECK(store.finishFeeding(execution, completed, reason, error, 99) == MotionWrite::Stored);
        CHECK(store.state().slot.kind == MotionSlotKind::Terminal);
        CHECK(store.state().slot.completed == completed && store.state().slot.uptimeMs == 99);
        CHECK(sameProductRequest(store.state().slot.request, before.slot.request));
        CHECK(!std::strcmp(store.state().slot.eventId, event.c_str()));
        CHECK(store.state().slot.targetPowderG == before.slot.targetPowderG);
        unchanged(store, [&] { return store.finishFeeding(execution, completed, reason, error, 99); }, MotionWrite::Unchanged);
        unchanged(store, [&] { return store.finishFeeding(execution, completed, reason, error, 100); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.finishFeeding(execution, !completed, reason, error, 99); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.finishFeeding(execution, completed, "other", error, 99); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.finishFeeding(execution, completed, reason, "other", 99); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.acknowledge(event.c_str(), completed, false); }, MotionWrite::Busy);
        unchanged(store, [&] { return store.acknowledge(event.c_str(), !completed, true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.acknowledge((event + "0").c_str(), completed, true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.finishOperation(execution, MotionOutcome::Succeeded, true); }, MotionWrite::Conflict);
        CHECK(store.saveContext(context(11, true)) == MotionWrite::Stored);
        CHECK(store.recordDecision(request(10), false, "busy") == MotionWrite::Stored);
        CHECK(store.recordCloudStop(12, "stop-12", execution, true, "accepted", true) == MotionWrite::Stored);
        auto expected = store.state();
        expected.slot = MotionExecutionSlot{};
        CHECK(store.acknowledge(event.c_str(), completed, true) == MotionWrite::Stored);
        CHECK(sameMotionState(store.state(), expected));
        unchanged(store, [&] { return store.acknowledge(event.c_str(), completed, true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.recordDecision(request(), true, "accepted", execution); }, MotionWrite::Expired);
        recover();
    });
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean})
        for (auto outcome : {MotionOutcome::Succeeded, MotionOutcome::Interrupted, MotionOutcome::Failed})
            for (unsigned newer = 0; newer < 3; ++newer)
                scenario("nonfeeding completion/reboot recovery never emits feeding event", [&] {
                    auto s = state(true, command);
                    if (newer == 2 && command == ProductCommand::Clean) {
                        s.slot.request = request(8, command, v4::Source::CloudCommand);
                        CHECK(requestDigest(s.slot.request, s.slot.digest));
                        s.cloudSequence = 8;
                        s.cloudResult = result(s.slot.request, true);
                        s.localSequence = 7;
                        s.localResult = result(request(7, ProductCommand::Clean));
                    }
                    seed(s);
                    recover();
                    MotionStateStore store;
                    CHECK(store.load(pairing()) == MotionLoad::Ready);
                    CHECK(!store.state().slot.eventId[0]);
                    unchanged(store, [&] { return store.finishFeeding(execution, true, "", "", 99); }, MotionWrite::Conflict);
                    unchanged(store, [&] { return store.finishOperation(execution, outcome, false); }, MotionWrite::Busy);
                    unchanged(store, [&] { return store.finishOperation(otherExecution, outcome, true); }, MotionWrite::Conflict);
                    if (newer == 1) CHECK(store.recordDecision(request(9), false, "busy") == MotionWrite::Stored);
                    if (newer == 2) CHECK(store.recordCloudStop(9, "stop-9", execution, true, "accepted", true) == MotionWrite::Stored);
                    auto expected = store.state();
                    auto& latest = s.slot.request.source == v4::Source::CloudCommand ? expected.cloudResult : expected.localResult;
                    if (latest.kind == MotionResultKind::Ordinary && sameProductRequest(latest.request, s.slot.request))
                        latest.outcome = outcome;
                    expected.slot = MotionExecutionSlot{};
                    CHECK(store.finishOperation(execution, outcome, true) == MotionWrite::Stored);
                    CHECK(sameMotionState(store.state(), expected));
                    CHECK(store.state().slot.kind == MotionSlotKind::Empty && !store.state().slot.eventId[0]);
                    recover();
                });
}

void stops() {
    scenario("stop records only after stationary and never lowers sequence or clears journal", [] {
        seed(state(true));
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto slot = slotBytes(store.state());
        unchanged(store, [&] { return store.recordCloudStop(9, "stop-9", execution, true, "accepted", false); }, MotionWrite::Busy);
        CHECK(store.recordCloudStop(9, "stop-9", execution, true, "accepted", true) == MotionWrite::Stored);
        CHECK(store.state().cloudSequence == 9 && store.state().localSequence == 8);
        CHECK(slotBytes(store.state()) == slot);
        unchanged(store, [&] { return store.recordCloudStop(9, "stop-9", execution, true, "accepted", true); }, MotionWrite::Unchanged);
        unchanged(store, [&] { return store.recordCloudStop(8, "stop-8", execution, true, "accepted", true); }, MotionWrite::Expired);
        unchanged(store, [&] { return store.recordCloudStop(9, "different", execution, true, "accepted", true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.recordCloudStop(9, "stop-9", otherExecution, true, "accepted", true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.recordCloudStop(9, "stop-9", execution, false, "stale_execution", true); }, MotionWrite::Conflict);
        unchanged(store, [&] { return store.recordDecision(request(9, ProductCommand::Clean, v4::Source::CloudCommand), false, "busy"); }, MotionWrite::Conflict);
        CHECK(store.recordCloudStop(10, "stop-10", execution, false, "stale_execution", true) == MotionWrite::Stored);
        CHECK(store.state().cloudSequence == 10 && slotBytes(store.state()) == slot);
        recover();
    });
    scenario("idle cloud stop stores empty observed-idle execution target", [] {
        seed(state());
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        CHECK(store.recordCloudStop(9, "idle-stop", "", true, "already_idle", true) == MotionWrite::Stored);
        CHECK(store.state().cloudSequence == 9 && store.state().slot.kind == MotionSlotKind::Empty);
        recover();
    });
}

void invalidInputs() {
    for (unsigned kind = 0; kind < 13; ++kind) scenario("invalid ordinary request never consumes sequence", [&] {
        seed(state());
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        auto r = request();
        bool accepted = true;
        const char* reason = "accepted";
        const char* id = execution;
        if (kind == 0) r.sequence = 0;
        if (kind == 1) r.deviceId[0] = 'X';
        if (kind == 2) r.commandId[0] = 'X';
        if (kind == 3) r.waterMl = 0;
        if (kind == 4) r.source = static_cast<v4::Source>(99);
        if (kind == 5) id = nullptr;
        if (kind == 6) id = idleExecution;
        if (kind == 7) id = "ABCDEF0123456789abcdef0123456789";
        if (kind == 8) id = "short";
        if (kind == 9) reason = "";
        if (kind == 10) { accepted = false; reason = "busy"; }
        if (kind == 11) { accepted = false; id = nullptr; }
        if (kind == 12) reason = "busy";
        unchanged(store, [&] { return store.recordDecision(r, accepted, reason, id); }, MotionWrite::Invalid);
    });
    for (unsigned kind = 0; kind < 5; ++kind) scenario("duplicate never creates intent from caller execution argument", [&] {
        seed(state(true));
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const char* id = kind == 0 ? nullptr : kind == 1 ? "" : kind == 2 ? idleExecution :
                         kind == 3 ? "short" : "123456789abcdef0123456789abcdef00";
        // Execution ID is required only for a NEW accepted motion request.
        unchanged(store, [&] { return store.recordDecision(request(), true, "accepted", id); }, MotionWrite::Unchanged);
    });
    for (const char* reason : {"bad reason", "bad-reason", "bad\nreason"})
        scenario("non-code decision reason rejected without poisoning healthy store", [&] {
            seed(state());
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            unchanged(store, [&] { return store.recordDecision(request(), false, reason); }, MotionWrite::Invalid);
        });
    scenario("terminal and operation argument validation", [] {
        seed(state(true));
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        unchanged(store, [&] { return store.finishFeeding(execution, true, "failure", "", 9); }, MotionWrite::Invalid);
        unchanged(store, [&] { return store.finishFeeding(execution, false, "", "", 9); }, MotionWrite::Invalid);
        unchanged(store, [&] { return store.finishFeeding(execution, false, "bad reason", "", 9); }, MotionWrite::Invalid);
        unchanged(store, [&] { return store.finishOperation(execution, MotionOutcome::None, true); }, MotionWrite::Invalid);
        unchanged(store, [&] { return store.acknowledge(nullptr, true, true); }, MotionWrite::Invalid);
    });
}

void reads() {
    const auto good = encode(state(true));
    for (size_t length = 0; length < good.size(); ++length) scenario("truncated record " + std::to_string(length), [&] {
        seed(Bytes(good.begin(), good.begin() + length));
        MotionStateStore store;
        const auto before = raw(store.state());
        CHECK(store.load(pairing()) == MotionLoad::Corrupt);
        latched(store, before);
        noWrites();
    });
    for (size_t byte = 0; byte < good.size(); ++byte) scenario("CRC/header corruption " + std::to_string(byte), [&] {
        auto bad = good;
        bad[byte] ^= 0x40;
        seed(bad);
        MotionStateStore store;
        const auto before = raw(store.state());
        CHECK(store.load(pairing()) == MotionLoad::Corrupt);
        latched(store, before);
        MotionStateStore installer;
        CHECK(installer.installInitial(pairing()) == MotionWrite::StorageFault);
        noWrites();
    });
    for (fake::Type type : {fake::Type::String, fake::Type::U32}) scenario("wrong NVS type", [&] {
        seed(good, type);
        MotionStateStore store;
        const auto before = raw(store.state());
        CHECK(store.load(pairing()) == MotionLoad::Corrupt);
        latched(store, before);
        noWrites();
    });
    for (Op op : {Op::OpenRO, Op::Query, Op::Read}) {
        for (esp_err_t error : {ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_TYPE_MISMATCH,
                               ESP_ERR_NVS_INVALID_LENGTH}) scenario("load SDK errors", [&] {
            seed(good);
            fake::fail(op, 1, error);
            MotionStateStore store;
            const auto before = raw(store.state());
            const auto expected = error == ESP_ERR_NVS_NOT_FOUND && op != Op::Read ? MotionLoad::Missing :
                error == ESP_ERR_NVS_TYPE_MISMATCH && op != Op::OpenRO ? MotionLoad::Corrupt : MotionLoad::IoError;
            CHECK(store.load(pairing()) == expected);
            CHECK(raw(store.state()) == before);
            if (expected != MotionLoad::Missing) latched(store, before);
            noWrites();
        });
        if (op == Op::OpenRO) continue;
        for (size_t length : {size_t(0), good.size() - 1, good.size() + 1, kMotionStateMaxSize + 1, SIZE_MAX})
            scenario("SDK success but inconsistent read length", [&] {
                seed(good);
                fake::fail(op, 1, ESP_OK, false, length);
                MotionStateStore store;
                const auto before = raw(store.state());
                const auto loaded = store.load(pairing());
                CHECK(loaded == MotionLoad::IoError || loaded == MotionLoad::Corrupt);
                latched(store, before);
                noWrites();
            });
    }
    scenario("key disappears during query/read", [] {
        seed(state(true));
        io.before = [](const fake::Call& call) { if (call.op == Op::Read) io.disk["productstate"].erase("record"); };
        MotionStateStore store;
        const auto before = raw(store.state());
        CHECK(store.load(pairing()) == MotionLoad::IoError);
        latched(store, before);
        noWrites();
    });
}

void operationIntegrity() {
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean})
        for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand}) {
            if (command == ProductCommand::Initialize && source == v4::Source::CloudCommand) continue;
            for (unsigned mismatch = 0; mismatch < 4; ++mismatch)
                scenario("CRC-valid unresolved operation must have matching intent", [&] {
                    auto s = state();
                    const auto r = request(9, command, source);
                    auto& recent = source == v4::Source::CloudCommand ? s.cloudResult : s.localResult;
                    (source == v4::Source::CloudCommand ? s.cloudSequence : s.localSequence) = 9;
                    recent = result(r, true);
                    // Start from a legal finished result; remove its outcome on
                    // the wire and recompute CRC so this exercises semantics.
                    recent.outcome = MotionOutcome::Succeeded;
                    if (mismatch) {
                        s.slot = state(true).slot;
                        if (source == v4::Source::CloudCommand) {
                            s.localSequence = 8;
                            s.localResult = result(s.slot.request, true);
                        }
                        if (mismatch == 2) {
                            s.slot.kind = MotionSlotKind::Terminal;
                            s.slot.completed = true;
                            s.slot.uptimeMs = 12;
                        }
                        if (mismatch == 3) {
                            // Same operation, older sequence: still not its intent.
                            s.slot = state(true, command).slot;
                            if (source == v4::Source::CloudCommand)
                                s.localResult = result(s.slot.request, true);
                        }
                    }
                    auto bytes = encode(s);
                    uint8_t identity[kRequestIdentityMaxSize];
                    const auto n = encodeRequestIdentity(r, identity, sizeof(identity));
                    CHECK(n);
                    const auto at = std::search(bytes.begin(), bytes.end(), identity, identity + n);
                    CHECK(at != bytes.end());
                    const size_t outcomeAt = size_t(at - bytes.begin()) + n + kProductDigestSize +
                        1 + 2 + std::strlen(recent.reason);
                    CHECK(bytes.at(outcomeAt) == uint8_t(MotionOutcome::Succeeded));
                    bytes[outcomeAt] = uint8_t(MotionOutcome::None);
                    detail::finishRecord(bytes.data(), bytes.size(), "BMS2");
                    recent.outcome = MotionOutcome::None;
                    CHECK(!validMotionState(s));
                    std::array<uint8_t, kMotionStateMaxSize> output{};
                    CHECK(!encodeMotionState(s, output.data(), output.size()));
                    seed(bytes);
                    MotionStateStore store;
                    const auto before = raw(store.state());
                    CHECK(store.load(pairing()) == MotionLoad::Corrupt);
                    latched(store, before);
                    noWrites();
                });
        }
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean})
        scenario("completion without matching operation cannot create result or event", [&] {
            seed(state());
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            unchanged(store, [&] { return store.finishOperation(execution, MotionOutcome::Failed, true); }, MotionWrite::Conflict);
            const auto r = request(9, command);
            CHECK(store.recordDecision(r, false, "not_ready") == MotionWrite::Stored);
            unchanged(store, [&] { return store.finishOperation(execution, MotionOutcome::Interrupted, true); }, MotionWrite::Conflict);
            CHECK(store.state().localResult.outcome == MotionOutcome::None);
            CHECK(store.state().slot.kind == MotionSlotKind::Empty);
            recover();
        });
}

void capacity() {
    scenario("maximum-width records use exact production bounds", [] {
        auto p = pairing();
        std::memset(p.deviceId, 'D', sizeof(p.deviceId) - 1);
        auto c = context(INT32_MAX);
        std::strcpy(c.deviceId, p.deviceId);
        std::memset(c.babyId, 'B', sizeof(c.babyId) - 1);
        c.powderGPer100Ml = 50;
        MotionStateStore store;
        CHECK(store.installInitial(p, &c) == MotionWrite::Stored);
        auto cloud = request(v4::kMaxSequence - 1, ProductCommand::Prepare, v4::Source::CloudCommand);
        std::strcpy(cloud.deviceId, p.deviceId);
        std::strcpy(cloud.babyId, c.babyId);
        std::memset(cloud.commandId, 'C', sizeof(cloud.commandId) - 1);
        cloud.profileVersion = c.profileVersion;
        cloud.powderGPer100Ml = c.powderGPer100Ml;
        cloud.waterMl = 500;
        cloud.temperatureC = 60;
        CHECK(store.recordDecision(cloud, true, "accepted", execution) == MotionWrite::Stored);
        auto local = cloud;
        local.source = v4::Source::LocalTouch;
        local.sequence = v4::kMaxSequence;
        auto brain = p;
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
        CHECK(makeLocalCommandId(brain, local.sequence, local.commandId));
        const std::string reason(64, 'r'), error(64, 'e');
        CHECK(store.recordDecision(local, false, reason.c_str()) == MotionWrite::Stored);
        cloud.sequence = v4::kMaxSequence;
        cloud.commandId[0] = 'N';
        CHECK(store.recordDecision(cloud, false, reason.c_str()) == MotionWrite::Stored);
        CHECK(store.finishFeeding(execution, false, reason.c_str(), error.c_str(), UINT32_MAX) == MotionWrite::Stored);
        const auto bytes = encode(store.state());
        CHECK(bytes.size() <= kMotionStateMaxSize && bytes.size() > 1491);
        for (size_t n : {size_t(0), bytes.size() - 1, bytes.size(), kMotionStateMaxSize}) {
            std::vector<uint8_t> buffer(n, 0xa5);
            const auto count = encodeMotionState(store.state(), buffer.data(), n);
            CHECK(count == (n < bytes.size() ? 0 : bytes.size()));
            if (!count) CHECK(std::all_of(buffer.begin(), buffer.end(), [](uint8_t b) { return b == 0xa5; }));
        }
        const auto disk = io.disk;
        fake::reboot();
        MotionStateStore fresh;
        CHECK(fresh.load(p) == MotionLoad::Ready);
        CHECK(encode(fresh.state()) == bytes && io.disk == disk);
        noWrites();
    });
}

enum class Action { Install, Import, ImportCleared, Context, ClearContext, LocalIntent,
    CloudIntent, Reject, Finish, FailFeeding, Operation, Acknowledge, Stop };
const std::vector<Action> actions{Action::Install, Action::Import, Action::ImportCleared,
    Action::Context, Action::ClearContext, Action::LocalIntent, Action::CloudIntent,
    Action::Reject, Action::Finish, Action::FailFeeding, Action::Operation, Action::Acknowledge, Action::Stop};
bool initial(Action action) { return action <= Action::ImportCleared; }
void prepare(Action action, MotionStateStore& store) {
    if (!initial(action)) {
        const bool pending = action != Action::LocalIntent && action != Action::CloudIntent;
        auto s = state(pending, action == Action::Operation ? ProductCommand::Clean : ProductCommand::Prepare);
        if (action == Action::Acknowledge) {
            s.slot.kind = MotionSlotKind::Terminal;
            s.slot.completed = true;
            s.slot.uptimeMs = 123;
        }
        seed(s);
        CHECK(store.load(pairing()) == MotionLoad::Ready);
    }
    io.calls.clear();
}
MotionWrite perform(Action action, MotionStateStore& store) {
    const auto imported = context(10, action == Action::ImportCleared);
    switch (action) {
    case Action::Install: return store.installInitial(pairing());
    case Action::Import: case Action::ImportCleared: return store.installInitial(pairing(), &imported);
    case Action::Context: return store.saveContext(context(11));
    case Action::ClearContext: return store.saveContext(context(11, true));
    case Action::LocalIntent: return store.recordDecision(request(), true, "accepted", execution);
    case Action::CloudIntent: return store.recordDecision(request(8, ProductCommand::Prepare, v4::Source::CloudCommand), true, "accepted", execution);
    case Action::Reject: return store.recordDecision(request(9), false, "busy");
    case Action::Finish: return store.finishFeeding(execution, true, "", "", 123);
    case Action::FailFeeding: return store.finishFeeding(execution, false, "reboot_during_feed", "stop_unconfirmed", 123);
    case Action::Operation: return store.finishOperation(execution, MotionOutcome::Interrupted, true);
    case Action::Acknowledge: return store.acknowledge("evt-0123456789abcdef0123456789abcdef-l-8", true, true);
    case Action::Stop: return store.recordCloudStop(9, "stop-9", execution, true, "accepted", true);
    }
    throw std::runtime_error("unknown action");
}
void writes() {
    for (Action action : actions) {
        std::vector<fake::Call> calls;
        Bytes expected;
        scenario("successful write readback barrier action " + std::to_string(int(action)), [&] {
            MotionStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            io.before = [&](const fake::Call&) { CHECK(raw(store.state()) == before); };
            CHECK(perform(action, store) == MotionWrite::Stored);
            io.before = {};
            CHECK(store.ready() && !store.faulted());
            expected = encode(store.state());
            CHECK(durable() == expected);
            CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            calls = io.calls;
            recover();
        });
        for (const auto& call : calls) {
            if (call.op == Op::Close) continue; // SDK close has no failure return.
            for (esp_err_t error : {ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_TYPE_MISMATCH,
                                   ESP_ERR_NVS_INVALID_LENGTH, ESP_ERR_NVS_NOT_ENOUGH_SPACE, ESP_ERR_NVS_REMOVE_FAILED}) {
                if (initial(action) && error == ESP_ERR_NVS_NOT_FOUND &&
                    ((call.op == Op::OpenRO && call.occurrence == 1) ||
                     (call.op == Op::Query && call.occurrence <= 2))) continue;
                for (bool apply : {false, true}) {
                    if (apply && call.op != Op::Set && call.op != Op::Commit) continue;
                    for (bool early : {false, true}) scenario("write fault action/op/n/error/apply/early " +
                        std::to_string(int(action)) + "/" + std::to_string(int(call.op)) + "/" +
                        std::to_string(call.occurrence) + "/" + std::to_string(error) + "/" +
                        std::to_string(apply) + "/" + std::to_string(early), [&] {
                        MotionStateStore store;
                        prepare(action, store);
                        io.durableOnSet = early;
                        const auto before = raw(store.state());
                        const auto old = durable();
                        fake::fail(call.op, call.occurrence, error, apply);
                        io.before = [&](const fake::Call&) { CHECK(raw(store.state()) == before); };
                        CHECK(perform(action, store) == MotionWrite::StorageFault);
                        io.before = {};
                        latched(store, before);
                        const bool persisted = fake::count(Op::Set) &&
                            ((call.op == Op::Set && apply) || (call.op == Op::Commit && (early || apply)) ||
                             (call.op != Op::Set && call.op != Op::Commit && fake::count(Op::Commit)));
                        CHECK(durable() == (persisted ? expected : old));
                        recover();
                    });
                }
            }
            if (call.op != Op::Query && call.op != Op::Read) continue;
            if (initial(action) && call.op == Op::Query && call.occurrence <= 2) continue;
            for (size_t length : {size_t(0), size_t(1), kMotionStateMaxSize + 1, SIZE_MAX})
                scenario("write-path SDK success wrong length", [&] {
                    MotionStateStore store;
                    prepare(action, store);
                    const auto before = raw(store.state());
                    fake::fail(call.op, call.occurrence, ESP_OK, false, length);
                    CHECK(perform(action, store) == MotionWrite::StorageFault);
                    latched(store, before);
                    recover();
                });
        }
    }
}

void races() {
    for (unsigned kind = 0; kind < 6; ++kind) scenario("commission race never overwrites existing record", [&] {
        MotionState s;
        s.pairing = pairing();
        if (kind == 1) CHECK(makeMotionContextBarrier(context(), s.context));
        auto bytes = encode(s);
        if (kind == 2) bytes[8] ^= 1;
        if (kind == 4) bytes.clear();
        if (kind == 5) bytes.resize(kMotionStateMaxSize + 1);
        io.before = [&](const fake::Call& call) {
            if (call.op == Op::OpenRW) seed(bytes, kind == 3 ? fake::Type::String : fake::Type::Blob);
        };
        MotionStateStore store;
        const auto before = raw(store.state());
        CHECK(store.installInitial(pairing()) == (kind == 0 ? MotionWrite::Unchanged :
              kind == 1 ? MotionWrite::Conflict : MotionWrite::StorageFault));
        CHECK(!fake::count(Op::Set) && !fake::count(Op::Commit) && durable() == bytes);
        if (kind >= 2) latched(store, before);
        io.before = {};
    });
    for (Action action : actions) {
        if (initial(action)) continue;
        for (Op point : {Op::OpenRO, Op::OpenRW}) for (unsigned kind = 0; kind < 5; ++kind)
            scenario("preflight detects missing, rollback, forward, identity or corrupt record", [&] {
                MotionStateStore store;
                prepare(action, store);
                const auto before = raw(store.state());
                auto changed = store.state();
                if (kind == 1) changed = state();
                if (kind == 2) CHECK(makeMotionContextBarrier(context(12), changed.context));
                if (kind == 3) changed.pairing.peerPhysicalId[0] = 'a';
                auto bytes = encode(changed);
                if (kind == 4) bytes[8] ^= 1;
                // Empty-intent baselines need a real rollback, not identical bytes.
                if (kind == 1 && bytes == durable()) { changed = state(); changed.context.profileVersion++; bytes = encode(changed); }
                bool hit = false;
                io.before = [&](const fake::Call& call) {
                    if (call.op != point || hit) return;
                    hit = true;
                    if (kind == 0) io.disk["productstate"].erase("record");
                    else seed(bytes);
                };
                CHECK(perform(action, store) == MotionWrite::StorageFault);
                CHECK(hit && !fake::count(Op::Set) && !fake::count(Op::Commit));
                latched(store, before);
                io.before = {};
            });
    }
    for (unsigned kind = 0; kind < 6; ++kind) scenario("same-instance reload cannot accept rollback or changed pairing", [&] {
        seed(state(true));
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto before = raw(store.state());
        auto changed = state(kind != 1);
        if (kind == 2) CHECK(makeMotionContextBarrier(context(11, true), changed.context));
        if (kind == 3) changed.pairing.peerPhysicalId[0] = 'a';
        if (kind == 4) { changed.localSequence = 9; changed.localResult = result(request(9)); }
        if (kind == 5) changed.pairing.epoch[0] = 'a';
        if (kind == 0) io.disk["productstate"].erase("record");
        else if (kind == 5) {
            changed = MotionState{};
            changed.pairing = pairing();
            changed.pairing.epoch[0] = 'a';
            seed(changed);
        } else seed(changed);
        const auto loaded = store.load(kind == 5 ? changed.pairing : pairing());
        CHECK(loaded != MotionLoad::Ready && loaded != MotionLoad::Missing);
        latched(store, before);
        noWrites();
    });
    for (Action action : actions) for (unsigned kind = 0; kind < 6; ++kind)
        scenario("SDK success but wrong durable readback action/kind " + std::to_string(int(action)) + "/" + std::to_string(kind), [&] {
            MotionStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            const auto old = durable();
            bool hit = false;
            io.before = [&](const fake::Call& call) {
                if (call.op != Op::OpenRO || !fake::count(Op::Commit)) return;
                hit = true;
                if (kind == 0) io.disk["productstate"].erase("record");
                if (kind == 1) seed(old);
                if (kind == 2) { auto s = state(); CHECK(makeMotionContextBarrier(context(22), s.context)); seed(s); }
                if (kind == 3) { auto bytes = durable(); bytes[8] ^= 1; seed(bytes); }
                if (kind == 4) seed(durable(), fake::Type::U32);
                if (kind == 5) { auto bytes = durable(); bytes.push_back(0); seed(bytes); }
            };
            CHECK(perform(action, store) == MotionWrite::StorageFault);
            CHECK(hit && fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            latched(store, before);
            io.before = {};
        });
    for (unsigned kind = 0; kind < 3; ++kind) scenario("SDK success writes wrong bytes/loses write before commit", [&] {
        MotionStateStore store;
        prepare(Action::LocalIntent, store);
        const auto before = raw(store.state());
        const auto old = durable();
        io.before = [&](const fake::Call& call) {
            if (call.op != Op::Commit) return;
            auto& pending = io.handles.at(call.handle).pending;
            if (kind == 0) pending.clear();
            if (kind == 1) pending["record"].bytes = old;
            if (kind == 2) pending["record"].bytes[8] ^= 1;
        };
        CHECK(perform(Action::LocalIntent, store) == MotionWrite::StorageFault);
        latched(store, before);
        io.before = {};
    });
    for (unsigned kind = 0; kind < 3; ++kind) scenario("loaded installer cannot blank missing/changed state", [&] {
        MotionState initialState;
        initialState.pairing = pairing();
        seed(initialState);
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto before = raw(store.state());
        if (kind == 0) io.disk["productstate"].erase("record");
        else if (kind == 1) seed(state());
        else { auto bytes = durable(); bytes[8] ^= 1; seed(bytes); }
        CHECK(store.installInitial(pairing()) == MotionWrite::StorageFault);
        latched(store, before);
        noWrites();
    });
}

void crypto() {
    for (Action action : actions) {
        if (action == Action::Install) continue; // Blank identity has no content digest.
        scenario("SDK SHA failure before mutation", [&] {
            MotionStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            const auto disk = io.disk;
            fake_product_crypto::fail = true;
            CHECK(perform(action, store) == MotionWrite::StorageFault);
            latched(store, before);
            CHECK(io.disk == disk);
            noWrites();
            fake_product_crypto::fail = false;
            latched(store, before);
            recover();
        });
    }
    for (Op point : {Op::OpenRW, Op::OpenRO}) scenario("SDK SHA failure encoding or postcommit readback", [&] {
        MotionStateStore store;
        prepare(Action::LocalIntent, store);
        const auto before = raw(store.state());
        io.before = [&](const fake::Call& call) {
            if (call.op == point && (point == Op::OpenRW || fake::count(Op::Commit))) fake_product_crypto::fail = true;
        };
        CHECK(perform(Action::LocalIntent, store) == MotionWrite::StorageFault);
        latched(store, before);
        CHECK(fake::count(Op::Set) == (point == Op::OpenRW ? 0 : 1));
        io.before = {};
        fake_product_crypto::fail = false;
        recover();
    });
    scenario("SDK SHA failure during load preserves pending evidence", [] {
        seed(state(true));
        MotionStateStore store;
        const auto before = raw(store.state());
        fake_product_crypto::fail = true;
        CHECK(store.load(pairing()) == MotionLoad::Corrupt);
        latched(store, before);
        noWrites();
        fake_product_crypto::fail = false;
        recover();
    });
}
} // namespace

int main(int argc, char** argv) {
    std::printf("Host-only sizes: MotionState=%zu, MotionStateStore=%zu bytes (not MCU/stack budgets)\n",
                sizeof(MotionState), sizeof(MotionStateStore));
    const std::vector<std::pair<const char*, std::function<void()>>> groups{
        {"basics", basics}, {"sequences", sequences}, {"contexts", contexts},
        {"terminals", terminals}, {"stops", stops}, {"invalid", invalidInputs},
        {"integrity", operationIntegrity}, {"capacity", capacity},
        {"read", reads}, {"write", writes}, {"races", races}, {"crypto", crypto}};
    bool found = false;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        groupFailures = 0;
        const unsigned before = scenarios;
        group.second();
        std::printf("%-10s %u scenarios, %u failures\n", group.first, scenarios - before, groupFailures);
        std::fflush(stdout);
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("MotionStateStore: %u scenarios, %u failures; real codec/digest, no erase, isolated namespaces\n",
                scenarios, failures);
    return failures ? 1 : 0;
}
