#include "BrainStateStore.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"
#include "RecordBytes.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
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
unsigned scenarios = 0;
static_assert(kBrainStateMaxSize <= 4096, "contract record budget");

v4::Pairing pairing() {
    v4::Pairing result{};
    result.role = v4::Role::Brain;
    std::strcpy(result.deviceId, "Babytech_01-test");
    std::strcpy(result.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(result.localPhysicalId, "012345abcdef");
    std::strcpy(result.peerPhysicalId, "fedcba987654");
    return result;
}
ProductContext context(uint32_t version = 10, bool cleared = false) {
    ProductContext result;
    std::strcpy(result.deviceId, pairing().deviceId);
    result.profileVersion = version;
    result.cleared = cleared;
    if (!cleared) {
        std::strcpy(result.babyId, "baby-original");
        std::strcpy(result.babyName, "Full baby name");
        std::strcpy(result.formulaBrand, "Full formula brand");
        result.waterMl = 120;
        result.temperatureC = 40;
        result.powderGPer100Ml = 13.5f;
    }
    assert(validProductContext(result));
    return result;
}
ProductRequest request(uint64_t sequence = 8, ProductCommand command = ProductCommand::Prepare) {
    ProductRequest result;
    result.command = command;
    result.sequence = sequence;
    std::strcpy(result.deviceId, pairing().deviceId);
    assert(makeLocalCommandId(pairing(), sequence, result.commandId));
    if (command == ProductCommand::Prepare) {
        const auto c = context();
        std::strcpy(result.babyId, c.babyId);
        result.profileVersion = c.profileVersion;
        result.waterMl = 180;
        result.temperatureC = 45;
        result.powderGPer100Ml = c.powderGPer100Ml;
    } else if (command == ProductCommand::SetTargetTemp) result.temperatureC = 45;
    assert(validProductRequest(result));
    return result;
}
BrainState state(bool pending = false) {
    BrainState result;
    result.pairing = pairing();
    result.hasContext = true;
    result.context = context();
    result.localSequence = pending ? 8 : 7;
    result.pending = pending;
    if (pending) {
        result.pendingRequest = request();
        assert(requestDigest(result.pendingRequest, result.pendingDigest));
    }
    assert(validBrainState(result));
    return result;
}
Bytes encode(const BrainState& value) {
    std::array<uint8_t, kBrainStateMaxSize> bytes{};
    const auto length = encodeBrainState(value, bytes.data(), bytes.size());
    assert(length);
    return Bytes(bytes.begin(), bytes.begin() + length);
}
void seed(const Bytes& bytes, fake::Type type = fake::Type::Blob) {
    io.disk["brainstate"]["record"] = {bytes, type};
}
void seed(const BrainState& value) { seed(encode(value)); }
Bytes raw(const BrainState& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    return Bytes(bytes, bytes + sizeof(value));
}
fake::Database protectedData() {
    auto disk = io.disk;
    const auto found = disk.find("brainstate");
    if (found != disk.end()) {
        found->second.erase("record");
        if (found->second.empty()) disk.erase(found);
    }
    return disk;
}
void scenario(const std::string& name, const std::function<void()>& run, bool space = true) {
    fake::reset();
    fake_product_crypto::reset();
    for (const char* name : {"productpair", "productstate", "productctx", "outbox",
                             "wifi-cfg", "actuatorcfg", "sensorcfg"}) {
        io.disk[name]["record"] = {{0, 0xff, 0x7f, 1}, fake::Type::Blob};
        io.disk[name]["payload"] = {{'o', 'l', 'd', 0}, fake::Type::String};
    }
    if (space) io.disk["brainstate"]["unrelated"] = {{1, 2, 3}, fake::Type::U32};
    const auto protectedBefore = protectedData();
    std::printf("[%u] %s\n", ++scenarios, name.c_str());
    std::fflush(stdout);
    run();
    fake::verifyFaults();
    assert(io.handles.empty());
    assert(protectedData() == protectedBefore);
    assert(fake::count(Op::Erase) == 0 && fake::count(Op::Init) == 0);
    for (const auto& call : io.calls) {
        assert(call.name == "brainstate");
        if (!call.key.empty()) assert(call.key == "record");
    }
}
void noWrites() {
    assert(!fake::count(Op::OpenRW) && !fake::count(Op::Set) && !fake::count(Op::Commit));
}
void latched(BrainStateStore& store, const Bytes& before) {
    assert(store.faulted() && !store.ready());
    assert(raw(store.state()) == before);
    const auto calls = io.calls.size();
    const auto disk = io.disk;
    const auto loadResult = store.load(pairing());
    assert(loadResult != BrainLoad::Ready && loadResult != BrainLoad::Missing);
    assert(store.load(v4::Pairing{}) == loadResult);
    assert(store.installInitial(pairing()) == BrainWrite::StorageFault);
    assert(store.saveContext(context(99)) == BrainWrite::StorageFault);
    assert(store.reserveLocal(request()) == BrainWrite::StorageFault);
    assert(store.clearPending(request()) == BrainWrite::StorageFault);
    assert(io.calls.size() == calls && io.disk == disk);
    assert(raw(store.state()) == before);
}
void recover() {
    fake::verifyFaults();
    assert(io.handles.empty());
    io.before = {};
    fake::reboot();
    fake_product_crypto::reset();
    const auto disk = io.disk;
    BrainStateStore fresh;
    const auto space = io.disk.find("brainstate");
    const bool present = space != io.disk.end() && space->second.count("record");
    assert(fresh.load(pairing()) == (present ? BrainLoad::Ready : BrainLoad::Missing));
    assert(!fresh.faulted());
    if (present) {
        BrainState persisted;
        const auto& bytes = space->second.at("record").bytes;
        assert(decodeBrainState(bytes.data(), bytes.size(), persisted));
        assert(sameBrainState(fresh.state(), persisted));
        // Loading the intent only restores evidence. No automatic clear/reserve or write.
        assert(fresh.state().pending == persisted.pending);
        assert(fresh.load(pairing()) == BrainLoad::Ready);
    }
    assert(io.disk == disk);
    noWrites();
}

void basics() {
    for (bool space : {false, true}) {
        scenario("Missing is read-only; never initialize automatically", [&] {
            const auto disk = io.disk;
            BrainStateStore store;
            assert(!store.ready() && !store.faulted());
            for (unsigned i = 0; i < 3; ++i) assert(store.load(pairing()) == BrainLoad::Missing);
            assert(store.saveContext(context()) == BrainWrite::StorageFault);
            assert(store.reserveLocal(request()) == BrainWrite::StorageFault);
            assert(store.clearPending(request()) == BrainWrite::StorageFault);
            noWrites();
            assert(io.disk == disk);
            assert(store.installInitial(pairing()) == BrainWrite::Stored);
            assert(store.ready() && !store.state().hasContext && !store.state().pending);
            assert(store.state().localSequence == 0);
            const auto sets = fake::count(Op::Set);
            assert(store.installInitial(pairing()) == BrainWrite::Unchanged);
            assert(fake::count(Op::Set) == sets);
            recover();
        }, space);
    }
    for (bool early : {false, true}) {
        for (bool cleared : {false, true}) {
            scenario("explicit import preserves full context or original-version tombstone", [&] {
                io.durableOnSet = early;
                BrainStateStore store;
                const auto imported = context(2147483647, cleared);
                assert(store.installInitial(pairing(), &imported) == BrainWrite::Stored);
                assert(sameProductContext(store.state().context, imported));
                assert(store.state().localSequence == 0 && !store.state().pending);
                const std::vector<Op> expected{Op::OpenRO, Op::Query, Op::Close,
                    Op::OpenRW, Op::Query, Op::Set, Op::Commit, Op::Close,
                    Op::OpenRO, Op::Query, Op::Read, Op::Close};
                assert(io.calls.size() == expected.size());
                for (size_t i = 0; i < expected.size(); ++i) assert(io.calls[i].op == expected[i]);
                assert(store.installInitial(pairing(), &imported) == BrainWrite::Unchanged);
                BrainStateStore another;
                assert(another.installInitial(pairing(), &imported) == BrainWrite::Unchanged);
                assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
                recover();
            });
        }
    }
    for (unsigned field = 0; field < 6; ++field) {
        scenario("paired identity rejects alternate caller " + std::to_string(field), [&] {
            seed(state());
            auto other = pairing();
            if (field == 0) other.role = v4::Role::Motion;
            if (field == 1) other.deviceId[0] = 'X';
            if (field == 2) other.epoch[0] = 'a';
            if (field == 3) other.localPhysicalId[0] = 'a';
            if (field == 4) other.peerPhysicalId[0] = 'a';
            if (field == 5) other = v4::Pairing{};
            BrainStateStore store;
            const auto before = raw(store.state());
            assert(store.load(other) == BrainLoad::IdentityMismatch);
            latched(store, before);
            noWrites();
            BrainStateStore installer;
            assert(installer.installInitial(other) ==
                   (field == 0 || field == 5 ? BrainWrite::Invalid : BrainWrite::Conflict));
            assert(!installer.faulted());
            noWrites();
        });
    }
    scenario("different initial import or used sequence cannot overwrite installed state", [] {
        BrainStateStore store;
        auto c = context();
        assert(store.installInitial(pairing(), &c) == BrainWrite::Stored);
        const auto before = raw(store.state());
        const auto disk = io.disk;
        assert(store.installInitial(pairing()) == BrainWrite::Conflict);
        c.profileVersion++;
        assert(store.installInitial(pairing(), &c) == BrainWrite::Conflict);
        assert(raw(store.state()) == before && io.disk == disk);
        assert(store.reserveLocal(request(1)) == BrainWrite::Stored);
        assert(store.clearPending(request(1)) == BrainWrite::Stored);
        c = context();
        assert(store.installInitial(pairing(), &c) == BrainWrite::Conflict);
        assert(store.state().localSequence == 1);
    });
    scenario("invalid imported context never writes", [] {
        BrainStateStore store;
        for (unsigned kind = 0; kind < 4; ++kind) {
            auto c = context();
            if (kind == 0) c.profileVersion = 0;
            if (kind == 1) c.deviceId[0] = 'X';
            if (kind == 2) c.cleared = true;
            if (kind == 3) std::memset(c.babyName, 'x', sizeof(c.babyName));
            assert(store.installInitial(pairing(), &c) == BrainWrite::Invalid);
        }
        noWrites();
    });
}

void semantics() {
    scenario("cache advances while pending; frozen request survives newer cache and tombstone", [] {
        seed(state());
        BrainStateStore store;
        assert(store.load(pairing()) == BrainLoad::Ready);
        const auto r = request();
        assert(store.reserveLocal(r) == BrainWrite::Stored);
        const auto frozen = store.state().pendingRequest;
        const Bytes digest(store.state().pendingDigest, store.state().pendingDigest + kProductDigestSize);
        auto newer = context(11);
        std::strcpy(newer.babyId, "new-baby");
        assert(store.saveContext(newer) == BrainWrite::Stored);
        assert(store.saveContext(context(12, true)) == BrainWrite::Stored);
        assert(sameProductRequest(store.state().pendingRequest, frozen));
        assert(Bytes(store.state().pendingDigest, store.state().pendingDigest + kProductDigestSize) == digest);
        assert(store.state().localSequence == 8);
        const auto disk = io.disk;
        assert(store.reserveLocal(request(9)) == BrainWrite::Busy);
        assert(store.saveContext(context(12, true)) == BrainWrite::Unchanged);
        assert(store.saveContext(context(12)) == BrainWrite::Conflict);
        assert(store.saveContext(context(11)) == BrainWrite::Conflict);
        assert(io.disk == disk);
        assert(store.clearPending(r) == BrainWrite::Stored);
        assert(!store.state().pending && store.state().localSequence == 8);
        assert(store.state().context.cleared && store.state().context.profileVersion == 12);
        assert(store.reserveLocal(request(9)) == BrainWrite::ContextRequired);
        recover();
    });
    scenario("equal and stale conflicts never overwrite or write flash", [] {
        seed(state());
        BrainStateStore store;
        assert(store.load(pairing()) == BrainLoad::Ready);
        const auto before = raw(store.state());
        const auto disk = io.disk;
        assert(store.saveContext(context()) == BrainWrite::Unchanged);
        assert(store.saveContext(context(9)) == BrainWrite::Conflict);
        for (unsigned field = 0; field < 7; ++field) {
            auto c = context();
            if (field == 0) c.babyId[0] = 'X';
            if (field == 1) c.babyName[0] = 'X';
            if (field == 2) c.formulaBrand[0] = 'X';
            if (field == 3) c.waterMl++;
            if (field == 4) c.temperatureC++;
            if (field == 5) c.powderGPer100Ml++;
            if (field == 6) c = context(10, true);
            assert(store.saveContext(c) == BrainWrite::Conflict);
        }
        auto invalid = context(11);
        invalid.deviceId[0] = 'X';
        assert(store.saveContext(invalid) == BrainWrite::Invalid);
        invalid = context(11);
        invalid.profileVersion = 0;
        assert(store.saveContext(invalid) == BrainWrite::Invalid);
        assert(raw(store.state()) == before && io.disk == disk);
        noWrites();
    });
    for (unsigned kind = 0; kind < 14; ++kind) {
        scenario("local admission rejects invalid identity/sequence/cache " + std::to_string(kind), [&] {
            auto initial = state();
            if (kind == 12) { initial.hasContext = false; initial.context = ProductContext{}; }
            if (kind == 13) initial.context = context(10, true);
            seed(initial);
            BrainStateStore store;
            assert(store.load(pairing()) == BrainLoad::Ready);
            auto r = request();
            if (kind == 0) r.source = v4::Source::CloudCommand;
            if (kind == 1) r.deviceId[0] = 'X';
            if (kind == 2) r.commandId[0] = 'X';
            if (kind == 3) r = request(7);
            if (kind == 4) r = request(9);
            if (kind == 5) r.sequence = 0;
            if (kind == 6) r.sequence = v4::kMaxSequence + 1;
            if (kind == 7) r.command = ProductCommand::None;
            if (kind == 8) r.powderGPer100Ml = std::numeric_limits<float>::quiet_NaN();
            if (kind == 9) r.babyId[0] = 'X';
            if (kind == 10) r.profileVersion++;
            if (kind == 11) r.powderGPer100Ml++;
            const auto before = raw(store.state());
            const auto disk = io.disk;
            assert(store.reserveLocal(r) == (kind < 9 ? BrainWrite::Invalid : BrainWrite::ContextRequired));
            assert(raw(store.state()) == before && io.disk == disk && !store.faulted());
            noWrites();
        });
    }
    for (ProductCommand command : {ProductCommand::Initialize, ProductCommand::Clean,
            ProductCommand::SetTargetTemp, ProductCommand::ResetError, ProductCommand::CheckFirmwareUpdate}) {
        scenario("non-prepare commands do not invent a context", [&] {
            BrainStateStore store;
            assert(store.installInitial(pairing()) == BrainWrite::Stored);
            const auto r = request(1, command);
            assert(store.reserveLocal(r) == BrainWrite::Stored);
            assert(!store.state().hasContext && store.state().localSequence == 1);
            assert(store.clearPending(r) == BrainWrite::Stored);
            assert(store.state().localSequence == 1);
            recover();
        });
    }
    for (unsigned field = 0; field < 10; ++field) {
        scenario("clear requires exact full request " + std::to_string(field), [&] {
            seed(state(true));
            BrainStateStore store;
            assert(store.load(pairing()) == BrainLoad::Ready);
            auto changed = request();
            if (field == 0) changed.commandId[0] = 'X';
            if (field == 1) changed.deviceId[0] = 'X';
            if (field == 2) changed.babyId[0] = 'X';
            if (field == 3) changed.profileVersion++;
            if (field == 4) changed.waterMl++;
            if (field == 5) changed.temperatureC++;
            if (field == 6) changed.powderGPer100Ml++;
            if (field == 7) changed.source = v4::Source::CloudCommand;
            if (field == 8) changed = request(9);
            if (field == 9) changed.command = ProductCommand::None;
            const auto before = raw(store.state());
            assert(store.clearPending(changed) == (field == 9 ? BrainWrite::Invalid : BrainWrite::Conflict));
            assert(raw(store.state()) == before);
            noWrites();
            assert(store.clearPending(request()) == BrainWrite::Stored);
            const auto sets = fake::count(Op::Set);
            assert(store.clearPending(request()) == BrainWrite::Conflict);
            assert(fake::count(Op::Set) == sets && store.state().localSequence == 8);
            assert(store.reserveLocal(request(9)) == BrainWrite::Stored);
        });
    }
    scenario("max local sequence accepted once, preserved by clear and never wraps", [] {
        auto s = state();
        s.localSequence = v4::kMaxSequence - 1;
        seed(s);
        BrainStateStore store;
        assert(store.load(pairing()) == BrainLoad::Ready);
        const auto r = request(v4::kMaxSequence);
        assert(store.reserveLocal(r) == BrainWrite::Stored);
        assert(store.clearPending(r) == BrainWrite::Stored);
        assert(store.state().localSequence == v4::kMaxSequence);
        const auto before = raw(store.state());
        const auto disk = io.disk;
        assert(store.reserveLocal(request(1)) == BrainWrite::Exhausted);
        assert(raw(store.state()) == before && io.disk == disk && !store.faulted());
        recover();
    });
    scenario("maximum field capacities remain inside production SDK buffer bound", [] {
        auto p = pairing();
        std::memset(p.deviceId, 'd', sizeof(p.deviceId) - 1);
        auto c = context();
        std::strcpy(c.deviceId, p.deviceId);
        std::memset(c.babyId, 'b', sizeof(c.babyId) - 1);
        std::memset(c.babyName, 'n', sizeof(c.babyName) - 1);
        std::memset(c.formulaBrand, 'f', sizeof(c.formulaBrand) - 1);
        BrainStateStore store;
        assert(store.installInitial(p, &c) == BrainWrite::Stored);
        auto r = request(1);
        std::strcpy(r.deviceId, p.deviceId);
        std::strcpy(r.babyId, c.babyId);
        assert(store.reserveLocal(r) == BrainWrite::Stored);
        const auto bytes = encode(store.state());
        assert(bytes.size() > 1400 && bytes.size() <= kBrainStateMaxSize);
        BrainStateStore fresh;
        assert(fresh.load(p) == BrainLoad::Ready);
        assert(sameBrainState(store.state(), fresh.state()));
    });
}

void readFailures() {
    const auto good = encode(state(true));
    std::vector<Bytes> corrupt{{}, Bytes(kBrainStateMaxSize + 1, 0), Bytes(kBrainStateMaxSize, 0)};
    for (unsigned kind = 0; kind < 5; ++kind) {
        auto bytes = good;
        if (kind == 0) bytes[8] ^= 1;
        if (kind == 1) bytes[4] = 2;
        if (kind == 2) bytes[0] = 'X';
        if (kind == 3) bytes.push_back(0);
        if (kind == 4) bytes.back() ^= 1;
        // Valid envelope CRC isolates schema/magic/semantic digest checks.
        if (kind != 0) {
            const auto crc = detail::recordCrc(bytes.data(), bytes.size());
            for (unsigned i = 0; i < 4; ++i) bytes[8 + i] = uint8_t(crc >> (8 * i));
        }
        corrupt.push_back(bytes);
    }
    for (size_t length = 1; length < good.size(); ++length)
        corrupt.emplace_back(good.begin(), good.begin() + length);
    for (const auto& bytes : corrupt) {
        scenario("corrupt/truncated record length " + std::to_string(bytes.size()), [&] {
            seed(bytes);
            const auto disk = io.disk;
            BrainStateStore store;
            const auto before = raw(store.state());
            assert(store.load(pairing()) == BrainLoad::Corrupt);
            latched(store, before);
            BrainStateStore installer;
            assert(installer.installInitial(pairing()) == BrainWrite::StorageFault);
            assert(installer.faulted());
            noWrites();
            assert(io.disk == disk);
        });
    }
    for (fake::Type type : {fake::Type::String, fake::Type::U32}) {
        scenario("wrong NVS type is corruption, not Missing", [&] {
            seed(good, type);
            BrainStateStore store;
            const auto before = raw(store.state());
            assert(store.load(pairing()) == BrainLoad::Corrupt);
            latched(store, before);
            noWrites();
        });
    }
    for (Op op : {Op::OpenRO, Op::Query, Op::Read}) {
        for (esp_err_t error : {ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_TYPE_MISMATCH,
                                ESP_ERR_NVS_INVALID_LENGTH}) {
            scenario("load SDK error " + std::to_string(int(op)) + "/" + std::to_string(error), [&] {
                seed(good);
                fake::fail(op, 1, error);
                BrainStateStore store;
                const auto before = raw(store.state());
                const auto expected = error == ESP_ERR_NVS_NOT_FOUND && op != Op::Read ? BrainLoad::Missing :
                    (error == ESP_ERR_NVS_TYPE_MISMATCH && op != Op::OpenRO ? BrainLoad::Corrupt : BrainLoad::IoError);
                assert(store.load(pairing()) == expected);
                assert(raw(store.state()) == before);
                if (expected != BrainLoad::Missing) latched(store, before);
                noWrites();
            });
        }
        if (op == Op::OpenRO) continue;
        for (size_t length : {size_t(0), good.size() - 1, good.size() + 1,
                              kBrainStateMaxSize + 1, SIZE_MAX}) {
            scenario("load inconsistent SDK length " + std::to_string(length), [&] {
                seed(good);
                fake::fail(op, 1, ESP_OK, false, length);
                BrainStateStore store;
                const auto before = raw(store.state());
                const auto expected = op == Op::Query && (!length || length > kBrainStateMaxSize)
                    ? BrainLoad::Corrupt : BrainLoad::IoError;
                assert(store.load(pairing()) == expected);
                latched(store, before);
                noWrites();
            });
        }
    }
    scenario("key disappears between query and data read", [] {
        seed(state());
        io.before = [](const fake::Call& call) {
            if (call.op == Op::Read) io.disk["brainstate"].erase("record");
        };
        BrainStateStore store;
        const auto before = raw(store.state());
        assert(store.load(pairing()) == BrainLoad::IoError);
        latched(store, before);
        noWrites();
    });
}

enum class Action { Install, Context, Reserve, Clear };
void prepare(Action action, BrainStateStore& store) {
    if (action != Action::Install) {
        seed(state(action == Action::Clear || action == Action::Context));
        assert(store.load(pairing()) == BrainLoad::Ready);
    }
    io.calls.clear();
}
BrainWrite perform(Action action, BrainStateStore& store) {
    const auto c = context();
    switch (action) {
    case Action::Install: return store.installInitial(pairing(), &c);
    case Action::Context: return store.saveContext(context(11, true));
    case Action::Reserve: return store.reserveLocal(request());
    case Action::Clear: return store.clearPending(request());
    }
    std::abort();
}
bool hasRecord() { return io.disk.at("brainstate").count("record") != 0; }
void writeFailures() {
    for (Action action : {Action::Install, Action::Context, Action::Reserve, Action::Clear}) {
        std::vector<fake::Call> successfulCalls;
        Bytes expected;
        scenario("successful write publishes only after fresh readback " + std::to_string(int(action)), [&] {
            BrainStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            io.before = [&](const fake::Call&) { assert(raw(store.state()) == before); };
            assert(perform(action, store) == BrainWrite::Stored);
            io.before = {};
            assert(store.ready() && !store.faulted());
            expected = encode(store.state());
            assert(io.disk.at("brainstate").at("record").bytes == expected);
            successfulCalls = io.calls;
            assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            recover();
        });
        for (const auto& call : successfulCalls) {
            if (call.op == Op::Close) continue;
            for (esp_err_t error : {ESP_FAIL, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_TYPE_MISMATCH,
                    ESP_ERR_NVS_INVALID_LENGTH, ESP_ERR_NVS_NOT_ENOUGH_SPACE, ESP_ERR_NVS_REMOVE_FAILED}) {
                // A Missing preflight on a never-loaded, explicitly commissioned store is permitted.
                if (action == Action::Install && error == ESP_ERR_NVS_NOT_FOUND &&
                    ((call.op == Op::OpenRO && call.occurrence == 1) ||
                     (call.op == Op::Query && call.occurrence <= 2))) continue;
                const bool writer = call.op == Op::Set || call.op == Op::Commit;
                for (bool apply : {false, true}) {
                    if (apply && !writer) continue;
                    for (bool early : {false, true}) {
                        scenario("write fault action/op/n/error/apply/early " + std::to_string(int(action)) + "/" +
                                 std::to_string(int(call.op)) + "/" + std::to_string(call.occurrence) + "/" +
                                 std::to_string(error) + "/" + std::to_string(apply) + "/" + std::to_string(early), [&] {
                            BrainStateStore store;
                            prepare(action, store);
                            io.durableOnSet = early;
                            const auto before = raw(store.state());
                            const Bytes old = hasRecord() ? io.disk.at("brainstate").at("record").bytes : Bytes{};
                            fake::fail(call.op, call.occurrence, error, apply);
                            io.before = [&](const fake::Call&) { assert(raw(store.state()) == before); };
                            assert(perform(action, store) == BrainWrite::StorageFault);
                            io.before = {};
                            latched(store, before);
                            const Bytes disk = hasRecord() ? io.disk.at("brainstate").at("record").bytes : Bytes{};
                            const bool persisted = fake::count(Op::Set) &&
                                ((call.op == Op::Set && apply) ||
                                 (call.op == Op::Commit && (early || apply)) ||
                                 (call.op != Op::Set && call.op != Op::Commit && fake::count(Op::Commit)));
                            assert(disk == (persisted ? expected : old));
                            recover();
                        });
                    }
                }
            }
            if (call.op != Op::Query && call.op != Op::Read) continue;
            if (action == Action::Install && call.op == Op::Query && call.occurrence <= 2) continue;
            for (size_t length : {size_t(0), size_t(1), kBrainStateMaxSize + 1, SIZE_MAX}) {
                scenario("write-path bad length action/op/n " + std::to_string(int(action)) + "/" +
                         std::to_string(int(call.op)) + "/" + std::to_string(call.occurrence), [&] {
                    BrainStateStore store;
                    prepare(action, store);
                    const auto before = raw(store.state());
                    fake::fail(call.op, call.occurrence, ESP_OK, false, length);
                    assert(perform(action, store) == BrainWrite::StorageFault);
                    latched(store, before);
                    recover();
                });
            }
        }
    }
}

void races() {
    for (unsigned kind = 0; kind < 6; ++kind) {
        scenario("initial record appears between read and RW open " + std::to_string(kind), [&] {
            BrainState initial;
            initial.pairing = pairing();
            initial.hasContext = true;
            initial.context = context();
            auto bytes = encode(initial);
            if (kind == 1) { initial.context.profileVersion++; bytes = encode(initial); }
            if (kind == 2) bytes[8] ^= 1;
            if (kind == 4) bytes.clear();
            if (kind == 5) bytes.resize(kBrainStateMaxSize + 1);
            io.before = [&](const fake::Call& call) {
                if (call.op == Op::OpenRW) seed(bytes, kind == 3 ? fake::Type::String : fake::Type::Blob);
            };
            BrainStateStore store;
            const auto before = raw(store.state());
            assert(perform(Action::Install, store) == (kind == 0 ? BrainWrite::Unchanged :
                   kind == 1 ? BrainWrite::Conflict : BrainWrite::StorageFault));
            assert(fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
            assert(io.disk.at("brainstate").at("record").bytes == bytes);
            if (kind >= 2) latched(store, before);
            io.before = {};
        });
    }
    for (Action action : {Action::Context, Action::Reserve, Action::Clear}) {
        for (Op point : {Op::OpenRO, Op::OpenRW}) {
            for (unsigned kind = 0; kind < 5; ++kind) {
                scenario("external change before mutation/read-write race " + std::to_string(int(action)) + "/" +
                         std::to_string(int(point)) + "/" + std::to_string(kind), [&] {
                    BrainStateStore store;
                    prepare(action, store);
                    const auto before = raw(store.state());
                    auto changed = state();
                    if (kind == 1) changed.localSequence--;
                    if (kind == 2) changed.context.profileVersion++;
                    if (kind == 3) changed.pairing.epoch[0] = 'a';
                    auto bytes = encode(changed);
                    if (kind == 4) bytes[8] ^= 1;
                    bool hit = false;
                    io.before = [&](const fake::Call& call) {
                        if (call.op != point || hit) return;
                        hit = true;
                        if (kind == 0) io.disk["brainstate"].erase("record");
                        else seed(bytes);
                    };
                    assert(perform(action, store) == BrainWrite::StorageFault);
                    assert(hit && !fake::count(Op::Set) && !fake::count(Op::Commit));
                    latched(store, before);
                    io.before = {};
                });
            }
        }
    }
    for (unsigned kind = 0; kind < 6; ++kind) {
        scenario("already-loaded store rejects Missing/rollback/forward/external identity " + std::to_string(kind), [&] {
            seed(state(true));
            BrainStateStore store;
            assert(store.load(pairing()) == BrainLoad::Ready);
            const auto before = raw(store.state());
            if (kind == 0) io.disk["brainstate"].erase("record");
            else {
                auto changed = state(kind >= 2);
                if (kind == 2) changed.context = context(11);
                if (kind == 3) changed.context = context(11, true);
                if (kind == 4) changed.pairing.peerPhysicalId[0] = 'a';
                if (kind == 5) changed.context.babyName[0] = 'X';
                seed(changed);
            }
            const auto result = store.load(pairing());
            assert(result != BrainLoad::Ready && result != BrainLoad::Missing);
            latched(store, before);
            noWrites();
        });
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
        scenario("post-commit readback detects altered durable record " + std::to_string(kind), [&] {
            BrainStateStore store;
            prepare(Action::Reserve, store);
            const auto before = raw(store.state());
            bool hit = false;
            io.before = [&](const fake::Call& call) {
                if (call.op != Op::OpenRO || call.occurrence != 2) return;
                hit = true;
                if (kind == 0) io.disk["brainstate"].erase("record");
                if (kind == 1) seed(state());
                if (kind == 2) { auto s = state(true); s.context = context(11); seed(s); }
                if (kind == 3) { auto bytes = encode(state(true)); bytes[8] ^= 1; seed(bytes); }
                if (kind == 4) seed(encode(state(true)), fake::Type::U32);
            };
            assert(store.reserveLocal(request()) == BrainWrite::StorageFault);
            assert(hit && fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            latched(store, before);
            io.before = {};
        });
    }
    for (unsigned kind = 0; kind < 4; ++kind) {
        scenario("loaded installer cannot reinitialize externally missing/changed state " + std::to_string(kind), [&] {
            BrainState initial;
            initial.pairing = pairing();
            seed(initial);
            BrainStateStore store;
            assert(store.load(pairing()) == BrainLoad::Ready);
            const auto before = raw(store.state());
            if (kind == 0) io.disk["brainstate"].erase("record");
            else {
                auto changed = initial;
                if (kind == 1) changed.localSequence = 1;
                if (kind == 2) { changed.hasContext = true; changed.context = context(); }
                auto bytes = encode(changed);
                if (kind == 3) bytes[8] ^= 1;
                seed(bytes);
            }
            const auto disk = io.disk;
            assert(store.installInitial(pairing()) == BrainWrite::StorageFault);
            latched(store, before);
            assert(io.disk == disk);
            noWrites();
        });
    }
    scenario("loaded store refuses changed pairing even if both caller and disk agree", [] {
        seed(state());
        BrainStateStore store;
        assert(store.load(pairing()) == BrainLoad::Ready);
        const auto before = raw(store.state());
        auto changed = state();
        changed.pairing.epoch[0] = 'a';
        seed(changed);
        assert(store.load(changed.pairing) == BrainLoad::IdentityMismatch);
        latched(store, before);
        noWrites();
    });
    scenario("fresh object can only see restored bytes, not detect backup rollback", [] {
        seed(state());
        const auto old = io.disk.at("brainstate").at("record");
        BrainStateStore first;
        assert(first.load(pairing()) == BrainLoad::Ready);
        assert(first.reserveLocal(request()) == BrainWrite::Stored);
        io.disk["brainstate"]["record"] = old;
        // No external monotonic authority exists in this primitive. Restoring NVS
        // must remain a controlled re-pairing operation, never ordinary recovery.
        fake::reboot();
        BrainStateStore fresh;
        assert(fresh.load(pairing()) == BrainLoad::Ready);
        assert(fresh.state().localSequence == 7 && !fresh.state().pending);
        noWrites();
    });
}

void cryptoFailures() {
    scenario("reserve SHA failure latches without advancing RAM or writing", [] {
        BrainStateStore store;
        prepare(Action::Reserve, store);
        const auto before = raw(store.state());
        const auto disk = io.disk;
        fake_product_crypto::fail = true;
        assert(store.reserveLocal(request()) == BrainWrite::StorageFault);
        latched(store, before);
        assert(io.disk == disk);
        noWrites();
        fake_product_crypto::fail = false;
        latched(store, before);
        recover();
    });
    for (Action action : {Action::Context, Action::Clear}) {
        scenario("pending record SHA verification failure blocks mutations", [&] {
            BrainStateStore store;
            prepare(action, store);
            const auto before = raw(store.state());
            fake_product_crypto::fail = true;
            assert(perform(action, store) == BrainWrite::StorageFault);
            latched(store, before);
            noWrites();
            fake_product_crypto::fail = false;
            recover();
        });
    }
    scenario("SHA failure during load is not Missing and never initializes", [] {
        seed(state(true));
        BrainStateStore store;
        const auto before = raw(store.state());
        fake_product_crypto::fail = true;
        assert(store.load(pairing()) == BrainLoad::Corrupt);
        latched(store, before);
        noWrites();
        fake_product_crypto::fail = false;
        recover();
    });
    scenario("SHA failure after commit cannot publish uncertain RAM", [] {
        BrainStateStore store;
        prepare(Action::Reserve, store);
        const auto before = raw(store.state());
        io.before = [](const fake::Call& call) {
            if (call.op == Op::OpenRO && call.occurrence == 2) fake_product_crypto::fail = true;
        };
        assert(store.reserveLocal(request()) == BrainWrite::StorageFault);
        latched(store, before);
        assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
        io.before = {};
        fake_product_crypto::fail = false;
        recover();
    });
    scenario("SHA failure encoding reserved intent never reaches set", [] {
        BrainStateStore store;
        prepare(Action::Reserve, store);
        const auto before = raw(store.state());
        const auto disk = io.disk;
        io.before = [](const fake::Call& call) {
            // Old non-pending state needs no digest. New pending record does.
            if (call.op == Op::OpenRW) fake_product_crypto::fail = true;
        };
        assert(store.reserveLocal(request()) == BrainWrite::StorageFault);
        latched(store, before);
        assert(io.disk == disk && fake::count(Op::Set) == 0 && fake::count(Op::Commit) == 0);
        io.before = {};
        fake_product_crypto::fail = false;
        recover();
    });
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups{
        {"basics", basics}, {"semantics", semantics}, {"read", readFailures},
        {"write", writeFailures}, {"races", races}, {"crypto", cryptoFailures}};
    bool found = false;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        group.second();
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("BrainStateStore: %u scenarios passed; production codec/store/SHA, SDK fault matrix, "
                "sticky failures, read-only recovery, no erase and namespace preservation\n", scenarios);
}
