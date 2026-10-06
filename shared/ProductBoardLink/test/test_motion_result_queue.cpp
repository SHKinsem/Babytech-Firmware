#include "MotionStateStore.h"
#include "BoardPairingRecord.h"
#include "RecordBytes.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

#include <algorithm>
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
unsigned scenarios = 0, failures = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
static_assert(kMotionResultQueueCapacity == 4, "approved offline result capacity");
static_assert(kMotionStateMaxSize <= 4096, "existing productstate budget");

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
        std::strcpy(c.babyId, version == 10 ? "baby-original" : "baby-next");
        std::strcpy(c.babyName, "Baby");
        std::strcpy(c.formulaBrand, "Formula");
        c.waterMl = 120;
        c.temperatureC = 40;
        c.powderGPer100Ml = version == 10 ? 13.5f : 15.f;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t sequence, ProductCommand command = ProductCommand::Prepare,
                       v4::Source source = v4::Source::LocalTouch, uint32_t version = 10) {
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
        const auto c = context(version);
        std::strcpy(r.babyId, c.babyId);
        r.profileVersion = c.profileVersion;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = c.powderGPer100Ml;
    }
    CHECK(validProductRequest(r));
    return r;
}
std::string execution(uint64_t sequence) {
    char id[33];
    std::snprintf(id, sizeof(id), "%032llx", static_cast<unsigned long long>(sequence));
    return id;
}
MotionResult result(const ProductRequest& r, bool accepted = true) {
    MotionResult value;
    value.kind = MotionResultKind::Ordinary;
    value.request = r;
    CHECK(requestDigest(r, value.digest));
    value.accepted = accepted;
    std::strcpy(value.reason, accepted ? "accepted" : "queue_full");
    return value;
}
MotionExecutionSlot slot(uint64_t sequence, MotionSlotKind kind = MotionSlotKind::Terminal,
                         v4::Source source = v4::Source::LocalTouch) {
    MotionExecutionSlot s;
    s.kind = kind;
    s.request = request(sequence, ProductCommand::Prepare, source);
    CHECK(requestDigest(s.request, s.digest));
    std::strcpy(s.executionId, execution(sequence).c_str());
    CHECK(makeProductEventId(pairing(), source, sequence, s.eventId));
    s.targetPowderG = 24.3f;
    if (kind == MotionSlotKind::Terminal) {
        s.completed = sequence % 2 != 0;
        s.uptimeMs = uint32_t(sequence * 100);
        if (!s.completed) {
            std::strcpy(s.reason, "interrupted");
            std::strcpy(s.errorCode, "motor_fault");
        }
    }
    return s;
}
MotionState fixture(unsigned count, MotionSlotKind active = MotionSlotKind::Empty) {
    CHECK(count <= kMotionResultQueueCapacity);
    MotionState s;
    s.pairing = pairing();
    CHECK(makeMotionContextBarrier(context(), s.context));
    s.localSequence = 10;
    s.localResult = result(request(10), false);
    s.pendingResultCount = uint8_t(count);
    for (unsigned i = 0; i < count; ++i) s.pendingResults[i] = slot(i + 1);
    if (active != MotionSlotKind::Empty) {
        s.slot = slot(20, active, v4::Source::CloudCommand);
        s.cloudSequence = 20;
        s.cloudResult = result(s.slot.request);
    }
    CHECK(validMotionState(s));
    return s;
}
Bytes encode(const MotionState& s) {
    Bytes bytes(kMotionStateMaxSize);
    const auto n = encodeMotionState(s, bytes.data(), bytes.size());
    CHECK(n && n <= bytes.size());
    bytes.resize(n);
    return bytes;
}
template<class T> Bytes raw(const T& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    return Bytes(bytes, bytes + sizeof(value));
}
void sameSlot(const MotionExecutionSlot& a, const MotionExecutionSlot& b) {
    CHECK(a.kind == b.kind);
    // Empty requests are intentionally invalid to sameProductRequest; their
    // zero fields are checked by validMotionState in sameState below.
    if (a.kind != MotionSlotKind::Empty) CHECK(sameProductRequest(a.request, b.request));
    CHECK(!std::memcmp(a.digest, b.digest, sizeof(a.digest)));
    CHECK(!std::strcmp(a.executionId, b.executionId) && !std::strcmp(a.eventId, b.eventId));
    CHECK(a.targetPowderG == b.targetPowderG && a.completed == b.completed && a.uptimeMs == b.uptimeMs);
    CHECK(!std::strcmp(a.reason, b.reason) && !std::strcmp(a.errorCode, b.errorCode));
}
void sameState(const MotionState& a, const MotionState& b) {
    // Explicit queue comparisons catch a codec/equality implementation that omits it.
    CHECK(a.pendingResultCount == b.pendingResultCount);
    for (size_t i = 0; i < kMotionResultQueueCapacity; ++i) sameSlot(a.pendingResults[i], b.pendingResults[i]);
    sameSlot(a.slot, b.slot);
    CHECK(sameMotionState(a, b));
    CHECK(encode(a) == encode(b));
}
void seed(const Bytes& bytes) { io.disk["productstate"]["record"] = {bytes, fake::Type::Blob}; }
void seed(const MotionState& s) { seed(encode(s)); }
Bytes durable() { return io.disk.at("productstate").at("record").bytes; }
fake::Database protectedData() {
    auto disk = io.disk;
    disk["productstate"].erase("record");
    return disk;
}
void audit() {
    CHECK(io.handles.empty());
    CHECK(!fake::count(Op::Erase) && !fake::count(Op::Init));
    for (const auto& call : io.calls) {
        CHECK(call.name == "productstate");
        CHECK(call.key.empty() || call.key == "record");
    }
}
void scenario(const std::string& name, const std::function<void()>& run) {
    fake::reset();
    fake_product_crypto::reset();
    for (const char* ns : {"productpair", "brainstate", "productctx", "outbox", "wifi-cfg",
                           "actuatorcfg", "sensorcfg"})
        io.disk[ns]["record"] = {{0, 0xff, 0x7f, 1}, fake::Type::Blob};
    io.disk["productstate"]["unrelated"] = {{1, 2, 3}, fake::Type::U32};
    const auto protectedBefore = protectedData();
    ++scenarios;
    try {
        run();
        fake::verifyFaults();
        audit();
        CHECK(protectedData() == protectedBefore);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        io.before = {};
        io.handles.clear();
    }
}
void unchanged(MotionStateStore& store, const std::function<MotionWrite()>& action, MotionWrite expected) {
    const auto before = raw(store.state());
    const auto disk = io.disk;
    const auto sets = fake::count(Op::Set), commits = fake::count(Op::Commit);
    CHECK(action() == expected);
    CHECK(store.ready() && !store.faulted());
    CHECK(raw(store.state()) == before && io.disk == disk);
    CHECK(fake::count(Op::Set) == sets && fake::count(Op::Commit) == commits);
}
void recover(const MotionState& expected) {
    fake::verifyFaults();
    audit();
    fake::reboot();
    const auto disk = io.disk;
    MotionStateStore fresh;
    CHECK(fresh.load(expected.pairing) == MotionLoad::Ready);
    sameState(fresh.state(), expected);
    CHECK(io.disk == disk && !fake::count(Op::OpenRW));
}
void removed(MotionState& expected, unsigned index) {
    CHECK(index < expected.pendingResultCount);
    for (size_t i = index + 1; i < expected.pendingResultCount; ++i)
        expected.pendingResults[i - 1] = expected.pendingResults[i];
    expected.pendingResults[--expected.pendingResultCount] = MotionExecutionSlot{};
}

