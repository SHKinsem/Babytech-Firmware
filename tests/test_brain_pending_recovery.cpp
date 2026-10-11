#include "brain_pending_recovery.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
using fake::Op;
using fake::io;
using State = babytech::brain::BrainPendingRecoveryState;

namespace {
unsigned scenarios = 0;
std::function<void(const ProductRequest&, uint32_t)> acceptanceHook;
void observeAcceptance(const ProductRequest& request, uint32_t at) {
    assert(acceptanceHook); acceptanceHook(request, at);
}

// Deliberately has no motion, Cloud, Status, reserve or boot-loading API.
// Deadlines and session invalidation belong to the real Link, not recovery.
struct FakeLink {
    bool available = true;
    ResultLookupState lookup = ResultLookupState::Idle;
    ResultQuery issued;
    QueriedResult response;
    std::vector<uint32_t> attempts;
    unsigned cancellations = 0;

    bool requestResult(const ResultQuery& query, uint32_t nowMs) {
        v4::Message wire;
        ResultQuery decoded;
        assert(encodeResultQuery(query, wire) && decodeResultQuery(wire, decoded));
        assert(sameResultQuery(decoded, query));
        attempts.push_back(nowMs);
        if (!available || lookup == ResultLookupState::Pending) return false;
        issued = decoded;
        lookup = ResultLookupState::Pending;
        return true;
    }
    ResultLookupState resultLookupState() const { return lookup; }
    const QueriedResult& resultQueryResponse() const { return response; }
    void cancelResultQuery() { ++cancellations; lookup = ResultLookupState::Idle; }
    void complete(const QueriedResult& result, bool codec = true) {
        if (codec) {
            v4::Message wire;
            assert(encodeQueriedResult(result, wire) && decodeQueriedResult(wire, response));
        } else response = result;
        lookup = ResultLookupState::Complete;
    }
};
using Recovery = babytech::brain::BrainPendingRecovery<FakeLink>;
static_assert(sizeof(Recovery) <= sizeof(ResultQuery) + 64, "no ProductRequest/Store/Link copy");
static_assert(!std::is_copy_constructible<Recovery>::value, "one recovery owner");

v4::Pairing pairing() {
    v4::Pairing pair;
    pair.role = v4::Role::Brain;
    std::strcpy(pair.deviceId, "Babytech_pending-test");
    std::strcpy(pair.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(pair.localPhysicalId, "012345abcdef");
    std::strcpy(pair.peerPhysicalId, "fedcba987654");
    return pair;
}
ProductContext context(uint32_t version = 10, bool cleared = false) {
    ProductContext c;
    std::strcpy(c.deviceId, pairing().deviceId);
    c.profileVersion = version;
    c.cleared = cleared;
    if (!cleared) {
        std::strcpy(c.babyId, "original-baby");
        std::strcpy(c.babyName, "Full original baby name");
        std::strcpy(c.formulaBrand, "Full original formula brand");
        c.waterMl = 120;
        c.temperatureC = 40;
        c.powderGPer100Ml = 13.5f;
    }
    assert(validProductContext(c));
    return c;
}
ProductRequest request(ProductCommand command = ProductCommand::Prepare, uint64_t sequence = 1) {
    ProductRequest r;
    r.command = command;
    r.sequence = sequence;
    std::strcpy(r.deviceId, pairing().deviceId);
    assert(makeLocalCommandId(pairing(), sequence, r.commandId));
    if (command == ProductCommand::Prepare) {
        std::strcpy(r.babyId, context().babyId);
        r.profileVersion = 10;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = 13.5f;
    } else if (command == ProductCommand::SetTargetTemp) r.temperatureC = 45;
    assert(validProductRequest(r));
    return r;
}
void noWrites() {
    assert(!fake::count(Op::OpenRW) && !fake::count(Op::Set) && !fake::count(Op::Commit));
}
fake::Database protectedData(bool motion = false) {
    auto disk = io.disk;
    const auto found = disk.find("brainstate");
    if (found != disk.end()) {
        found->second.erase("record");
        if (found->second.empty()) disk.erase(found);
    }
    if (motion) disk["productstate"].erase("record");
    return disk;
}
void scenario(const std::string& name, const std::function<void()>& run, bool motion = false) {
    fake::reset();
    fake_product_crypto::reset();
    acceptanceHook = {};
    for (const char* space : {"productpair", "productstate", "productctx", "outbox",
                              "wifi-cfg", "actuatorcfg", "sensorcfg", "brainstate"})
        io.disk[space]["unrelated"] = {{0, 0xff, 0x7f}, fake::Type::Blob};
    const auto protectedBefore = protectedData(motion);
    std::printf("[%u] %s\n", ++scenarios, name.c_str());
    run();
    fake::verifyFaults();
    assert(io.handles.empty() && protectedData(motion) == protectedBefore);
    assert(!fake::count(Op::Erase) && !fake::count(Op::Init));
    for (const auto& call : io.calls) {
        assert(call.name == "brainstate" || (motion && call.name == "productstate"));
        if (!call.key.empty()) assert(call.key == "record");
    }
    acceptanceHook = {};
}
BrainState persistAndReboot(ProductCommand command = ProductCommand::Prepare,
                            unsigned updatedContext = 0, bool early = false) {
    io.durableOnSet = early;
    BrainState expected;
    {
        BrainStateStore oldBoot;
        const auto c = context();
        assert(oldBoot.installInitial(pairing(), &c) == BrainWrite::Stored);
        assert(oldBoot.reserveLocal(request(command)) == BrainWrite::Stored);
        if (updatedContext) {
            auto newer = context(11, updatedContext == 2);
            if (!newer.cleared) std::strcpy(newer.babyId, "new-baby");
            assert(oldBoot.saveContext(newer) == BrainWrite::Stored);
        }
        expected = oldBoot.state();
        assert(expected.pending && expected.localSequence == 1);
    }
    assert(io.handles.empty());
    fake::reboot();
    fake_product_crypto::reset();
    return expected;
}
void load(BrainStateStore& store, const BrainState& expected) {
    const auto disk = io.disk;
    assert(store.load(pairing()) == BrainLoad::Ready);
    assert(store.ready() && sameBrainState(store.state(), expected));
    assert(io.disk == disk);
    noWrites();
}
void assertPending(const BrainStateStore& store, const BrainState& expected,
                   const fake::Database& disk) {
    assert(store.ready() && sameBrainState(store.state(), expected));
    assert(io.disk == disk);
    noWrites();
}
QueriedResult known(const FakeLink& link, const BrainState& state, bool accepted = true,
                    MotionOutcome outcome = MotionOutcome::None) {
    QueriedResult result;
    result.query = link.issued;
    result.status = ResultQueryStatus::Known;
    result.accepted = accepted;
    result.outcome = outcome;
    std::strcpy(result.reason, accepted ? "accepted" : "not_ready");
    const auto r = request(state.pendingRequest.command, state.pendingRequest.sequence);
    uint8_t digest[kProductDigestSize];
    assert(requestDigest(r, digest));
    assert(!std::memcmp(digest, state.pendingDigest, sizeof(digest)));
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        result.requestDigestHex[2 * i] = hex[digest[i] >> 4];
        result.requestDigestHex[2 * i + 1] = hex[digest[i] & 15];
    }
    return result;
}
void start(Recovery& recovery, FakeLink& link, const BrainState& expected, uint32_t at = 0) {
    const auto calls = io.calls.size();
    recovery.poll(at);
    assert(recovery.state() == State::Querying && !recovery.clearFault());
    assert(link.attempts.size() == 1 && link.attempts.back() == at);
    assert(link.issued.source == expected.pendingRequest.source);
    assert(link.issued.sequence == expected.pendingRequest.sequence);
    assert(!std::strcmp(link.issued.deviceId, expected.pendingRequest.deviceId));
    assert(!std::strcmp(link.issued.commandId, expected.pendingRequest.commandId));
    assert(io.calls.size() == calls);
    noWrites();
}
void resolved(Recovery& recovery, FakeLink& link, BrainStateStore& store,
              const BrainState& before, uint32_t at = 1) {
    recovery.poll(at);
    assert(recovery.state() == State::Resolved && !recovery.clearFault());
    assert(store.ready() && !store.state().pending && store.state().localSequence == before.localSequence);
    assert(sameProductContext(store.state().context, before.context));
    assert(fake::count(Op::OpenRW) == 1 && fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
    const auto calls = io.calls.size();
    const auto attempts = link.attempts.size();
    const auto cancels = link.cancellations;
    for (uint32_t now : {at + 1, at + 1000, at + 100000}) recovery.poll(now);
    assert(recovery.state() == State::Resolved && io.calls.size() == calls);
    assert(link.attempts.size() == attempts && link.cancellations == cancels);
    fake::reboot();
    BrainStateStore rebooted;
    assert(rebooted.load(pairing()) == BrainLoad::Ready);
    assert(!rebooted.state().pending && rebooted.state().localSequence == before.localSequence);
    assert(sameProductContext(rebooted.state().context, before.context));
    noWrites();
}

void basics() {
    scenario("not loaded: no implicit load; same store becomes ready later", [] {
        const auto expected = persistAndReboot();
        BrainStateStore store;
        FakeLink link;
        Recovery recovery(link, store);
        recovery.poll(0);
        assert(recovery.state() == State::StoreUnavailable && io.calls.empty());
        assert(link.attempts.empty());
        load(store, expected);
        start(recovery, link, expected, 1);
    });
    scenario("ready without pending: no query/reservation/sequence increment", [] {
        BrainStateStore store;
        const auto c = context();
        assert(store.installInitial(pairing(), &c) == BrainWrite::Stored);
        fake::reboot();
        BrainStateStore fresh;
        assert(fresh.load(pairing()) == BrainLoad::Ready);
        FakeLink link;
        Recovery recovery(link, fresh);
        const auto calls = io.calls.size();
        for (uint32_t now : {0u, 1000u, 0xffffffffu}) recovery.poll(now);
        assert(recovery.state() == State::Idle && fresh.state().localSequence == 0);
        assert(link.attempts.empty() && !link.cancellations && io.calls.size() == calls);
        noWrites();
    });
    for (bool corrupt : {false, true}) scenario("missing/corrupt storage remains untouched", [=] {
        if (corrupt) {
            persistAndReboot();
            io.disk["brainstate"]["record"].bytes.back() ^= 1;
        }
        const auto disk = io.disk;
        BrainStateStore store;
        assert(store.load(pairing()) == (corrupt ? BrainLoad::Corrupt : BrainLoad::Missing));
        FakeLink link;
        Recovery recovery(link, store);
        const auto calls = io.calls.size();
        recovery.poll(0);
        recovery.poll(1000);
        assert(recovery.state() == State::StoreUnavailable && link.attempts.empty());
        assert(io.calls.size() == calls && io.disk == disk);
        noWrites();
    });
}

void restore() {
    for (unsigned command = 1; command <= 6; ++command)
        for (bool accepted : {false, true}) scenario("rebooted ordinary command " + std::to_string(command) +
                                                     (accepted ? " accepted" : " rejected"), [=] {
            const auto expected = persistAndReboot(ProductCommand(command));
            BrainStateStore store;
            load(store, expected);
            FakeLink link;
            Recovery recovery(link, store);
            start(recovery, link, expected);
            link.complete(known(link, expected, accepted));
            resolved(recovery, link, store, expected);
        });
    for (unsigned update : {1u, 2u}) for (bool early : {false, true})
        scenario("original pending survives newer context/tombstone and NVS mode", [=] {
            const auto expected = persistAndReboot(ProductCommand::Prepare, update, early);
            BrainStateStore store;
            load(store, expected);
            FakeLink link;
            Recovery recovery(link, store);
            start(recovery, link, expected);
            link.complete(known(link, expected));
            resolved(recovery, link, store, expected);
        });
    for (auto outcome : {MotionOutcome::None, MotionOutcome::Succeeded,
                         MotionOutcome::Failed, MotionOutcome::Interrupted})
        scenario("ACK resolution is independent of execution outcome", [=] {
            const auto expected = persistAndReboot();
            BrainStateStore store;
            load(store, expected);
            FakeLink link;
            Recovery recovery(link, store);
            start(recovery, link, expected);
            link.complete(known(link, expected, true, outcome));
            resolved(recovery, link, store, expected);
        });
    scenario("context updated in flight does not rewrite query/digest", [] {
        const auto expected = persistAndReboot();
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        const auto newer = context(11, true);
        assert(store.saveContext(newer) == BrainWrite::Stored);
        const auto changed = store.state();
        // Discard call history, not bytes or this same ready store.
        io.calls.clear();
        link.complete(known(link, expected));
        resolved(recovery, link, store, changed);
    });
}

void acceptanceCallbacks() {
    for (auto command : {ProductCommand::Prepare, ProductCommand::Initialize, ProductCommand::Clean,
                         ProductCommand::SetTargetTemp, ProductCommand::ResetError, ProductCommand::CheckFirmwareUpdate})
        for (bool accepted : {false, true})
            scenario("recovered callback frozen request before clear / " + std::to_string(unsigned(command)) +
                     (accepted ? " accepted" : " rejected"), [=] {
                const auto expected = persistAndReboot(command, 2);
                BrainStateStore store; load(store, expected);
                FakeLink link; Recovery recovery(link, store); start(recovery, link, expected, 100);
                const auto disk = io.disk; const auto callsBefore = io.calls.size();
                unsigned calls = 0;
                recovery.setAcceptanceHandler(observeAcceptance);
                acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
                    ++calls; assert(sameProductRequest(request, expected.pendingRequest) && at == 123);
                    assert(store.state().pending && sameBrainState(store.state(), expected));
                    assert(io.disk == disk && io.calls.size() == callsBefore); noWrites();
                };
                link.complete(known(link, expected, accepted));
                recovery.poll(123);
                assert(calls == unsigned(accepted) && recovery.state() == State::Resolved && !store.state().pending);
                recovery.poll(124); recovery.poll(1000); assert(calls == unsigned(accepted));
            });
    for (unsigned fault = 0; fault < 9; ++fault)
        scenario("recovered callback excludes wrong/uncertain proof / " + std::to_string(fault), [=] {
            const auto expected = persistAndReboot();
            BrainStateStore store; load(store, expected);
            FakeLink link; Recovery recovery(link, store); start(recovery, link, expected, 100);
            const auto disk = io.disk;
            unsigned calls = 0;
            recovery.setAcceptanceHandler(observeAcceptance);
            acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
                ++calls; assert(sameProductRequest(request, expected.pendingRequest) && at == 1124);
                assert(store.state().pending && io.disk == disk); noWrites();
            };
            auto reply = known(link, expected);
            if (fault == 0) reply.query.source = v4::Source::CloudCommand;
            if (fault == 1) ++reply.query.sequence;
            if (fault == 2) std::strcpy(reply.query.deviceId, "other-device");
            if (fault == 3) std::strcpy(reply.query.commandId, "other-command");
            if (fault == 4) reply.requestDigestHex[0] = reply.requestDigestHex[0] == '0' ? '1' : '0';
            if (fault >= 5) {
                reply.status = ResultQueryStatus(fault - 4);
                reply.requestDigestHex[0] = 0;
                const char* reasons[] = {"unknown", "result_expired", "request_conflict", "storage_fault"};
                std::strcpy(reply.reason, reasons[fault - 5]);
            }
            link.complete(reply, fault < 5); recovery.poll(123);
            assert(!calls && recovery.state() == State::Waiting); assertPending(store, expected, disk);
            recovery.poll(1123); link.complete(known(link, expected)); recovery.poll(1124);
            assert(calls == 1 && !store.state().pending); recovery.poll(1125); assert(calls == 1);
        });
    scenario("recovered acceptance callback survives pending-clear storage failure", [] {
        const auto expected = persistAndReboot();
        BrainStateStore store; load(store, expected);
        FakeLink link; Recovery recovery(link, store); start(recovery, link, expected, 100);
        unsigned calls = 0;
        recovery.setAcceptanceHandler(observeAcceptance);
        acceptanceHook = [&](const ProductRequest& request, uint32_t at) {
            ++calls; assert(sameProductRequest(request, expected.pendingRequest) && at == 123);
            assert(store.state().pending); noWrites();
        };
        fake::fail(Op::Set, 1);
        link.complete(known(link, expected)); recovery.poll(123); recovery.poll(124);
        assert(calls == 1 && store.state().pending && recovery.clearFault());
    });
}

