#include "BoardCommissioning.h"
#include "FakeCommissioning.h"
#include "FakeProductCrypto.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace babytech::boardlink;
namespace v4 = babytech::v4;
namespace fake = fake_brain;
namespace extra = fake_commissioning;
using fake::Bytes;
using fake::Op;
using fake::io;
using Result = CommissioningResult;

namespace {
unsigned scenarios = 0, failures = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)
constexpr const char* activeJson = R"({"type":"feeding_context","device_id":"Babytech_01-test","baby_id":"baby-1","baby_name":"Full baby name","formula_brand":"Full formula brand","water_ml":180,"temp":45,"powder_g_per_100ml":25,"profile_version":12345})";
constexpr const char* clearedJson = R"({"type":"feeding_context","device_id":"Babytech_01-test","cleared":true,"profile_version":2147483647})";
constexpr const char* execution = "123456789abcdef0123456789abcdef0";

fake::Value stringValue(const std::string& value) {
    Bytes bytes(value.begin(), value.end());
    bytes.push_back(0);
    return {bytes, fake::Type::String};
}

CommissioningImport request(bool motion, int contextKind = 1) {
    CommissioningImport r;
    r.pairing.role = motion ? v4::Role::Motion : v4::Role::Brain;
    std::strcpy(r.pairing.deviceId, "Babytech_01-test");
    std::strcpy(r.pairing.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(r.pairing.localPhysicalId, "012345abcdef");
    std::strcpy(r.pairing.peerPhysicalId, "fedcba987654");
    r.hasContext = contextKind != 0;
    if (r.hasContext) {
        const char* json = contextKind == 2 ? clearedJson : activeJson;
        CHECK(decodeProductContext(reinterpret_cast<const uint8_t*>(json), std::strlen(json),
                                   r.pairing.deviceId, r.context));
    }
    return r;
}

void seedLegacy(int kind) {
    if (kind) io.disk["productctx"]["payload"] = stringValue(kind == 2 ? clearedJson : activeJson);
}

Bytes pairBytes(const v4::Pairing& p) {
    std::array<uint8_t, kPairingRecordMaxSize> bytes{};
    const size_t n = encodePairingRecord(p, bytes.data(), bytes.size());
    CHECK(n);
    return Bytes(bytes.begin(), bytes.begin() + n);
}

bool present(const char* name) {
    const auto found = io.disk.find(name);
    return found != io.disk.end() && found->second.count("record");
}

const char* stateSpace(bool motion) { return motion ? "productstate" : "brainstate"; }

fake::Database protectedData(bool motion) {
    auto disk = io.disk;
    for (const char* name : {"productpair", stateSpace(motion)}) {
        auto found = disk.find(name);
        if (found != disk.end()) {
            found->second.erase("record");
            if (found->second.empty()) disk.erase(found);
        }
    }
    return disk;
}

void noWrites() {
    CHECK(fake::count(Op::OpenRW) == 0);
    CHECK(fake::count(Op::Set) == 0);
    CHECK(fake::count(Op::Commit) == 0);
}

void audit(bool motion) {
    CHECK(io.handles.empty());
    CHECK(fake::count(Op::Erase) == 0 && fake::count(Op::Init) == 0);
    for (const auto& call : io.calls) {
        if (call.op == Op::OpenRW || call.op == Op::Set || call.op == Op::Commit) {
            CHECK(call.name == stateSpace(motion) || call.name == "productpair");
            if (call.op == Op::Set) CHECK(call.key == "record");
        }
    }
}

struct Guard : CommissioningGuard {
    unsigned calls = 0, denyAt = 0;
    v4::Role expected;
    CommissioningImport expectedRequest;
    bool stationary = true, operatorApproved = true, mqttHandoff = true;
    bool motionExportVerified = true;
    std::function<void(unsigned)> before;
    explicit Guard(bool motion) : expected(motion ? v4::Role::Motion : v4::Role::Brain) {}
    bool allowImport(const CommissioningImport& request) override {
        const auto role = request.pairing.role;
        CHECK(role == expected);
        CHECK(pairBytes(request.pairing) == pairBytes(expectedRequest.pairing));
        CHECK(request.hasContext == expectedRequest.hasContext);
        if (request.hasContext) CHECK(sameProductContext(request.context, expectedRequest.context));
        ++calls;
        if (before) before(calls);
        return calls != denyAt && stationary && operatorApproved && mqttHandoff &&
            (role != v4::Role::Brain || motionExportVerified);
    }
};

struct Session {
    bool motion;
    BoardCommissioning coordinator;
    BrainStateStore brain;
    MotionStateStore motor;
    Guard guard;
    explicit Session(bool isMotion) : motion(isMotion), guard(isMotion) {}
    Result run(const CommissioningImport& r) {
        guard.expectedRequest = r;
        const auto result = motion ? coordinator.importMotion(r, motor, guard)
                                   : coordinator.importBrain(r, brain, guard);
        audit(motion);
        return result;
    }
    void initial(const CommissioningImport& r) {
        const auto* context = r.hasContext ? &r.context : nullptr;
        if (motion) CHECK(motor.installInitial(r.pairing, context) == MotionWrite::Stored);
        else CHECK(brain.installInitial(r.pairing, context) == BrainWrite::Stored);
    }
    bool storeFaulted() const { return motion ? motor.faulted() : brain.faulted(); }
};

void verifyInitial(const CommissioningImport& r, bool motion) {
    CHECK(present(stateSpace(motion)));
    const auto& bytes = io.disk.at(stateSpace(motion)).at("record").bytes;
    if (motion) {
        MotionState actual, expected;
        expected.pairing = r.pairing;
        if (r.hasContext) CHECK(makeMotionContextBarrier(r.context, expected.context));
        CHECK(decodeMotionState(bytes.data(), bytes.size(), actual));
        CHECK(sameMotionState(actual, expected));
    } else {
        BrainState actual, expected;
        expected.pairing = r.pairing;
        expected.hasContext = r.hasContext;
        expected.context = r.context;
        CHECK(decodeBrainState(bytes.data(), bytes.size(), actual));
        CHECK(sameBrainState(actual, expected));
    }
}

void verifyPair(const CommissioningImport& r) {
    CHECK(present("productpair"));
    CHECK(io.disk.at("productpair").at("record").bytes == pairBytes(r.pairing));
}

void assertSticky(Session& s, const CommissioningImport& r) {
    CHECK(s.coordinator.faulted());
    const auto disk = io.disk;
    const auto calls = io.calls.size();
    const auto guards = s.guard.calls;
    CHECK(s.run(r) == Result::StorageFault);
    CHECK(io.calls.size() == calls && s.guard.calls == guards && io.disk == disk);
    // Changing the supplied store cannot reset the persistent coordinator.
    BrainStateStore brain;
    MotionStateStore motion;
    const auto result = s.motion ? s.coordinator.importMotion(r, motion, s.guard)
                                 : s.coordinator.importBrain(r, brain, s.guard);
    CHECK(result == Result::StorageFault && io.calls.size() == calls);
}

void scenario(const std::string& name, const std::function<void()>& body) {
    fake::reset();
    extra::reset();
    fake_product_crypto::reset();
    for (const char* nameSpace : {"wifi-cfg", "actuatorcfg", "sensorcfg", "outbox"})
        io.disk[nameSpace]["payload"] = {{0xff, 0, 1}, fake::Type::Blob};
    for (const char* nameSpace : {"productpair", "productstate", "brainstate", "formulaevt", "productctx"})
        io.disk[nameSpace]["unrelated"] = {{1, 2, 3, 4}, fake::Type::U32};
    ++scenarios;
    try {
        body();
        CHECK(io.handles.empty());
        fake::verifyFaults();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        fake::reboot();
    }
}

void success() {
    for (bool motion : {false, true}) for (int kind : {0, 1, 2}) for (bool early : {false, true}) {
        scenario("initial, exact idempotence, tombstone, namespaces", [=] {
            io.durableOnSet = early;
            seedLegacy(kind);
            const auto r = request(motion, kind);
            const auto protectedBefore = protectedData(motion);
            Session s(motion);
            s.guard.before = [&](unsigned phase) {
                if (phase <= 2) noWrites();
                else {
                    CHECK(phase == 3);
                    verifyInitial(r, motion);
                    CHECK(!present("productpair"));
                    CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
                    CHECK(std::any_of(io.calls.begin(), io.calls.end(), [=](const fake::Call& c) {
                        return c.op == Op::Read && c.name == stateSpace(motion);
                    }));
                }
            };
            io.before = [&](const fake::Call& call) {
                if (call.op == Op::Set && call.name == "productpair") {
                    CHECK(s.guard.calls == 3);
                    verifyInitial(r, motion);
                }
            };
            CHECK(s.run(r) == Result::Installed);
            CHECK(s.guard.calls == 3 && !s.coordinator.faulted());
            CHECK(fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 2);
            verifyInitial(r, motion);
            verifyPair(r);
            CHECK(protectedData(motion) == protectedBefore);
            if (motion) CHECK(extra::stringQueries == 4);
            const auto disk = io.disk;
            io.before = {};
            s.guard.before = {};
            fake::reboot();
            CHECK(s.run(r) == Result::AlreadyInstalled);
            noWrites();
            CHECK(io.disk == disk);
            fake::reboot();
            Session rebooted(motion);
            CHECK(rebooted.run(r) == Result::AlreadyInstalled);
            noWrites();
            CHECK(io.disk == disk);
        });
    }
}

void guards() {
    for (bool motion : {false, true}) for (unsigned deny : {1u, 2u, 3u}) {
        scenario("guard phase failure and explicit resumed import", [=] {
            seedLegacy(1);
            const auto r = request(motion);
            const auto before = protectedData(motion);
            Session s(motion);
            s.guard.denyAt = deny;
            CHECK(s.run(r) == Result::Unsafe);
            CHECK(s.guard.calls == deny && !s.coordinator.faulted());
            CHECK(!present("productpair"));
            CHECK(present(stateSpace(motion)) == (deny == 3));
            if (deny < 3) noWrites();
            else verifyInitial(r, motion);
            CHECK(protectedData(motion) == before);
            fake::reboot();
            Session resumed(motion);
            CHECK(resumed.run(r) == Result::Installed);
            CHECK(fake::count(Op::Set) == (deny == 3 ? 1u : 2u));
            verifyInitial(r, motion);
            verifyPair(r);
        });
    }
    for (unsigned permission = 0; permission < 4; ++permission) {
        scenario("Brain guard owns Motion export and maintenance provenance", [=] {
            const auto r = request(false);
            Session s(false);
            if (permission == 0) s.guard.stationary = false;
            if (permission == 1) s.guard.operatorApproved = false;
            if (permission == 2) s.guard.mqttHandoff = false;
            if (permission == 3) s.guard.motionExportVerified = false;
            CHECK(s.run(r) == Result::Unsafe);
            CHECK(io.calls.empty());
            noWrites();
        });
    }
    scenario("Brain uses externally verified Motion export, not local legacy", [] {
        io.disk["productctx"]["payload"] = stringValue("broken");
        io.disk["formulaevt"]["payload"] = stringValue("pending-on-different-board");
        const auto before = protectedData(false);
        Session s(false);
        CHECK(s.run(request(false)) == Result::Installed);
        CHECK(extra::stringQueries == 0 && extra::stringReads == 0);
        CHECK(protectedData(false) == before);
    });
}

void validation() {
    for (bool motion : {false, true}) {
        for (unsigned mutation = 0; mutation < 8; ++mutation) {
            scenario("invalid import before any durable write", [=] {
                seedLegacy(1);
                auto r = request(motion);
                if (mutation == 0) r.pairing.epoch[0] = 'Z';
                if (mutation == 1) r.pairing.role = motion ? v4::Role::Brain : v4::Role::Motion;
                if (mutation == 2) r.context.profileVersion = 0;
                if (mutation == 3) std::strcpy(r.context.deviceId, "other-device");
                if (mutation == 4) r.hasContext = false;
                if (mutation == 5) r.context.powderGPer100Ml = -1;
                if (mutation == 6) r.pairing.peerPhysicalId[0] = '!';
                if (mutation == 7) { r = request(motion, 0); r.context.powderGPer100Ml = -0.0f; }
                Session s(motion);
                CHECK(s.run(r) == Result::Invalid);
                noWrites();
                CHECK(!s.coordinator.faulted());
            });
        }
        scenario("actual hardware mismatch before state creation", [=] {
            seedLegacy(1);
            extra::mac[0] ^= 0x10;
            Session s(motion);
            CHECK(s.run(request(motion)) == Result::IdentityMismatch);
            CHECK(extra::macCalls > 0);
            noWrites();
        });
        scenario("hardware read failure is sticky", [=] {
            extra::failMacCall = 1;
            Session s(motion);
            const auto r = request(motion);
            CHECK(s.run(r) == Result::StorageFault);
            noWrites();
            assertSticky(s, r);
        });
        for (unsigned identity = 0; identity < 4; ++identity) {
            scenario("existing pair identity cannot be replaced", [=] {
                auto other = request(motion).pairing;
                if (identity == 0) other.epoch[0] = '2';
                if (identity == 1) std::strcpy(other.deviceId, "other-device");
                if (identity == 2) other.peerPhysicalId[0] = '1';
                if (identity == 3) other.localPhysicalId[0] = '1';
                io.disk["productpair"]["record"] = {pairBytes(other), fake::Type::Blob};
                const auto disk = io.disk;
                Session s(motion);
                CHECK(s.run(request(motion)) == Result::Conflict);
                noWrites();
                CHECK(io.disk == disk);
            });
        }
    }
}

void legacy() {
    const std::vector<fake::Value> events = {
        stringValue("{}"), stringValue("{bad-json"), stringValue("cleared"),
        {{0}, fake::Type::String}, {{}, fake::Type::String},
        {Bytes(2049, 'x'), fake::Type::String}, {{1, 2}, fake::Type::Blob},
        {{1, 0, 0, 0}, fake::Type::U32}, {{'x', 'y'}, fake::Type::String}
    };
    for (size_t i = 0; i < events.size(); ++i) scenario("any old formulaevt payload blocks", [&, i] {
        seedLegacy(1);
        io.disk["formulaevt"]["payload"] = events[i];
        const auto disk = io.disk;
        Session s(true);
        const auto r = request(true);
        const auto expected = i < 3 || i == 8 ? Result::LegacyPending : Result::StorageFault;
        CHECK(s.run(r) == expected);
        noWrites();
        CHECK(io.disk == disk);
        CHECK(extra::stringQueries == 1 && extra::stringReads == 0);
        if (expected == Result::StorageFault) assertSticky(s, r);
    });
    for (bool namespaceMissing : {false, true}) scenario("missing old namespace/key is not new-machine permission", [=] {
        if (namespaceMissing) { io.disk.erase("formulaevt"); io.disk.erase("productctx"); }
        Session s(true);
        CHECK(s.run(request(true)) == Result::Conflict);
        noWrites();
        auto r = request(true, 0);
        s.guard.operatorApproved = false;
        CHECK(s.run(r) == Result::Unsafe);
        noWrites();
        s.guard.operatorApproved = true;
        CHECK(s.run(r) == Result::Installed);
        verifyInitial(r, true);
    });
    for (unsigned field = 0; field < 9; ++field) scenario("exact complete legacy provenance", [=] {
        seedLegacy(1);
        auto r = request(true);
        if (field == 0) std::strcat(r.context.babyName, " changed suffix");
        if (field == 1) std::strcat(r.context.formulaBrand, " changed suffix");
        if (field == 2) ++r.context.profileVersion;
        if (field == 3) ++r.context.waterMl;
        if (field == 4) ++r.context.temperatureC;
        if (field == 5) r.context.powderGPer100Ml += 0.5f;
        if (field == 6) std::strcat(r.context.babyId, "-other");
        if (field == 7) r = request(true, 0);
        if (field == 8) r = request(true, 2);
        Session s(true);
        const auto disk = io.disk;
        CHECK(s.run(r) == Result::Conflict);
        noWrites();
        CHECK(io.disk == disk);
    });
    for (int requested : {0, 1}) scenario("cleared legacy cannot become absent or active", [=] {
        seedLegacy(2);
        Session s(true);
        CHECK(s.run(request(true, requested)) == Result::Conflict);
        noWrites();
    });
    for (const auto& bad : {stringValue("{broken"), stringValue(""),
                            fake::Value{{1, 2, 3}, fake::Type::Blob},
                            fake::Value{{'{', '}'}, fake::Type::String}}) {
        scenario("legacy context corrupt never treated as absent", [&] {
            io.disk["productctx"]["payload"] = bad;
            const auto disk = io.disk;
            Session s(true);
            const auto r = request(true, 0);
            CHECK(s.run(r) == Result::StorageFault);
            noWrites();
            CHECK(io.disk == disk);
            assertSticky(s, r);
        });
    }
    for (bool event : {false, true}) scenario("legacy evidence rechecked after pre-state guard", [=] {
        seedLegacy(1);
        Session s(true);
        s.guard.before = [&](unsigned phase) {
            if (phase == 2) {
                if (event) io.disk["formulaevt"]["payload"] = stringValue("new pending");
                else seedLegacy(2);
            }
        };
        CHECK(s.run(request(true)) == (event ? Result::LegacyPending : Result::Conflict));
        noWrites();
        CHECK(s.guard.calls == 2);
    });
}

ProductRequest command(const CommissioningImport& r, bool cloud = false) {
    ProductRequest q;
    q.command = ProductCommand::Clean;
    q.sequence = 1;
    q.source = cloud ? v4::Source::CloudCommand : v4::Source::LocalTouch;
    std::strcpy(q.deviceId, r.pairing.deviceId);
    auto brain = r.pairing;
    if (brain.role == v4::Role::Motion) {
        brain.role = v4::Role::Brain;
        std::swap(brain.localPhysicalId, brain.peerPhysicalId);
    }
    if (cloud) std::strcpy(q.commandId, "cloud-clean-1");
    else CHECK(makeLocalCommandId(brain, q.sequence, q.commandId));
    CHECK(validProductRequest(q));
    return q;
}

void recovery() {
    for (bool motion : {false, true}) {
        scenario("pair without state is sticky fault, never auto-created", [=] {
            seedLegacy(1);
            const auto r = request(motion);
            io.disk["productpair"]["record"] = {pairBytes(r.pairing), fake::Type::Blob};
            const auto disk = io.disk;
            Session s(motion);
            CHECK(s.run(r) == Result::StateMissing);
            noWrites();
            CHECK(io.disk == disk);
            assertSticky(s, r);
        });
        for (int kind : {0, 1, 2}) scenario("state-only resumes exact import", [=] {
            seedLegacy(kind);
            const auto r = request(motion, kind);
            Session first(motion);
            first.initial(r);
            const auto state = io.disk.at(stateSpace(motion)).at("record");
            fake::reboot();
            Session resumed(motion);
            CHECK(resumed.run(r) == Result::Installed);
            CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            CHECK(io.disk.at(stateSpace(motion)).at("record") == state);
            verifyPair(r);
        });
        for (bool paired : {false, true}) for (unsigned used = 0; used < (motion ? 5u : 3u); ++used) {
            scenario("consumed sequence, pending or different cache never reset", [=] {
                seedLegacy(1);
                const auto r = request(motion);
                Session first(motion);
                first.initial(r);
                if (used == 2) {
                    auto newer = r.context;
                    ++newer.profileVersion;
                    if (motion) CHECK(first.motor.saveContext(newer) == MotionWrite::Stored);
                    else CHECK(first.brain.saveContext(newer) == BrainWrite::Stored);
                } else if (motion) {
                    const auto q = command(r, used >= 3);
                    const bool pending = used == 1 || used == 4;
                    CHECK(first.motor.recordDecision(q, pending, pending ? "accepted" : "busy",
                                                     pending ? execution : nullptr) == MotionWrite::Stored);
                } else {
                    const auto q = command(r);
                    CHECK(first.brain.reserveLocal(q) == BrainWrite::Stored);
                    if (used == 0) CHECK(first.brain.clearPending(q) == BrainWrite::Stored);
                }
                if (paired) io.disk["productpair"]["record"] = {pairBytes(r.pairing), fake::Type::Blob};
                fake::reboot();
                const auto disk = io.disk;
                Session resumed(motion);
                CHECK(resumed.run(r) == Result::Conflict);
                noWrites();
                CHECK(io.disk == disk);
            });
        }
        for (unsigned identity = 0; identity < 4; ++identity) scenario("state-only identity mismatch refuses resume", [=] {
            seedLegacy(1);
            auto other = request(motion);
            if (identity == 0) other.pairing.epoch[0] = '2';
            if (identity == 1) {
                std::strcpy(other.pairing.deviceId, "other-device");
                std::strcpy(other.context.deviceId, "other-device");
            }
            if (identity == 2) other.pairing.peerPhysicalId[0] = '1';
            if (identity == 3) other.pairing.localPhysicalId[0] = '1';
            Session first(motion);
            first.initial(other);
            fake::reboot();
            const auto disk = io.disk;
            Session resumed(motion);
            const auto r = request(motion);
            CHECK(resumed.run(r) == Result::StorageFault);
            noWrites();
            CHECK(io.disk == disk && resumed.storeFaulted());
            assertSticky(resumed, r);
        });
        for (const char* space : {stateSpace(motion), "productpair"}) scenario("corrupt record never initialized over", [=] {
            seedLegacy(1);
            io.disk[space]["record"] = {{1, 0xff, 2}, fake::Type::Blob};
            const auto disk = io.disk;
            Session s(motion);
            const auto r = request(motion);
            CHECK(s.run(r) == Result::StorageFault);
            noWrites();
            CHECK(io.disk == disk);
            assertSticky(s, r);
        });
    }
}

// Establish a known starting disk using production encoders/stores, then replay
// every observed I/O boundary. Assertions never infer success from raw writes.
fake::Database startingDisk(bool motion, unsigned stage) {
    seedLegacy(1);
    const auto r = request(motion);
    if (stage) {
        Session s(motion);
        s.initial(r);
        if (stage == 2) io.disk["productpair"]["record"] = {pairBytes(r.pairing), fake::Type::Blob};
    }
    fake::reboot();
    return io.disk;
}

void faults() {
    for (bool motion : {false, true}) for (unsigned stage : {0u, 1u, 2u}) {
        fake::Database start;
        std::vector<fake::Call> trace;
        unsigned macCalls = 0;
        scenario("baseline for I/O replay", [&] {
            start = startingDisk(motion, stage);
            Session s(motion);
            CHECK(s.run(request(motion)) == (stage == 2 ? Result::AlreadyInstalled : Result::Installed));
            trace = io.calls;
            macCalls = extra::macCalls;
        });
        for (const auto& call : trace) {
            if (call.op == Op::Close) continue;
            const bool write = call.op == Op::Set || call.op == Op::Commit;
            for (bool apply : {false, true}) {
                if (apply && !write) continue;
                for (bool early : {false, true}) scenario("I/O error at " + call.name + "/" + call.key +
                    " op=" + std::to_string(int(call.op)) + " n=" + std::to_string(call.occurrence), [&] {
                    io.disk = start;
                    io.durableOnSet = early;
                    const auto protectedBefore = protectedData(motion);
                    fake::fail(call.op, call.occurrence, ESP_FAIL, apply);
                    const auto r = request(motion);
                    Session s(motion);
                    CHECK(s.run(r) == Result::StorageFault);
                    fake::verifyFaults();
                    assertSticky(s, r);
                    CHECK(protectedData(motion) == protectedBefore);
                    if (call.name == stateSpace(motion)) CHECK(s.storeFaulted());
                    if (present("productpair")) {
                        verifyInitial(r, motion);
                        verifyPair(r);
                    }
                    const bool hadState = present(stateSpace(motion));
                    const bool hadPair = present("productpair");
                    const auto state = hadState ? io.disk.at(stateSpace(motion)).at("record") : fake::Value{};
                    fake::reboot();
                    Session rebooted(motion);
                    CHECK(rebooted.run(r) == (hadPair ? Result::AlreadyInstalled : Result::Installed));
                    CHECK(fake::count(Op::Set) == unsigned(!hadState) + unsigned(!hadPair));
                    if (hadState) CHECK(io.disk.at(stateSpace(motion)).at("record") == state);
                    verifyInitial(r, motion);
                    verifyPair(r);
                });
            }
        }
        for (unsigned macCall = 1; macCall <= macCalls; ++macCall) scenario("MAC failure at each pairing phase", [&] {
            io.disk = start;
            extra::failMacCall = macCall;
            Session s(motion);
            const auto r = request(motion);
            CHECK(s.run(r) == Result::StorageFault);
            CHECK(extra::macCalls == macCall);
            assertSticky(s, r);
            if (present("productpair")) verifyInitial(r, motion);
        });
    }
}

struct PowerCut {};
void powerCuts() {
    for (bool motion : {false, true}) {
        fake::Database start;
        size_t boundaries = 0;
        scenario("power-cut baseline", [&] {
            start = startingDisk(motion, 0);
            Session s(motion);
            CHECK(s.run(request(motion)) == Result::Installed);
            boundaries = io.calls.size();
        });
        for (size_t cut = 1; cut <= boundaries + 1; ++cut) for (bool early : {false, true}) {
            scenario("cut before I/O boundary " + std::to_string(cut), [&] {
                io.disk = start;
                io.durableOnSet = early;
                const auto protectedBefore = protectedData(motion);
                const auto r = request(motion);
                bool hit = false;
                io.before = [&](const fake::Call&) {
                    if (io.calls.size() == cut) { hit = true; throw PowerCut{}; }
                };
                try {
                    Session s(motion);
                    CHECK(s.run(r) == Result::Installed);
                    CHECK(cut == boundaries + 1);
                } catch (const PowerCut&) { CHECK(hit); }
                CHECK(hit || cut == boundaries + 1);
                CHECK(protectedData(motion) == protectedBefore);
                const bool hadState = present(stateSpace(motion));
                const bool hadPair = present("productpair");
                CHECK(!hadPair || hadState);
                if (hadState) verifyInitial(r, motion);
                if (hadPair) verifyPair(r);
                fake::reboot();
                Session resumed(motion);
                CHECK(resumed.run(r) == (hadPair ? Result::AlreadyInstalled : Result::Installed));
                CHECK(fake::count(Op::Set) == unsigned(!hadState) + unsigned(!hadPair));
                verifyInitial(r, motion);
                verifyPair(r);
                CHECK(protectedData(motion) == protectedBefore);
            });
        }
    }
}

void lastPairCheck() {
    for (bool motion : {false, true}) for (bool same : {false, true}) scenario("pair installer rechecks write handle", [=] {
        seedLegacy(1);
        const auto r = request(motion);
        auto arriving = r.pairing;
        if (!same) arriving.epoch[0] = '2';
        const auto bytes = pairBytes(arriving);
        bool hit = false;
        io.before = [&](const fake::Call& call) {
            if (call.op == Op::OpenRW && call.name == "productpair") {
                CHECK(!hit);
                hit = true;
                io.disk["productpair"]["record"] = {bytes, fake::Type::Blob};
            }
        };
        Session s(motion);
        CHECK(s.run(r) == (same ? Result::Installed : Result::Conflict));
        CHECK(hit && fake::count(Op::Set) == 1);
        CHECK(io.disk.at("productpair").at("record").bytes == bytes);
        verifyInitial(r, motion);
    });
    for (bool motion : {false, true}) scenario("pair readback mismatch latches without erase or retry", [=] {
        seedLegacy(1);
        const auto r = request(motion);
        auto other = r.pairing;
        other.epoch[0] = '2';
        bool changed = false;
        io.before = [&](const fake::Call& call) {
            if (!changed && call.op == Op::Read && call.name == "productpair" && fake::count(Op::Commit) == 2) {
                changed = true;
                io.disk["productpair"]["record"] = {pairBytes(other), fake::Type::Blob};
            }
        };
        Session s(motion);
        CHECK(s.run(r) == Result::StorageFault);
        CHECK(changed && fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 2);
        verifyInitial(r, motion);
        assertSticky(s, r);
    });
}
}

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, void (*)()>> groups = {
        {"success", success}, {"guards", guards}, {"validation", validation}, {"legacy", legacy},
        {"recovery", recovery}, {"faults", faults}, {"cuts", powerCuts}, {"pair", lastPairCheck}
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
    std::printf("BoardCommissioning: %u scenarios, %u failures\n", scenarios, failures);
    return failures ? 1 : 0;
}