void offline() {
    scenario("four offline finishes reserve capacity; fifth rejection and one confirmed receipt", [] {
        MotionStateStore store;
        const auto c = context();
        CHECK(store.installInitial(pairing(), &c) == MotionWrite::Stored);
        std::vector<MotionExecutionSlot> events;
        for (unsigned seq = 1; seq <= 4; ++seq) {
            CHECK(store.recordDecision(request(seq), true, "accepted", execution(seq).c_str()) == MotionWrite::Stored);
            CHECK(store.state().slot.kind == MotionSlotKind::Intent);
            CHECK(store.state().pendingResultCount == seq - 1);
            const auto intent = store.state();
            recover(intent);
            unchanged(store, [&] { return store.archiveFeeding(true); }, MotionWrite::Busy);
            unchanged(store, [&] { return store.recordDecision(request(seq + 1), true, "accepted",
                execution(seq + 1).c_str()); }, MotionWrite::Busy);
            CHECK(store.finishFeeding(execution(seq).c_str(), true, "", "", seq * 100) == MotionWrite::Stored);
            events.push_back(store.state().slot);
            unchanged(store, [&] { return store.archiveFeeding(false); }, MotionWrite::Busy);
            CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
            CHECK(store.state().slot.kind == MotionSlotKind::Empty);
            CHECK(store.state().pendingResultCount == seq);
            for (unsigned i = 0; i < seq; ++i) sameSlot(store.state().pendingResults[i], events[i]);
            unchanged(store, [&] { return store.archiveFeeding(false); }, MotionWrite::Unchanged);
            const auto archived = store.state();
            recover(archived);
        }
        unchanged(store, [&] { return store.recordDecision(request(5), true, "accepted", execution(5).c_str()); },
                  MotionWrite::QueueFull);
        CHECK(store.state().localSequence == 4);
        CHECK(store.recordDecision(request(5), false, "queue_full") == MotionWrite::Stored);
        CHECK(store.state().localSequence == 5 && !store.state().localResult.accepted);
        const auto cloud = request(7, ProductCommand::Prepare, v4::Source::CloudCommand);
        unchanged(store, [&] { return store.recordDecision(cloud, true, "accepted", execution(7).c_str()); },
                  MotionWrite::QueueFull);
        CHECK(store.state().cloudSequence == 0);
        CHECK(store.recordDecision(cloud, false, "queue_full") == MotionWrite::Stored);
        CHECK(store.state().cloudSequence == 7 && !store.state().cloudResult.accepted);
        for (unsigned i = 0; i < 4; ++i) sameSlot(store.state().pendingResults[i], events[i]);
        const auto full = store.state();
        recover(full);
        auto expected = full;
        removed(expected, 0);
        CHECK(store.acknowledge(events[0].eventId, true, false) == MotionWrite::Stored);
        sameState(store.state(), expected);
        unchanged(store, [&] { return store.recordDecision(request(5), false, "queue_full"); }, MotionWrite::Unchanged);
        unchanged(store, [&] { return store.recordDecision(request(5), true, "accepted", execution(5).c_str()); },
                  MotionWrite::Conflict);
        unchanged(store, [&] { return store.recordDecision(request(4), true, "accepted", execution(4).c_str()); },
                  MotionWrite::Expired);
        CHECK(store.recordDecision(request(6), true, "accepted", execution(6).c_str()) == MotionWrite::Stored);
        CHECK(store.state().pendingResultCount == 3 && store.state().slot.kind == MotionSlotKind::Intent);
        CHECK(store.finishFeeding(execution(6).c_str(), false, "interrupted", "motor_fault", 600) == MotionWrite::Stored);
        CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
        CHECK(store.state().pendingResultCount == 4 && !store.state().pendingResults[3].completed);
        const auto final = store.state();
        recover(final);
    });
    for (auto command : {ProductCommand::Initialize, ProductCommand::Clean})
        scenario("full result queue permits non-feeding operation " + std::to_string(int(command)), [=] {
            const auto initial = fixture(4);
            seed(initial);
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            CHECK(store.recordDecision(request(11, command), true, "accepted", execution(11).c_str()) == MotionWrite::Stored);
            CHECK(store.state().pendingResultCount == 4 && store.state().slot.kind == MotionSlotKind::Intent);
            const auto active = store.state();
            recover(active);
            CHECK(store.finishOperation(execution(11).c_str(), MotionOutcome::Succeeded, true) == MotionWrite::Stored);
            CHECK(store.state().slot.kind == MotionSlotKind::Empty);
            for (unsigned i = 0; i < 4; ++i) sameSlot(store.state().pendingResults[i], initial.pendingResults[i]);
            const auto finished = store.state();
            recover(finished);
        });
}