void mismatches() {
    for (unsigned field = 0; field < 7; ++field) scenario("Known identity mismatch " + std::to_string(field), [=] {
        const auto expected = persistAndReboot();
        const auto disk = io.disk;
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        auto result = known(link, expected);
        if (field == 0) result.query.source = v4::Source::CloudCommand;
        if (field == 1) result.query.deviceId[0] = 'X';
        if (field == 2) result.query.commandId[0] = 'X';
        if (field == 3) ++result.query.sequence;
        if (field == 4) result.query.source = v4::Source(99);
        if (field == 5) std::memset(result.query.deviceId, 'a', sizeof(result.query.deviceId));
        if (field == 6) std::memset(result.query.commandId, 'a', sizeof(result.query.commandId));
        link.complete(result, field < 4);
        recovery.poll(1);
        assert(recovery.state() == State::Waiting && link.cancellations == 1);
        assertPending(store, expected, disk);
    });
    for (unsigned nibble = 0; nibble < 64; ++nibble)
        scenario("Known digest mismatch at nibble " + std::to_string(nibble), [=] {
            const auto expected = persistAndReboot();
            const auto disk = io.disk;
            BrainStateStore store;
            load(store, expected);
            FakeLink link;
            Recovery recovery(link, store);
            start(recovery, link, expected);
            auto result = known(link, expected);
            result.requestDigestHex[nibble] = result.requestDigestHex[nibble] == '0' ? '1' : '0';
            link.complete(result);
            recovery.poll(1);
            assert(recovery.state() == State::Waiting && !recovery.clearFault());
            assertPending(store, expected, disk);
        });
    for (unsigned shape = 0; shape < 6; ++shape) scenario("Known malformed digest " + std::to_string(shape), [=] {
        const auto expected = persistAndReboot();
        const auto disk = io.disk;
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        auto result = known(link, expected);
        if (shape == 0) result.requestDigestHex[0] = 0;
        if (shape == 1) result.requestDigestHex[63] = 0;
        if (shape == 2) result.requestDigestHex[64] = 'a';
        if (shape == 3) std::memset(result.requestDigestHex, 'f', sizeof(result.requestDigestHex));
        if (shape == 4) result.requestDigestHex[0] = 'g';
        if (shape == 5) {
            bool changed = false;
            for (unsigned i = 0; i < 64; ++i) if (result.requestDigestHex[i] >= 'a') {
                result.requestDigestHex[i] -= 'a' - 'A';
                changed = true;
            }
            assert(changed);
        }
        link.complete(result, false);
        recovery.poll(1);
        assertPending(store, expected, disk);
    });
}

