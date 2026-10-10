#include "MotionStateRecovery.h"
#include "ProductSession.h"
#include "FakeBrainNvs.h"
#include "FakeProductCrypto.h"

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
using motion::MotionStateRecovery;
using babytech::display::DisplayStage;
using babytech::display::DisplayError;

namespace {
unsigned scenarios = 0, failures = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + \
    std::to_string(__LINE__) + ": " #x); } while (false)

v4::Pairing pairing() {
    v4::Pairing p{};
    p.role = v4::Role::Motion;
    std::strcpy(p.deviceId, "Babytech_01-recovery");
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
        std::strcpy(c.babyId, version == 10 ? "baby-original" : "baby-current");
        std::strcpy(c.babyName, "Full baby name");
        std::strcpy(c.formulaBrand, "Full formula brand");
        c.waterMl = 120;
        c.temperatureC = 40;
        c.powderGPer100Ml = version == 10 ? 13.5f : 16.25f;
    }
    CHECK(validProductContext(c));
    return c;
}
ProductRequest request(uint64_t sequence, ProductCommand command = ProductCommand::Prepare,
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
        std::strcpy(r.babyId, context().babyId);
        r.profileVersion = 10;
        r.waterMl = 180;
        r.temperatureC = 45;
        r.powderGPer100Ml = 13.5f;
    }
    CHECK(validProductRequest(r));
    return r;
}
std::string executionId(uint64_t sequence) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032llx", static_cast<unsigned long long>(sequence));
    return text;
}
Bytes encode(const MotionState& state) {
    std::array<uint8_t, kMotionStateMaxSize> buffer{};
    const size_t length = encodeMotionState(state, buffer.data(), buffer.size());
    CHECK(length);
    return Bytes(buffer.begin(), buffer.begin() + length);
}
Bytes durable() { return io.disk.at("productstate").at("record").bytes; }
MotionState decodedDisk() {
    MotionState state;
    const auto bytes = durable();
    CHECK(decodeMotionState(bytes.data(), bytes.size(), state));
    return state;
}
void seed(const MotionState& state) {
    io.disk["productstate"]["record"] = {encode(state), fake::Type::Blob};
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
void audit() {
    fake::verifyFaults();
    CHECK(io.handles.empty());
    CHECK(!fake::count(Op::Erase) && !fake::count(Op::Init));
    for (const auto& call : io.calls) {
        CHECK(call.name == "productstate");
        CHECK(call.key.empty() || call.key == "record");
    }
}
void noWrites() {
    CHECK(!fake::count(Op::OpenRW) && !fake::count(Op::Set) && !fake::count(Op::Commit));
}
void scenario(const std::string& name, const std::function<void()>& run, bool space = true) {
    fake::reset();
    fake_product_crypto::reset();
    for (const char* ns : {"productpair", "brainstate", "productctx", "outbox", "wifi-cfg",
                           "actuatorcfg", "sensorcfg"})
        io.disk[ns]["record"] = {{0, 0xff, 1, 2}, fake::Type::Blob};
    if (space) io.disk["productstate"]["unrelated"] = {{1, 2, 3}, fake::Type::U32};
    const auto protectedBefore = protectedData();
    ++scenarios;
    try {
        run();
        audit();
        CHECK(protectedData() == protectedBefore);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
        io.before = {};
        io.handles.clear();
    }
}

// Build every baseline through production store methods, including real hashes.
MotionState baseline(unsigned queued = 0, MotionSlotKind kind = MotionSlotKind::Intent,
                     ProductCommand command = ProductCommand::Prepare,
                     v4::Source source = v4::Source::LocalTouch, bool completed = false) {
    MotionStateStore store;
    const auto c = context();
    CHECK(store.installInitial(pairing(), &c) == MotionWrite::Stored);
    CHECK(store.recordDecision(request(5, ProductCommand::Clean, v4::Source::CloudCommand),
                               false, "busy") == MotionWrite::Stored);
    for (unsigned i = 0; i < queued; ++i) {
        const auto r = request(10 + i);
        const auto id = executionId(r.sequence);
        CHECK(store.recordDecision(r, true, "accepted", id.c_str()) == MotionWrite::Stored);
        const bool success = i % 2 == 0;
        CHECK(store.finishFeeding(id.c_str(), success, success ? "" : "stopped",
                                 success ? "" : "E_STOPPED", 500 + i) == MotionWrite::Stored);
        CHECK(store.archiveFeeding(true) == MotionWrite::Stored);
    }
    if (kind != MotionSlotKind::Empty) {
        const auto r = request(30, command, source);
        CHECK(store.recordDecision(r, true, "accepted", executionId(30).c_str()) == MotionWrite::Stored);
        if (kind == MotionSlotKind::Terminal)
            CHECK(store.finishFeeding(executionId(30).c_str(), completed,
                                     completed ? "" : "low_water",
                                     completed ? "" : "E_LOW_WATER", 9876) == MotionWrite::Stored);
    } else if (!queued) {
        CHECK(store.recordDecision(request(7, ProductCommand::Clean), false, "busy") == MotionWrite::Stored);
    }
    const auto state = store.state();
    CHECK(sameMotionState(state, decodedDisk()));
    audit();
    fake::reboot();
    fake_product_crypto::reset();
    return state;
}

struct Hardware : motion::MotionRecoveryHardware {
    unsigned stops = 0;
    uint32_t stoppedAt = 0;
    bool fresh = true, stationaryValue = true;
    void supervisedStop(uint32_t now) override {
        ++stops;
        stoppedAt = now;
        fresh = false; // pre-stop samples are deliberately unusable
    }
    bool stationary() const override { return fresh && stationaryValue; }
    void confirm() { fresh = stationaryValue = true; }
};

Status projected(MotionStateRecovery& recovery) {
    Status status;
    status.snapshot.startEnabled = status.executionAuthorized = true;
    status.snapshot.stage = DisplayStage::Ready;
    status.snapshot.waterMl = 180;
    status.snapshot.temperatureC = 45;
    status.sampleUptimeMs = 1234;
    status.lowWaterValid = status.powderValid = true;
    status.powderGrams = 250;
    status.actuatorOperational = status.actuatorConfigValid = status.actuatorBusHealthy = true;
    recovery.project(status);
    return status;
}
void roundtrip(const Status& status) {
    v4::Message first, second;
    CHECK(encodeStatus(status, first));
    Status decoded;
    CHECK(decodeStatus(first, decoded));
    CHECK(encodeStatus(decoded, second));
    CHECK(first.kind == second.kind && first.length == second.length);
    CHECK(!std::memcmp(first.payload, second.payload, first.length));
}
void projectionMatches(MotionStateRecovery& recovery, const MotionState& state) {
    const auto status = projected(recovery);
    CHECK(!status.snapshot.startEnabled && !status.executionAuthorized);
    CHECK(status.sampleUptimeMs == 1234 && status.powderGrams == 250);
    CHECK(status.snapshot.waterMl == 180 && status.snapshot.temperatureC == 45);
    CHECK(status.actuatorOperational && status.actuatorConfigValid && status.actuatorBusHealthy);
    CHECK(std::string(status.cloudWatermark) == std::to_string(state.cloudSequence));
    CHECK(std::string(status.localWatermark) == std::to_string(state.localSequence));
    CHECK(!std::strcmp(status.activeExecutionId,
          recovery.motionPending() && state.slot.kind != MotionSlotKind::Empty ? state.slot.executionId : ""));
    CHECK(status.executionOwner == (status.activeExecutionId[0] ? ExecutionOwner::Product : ExecutionOwner::None));
    const char* pending = state.slot.kind == MotionSlotKind::Terminal ? state.slot.eventId :
        (state.pendingResultCount ? state.pendingResults[0].eventId : "");
    CHECK(!std::strcmp(status.pendingEventId, pending));
    CHECK(status.eventPending == (state.pendingResultCount != 0 || state.slot.kind == MotionSlotKind::Terminal));
    CHECK(status.contextVersion == (state.context.present ? state.context.profileVersion : 0));
    CHECK(status.feedingContextConfigured == (state.context.present && !state.context.cleared));
    CHECK(!std::strcmp(status.babyId, status.feedingContextConfigured ? state.context.babyId : ""));
    CHECK(!std::strcmp(status.productProgress, "noready") && !std::strcmp(status.productError, "NONE"));
    roundtrip(status);
}
void faultProjection(MotionStateRecovery& recovery) {
    Status status;
    status.snapshot.startEnabled = status.executionAuthorized = true;
    status.feedingContextConfigured = true;
    status.contextVersion = 99;
    std::strcpy(status.babyId, "not_verified");
    recovery.project(status);
    CHECK(!status.snapshot.startEnabled && !status.executionAuthorized);
    CHECK(!status.feedingContextConfigured && !status.contextVersion && !status.babyId[0]);
    CHECK(!std::strcmp(status.productProgress, "error"));
    CHECK(!std::strcmp(status.productError, "E_STORAGE_FAULT"));
    CHECK(status.snapshot.stage == DisplayStage::Error && status.snapshot.error == DisplayError::Unknown);
    CHECK(!std::strcmp(status.cloudWatermark, "0") && !std::strcmp(status.localWatermark, "0"));
    CHECK(!status.activeExecutionId[0] && !status.pendingEventId[0] && !status.eventPending);
    roundtrip(status);
    for (const char* error : {"NONE", "E_CAN_FAULT", "E_CAP_UNSCREW_TIMEOUT"})
        for (DisplayError displayError : {DisplayError::None, DisplayError::CanFault,
                                         DisplayError::CapUnscrewTimeout}) {
            Status mechanical;
            std::strcpy(mechanical.productProgress, "unscrewing_cap");
            std::strcpy(mechanical.productError, error);
            mechanical.snapshot.error = displayError;
            recovery.project(mechanical);
            CHECK(!std::strcmp(mechanical.productError,
                               !std::strcmp(error, "NONE") ? "E_STORAGE_FAULT" : error));
            CHECK(mechanical.snapshot.error ==
                  (displayError == DisplayError::None ? DisplayError::Unknown : displayError));
            CHECK(!std::strcmp(mechanical.productProgress, "error"));
            CHECK(mechanical.snapshot.stage == DisplayStage::Error);
            roundtrip(mechanical);
        }
}
MotionState recoveredFeeding(MotionState state, uint32_t now) {
    if (state.slot.kind == MotionSlotKind::Intent) {
        state.slot.kind = MotionSlotKind::Terminal;
        state.slot.completed = false;
        state.slot.uptimeMs = now;
        std::strcpy(state.slot.reason, "reboot_during_feed");
        std::strcpy(state.slot.errorCode, "E_REBOOT_DURING_FEED");
    }
    CHECK(state.pendingResultCount < kMotionResultQueueCapacity);
    state.pendingResults[state.pendingResultCount++] = state.slot;
    state.slot = MotionExecutionSlot{};
    CHECK(validMotionState(state));
    return state;
}
void steady(MotionStateRecovery& recovery, Hardware& hardware, const MotionState& expected) {
    const auto calls = io.calls.size();
    const auto disk = io.disk;
    for (unsigned i = 0; i < 5; ++i) recovery.poll();
    CHECK(io.calls.size() == calls && io.disk == disk);
    CHECK(hardware.stops == 1 && !recovery.motionPending());
    CHECK(sameMotionState(expected, decodedDisk()));
}

void boot() {
    scenario("before begin does not touch storage, hardware or status", [] {
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        Status status;
        status.executionAuthorized = status.snapshot.startEnabled = true;
        v4::Message before, after;
        CHECK(encodeStatus(status, before));
        recovery.poll();
        recovery.project(status);
        CHECK(encodeStatus(status, after));
        CHECK(before.length == after.length && !std::memcmp(before.payload, after.payload, before.length));
        CHECK(!recovery.began() && !recovery.motionPending() && hardware.stops == 0 && io.calls.empty());
    });
    // Missing namespace and missing key must both remain read-only, never installed.
    for (unsigned kind = 0; kind < 15; ++kind) scenario("paired boot fault " + std::to_string(kind), [=] {
        auto state = baseline();
        MotionLoad expected = MotionLoad::Corrupt;
        if (kind == 0 || kind == 1) {
            io.disk.at("productstate").erase("record");
            expected = MotionLoad::Missing;
            if (kind == 0) io.disk.erase("productstate");
        } else if (kind == 2) {
            io.disk.at("productstate").at("record").bytes[8] ^= 1;
        } else if (kind == 3) {
            io.disk.at("productstate").at("record").bytes.pop_back();
        } else if (kind == 4) {
            io.disk.at("productstate").at("record").type = fake::Type::String;
        } else if (kind >= 5 && kind <= 8) {
            const Op op = kind == 5 ? Op::OpenRO : kind == 6 ? Op::Query : Op::Read;
            fake::fail(op, 1, kind == 8 ? ESP_OK : ESP_FAIL, false,
                       kind == 8 ? std::optional<size_t>(1) : std::nullopt);
            expected = MotionLoad::IoError;
        } else if (kind == 9) {
            fake_product_crypto::fail = true;
        } else if (kind < 14) {
            if (kind == 10) state.pairing.epoch[0] = 'a';
            if (kind == 11) state.pairing.localPhysicalId[0] = 'a';
            if (kind == 12) state.pairing.peerPhysicalId[0] = 'a';
            if (kind == 13) std::strcpy(state.pairing.deviceId, "OtherDevice");
            // Blank records allow a changed pairing without frozen ID inconsistencies.
            MotionState other;
            other.pairing = state.pairing;
            seed(other);
            expected = MotionLoad::IdentityMismatch;
        } else {
            expected = MotionLoad::IdentityMismatch;
        }
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        const auto disk = io.disk;
        auto p = pairing();
        if (kind == 14) p.role = v4::Role::Brain;
        CHECK(recovery.begin(p, 100) == expected);
        CHECK(recovery.began() && recovery.loadResult() == expected);
        CHECK(!store.ready() && recovery.motionPending() && hardware.stops == 1 && hardware.stoppedAt == 100);
        const auto calls = io.calls.size();
        CHECK(recovery.begin(pairing(), 900) == recovery.loadResult());
        CHECK(io.calls.size() == calls && hardware.stops == 1);
        for (unsigned i = 0; i < 5; ++i) recovery.poll();
        CHECK(recovery.motionPending());
        hardware.fresh = true;
        hardware.stationaryValue = false;
        recovery.poll();
        CHECK(recovery.motionPending());
        hardware.confirm();
        recovery.poll();
        CHECK(!recovery.motionPending()); // storage fault does not permanently own debug motion
        for (unsigned i = 0; i < 5; ++i) recovery.poll();
        CHECK(hardware.stops == 1 && io.disk == disk && io.calls.size() == calls);
        noWrites();
        faultProjection(recovery);
    }, kind != 0);
    scenario("begin is single-shot even when storage later changes", [] {
        const auto before = baseline();
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        CHECK(recovery.begin(pairing(), 41) == MotionLoad::Ready);
        const auto calls = io.calls.size();
        io.disk.at("productstate").erase("record");
        auto p = pairing();
        p.epoch[0] = 'a';
        CHECK(recovery.begin(p, 99) == MotionLoad::Ready);
        CHECK(io.calls.size() == calls && hardware.stops == 1 && hardware.stoppedAt == 41);
        CHECK(sameMotionState(store.state(), before));
        noWrites();
    });
}

void idle() {
    for (unsigned queued = 0; queued <= kMotionResultQueueCapacity; ++queued)
        for (unsigned barrier = 0; barrier < (queued ? 2u : 3u); ++barrier)
            scenario("idle/queue-only count/barrier " + std::to_string(queued) + "/" + std::to_string(barrier), [=] {
                auto state = baseline(queued, MotionSlotKind::Empty);
                if (barrier == 1) CHECK(makeMotionContextBarrier(context(11, true), state.context));
                if (barrier == 2) state.context = MotionContextBarrier{};
                seed(state);
                MotionStateStore store;
                Hardware hardware;
                hardware.fresh = hardware.stationaryValue = false;
                MotionStateRecovery recovery(store, hardware);
                const auto disk = io.disk;
                CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
                CHECK(!recovery.motionPending() && !hardware.stops);
                const auto calls = io.calls.size();
                for (unsigned i = 0; i < 10; ++i) recovery.poll();
                CHECK(io.calls.size() == calls && io.disk == disk);
                CHECK(sameMotionState(state, store.state()));
                projectionMatches(recovery, state);
                Status status;
                status.eventPending = true;
                recovery.project(status);
                CHECK(status.eventPending); // do not clear the separate legacy pending-event flag
                noWrites();
            });
    scenario("64-bit exact status watermarks survive JSON", [] {
        auto state = baseline(0, MotionSlotKind::Empty);
        MotionStateStore builder;
        CHECK(builder.load(pairing()) == MotionLoad::Ready);
        CHECK(builder.recordDecision(request(v4::kMaxSequence, ProductCommand::Clean), false, "busy") == MotionWrite::Stored);
        CHECK(builder.recordDecision(request(v4::kMaxSequence - 1, ProductCommand::Clean,
                                            v4::Source::CloudCommand), false, "busy") == MotionWrite::Stored);
        state = builder.state();
        fake::reboot();
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        CHECK(recovery.begin(pairing(), 1) == MotionLoad::Ready);
        projectionMatches(recovery, state);
        noWrites();
    });
    scenario("ready projection preserves supplied physical status and mechanical errors", [] {
        const auto state = baseline(1, MotionSlotKind::Empty);
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
        Status status;
        status.motionBusy = status.isPreparing = true;
        status.stationary = false;
        status.snapshot.stage = DisplayStage::Error;
        status.snapshot.error = DisplayError::CanFault;
        std::strcpy(status.productProgress, "error");
        std::strcpy(status.productError, "E_CAN_FAULT");
        std::strcpy(status.activeExecutionId, executionId(99).c_str());
        std::strcpy(status.pendingEventId, "stale_event");
        std::strcpy(status.babyId, "stale_baby");
        recovery.project(status);
        CHECK(status.motionBusy && status.isPreparing && !status.stationary);
        CHECK(status.snapshot.stage == DisplayStage::Error && status.snapshot.error == DisplayError::CanFault);
        CHECK(!std::strcmp(status.productProgress, "error") && !std::strcmp(status.productError, "E_CAN_FAULT"));
        CHECK(!status.activeExecutionId[0]);
        CHECK(!std::strcmp(status.pendingEventId, state.pendingResults[0].eventId));
        CHECK(!std::strcmp(status.babyId, state.context.babyId));
        roundtrip(status);
        noWrites();
    });
}

void feeding() {
    for (auto source : {v4::Source::LocalTouch, v4::Source::CloudCommand})
        for (unsigned queued = 0; queued < kMotionResultQueueCapacity; ++queued)
            for (uint32_t now : {uint32_t(0), uint32_t(100), UINT32_MAX})
                scenario("prepare recovery source/queue/time " + std::to_string(int(source)) + "/" +
                         std::to_string(queued) + "/" + std::to_string(now), [=] {
                    const auto before = baseline(queued, MotionSlotKind::Intent, ProductCommand::Prepare, source);
                    const auto expected = recoveredFeeding(before, now);
                    MotionStateStore store;
                    Hardware hardware;
                    MotionStateRecovery recovery(store, hardware);
                    CHECK(recovery.begin(pairing(), now) == MotionLoad::Ready);
                    CHECK(recovery.motionPending() && hardware.stops == 1);
                    projectionMatches(recovery, before);
                    recovery.poll(); // pre-stop stationary sample is not fresh evidence
                    hardware.fresh = true;
                    hardware.stationaryValue = false;
                    recovery.poll();
                    noWrites();
                    CHECK(recovery.motionPending() && sameMotionState(store.state(), before));
                    hardware.confirm();
                    recovery.poll();
                    CHECK(store.ready() && sameMotionState(store.state(), expected));
                    CHECK(fake::count(Op::Set) == 2 && fake::count(Op::Commit) == 2);
                    projectionMatches(recovery, expected);
                    steady(recovery, hardware, expected);
                    audit();
                    fake::reboot();
                    MotionStateStore rebooted;
                    Hardware nextHardware;
                    MotionStateRecovery next(rebooted, nextHardware);
                    CHECK(next.begin(pairing(), 500) == MotionLoad::Ready);
                    CHECK(!nextHardware.stops && !next.motionPending());
                    next.poll();
                    CHECK(sameMotionState(rebooted.state(), expected));
                    noWrites();
                });
}

void operations() {
    for (ProductCommand command : {ProductCommand::Initialize, ProductCommand::Clean})
        for (unsigned queued : {0u, unsigned(kMotionResultQueueCapacity)})
            for (bool superseded : {false, true})
                scenario("initialize/clean interruption queue/superseded " + std::to_string(int(command)) + "/" +
                         std::to_string(queued) + "/" + std::to_string(superseded), [=] {
                    const auto before = baseline(queued, MotionSlotKind::Intent, command);
                    auto expected = before;
                    if (superseded) {
                        MotionStateStore writer;
                        CHECK(writer.load(pairing()) == MotionLoad::Ready);
                        CHECK(writer.recordDecision(request(31, ProductCommand::Clean), false, "busy") == MotionWrite::Stored);
                        expected = writer.state();
                        audit();
                        fake::reboot();
                    }
                    expected.slot = MotionExecutionSlot{};
                    if (!superseded) expected.localResult.outcome = MotionOutcome::Interrupted;
                    MotionStateStore store;
                    Hardware hardware;
                    MotionStateRecovery recovery(store, hardware);
                    CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
                    recovery.poll();
                    CHECK(recovery.motionPending());
                    noWrites();
                    hardware.confirm();
                    recovery.poll();
                    CHECK(sameMotionState(store.state(), expected));
                    CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
                    CHECK(expected.pendingResultCount == before.pendingResultCount);
                    projectionMatches(recovery, expected);
                    steady(recovery, hardware, expected);
                });
    scenario("cloud clean interrupted without feeding event", [] {
        auto before = baseline(2, MotionSlotKind::Intent, ProductCommand::Clean, v4::Source::CloudCommand);
        auto expected = before;
        expected.cloudResult.outcome = MotionOutcome::Interrupted;
        expected.slot = MotionExecutionSlot{};
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
        hardware.confirm();
        recovery.poll();
        CHECK(sameMotionState(store.state(), expected));
        steady(recovery, hardware, expected);
    });
}

void terminalAndContext() {
    for (bool completed : {false, true}) for (unsigned queued = 0; queued < kMotionResultQueueCapacity; ++queued)
        scenario("existing terminal outcome retained " + std::to_string(completed) + "/" + std::to_string(queued), [=] {
            const auto before = baseline(queued, MotionSlotKind::Terminal, ProductCommand::Prepare,
                                         v4::Source::CloudCommand, completed);
            const auto expected = recoveredFeeding(before, 123);
            MotionStateStore store;
            Hardware hardware;
            MotionStateRecovery recovery(store, hardware);
            CHECK(recovery.begin(pairing(), 123) == MotionLoad::Ready);
            projectionMatches(recovery, before);
            recovery.poll();
            CHECK(recovery.motionPending());
            noWrites();
            hardware.confirm();
            recovery.poll();
            CHECK(sameMotionState(store.state(), expected));
            CHECK(fake::count(Op::Set) == 1 && fake::count(Op::Commit) == 1);
            steady(recovery, hardware, expected);
        });
    for (bool cleared : {false, true}) for (bool superseded : {false, true})
        scenario("current context/tombstone never rewrites frozen run " + std::to_string(cleared) + "/" +
                 std::to_string(superseded), [=] {
            const auto original = baseline(2);
            MotionStateStore writer;
            CHECK(writer.load(pairing()) == MotionLoad::Ready);
            CHECK(writer.saveContext(context(11, cleared)) == MotionWrite::Stored);
            if (superseded) CHECK(writer.recordDecision(request(31, ProductCommand::Clean), false, "busy") == MotionWrite::Stored);
            const auto before = writer.state();
            const auto expected = recoveredFeeding(before, 100);
            audit();
            fake::reboot();
            MotionStateStore store;
            Hardware hardware;
            MotionStateRecovery recovery(store, hardware);
            CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
            projectionMatches(recovery, before);
            hardware.confirm();
            recovery.poll();
            CHECK(sameMotionState(store.state(), expected));
            const auto& frozen = store.state().pendingResults[2];
            CHECK(sameProductRequest(frozen.request, original.slot.request));
            CHECK(!std::memcmp(frozen.digest, original.slot.digest, sizeof(frozen.digest)));
            CHECK(frozen.targetPowderG == original.slot.targetPowderG);
            CHECK(!std::strcmp(frozen.executionId, original.slot.executionId));
            CHECK(!std::strcmp(frozen.eventId, original.slot.eventId));
            projectionMatches(recovery, expected);
            steady(recovery, hardware, expected);
        });
    scenario("capacity reserved for interrupted prepare; historical receipt only removes its event", [] {
        const auto before = baseline(3);
        MotionStateStore store;
        Hardware hardware;
        MotionStateRecovery recovery(store, hardware);
        CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
        hardware.confirm();
        recovery.poll();
        CHECK(store.state().pendingResultCount == kMotionResultQueueCapacity);
        CHECK(store.recordDecision(request(31), true, "accepted", executionId(31).c_str()) == MotionWrite::QueueFull);
        CHECK(store.recordDecision(request(31, ProductCommand::Clean), true, "accepted", executionId(31).c_str()) == MotionWrite::Stored);
        const auto active = store.state().slot;
        const auto historical = before.pendingResults[1];
        CHECK(store.acknowledge(historical.eventId, historical.completed, false) == MotionWrite::Stored);
        CHECK(store.state().pendingResultCount == 3);
        CHECK(!std::strcmp(store.state().pendingResults[0].eventId, before.pendingResults[0].eventId));
        CHECK(!std::strcmp(store.state().pendingResults[1].eventId, before.pendingResults[2].eventId));
        CHECK(!std::strcmp(store.state().pendingResults[2].eventId, before.slot.eventId));
        CHECK(!std::strcmp(store.state().slot.executionId, active.executionId));
        const auto calls = io.calls.size();
        recovery.poll();
        CHECK(io.calls.size() == calls && hardware.stops == 1); // no takeover of a newer operation
    });
}

void evidence() {
    for (Op point : {Op::Set, Op::Commit, Op::OpenRO})
        scenario("fresh evidence lost inside finish write op " + std::to_string(int(point)), [=] {
            const auto before = baseline(2);
            MotionStateStore store;
            Hardware hardware;
            MotionStateRecovery recovery(store, hardware);
            CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
            hardware.confirm();
            bool hit = false;
            io.before = [&](const fake::Call& call) {
                if (!hit && call.op == point && (point != Op::OpenRO || fake::count(Op::Commit))) {
                    hit = true;
                    hardware.fresh = false;
                }
            };
            recovery.poll();
            io.before = {};
            CHECK(hit && store.ready() && store.state().slot.kind == MotionSlotKind::Terminal);
            CHECK(store.state().pendingResultCount == 2 && fake::count(Op::Commit) == 1);
            const auto terminal = store.state();
            CHECK(sameProductRequest(terminal.slot.request, before.slot.request));
            projectionMatches(recovery, terminal);
            const auto calls = io.calls.size();
            for (unsigned i = 0; i < 5; ++i) recovery.poll();
            CHECK(io.calls.size() == calls); // no archive based on prior stationary confirmation
            hardware.fresh = true;
            hardware.stationaryValue = false;
            recovery.poll();
            CHECK(io.calls.size() == calls);
            hardware.confirm();
            recovery.poll();
            const auto expected = recoveredFeeding(before, 100);
            CHECK(sameMotionState(store.state(), expected));
            CHECK(fake::count(Op::Set) == 2);
            steady(recovery, hardware, expected);
        });
}

void writeFaults() {
    // Enumerate the actual successful recovery I/O trace, not assumed SDK call numbers.
    for (ProductCommand command : {ProductCommand::Prepare, ProductCommand::Clean}) {
        std::vector<fake::Call> trace;
        scenario("capture recovery write trace " + std::to_string(int(command)), [&] {
            baseline(2, MotionSlotKind::Intent, command);
            MotionStateStore store;
            Hardware hardware;
            MotionStateRecovery recovery(store, hardware);
            CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
            const size_t beginCalls = io.calls.size();
            hardware.confirm();
            recovery.poll();
            trace.assign(io.calls.begin() + beginCalls, io.calls.end());
        });
        for (const auto& call : trace) {
            if (call.op == Op::Close) continue;
            for (bool apply : {false, true}) {
                if (apply && call.op != Op::Set && call.op != Op::Commit) continue;
                for (bool early : {false, true})
                    scenario("recovery write fault command/op/n/apply/early " + std::to_string(int(command)) + "/" +
                             std::to_string(int(call.op)) + "/" + std::to_string(call.occurrence) + "/" +
                             std::to_string(apply) + "/" + std::to_string(early), [=] {
                        const auto before = baseline(2, MotionSlotKind::Intent, command);
                        MotionStateStore store;
                        Hardware hardware;
                        MotionStateRecovery recovery(store, hardware);
                        CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
                        hardware.confirm();
                        io.durableOnSet = early;
                        fake::fail(call.op, call.occurrence, ESP_FAIL, apply);
                        MotionState expectedRam;
                        bool reached = false;
                        io.before = [&](const fake::Call& current) {
                            if (current.op == call.op && current.occurrence == call.occurrence) {
                                reached = true;
                                expectedRam = store.state();
                            }
                        };
                        recovery.poll();
                        io.before = {};
                        CHECK(store.faulted() && !store.ready());
                        CHECK(reached && sameMotionState(store.state(), expectedRam));
                        CHECK(!recovery.motionPending() && hardware.stops == 1);
                        const auto retained = encode(store.state());
                        const auto persisted = decodedDisk();
                        const size_t calls = io.calls.size();
                        for (unsigned i = 0; i < 10; ++i) recovery.poll();
                        CHECK(io.calls.size() == calls && encode(store.state()) == retained);
                        faultProjection(recovery);
                        audit();
                        fake::reboot(); // errors can have persisted a Terminal or even the archive
                        MotionStateStore rebooted;
                        Hardware nextHardware;
                        MotionStateRecovery next(rebooted, nextHardware);
                        CHECK(next.begin(pairing(), 200) == MotionLoad::Ready);
                        CHECK(nextHardware.stops == (persisted.slot.kind == MotionSlotKind::Empty ? 0u : 1u));
                        auto expected = persisted;
                        if (command == ProductCommand::Prepare) {
                            if (persisted.slot.kind != MotionSlotKind::Empty) expected = recoveredFeeding(persisted, 200);
                            CHECK(expected.pendingResultCount == before.pendingResultCount + 1);
                            CHECK(sameProductRequest(expected.pendingResults[2].request, before.slot.request));
                            CHECK(!std::strcmp(expected.pendingResults[2].eventId, before.slot.eventId));
                            CHECK(!std::strcmp(expected.pendingResults[2].errorCode, "E_REBOOT_DURING_FEED"));
                            CHECK(expected.pendingResults[2].uptimeMs ==
                                  (persisted.slot.kind == MotionSlotKind::Intent ? 200u : 100u));
                        } else if (persisted.slot.kind != MotionSlotKind::Empty) {
                            expected.slot = MotionExecutionSlot{};
                            expected.localResult.outcome = MotionOutcome::Interrupted;
                        }
                        nextHardware.confirm();
                        next.poll();
                        CHECK(rebooted.ready() && sameMotionState(rebooted.state(), expected));
                        const auto after = io.calls.size();
                        for (unsigned i = 0; i < 5; ++i) next.poll();
                        CHECK(io.calls.size() == after);
                        CHECK(expected.pendingResultCount <= kMotionResultQueueCapacity);
                        for (unsigned i = 0; i < before.pendingResultCount; ++i) {
                            CHECK(!std::strcmp(expected.pendingResults[i].eventId, before.pendingResults[i].eventId));
                            CHECK(expected.pendingResults[i].completed == before.pendingResults[i].completed);
                        }
                        projectionMatches(next, expected);
                    });
            }
        }
    }
    for (unsigned point = 0; point < 3; ++point)
        scenario("hash failure before finish/encode/readback " + std::to_string(point), [=] {
            const auto before = baseline(1);
            MotionStateStore store;
            Hardware hardware;
            MotionStateRecovery recovery(store, hardware);
            CHECK(recovery.begin(pairing(), 100) == MotionLoad::Ready);
            hardware.confirm();
            if (point == 0) fake_product_crypto::fail = true;
            else io.before = [=](const fake::Call& call) {
                if ((point == 1 && call.op == Op::OpenRW) ||
                    (point == 2 && call.op == Op::OpenRO && fake::count(Op::Commit)))
                    fake_product_crypto::fail = true;
            };
            recovery.poll();
            io.before = {};
            CHECK(store.faulted() && !store.ready() && !recovery.motionPending());
            // sameMotionState also uses hashes; restore the crypto backend before comparison.
            fake_product_crypto::fail = false;
            CHECK(sameMotionState(store.state(), before));
            CHECK(fake::count(Op::Set) == (point == 2 ? 1u : 0u));
            const auto persisted = decodedDisk();
            audit();
            fake::reboot();
            MotionStateStore nextStore;
            Hardware nextHardware;
            MotionStateRecovery next(nextStore, nextHardware);
            CHECK(next.begin(pairing(), 200) == MotionLoad::Ready);
            nextHardware.confirm();
            next.poll();
            CHECK(sameMotionState(nextStore.state(), recoveredFeeding(persisted, 200)));
        });
}

struct Executor : motion::DemoExecutor {
    bool fresh = true, still = true, healthyValue = true;
    std::array<bool, 256> missing{};
    unsigned starts = 0, stops = 0;
    motion::DemoExecution executionValue = motion::DemoExecution::Done;
    bool healthy() const override { return healthyValue; }
    bool available() const override { return true; }
    motion::DemoEvidence evidence(uint8_t id) const override {
        return {fresh && !missing[id], still, !healthyValue, 100};
    }
    bool start(const motion::DemoScript&, bool, const std::array<int32_t, 256>&, uint32_t) override {
        ++starts;
        executionValue = motion::DemoExecution::Running;
        return true;
    }
    motion::DemoExecution execution() const override { return executionValue; }
    bool stop() override { ++stops; fresh = false; return true; }
    bool reset() override { return true; }
};
struct SessionHardware : motion::MotionRecoveryHardware {
    motion::ProductSession& product;
    motion::DemoFlowController& flow;
    SessionHardware(motion::ProductSession& p, motion::DemoFlowController& f) : product(p), flow(f) {}
    void supervisedStop(uint32_t now) override { product.recoverAfterRestart(now); }
    bool stationary() const override { return !flow.busy() && !product.ownsMotion() && flow.stationary(); }
};
void integration() {
    for (bool storageFault : {false, true})
        scenario("production ProductSession supervised Stop " + std::to_string(storageFault), [=] {
            const auto before = baseline(2);
            if (storageFault) io.disk.at("productstate").at("record").bytes[8] ^= 1;
            Executor executor;
            motion::DemoFlowController flow(executor);
            motion::DemoConfig config;
            config.configured = true;
            config.axes = {{1, 10, 0, true}, {2, 10, 0, true}};
            CHECK(flow.apply(config) && flow.initialize(10));
            flow.tick(10);
            executor.executionValue = motion::DemoExecution::Done;
            flow.tick(11);
            CHECK(flow.referenceValid() && flow.stage() == DisplayStage::Ready);
            motion::ProductSession product(flow);
            SessionHardware hardware(product, flow);
            MotionStateStore store;
            MotionStateRecovery recovery(store, hardware);
            const auto starts = executor.starts;
            CHECK(recovery.begin(pairing(), 100) == (storageFault ? MotionLoad::Corrupt : MotionLoad::Ready));
            CHECK(executor.stops == 1 && !flow.referenceValid() && flow.busy());
            CHECK(!product.active() && !product.ownsMotion() && !product.eventPending());
            recovery.poll();
            flow.tick(3101); // software Stop timeout cannot replace physical feedback
            product.tick(3101);
            recovery.poll();
            CHECK(!flow.busy() && recovery.motionPending() && executor.starts == starts);
            noWrites();
            executor.fresh = true;
            executor.still = false;
            recovery.poll();
            CHECK(recovery.motionPending());
            executor.still = true;
            executor.missing[2] = true;
            recovery.poll();
            CHECK(recovery.motionPending()); // every configured axis needs fresh evidence
            executor.missing[2] = false;
            recovery.poll();
            CHECK(!recovery.motionPending());
            CHECK(executor.starts == starts && executor.stops == 1 && !flow.referenceValid());
            CHECK(!product.eventPending() && !product.active()); // durable queue is not the legacy gate
            motion::ProductTerminal terminal;
            CHECK(!product.takeTerminal(terminal));
            if (storageFault) {
                CHECK(!store.ready());
                noWrites();
                faultProjection(recovery);
            } else CHECK(sameMotionState(store.state(), recoveredFeeding(before, 100)));
            const auto calls = io.calls.size();
            CHECK(recovery.begin(pairing(), 4000) == recovery.loadResult());
            for (uint32_t now = 4000; now < 4005; ++now) {
                flow.tick(now);
                product.tick(now);
                recovery.poll();
            }
            CHECK(io.calls.size() == calls && executor.starts == starts && executor.stops == 1);
        });
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<const char*, std::function<void()>>> groups{
        {"boot", boot}, {"idle", idle}, {"feeding", feeding}, {"operations", operations},
        {"terminal", terminalAndContext}, {"evidence", evidence}, {"write", writeFaults},
        {"integration", integration}};
    bool found = false;
    for (const auto& group : groups) {
        if (argc > 1 && std::strcmp(argv[1], group.first)) continue;
        found = true;
        const auto oldScenarios = scenarios, oldFailures = failures;
        group.second();
        std::printf("%-12s %u scenarios, %u failures\n", group.first,
                    scenarios - oldScenarios, failures - oldFailures);
        std::fflush(stdout);
    }
    if (!found) { std::fprintf(stderr, "Unknown test group\n"); return 2; }
    std::printf("MotionStateRecovery: %u scenarios, %u failures; production recovery/store/hash/status/session/flow\n",
                scenarios, failures);
    return failures ? 1 : 0;
}