void receipts() {
    for (unsigned index = 0; index < 3; ++index) for (bool stationary : {false, true})
        scenario("ack preserves active Intent index/stationary " + std::to_string(index) + "/" +
                 std::to_string(stationary), [=] {
            auto expected = fixture(3, MotionSlotKind::Intent);
            const auto event = expected.pendingResults[index];
            seed(expected);
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            removed(expected, index);
            CHECK(store.acknowledge(event.eventId, event.completed, stationary) == MotionWrite::Stored);
            sameState(store.state(), expected);
            unchanged(store, [&] { return store.acknowledge(event.eventId, event.completed, true); }, MotionWrite::Conflict);
            recover(expected);
        });
    scenario("wrong receipt cannot remove queued or active evidence", [] {
        const auto initial = fixture(3, MotionSlotKind::Intent);
        seed(initial);
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        for (bool stationary : {false, true}) {
            for (unsigned i = 0; i < initial.pendingResultCount; ++i) {
                const auto& event = initial.pendingResults[i];
                unchanged(store, [&] { return store.acknowledge(event.eventId, !event.completed, stationary); },
                          MotionWrite::Conflict);
            }
            // Unmatched receipts may be Busy or Conflict; neither may mutate anything.
            for (const char* id : {"evt-wrong", initial.slot.eventId})
                unchanged(store, [&] {
                    const auto reply = store.acknowledge(id, true, stationary);
                    CHECK(reply == MotionWrite::Busy || reply == MotionWrite::Conflict);
                    return reply == MotionWrite::Busy ? MotionWrite::Conflict : reply;
                }, MotionWrite::Conflict);
        }
        unchanged(store, [&] { return store.acknowledge(nullptr, true, false); }, MotionWrite::Invalid);
        unchanged(store, [&] { return store.acknowledge("", true, true); }, MotionWrite::Invalid);
        recover(initial);
    });
    scenario("active Terminal ack still needs stationary and leaves queued results intact", [] {
        auto expected = fixture(3, MotionSlotKind::Terminal);
        const auto event = expected.slot;
        seed(expected);
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        unchanged(store, [&] { return store.acknowledge(event.eventId, event.completed, false); }, MotionWrite::Busy);
        unchanged(store, [&] { return store.acknowledge(event.eventId, !event.completed, true); }, MotionWrite::Conflict);
        CHECK(store.acknowledge(event.eventId, event.completed, true) == MotionWrite::Stored);
        expected.slot = MotionExecutionSlot{};
        sameState(store.state(), expected);
        recover(expected);
    });
    for (bool cleared : {false, true}) scenario("changed context preserves old events and active Intent", [=] {
        auto expected = fixture(3, MotionSlotKind::Intent);
        seed(expected);
        MotionStateStore store;
        CHECK(store.load(pairing()) == MotionLoad::Ready);
        const auto next = context(11, cleared);
        CHECK(store.saveContext(next) == MotionWrite::Stored);
        CHECK(makeMotionContextBarrier(next, expected.context));
        sameState(store.state(), expected);
        recover(expected);
        CHECK(store.finishFeeding(expected.slot.executionId, false, "interrupted", "motor_fault", 2000) == MotionWrite::Stored);
        expected.slot = slot(20, MotionSlotKind::Terminal, v4::Source::CloudCommand);
        expected.pendingResults[expected.pendingResultCount++] = expected.slot;
        expected.slot = MotionExecutionSlot{};
        CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
        sameState(store.state(), expected);
        const auto old = expected.pendingResults[1];
        CHECK(store.acknowledge(old.eventId, old.completed, false) == MotionWrite::Stored);
        removed(expected, 1);
        sameState(store.state(), expected);
        recover(expected);
    });
}