void retries() {
    for (unsigned status = 1; status <= 4; ++status)
        scenario("uncertain result retains pending; 1s read-only retry " + std::to_string(status), [=] {
            const auto expected = persistAndReboot();
            const auto disk = io.disk;
            BrainStateStore store;
            load(store, expected);
            FakeLink link;
            Recovery recovery(link, store);
            start(recovery, link, expected);
            QueriedResult result;
            result.query = link.issued;
            result.status = ResultQueryStatus(status);
            const char* reasons[] = {"known", "unknown", "result_expired", "request_conflict", "storage_fault"};
            std::strcpy(result.reason, reasons[status]);
            const auto calls = io.calls.size();
            for (uint32_t attempt = 0; attempt < 3; ++attempt) {
                link.complete(result);
                const uint32_t end = attempt * 1001 + 1;
                recovery.poll(end);
                for (uint32_t wait = 0; wait < 1000; ++wait) recovery.poll(end + wait);
                assert(link.attempts.size() == attempt + 1);
                recovery.poll(end + 1000);
                assert(link.attempts.size() == attempt + 2 && recovery.state() == State::Querying);
                assert(sameResultQuery(link.issued, result.query));
                assert(io.calls.size() == calls);
                assertPending(store, expected, disk);
            }
            link.complete(known(link, expected));
            resolved(recovery, link, store, expected, 4000);
        });
    for (auto terminal : {ResultLookupState::Idle, ResultLookupState::TimedOut, ResultLookupState::Unavailable})
        scenario("transport terminal state retains pending and retries", [=] {
            const auto expected = persistAndReboot();
            const auto disk = io.disk;
            BrainStateStore store;
            load(store, expected);
            FakeLink link;
            Recovery recovery(link, store);
            start(recovery, link, expected, 10);
            recovery.poll(11);
            assert(recovery.state() == State::Querying && link.attempts.size() == 1);
            link.lookup = terminal;
            recovery.poll(1010);
            recovery.poll(2009);
            assert(link.attempts.size() == 1 && link.cancellations == 1);
            recovery.poll(2010);
            assert(link.attempts.size() == 2);
            assertPending(store, expected, disk);
        });
    for (bool occupied : {false, true}) scenario("failed start is bounded and does not cancel another query", [=] {
        const auto expected = persistAndReboot();
        const auto disk = io.disk;
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        link.available = occupied;
        if (occupied) link.lookup = ResultLookupState::Pending;
        Recovery recovery(link, store);
        recovery.poll(0);
        for (uint32_t now = 1; now < 1000; ++now) recovery.poll(now);
        assert(recovery.state() == State::Waiting && link.attempts.size() == 1);
        recovery.poll(1000);
        assert(link.attempts.size() == 2 && !link.cancellations);
        assertPending(store, expected, disk);
        link.available = true;
        link.lookup = ResultLookupState::Idle;
        recovery.poll(2000);
        assert(recovery.state() == State::Querying && link.attempts.size() == 3);
    });
    for (bool started : {false, true}) scenario("1s retry survives uint32 wrap", [=] {
        const auto expected = persistAndReboot();
        const auto disk = io.disk;
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        link.available = started;
        Recovery recovery(link, store);
        constexpr uint32_t at = 0xffffff00u;
        recovery.poll(at);
        const uint32_t end = started ? at + 10 : at;
        if (started) { link.lookup = ResultLookupState::TimedOut; recovery.poll(end); }
        recovery.poll(end + 999);
        assert(link.attempts.size() == 1);
        link.available = true;
        recovery.poll(end + 1000);
        assert(link.attempts.size() == 2 && link.attempts.back() == uint32_t(end + 1000));
        assertPending(store, expected, disk);
    });
}

void maintenance() {
    for (unsigned timing = 0; timing < 3; ++timing) scenario("maintenance pauses before/during/after reply", [=] {
        const auto expected = persistAndReboot();
        const auto disk = io.disk;
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        if (timing) start(recovery, link, expected);
        if (timing == 2) link.complete(known(link, expected));
        const auto calls = io.calls.size();
        recovery.poll(1, true);
        assert(recovery.state() == State::Paused && !recovery.clearFault());
        for (uint32_t now = 2; now < 1001; ++now) recovery.poll(now, true);
        assert(link.attempts.size() == (timing ? 1u : 0u));
        assert(link.cancellations == (timing ? 1u : 0u));
        assert(io.calls.size() == calls);
        assertPending(store, expected, disk);
        if (timing) {
            // A stale completion after cancellation is not owned evidence.
            link.complete(known(link, expected));
            recovery.poll(1000);
            assert(recovery.state() == State::Waiting);
            assertPending(store, expected, disk);
        }
        recovery.poll(1001);
        assert(recovery.state() == State::Querying);
        link.complete(known(link, expected));
        resolved(recovery, link, store, expected, 1002);
    });
    scenario("replacement pending cannot be cleared by old query", [] {
        const auto expected = persistAndReboot(ProductCommand::Clean);
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        const auto oldReply = known(link, expected);
        assert(store.clearPending(expected.pendingRequest) == BrainWrite::Stored);
        assert(store.reserveLocal(request(ProductCommand::Clean, 2)) == BrainWrite::Stored);
        const auto replacement = store.state();
        const auto disk = io.disk;
        io.calls.clear();
        link.complete(oldReply);
        recovery.poll(1);
        assert(recovery.state() == State::Waiting && link.cancellations == 1);
        assertPending(store, replacement, disk);
        recovery.poll(1001);
        assert(link.issued.sequence == 2);
        link.complete(oldReply);
        recovery.poll(1002);
        assertPending(store, replacement, disk);
        recovery.poll(2002);
        link.complete(known(link, replacement));
        resolved(recovery, link, store, replacement, 2003);
    });
    scenario("external pending resolution cancels owned transient query", [] {
        const auto expected = persistAndReboot();
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        assert(store.clearPending(expected.pendingRequest) == BrainWrite::Stored);
        const auto calls = io.calls.size();
        recovery.poll(1);
        recovery.poll(1001);
        assert(recovery.state() == State::Idle && link.cancellations == 1);
        assert(link.attempts.size() == 1 && io.calls.size() == calls);
    });
}