// Freeze the BMS1 outer layout independently of the production Motion codec.
// Only nested pairing/request encoders and the common envelope are reused.
Bytes legacy(const MotionState& s) {
    CHECK(s.pendingResultCount == 0);
    Bytes bytes(kMotionStateMaxSize);
    detail::ByteWriter w(bytes.data() + 12, bytes.size() - 12);
    uint8_t pair[kPairingRecordMaxSize];
    const auto n = encodePairingRecord(s.pairing, pair, sizeof(pair));
    CHECK(n && w.integer(n, 2) && w.raw(pair, n));
    CHECK(w.integer(s.context.present, 1));
    const auto putFloat = [&](float f) { uint32_t bits; std::memcpy(&bits, &f, 4); CHECK(w.integer(bits, 4)); };
    const auto putRequest = [&](const ProductRequest& r, const uint8_t* digest) {
        uint8_t requestBytes[kRequestIdentityMaxSize];
        const auto length = encodeRequestIdentity(r, requestBytes, sizeof(requestBytes));
        CHECK(length && w.integer(length, 2) && w.raw(requestBytes, length) && w.raw(digest, 32));
    };
    if (s.context.present) {
        CHECK(w.integer(s.context.profileVersion, 4) && w.integer(s.context.cleared, 1) &&
              w.raw(s.context.digest, 32) && w.text(s.context.babyId));
        putFloat(s.context.powderGPer100Ml);
    }
    CHECK(w.integer(s.cloudSequence, 8) && w.integer(s.localSequence, 8));
    for (const auto* r : {&s.cloudResult, &s.localResult}) {
        CHECK(w.integer(uint8_t(r->kind), 1));
        if (r->kind == MotionResultKind::None) continue;
        CHECK(r->kind == MotionResultKind::Ordinary);
        putRequest(r->request, r->digest);
        CHECK(w.integer(r->accepted, 1) && w.text(r->reason) && w.integer(uint8_t(r->outcome), 1));
    }
    CHECK(w.integer(uint8_t(s.slot.kind), 1));
    if (s.slot.kind != MotionSlotKind::Empty) {
        putRequest(s.slot.request, s.slot.digest);
        CHECK(w.text(s.slot.executionId) && w.text(s.slot.eventId));
        putFloat(s.slot.targetPowderG);
        if (s.slot.kind == MotionSlotKind::Terminal)
            CHECK(w.integer(s.slot.completed, 1) && w.integer(s.slot.uptimeMs, 4) &&
                  w.text(s.slot.reason) && w.text(s.slot.errorCode));
    }
    bytes.resize(12 + w.size());
    detail::finishRecord(bytes.data(), bytes.size(), "BMS1");
    return bytes;
}
void codec() {
    for (unsigned count = 0; count <= 4; ++count) scenario("BMS2 queue roundtrip count " + std::to_string(count), [=] {
        const auto initial = fixture(count);
        const auto bytes = encode(initial);
        CHECK(!std::memcmp(bytes.data(), "BMS2", 4));
        auto decoded = fixture(3, MotionSlotKind::Intent);
        CHECK(decodeMotionState(bytes.data(), bytes.size(), decoded));
        sameState(decoded, initial);
        seed(bytes);
        recover(initial);
        for (size_t capacity : {size_t(0), bytes.size() - 1, bytes.size()}) {
            Bytes out(kMotionStateMaxSize + 16, 0xa5);
            const auto n = encodeMotionState(initial, out.data(), capacity);
            CHECK(n == (capacity < bytes.size() ? 0 : bytes.size()));
            CHECK(std::all_of(out.begin() + n, out.end(), [](uint8_t b) { return b == 0xa5; }));
        }
        for (size_t length = 0; length < bytes.size(); ++length) {
            const auto before = raw(decoded);
            CHECK(!decodeMotionState(bytes.data(), length, decoded));
            CHECK(raw(decoded) == before);
        }
    });
    for (auto kind : {MotionSlotKind::Empty, MotionSlotKind::Intent, MotionSlotKind::Terminal})
        scenario("BMS1 read-only recovery then BMS2 mutation kind " + std::to_string(int(kind)), [=] {
            auto expected = fixture(0, kind);
            const auto bytes = legacy(expected);
            auto decoded = fixture(4);
            CHECK(decodeMotionState(bytes.data(), bytes.size(), decoded));
            sameState(decoded, expected);
            seed(bytes);
            MotionStateStore store;
            CHECK(store.load(pairing()) == MotionLoad::Ready);
            sameState(store.state(), expected);
            CHECK(durable() == bytes && !fake::count(Op::OpenRW));
            if (kind == MotionSlotKind::Intent)
                unchanged(store, [&] { return store.archiveFeeding(true); }, MotionWrite::Busy);
            if (kind == MotionSlotKind::Terminal) {
                unchanged(store, [&] { return store.archiveFeeding(false); }, MotionWrite::Busy);
                CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
                expected.pendingResults[expected.pendingResultCount++] = expected.slot;
                expected.slot = MotionExecutionSlot{};
            } else {
                CHECK(store.saveContext(context(11)) == MotionWrite::Stored);
                CHECK(makeMotionContextBarrier(context(11), expected.context));
            }
            CHECK(!std::memcmp(durable().data(), "BMS2", 4));
            sameState(store.state(), expected);
            recover(expected);
        });
    scenario("queue equality and malformed queue cannot silently lose evidence", [] {
        const auto initial = fixture(3);
        auto changed = initial;
        changed.pendingResults[1].uptimeMs++;
        CHECK(validMotionState(changed) && !sameMotionState(initial, changed));
        CHECK(encode(initial) != encode(changed));
        for (unsigned mutation = 0; mutation < 5; ++mutation) {
            auto bad = initial;
            if (mutation == 0) bad.pendingResultCount = 5;
            if (mutation == 1) bad.pendingResults[1].kind = MotionSlotKind::Intent;
            if (mutation == 2) bad.pendingResults[1].digest[0] ^= 1;
            if (mutation == 3) bad.pendingResults[1] = bad.pendingResults[0];
            if (mutation == 4) {
                bad = fixture(4);
                bad.slot = slot(20, MotionSlotKind::Intent, v4::Source::CloudCommand);
                bad.cloudSequence = 20;
                bad.cloudResult = result(bad.slot.request);
            }
            CHECK(!validMotionState(bad));
            Bytes out(kMotionStateMaxSize, 0xa5);
            CHECK(!encodeMotionState(bad, out.data(), out.size()));
            CHECK(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0xa5; }));
        }
    });
    scenario("BMS2 rejects CRC-valid malformed queue without changing destination", [] {
        const auto initial = fixture(3);
        auto prefix = initial;
        prefix.pendingResultCount = 0;
        for (auto& item : prefix.pendingResults) item = MotionExecutionSlot{};
        const size_t countAt = legacy(prefix).size();
        const auto bytes = encode(initial);
        CHECK(bytes.at(countAt) == 3 && bytes.at(countAt + 1) == uint8_t(MotionSlotKind::Terminal));
        auto destination = fixture(2, MotionSlotKind::Intent);
        const auto before = raw(destination);
        const auto reject = [&](Bytes bad) {
            detail::finishRecord(bad.data(), bad.size(), "BMS2");
            CHECK(!decodeMotionState(bad.data(), bad.size(), destination));
            CHECK(raw(destination) == before);
        };
        for (uint8_t count : {uint8_t(0), uint8_t(2), uint8_t(4), uint8_t(5), uint8_t(255)}) {
            auto bad = bytes;
            bad[countAt] = count;
            reject(bad);
        }
        for (uint8_t kind : {uint8_t(0), uint8_t(1), uint8_t(255)}) {
            auto bad = bytes;
            bad[countAt + 1] = kind;
            reject(bad);
        }
        for (size_t length = countAt; length < bytes.size(); ++length)
            reject(Bytes(bytes.begin(), bytes.begin() + length));
        auto trailing = bytes;
        trailing.push_back(0);
        reject(trailing);
    });
}