void failures() {
    struct Injection { Op op; unsigned next; };
    const Injection faults[] = {{Op::OpenRO, 1}, {Op::Query, 1}, {Op::Read, 1},
        {Op::OpenRW, 1}, {Op::Query, 2}, {Op::Read, 2}, {Op::Set, 1}, {Op::Commit, 1},
        {Op::OpenRO, 2}, {Op::Query, 3}, {Op::Read, 3}};
    for (bool early : {false, true}) for (const auto& fault : faults)
        for (bool apply : {false, true}) {
            if (apply && fault.op != Op::Set && fault.op != Op::Commit) continue;
            scenario("clear fault op " + std::to_string(unsigned(fault.op)) +
                     " occurrence " + std::to_string(fault.next) + (early ? " early" : " deferred") +
                     (apply ? " applied-error" : " error"), [=] {
                const auto expected = persistAndReboot(ProductCommand::Prepare, 2, early);
                BrainStateStore store;
                load(store, expected);
                FakeLink link;
                Recovery recovery(link, store);
                start(recovery, link, expected);
                link.complete(known(link, expected));
                fake::fail(fault.op, fake::count(fault.op) + fault.next, ESP_FAIL, apply);
                recovery.poll(1);
                fake::verifyFaults();
                assert(recovery.state() == State::ClearFault && recovery.clearFault());
                assert(store.faulted() && !store.ready() && sameBrainState(store.state(), expected));
                const auto calls = io.calls.size();
                const auto disk = io.disk;
                for (uint32_t now : {2u, 1001u, 100000u, 0xffffffffu}) {
                    recovery.poll(now);
                    recovery.poll(now, true);
                }
                assert(recovery.state() == State::ClearFault && io.calls.size() == calls);
                assert(link.attempts.size() == 1 && link.cancellations == 1 && io.disk == disk);
                // Flash may already contain the verified new bytes even when
                // commit/readback reports failure. Only a fresh boot can load it.
                fake::reboot();
                BrainStateStore fresh;
                assert(fresh.load(pairing()) == BrainLoad::Ready);
                const bool wrote = fault.op == Op::OpenRO && fault.next == 2;
                const bool readback = (fault.op == Op::Query || fault.op == Op::Read) && fault.next == 3;
                const bool persisted = wrote || readback || apply ||
                    (early && fault.op == Op::Commit);
                assert(fresh.state().pending != persisted);
                assert(fresh.state().localSequence == 1 && sameProductContext(fresh.state().context, expected.context));
                noWrites();
            });
        }
    scenario("store fault in flight cancels query without repeated I/O", [] {
        const auto expected = persistAndReboot();
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        fake::fail(Op::OpenRO, fake::count(Op::OpenRO) + 1);
        assert(store.load(pairing()) == BrainLoad::IoError);
        fake::verifyFaults();
        const auto calls = io.calls.size();
        const auto disk = io.disk;
        for (uint32_t now : {1u, 1001u, 100000u}) recovery.poll(now);
        assert(recovery.state() == State::StoreUnavailable && !recovery.clearFault());
        assert(link.cancellations == 1 && link.attempts.size() == 1);
        assert(io.calls.size() == calls && io.disk == disk);
        noWrites();
    });
    scenario("SHA failure during definitive clear latches without writes", [] {
        const auto expected = persistAndReboot();
        BrainStateStore store;
        load(store, expected);
        FakeLink link;
        Recovery recovery(link, store);
        start(recovery, link, expected);
        link.complete(known(link, expected));
        fake_product_crypto::fail = true;
        recovery.poll(1);
        assert(recovery.clearFault() && recovery.state() == State::ClearFault);
        const auto calls = io.calls.size();
        recovery.poll(1001);
        assert(io.calls.size() == calls);
        noWrites();
        fake_product_crypto::reset();
        assert(sameBrainState(store.state(), expected));
    });
}