template<size_t N> void fillMaximum(char (&value)[N], char byte) {
    std::memset(value, byte, N - 1);
    value[N - 1] = '\0';
}
void maximum() {
    static_assert(kMotionStateMaxSize <= 4073, "approved queue record bound");
    for (auto active : {ProductCommand::Clean, ProductCommand::Prepare})
        scenario("maximum legal fields with active command " + std::to_string(int(active)), [=] {
            auto p = pairing();
            fillMaximum(p.deviceId, 'D');
            auto c = context(INT32_MAX);
            std::strcpy(c.deviceId, p.deviceId);
            fillMaximum(c.babyId, 'B');
            fillMaximum(c.babyName, 'N');
            fillMaximum(c.formulaBrand, 'F');
            c.waterMl = 500;
            c.temperatureC = 60;
            c.powderGPer100Ml = 50;
            CHECK(validProductContext(c));
            const auto maximumRequest = [&](uint64_t sequence, ProductCommand command, v4::Source source) {
                ProductRequest r;
                r.sequence = sequence;
                r.command = command;
                r.source = source;
                std::strcpy(r.deviceId, p.deviceId);
                if (source == v4::Source::CloudCommand) {
                    fillMaximum(r.commandId, 'C');
                    // Keep each maximum-width command ID distinct as well as its sequence.
                    const auto suffix = execution(sequence);
                    std::memcpy(r.commandId, suffix.data(), suffix.size());
                } else {
                    auto brain = p;
                    brain.role = v4::Role::Brain;
                    std::swap(brain.localPhysicalId, brain.peerPhysicalId);
                    CHECK(makeLocalCommandId(brain, sequence, r.commandId));
                    CHECK(std::strlen(r.commandId) == 58);
                }
                if (command == ProductCommand::Prepare) {
                    std::strcpy(r.babyId, c.babyId);
                    r.profileVersion = c.profileVersion;
                    r.waterMl = c.waterMl;
                    r.temperatureC = c.temperatureC;
                    r.powderGPer100Ml = c.powderGPer100Ml;
                }
                CHECK(validProductRequest(r));
                return r;
            };
            const std::string reason(64, 'R'), error(64, 'E');
            MotionStateStore store;
            CHECK(store.installInitial(p, &c) == MotionWrite::Stored);
            const unsigned count = active == ProductCommand::Clean ? 4 : 3;
            for (unsigned i = 0; i < count; ++i) {
                const auto seq = v4::kMaxSequence - 6 + i;
                const auto r = maximumRequest(seq, ProductCommand::Prepare, v4::Source::CloudCommand);
                CHECK(store.recordDecision(r, true, "accepted", execution(seq).c_str()) == MotionWrite::Stored);
                CHECK(store.finishFeeding(execution(seq).c_str(), false, reason.c_str(), error.c_str(), UINT32_MAX)
                      == MotionWrite::Stored);
                CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
            }
            const auto activeSeq = v4::kMaxSequence - 2;
            const auto r = maximumRequest(activeSeq, active, v4::Source::CloudCommand);
            CHECK(store.recordDecision(r, true, "accepted", execution(activeSeq).c_str()) == MotionWrite::Stored);
            // Later rejected prepares maximize both retained decisions without
            // replacing the active execution or its reserved result capacity.
            for (auto source : {v4::Source::CloudCommand, v4::Source::LocalTouch})
                CHECK(store.recordDecision(maximumRequest(v4::kMaxSequence, ProductCommand::Prepare, source),
                                           false, reason.c_str()) == MotionWrite::Stored);
            CHECK(store.state().pendingResultCount == count);
            CHECK(store.state().slot.kind == MotionSlotKind::Intent);
            CHECK(store.state().slot.request.command == active);
            for (unsigned i = 0; i < count; ++i) {
                const auto& item = store.state().pendingResults[i];
                CHECK(item.kind == MotionSlotKind::Terminal && !item.completed);
                CHECK(std::strlen(item.eventId) == 58 && std::strlen(item.request.commandId) == 128);
                CHECK(std::strlen(item.reason) == 64 && std::strlen(item.errorCode) == 64);
            }
            const auto verifyCapacity = [&](const char* label) {
                const auto expected = store.state();
                CHECK(validMotionState(expected));
                const auto bytes = encode(expected);
                CHECK(bytes.size() <= 4073);
                CHECK(durable() == bytes);
                CHECK(!std::memcmp(bytes.data(), "BMS2", 4));
                auto decoded = fixture(2, MotionSlotKind::Intent);
                CHECK(decodeMotionState(bytes.data(), bytes.size(), decoded));
                sameState(decoded, expected);
                recover(expected);
                std::printf("Maximum-field %s: %zu bytes (limit 4073)\n", label, bytes.size());
            };
            verifyCapacity(active == ProductCommand::Clean ? "queue4 + Clean Intent" : "queue3 + Prepare Intent");
            if (active == ProductCommand::Prepare) {
                CHECK(store.finishFeeding(execution(activeSeq).c_str(), false, reason.c_str(), error.c_str(), UINT32_MAX)
                      == MotionWrite::Stored);
                verifyCapacity("queue3 + Prepare Terminal");
                CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
                CHECK(store.state().pendingResultCount == 4 && store.state().slot.kind == MotionSlotKind::Empty);
                verifyCapacity("queue4 + Empty after reserved result archival");
            }
        });
}