// The handler is exactly the production snapshot lookup over the loaded store.
const MotionStateStore* handlerStore = nullptr;
unsigned handlerCalls = 0;
bool answerFromMotion(const ResultQuery& query, QueriedResult& result) {
    assert(handlerStore);
    ++handlerCalls;
    return queryMotionResult(*handlerStore, query, result);
}
struct ShortWire : v4::ByteSink {
    std::vector<uint8_t> bytes;
    v4::Parser observer;
    unsigned writes = 0, shortWrites = 0, zeroWrites = 0, queries = 0, results = 0;
    bool idle() const override { return bytes.empty(); }
    size_t available() const override { return 128 - bytes.size(); }
    size_t write(const uint8_t* input, size_t size) override {
        assert(size <= available());
        const size_t taken = (++writes % 5) ? std::min(size, size_t(17)) : 0;
        if (taken < size) ++shortWrites;
        if (!taken) ++zeroWrites;
        bytes.insert(bytes.end(), input, input + taken);
        return taken;
    }
    void deliver(ReadOnlyLink& peer, uint32_t now) {
        for (uint8_t byte : bytes) {
            v4::Frame frame;
            if (observer.push(byte, now, frame)) {
                // Check every decoded frame, including all message fragments.
                assert(frame.kind == v4::Kind::Hello || frame.kind == v4::Kind::HelloAck ||
                       frame.kind == v4::Kind::Heartbeat || frame.kind == v4::Kind::ResultQuery ||
                       frame.kind == v4::Kind::Result || frame.kind == v4::Kind::LinkAck ||
                       frame.kind == v4::Kind::LinkReject);
                if (!frame.offset && frame.kind == v4::Kind::ResultQuery) ++queries;
                if (!frame.offset && frame.kind == v4::Kind::Result) ++results;
            }
            peer.receive(byte, now);
        }
        bytes.clear();
    }
};
void production() {
    for (bool accepted : {false, true}) scenario(accepted ? "production UART Known acceptance after NVS reboot"
                                                        : "production UART Known rejection after NVS reboot", [=] {
        const auto expected = persistAndReboot();
        auto motionPair = pairing();
        motionPair.role = v4::Role::Motion;
        std::swap(motionPair.localPhysicalId, motionPair.peerPhysicalId);
        {
            MotionStateStore oldMotion;
            const auto c = context();
            assert(oldMotion.installInitial(motionPair, &c) == MotionWrite::Stored);
            assert(oldMotion.recordDecision(expected.pendingRequest, accepted,
                accepted ? "accepted" : "not_ready", accepted ? "11111111111111111111111111111111" : nullptr)
                == MotionWrite::Stored);
        }
        fake::reboot();
        BrainStateStore brainStore;
        MotionStateStore motionStore;
        load(brainStore, expected);
        assert(motionStore.load(motionPair) == MotionLoad::Ready);
        const auto motionBefore = motionStore.state();
        const auto diskBefore = io.disk;
        const auto callsBefore = io.calls.size();
        ReadOnlyLink brain, motion;
        ShortWire toMotion, toBrain;
        assert(brain.begin(pairing(), 101) && motion.begin(motionPair, 202));
        handlerStore = &motionStore;
        handlerCalls = 0;
        assert(motion.setResultQueryHandler(answerFromMotion));
        uint32_t now = 0;
        auto step = [&] {
            brain.poll(now, toMotion);
            motion.poll(now, toBrain);  // No STATUS, Cloud or motor handler.
            toMotion.deliver(motion, now);
            toBrain.deliver(brain, now++);
        };
        for (unsigned tick = 0; tick < 200; ++tick) step();
        assert(brain.connected(now) && motion.connected(now) && !brain.freshStatus(now));
        babytech::brain::BrainPendingRecovery<ReadOnlyLink> recovery(brain, brainStore);
        recovery.poll(now);
        assert(recovery.state() == State::Querying);
        for (unsigned tick = 0; tick < 500 && brain.resultLookupState() != ResultLookupState::Complete; ++tick)
            step();
        assert(brain.resultLookupState() == ResultLookupState::Complete && handlerCalls == 1);
        const auto& result = brain.resultQueryResponse();
        assert(result.status == ResultQueryStatus::Known && result.accepted == accepted);
        assert(result.outcome == MotionOutcome::None && result.query.sequence == 1);
        assert(io.calls.size() == callsBefore && io.disk == diskBefore);
        recovery.poll(now);
        assert(recovery.state() == State::Resolved && !recovery.clearFault());
        assert(!brainStore.state().pending && brainStore.state().localSequence == 1);
        assert(sameMotionState(motionStore.state(), motionBefore));
        assert(io.disk.at("productstate") == diskBefore.at("productstate"));
        assert(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
        const auto callsAfter = io.calls.size();
        for (unsigned tick = 0; tick < 200; ++tick) { step(); recovery.poll(now); }
        assert(io.calls.size() == callsAfter && toMotion.queries == 1 && toBrain.results == 1);
        assert(!toBrain.queries && !toMotion.results);
        assert(toMotion.shortWrites && toBrain.shortWrites && toMotion.zeroWrites && toBrain.zeroWrites);
        std::printf("  production UART: queries=%u results=%u handler=%u short-writes=%u zero-writes=%u\n",
                    toMotion.queries, toBrain.results, handlerCalls,
                    toMotion.shortWrites + toBrain.shortWrites, toMotion.zeroWrites + toBrain.zeroWrites);
        handlerStore = nullptr;
        fake::reboot();
        BrainStateStore fresh;
        assert(fresh.load(pairing()) == BrainLoad::Ready && !fresh.state().pending);
        assert(fresh.state().localSequence == 1 && sameProductContext(fresh.state().context, expected.context));
        noWrites();
    }, true);
}
}  // namespace

int main(int argc, char** argv) {
    const std::string selected = argc == 2 ? argv[1] : "all";
    const struct { const char* name; void (*run)(); } groups[] = {
        {"basics", basics}, {"restore", restore}, {"mismatches", mismatches},
        {"retries", retries}, {"maintenance", maintenance}, {"failures", failures},
        {"production", production}, {"acceptance", acceptanceCallbacks}};
    bool found = selected == "all";
    for (const auto& group : groups) if (selected == "all" || selected == group.name) {
        found = true;
        group.run();
    }
    if (!found || argc > 2) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("Brain pending recovery: %u scenarios passed; SDK SHA major %d; owner %zu bytes\n",
                scenarios, MBEDTLS_VERSION_MAJOR, sizeof(Recovery));
    return 0;
}