enum class Action { Archive, AckOldest, AckMiddle, AckNewest };
MotionState beforeAction(Action action) {
    return fixture(3, action == Action::Archive ? MotionSlotKind::Terminal : MotionSlotKind::Intent);
}
MotionState afterAction(Action action) {
    auto s = beforeAction(action);
    if (action == Action::Archive) {
        s.pendingResults[s.pendingResultCount++] = s.slot;
        s.slot = MotionExecutionSlot{};
    } else removed(s, unsigned(action) - unsigned(Action::AckOldest));
    return s;
}
void prepare(Action action, MotionStateStore& store) {
    seed(beforeAction(action));
    CHECK(store.load(pairing()) == MotionLoad::Ready);
    sameState(store.state(), beforeAction(action));
    io.calls.clear();
}
MotionWrite perform(Action action, MotionStateStore& store) {
    if (action == Action::Archive) return store.archiveFeeding(true);
    const auto event = slot(unsigned(action));
    return store.acknowledge(event.eventId, event.completed, false);
}
void latched(MotionStateStore& store, const Bytes& before) {
    CHECK(store.faulted() && !store.ready() && raw(store.state()) == before);
    const auto calls = io.calls.size();
    const auto disk = io.disk;
    CHECK(store.archiveFeeding(true) == MotionWrite::StorageFault);
    CHECK(store.acknowledge(slot(1).eventId, true, false) == MotionWrite::StorageFault);
    CHECK(store.recordDecision(request(30), true, "accepted", execution(30).c_str()) == MotionWrite::StorageFault);
    CHECK(store.load(pairing()) != MotionLoad::Ready);
    CHECK(io.calls.size() == calls && io.disk == disk && raw(store.state()) == before);
}
void faults() {
    for (auto action : {Action::Archive, Action::AckOldest, Action::AckMiddle, Action::AckNewest}) {
        const auto label = std::to_string(int(action));
        std::vector<fake::Call> calls;
        scenario("atomic replacement with verified readback action " + label, [&] {
            MotionStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            io.before = [&](const fake::Call&) { CHECK(raw(store.state()) == before); };
            CHECK(perform(action, store) == MotionWrite::Stored);
            io.before = {};
            sameState(store.state(), afterAction(action));
            CHECK(durable() == encode(afterAction(action)));
            CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            calls = io.calls;
            recover(afterAction(action));
        });
        for (const auto& call : calls) {
            if (call.op == Op::Close) continue;
            for (bool early : {false, true}) for (bool apply : {false, true}) {
                if (apply && call.op != Op::Set && call.op != Op::Commit) continue;
                for (auto error : {ESP_FAIL, ESP_ERR_NVS_NOT_ENOUGH_SPACE})
                    scenario("fault action/op/n/early/apply/error " + label + "/" + std::to_string(int(call.op)) +
                        "/" + std::to_string(call.occurrence) + "/" + std::to_string(early) + "/" +
                        std::to_string(apply) + "/" + std::to_string(error), [&] {
                        MotionStateStore store;
                        prepare(action, store);
                        io.durableOnSet = early;
                        const auto before = raw(store.state());
                        fake::fail(call.op, call.occurrence, error, apply);
                        io.before = [&](const fake::Call&) { CHECK(raw(store.state()) == before); };
                        CHECK(perform(action, store) == MotionWrite::StorageFault);
                        io.before = {};
                        latched(store, before);
                        const bool persisted = fake::count(Op::Set) &&
                            ((call.op == Op::Set && apply) || (call.op == Op::Commit && (early || apply)) ||
                             (call.op != Op::Set && call.op != Op::Commit && fake::count(Op::Commit)));
                        const auto expected = persisted ? afterAction(action) : beforeAction(action);
                        CHECK(durable() == encode(expected));
                        recover(expected);
                    });
            }
            if (call.op == Op::Query || call.op == Op::Read)
                for (size_t length : {size_t(0), size_t(1), kMotionStateMaxSize + 1, SIZE_MAX})
                    scenario("SDK success wrong read length action " + label, [&] {
                        MotionStateStore store;
                        prepare(action, store);
                        const auto before = raw(store.state());
                        fake::fail(call.op, call.occurrence, ESP_OK, false, length);
                        CHECK(perform(action, store) == MotionWrite::StorageFault);
                        latched(store, before);
                        const auto expected = fake::count(Op::Commit) ? afterAction(action) : beforeAction(action);
                        CHECK(durable() == encode(expected));
                        recover(expected);
                    });
        }
        scenario("successful commit but stale readback never reports success action " + label, [&] {
            MotionStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            const auto old = durable();
            bool hit = false;
            io.before = [&](const fake::Call& call) {
                if (call.op != Op::OpenRO || !fake::count(Op::Commit)) return;
                hit = true;
                seed(old);
            };
            CHECK(perform(action, store) == MotionWrite::StorageFault);
            io.before = {};
            CHECK(hit && fake::count(Op::Commit) == 1);
            latched(store, before);
            CHECK(durable() == old);
            recover(beforeAction(action));
        });
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups{
        {"offline", offline}, {"receipts", receipts}, {"codec", codec}, {"maximum", maximum}, {"faults", faults}};
    if (argc > 2) return 2;
    bool found = false;
    for (const auto& group : groups) {
        if (argc == 2 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        const auto before = scenarios, failed = failures;
        group.second();
        std::printf("%-10s %u scenarios, %u failures\n", group.first, scenarios - before, failures - failed);
        std::fflush(stdout);
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("MotionResultQueue: %u scenarios, %u failures; production codec/store, host NVS/SHA only\n",
                scenarios, failures);
    return failures ? 1 : 0;
}
