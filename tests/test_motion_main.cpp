#include "FakeMotionIo.h"
#include "FakeBrainNvs.h"
#include "FakeMotionNvs.h"
#include "BoardPairingRecord.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include "motion_main_crypto_fixture.h"

#include "../device-controller/src/main.cpp"

namespace {
unsigned checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool readOnlyQuery(const twai_message_t& f) {
    const auto id = f.identifier >> 8;
    return f.extd && !f.rtr && !(f.identifier & 255) && id >= 1 && id <= 5 &&
        f.data_length_code == 2 && f.data[1] == 0x6b &&
        (f.data[0] == 0x36 || f.data[0] == 0x35 || f.data[0] == 0x3a || f.data[0] == 0x3b || f.data[0] == 0x27);
}
babytech::v4::Pairing seedPair() {
    using namespace babytech::boardlink;
    babytech::v4::Pairing pair;
    pair.role = babytech::v4::Role::Motion;
    std::strcpy(pair.deviceId, "bt-motion-main");
    std::strcpy(pair.epoch, "0123456789abcdef0123456789abcdef");
    std::strcpy(pair.localPhysicalId, "aabbccddeeff");
    std::strcpy(pair.peerPhysicalId, "112233445566");
    std::array<uint8_t, kPairingRecordMaxSize> bytes{};
    const auto n = encodePairingRecord(pair, bytes.data(), bytes.size()); check(n != 0, "pair encoding");
    fake_brain::io.disk["productpair"]["record"] = {{bytes.begin(), bytes.begin() + n}, fake_brain::Type::Blob};
    MotionStateStore store;
    check(store.installInitial(pair) == MotionWrite::Stored, "state fixture install");
    return pair;
}
using namespace babytech::boardlink;
namespace v4 = babytech::v4;
ProductContext fixtureContext() {
    ProductContext c;
    std::strcpy(c.deviceId, "bt-motion-main"); c.profileVersion = 1;
    std::strcpy(c.babyId, "baby-original"); std::strcpy(c.babyName, "Original baby");
    std::strcpy(c.formulaBrand, "Test formula");
    c.waterMl = 180; c.temperatureC = 45; c.powderGPer100Ml = 25;
    check(validProductContext(c), "context fixture invalid"); return c;
}
void tick(unsigned count = 1) {
    for (unsigned i = 0; i < count; ++i) { motion_io::now += 10; loop(); }
}
void waterSampling() {
    seedPair(); setup();
    check(!lowWaterValid, "boot fabricated stable low-water input");
    motion_io::freezeClock = true;
    const auto sample = [] { motion_io::now = lastLowWaterSampleAt + 100; loop(); };
    if (BABYTECH_LOW_WATER_PIN < 0) {
        for (unsigned i = 0; i < 8; ++i) sample();
        check(!lowWaterValid && lowWaterSamples == 0, "unconfigured input became valid");
    } else {
        check(BABYTECH_LOW_WATER_PIN == 21, "unexpected host water fixture GPIO");
        for (unsigned i = 1; i <= 5; ++i) {
            sample();
            check(lowWater && lowWaterSamples == i && lowWaterValid == (i == 5),
                  "high GPIO bypassed five-sample low-water debounce");
        }
        motion_io::lowWaterLevel = LOW;
        motion_io::now = lastLowWaterSampleAt + 99; loop();
        check(lowWater && lowWaterValid, "GPIO changed before sampling deadline");
        for (unsigned i = 1; i <= 5; ++i) {
            sample();
            check(!lowWater && lowWaterSamples == i && lowWaterValid == (i == 5),
                  "normal GPIO bypassed five-sample recovery debounce");
        }
    }
    const auto resources = productResources(motion_io::now);
    if (BABYTECH_V1_MOTOR_TEST == 1)
        check(resources.waterValid && !resources.lowWater && resources.powderValid &&
              resources.powderGrams == 300.0f && !powderScale.snapshot().calibrated,
              "motor test did not select fixed resources independently of the HX711");
    else
        check(resources.waterValid == lowWaterValid && resources.lowWater == lowWater &&
              !resources.powderValid, "real-sensor mode replaced missing powder with test data");
    check(product.executionAuthorized() && !product.canStart(), "resources bypassed initialization");
    motion_io::freezeClock = false;
}
WebServer::Response http(HTTPMethod method, const char* path, WebServer::Arguments args = {}) {
    const auto before = server.responses.size();
    WebServer::Request req; req.method = method; req.uri = path; req.arguments = std::move(args);
    server.enqueue(std::move(req)); tick();
    check(server.responses.size() == before + 1 && !server.pendingRequests(), "HTTP did not dispatch in loop");
    const auto result = server.responses.back();
    check(result.handlerCalled && result.sendCalls == 1 && result.uri == path && result.method == method, "HTTP handler/response mismatch");
    return result;
}
DynamicJsonDocument parseJson(const std::string& value) {
    // The complete 48-frame CAN trace needs more DOM space on a 64-bit host.
    DynamicJsonDocument doc(32768);
    const auto error = deserializeJson(doc, value);
    if (error) throw std::runtime_error(std::string("actual HTTP JSON invalid: ") + error.c_str());
    check(true, "actual HTTP JSON parsed"); return doc;
}
class BrainPeer {
    class Sink : public v4::ByteSink {
    public:
        bool idle() const override { return true; }
        size_t available() const override { return 64; }
        size_t write(const uint8_t* p, size_t n) override {
            motion_io::uartRx.insert(motion_io::uartRx.end(), p, p + n); return n;
        }
    } sink;
    size_t offset = 0;
public:
    ReadOnlyLink link;
    explicit BrainPeer(v4::Pairing pair) {
        pair.role = v4::Role::Brain; std::swap(pair.localPhysicalId, pair.peerPhysicalId);
        check(link.begin(pair, 0x100000006ULL), "peer begin");
    }
    void step(unsigned count = 1) {
        for (unsigned i = 0; i < count; ++i) {
            while (offset < motion_io::uartTx.size()) link.receive(motion_io::uartTx[offset++], motion_io::now);
            link.poll(motion_io::now, sink);
            tick();
        }
    }
    void connect() {
        step(150);
        check(link.connected(motion_io::now) && link.freshStatus(motion_io::now), "actual main UART handshake/status failed");
    }
};
std::string workbenchId() {
    Status status;
    productRuntime.project(status, true, productRecovery.motionPending());
    check(status.executionOwner == ExecutionOwner::Workbench, "missing workbench identity");
    return status.activeExecutionId;
}
v4::StopRequest stopTarget(const std::string& id) {
    v4::StopRequest stop; stop.scope = v4::StopScope::Workbench;
    check(id.size() == 32, "invalid Stop target");
    for (size_t i = 0; i < 16; ++i) stop.executionId[i] = uint8_t(std::stoul(id.substr(i * 2, 2), nullptr, 16));
    return stop;
}
void commonHttp() {
    auto response = http(HTTP_GET, "/");
    const std::string original(reinterpret_cast<const char*>(indexHtmlStart), size_t(indexHtmlEnd - indexHtmlStart - 1));
    check(response.status == 200 && response.body == original && original.size() > 1000, "wrong embedded debug page");
    for (const auto* path : {"/api/status", "/api/queue", "/api/demo", "/api/scale", "/api/wifi", "/api/ota/status"}) {
        response = http(HTTP_GET, path, !std::strcmp(path, "/api/status") ? WebServer::Arguments{{"id", "1"}} : WebServer::Arguments{});
        if (response.status != 200) throw std::runtime_error(std::string(path) + " status=" + std::to_string(response.status) + " body=" + response.body);
        parseJson(response.body);
    }
    check(http(HTTP_GET, "/api/control/reset").status == 404, "GET reset was allowed");
    check(http(HTTP_GET, "/unregistered").status == 404, "notFound route missing");
    check(http(HTTP_GET, "/api/status", {{"id", std::string("1\0bad", 5)}}).status == 400,
          "embedded NUL parameter bypassed numeric validation");
    check(!motion_io::restarts, "unexpected OTA restart");
}
void recovery(v4::Pairing pair, bool defaultBudget) {
    if (!defaultBudget) {
        // Explicit saved bench configuration, NOT a change to production defaults.
        Preferences prefs; check(prefs.begin("can-query"), "budget fixture open");
        const uint16_t record[] = {1, 50, 20, 100, 100, 2};
        check(prefs.putBytes("v1", record, sizeof(record)) == sizeof(record), "budget fixture save"); prefs.end();
    }
    MotionStateStore prepared;
    check(prepared.load(pair) == MotionLoad::Ready, "recovery fixture load");
    const auto c = fixtureContext();
    check(prepared.saveContext(c) == MotionWrite::Stored, "recovery context save");
    ProductRequest request; request.source = v4::Source::CloudCommand;
    request.command = ProductCommand::Prepare; request.sequence = 1;
    std::strcpy(request.deviceId, c.deviceId); std::strcpy(request.commandId, "before-reset");
    std::strcpy(request.babyId, c.babyId); request.profileVersion = c.profileVersion;
    request.waterMl = c.waterMl; request.temperatureC = c.temperatureC; request.powderGPer100Ml = c.powderGPer100Ml;
    check(prepared.recordDecision(request, true, "accepted", "11111111111111111111111111111111") == MotionWrite::Stored, "recovery intent seed");
    const auto originalSlot = prepared.state().slot;
    setup();
    check(motor.queries().config().queriesPerSecond == (defaultBudget ? 10 : 50), "wrong boot query budget");
    const auto& budget = motor.queries().config();
    check(budget.gapMs == (defaultBudget ? 100 : 20) && budget.timeoutMs == (defaultBudget ? 500 : 100) &&
          budget.cooldownMs == (defaultBudget ? 500 : 100) && budget.maxInflight == 2, "boot budget fields differ from fixture");
    check(productRecovery.motionPending() && productRecovery.executionPending(), "Intent boot didn't request supervised Stop");
    check(!safeForInstallation() && !migrationMaintenance.safeToAcquire(),
          "pending recovery admitted installation without motor feedback");
    const auto commandsBefore = motion_io::canTx.size();
    check(commandsBefore > 0, "Intent boot did not transmit Stop");
    for (const auto& f : motion_io::canTx) check(f.data[0] == 0xfe || f.data[0] == 0x9c, "Intent boot resumed/ enabled motion");
    tick(150);
    check(productRecovery.motionPending() && productState.state().pendingResultCount == 0, "elapsed clock became stationary proof");
    motion_io::automaticFeedback = true; motion_io::missingId = 5;
    tick(400);
    check(productRecovery.motionPending() && productState.state().slot.kind == MotionSlotKind::Intent, "missing configured axis was ignored");
    motion_io::missingId = 0; motion_io::movingId = 5; tick(400);
    check(productRecovery.motionPending() && productState.state().pendingResultCount == 0, "moving axis was ignored");
    motion_io::movingId = 0; tick(400);
    check(!productRecovery.motionPending() && !productRecovery.executionPending(), "fresh configured-axis Stop proof never recovered");
    check(productState.state().slot.kind == MotionSlotKind::Empty && productState.state().pendingResultCount == 1, "interrupted result was not archived");
    const auto& result = productState.state().pendingResults[0];
    check(!result.completed && result.request.sequence == 1 && sameProductRequest(result.request, request) &&
          !std::strcmp(result.eventId, originalSlot.eventId) && !std::strcmp(result.reason, "reboot_during_feed"), "recovery lost original result/recipe");
    for (const auto& f : motion_io::canTx)
        check(readOnlyQuery(f) || f.data[0] == 0xfe || f.data[0] == 0x9c, "recovery replayed movement");
    MotionStateStore readback;
    check(readback.load(pair) == MotionLoad::Ready && readback.state().pendingResultCount == 1 &&
          !std::strcmp(readback.state().pendingResults[0].eventId, originalSlot.eventId), "archive not persisted");
    commonHttp();
}
void workbench(v4::Pairing pair, bool partial) {
    setup(); commonHttp(); BrainPeer peer(pair); peer.connect();
    const auto before = motion_io::canTx.size();
    check(http(HTTP_POST, "/api/queue/start", {{"program", "not-a-command"}, {"repeat", "1"}}).status == 400, "invalid queue did not reject");
    for (size_t i = before; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "invalid queue transmitted a motor command");
    check(peer.link.peerStatus().executionOwner == ExecutionOwner::None, "invalid queue claimed owner");
    if (partial) {
        check(http(HTTP_POST, "/api/enable", {{"id", "1"}, {"enabled", "1"}}).status == 202, "enable preflight failed");
        motion_io::reply(1, {0xf3, 2, 0x6b}); motion_io::reply(1, {0x3a, 1, 0x6b});
        motion_io::reply(1, {0x36, 0, 0, 0, 0, 0, 0x6b}); motion_io::reply(1, {0x35, 0, 0, 0, 0x6b}); tick();
        motion_io::rejectedOpcode = 0xcd; motion_io::rejectedPacket = 1;
        const auto r = http(HTTP_POST, "/api/command", {{"hex", "01CD00003C003C012C00000064020003206B"}});
        if (r.status != 503) throw std::runtime_error("partial TX status=" + std::to_string(r.status) + " body=" + r.body);
        check(r.status == 503 && motor.movementGeneration() > 0, "partial TX evidence was not retained");
        motion_io::rejectedOpcode = motion_io::rejectedPacket = -1;
    } else {
        check(http(HTTP_POST, "/api/queue/start", {{"program", "move 1 10 deg 6 60 300 800\nwait 10000"}, {"repeat", "1"}}).status == 202, "valid queue refused");
    }
    const auto identity = workbenchId();
    peer.step(80);
    check(peer.link.peerStatus().executionOwner == ExecutionOwner::Workbench &&
          identity == peer.link.peerStatus().activeExecutionId, "actual UART STATUS lost workbench ID");
    check(http(HTTP_POST, "/api/command", {{"hex", "bad"}}).status == 400 && workbenchId() == identity, "rejected HTTP changed identity");
    auto stale = stopTarget("22222222222222222222222222222222");
    const auto tx = motion_io::canTx.size();
    check(peer.link.requestStop(stale, motion_io::now), "stale Stop not queued"); peer.step(40);
    check(peer.link.stopSendState() == StopSendState::Rejected && workbenchId() == identity, "stale UART Stop reached later workbench");
    for (size_t i = tx; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "stale Stop transmitted cancellation");
    const auto matching = stopTarget(identity);
    check(peer.link.requestStop(matching, motion_io::now), "matching Stop not queued"); peer.step(40);
    check(peer.link.stopSendState() == StopSendState::Received, "matching UART Stop not acknowledged");
    check(!queue.active() && !motor.affectedAxesStationary(), "Stop receipt falsely claimed stationary");
    check(workbenchId() == identity, "Stop receipt erased unconfirmed owner");
    const auto r = http(HTTP_POST, "/api/control/reset");
    check(r.status == 200 && parseJson(r.body)["stateCleared"] == true, "reset route failed");
    check(!motor.affectedAxesStationary() && !safeForOta(), "software reset became physical stop/OTA proof");
    check(productState.state().pendingResultCount == 0 && productState.state().slot.kind == MotionSlotKind::Empty, "debug operation fabricated feeding record");
}
void uartContext(v4::Pairing pair) {
    const auto context = fixtureContext();
    // An already-persisted profile may rehydrate without mechanical readiness.
    // New profile writes still need stationary feedback (covered separately).
    MotionStateStore cached;
    check(cached.load(pair) == MotionLoad::Ready && cached.saveContext(context) == MotionWrite::Stored,
          "cached context fixture save");
    setup(); BrainPeer peer(pair); peer.connect();
    check(peer.link.requestContext(context, motion_io::now), "context not queued"); peer.step(100);
    if (peer.link.contextSendState() != ContextSendState::Complete || !matchesContextResult(peer.link.contextResponse(), context) ||
        peer.link.contextResponse().status != ContextStatus::Unchanged)
        throw std::runtime_error("context state=" + std::to_string(int(peer.link.contextSendState())) +
            " response=" + std::to_string(int(peer.link.contextResponse().status)));
    check(true, "actual main context callback synchronized");
    check(productState.state().context.profileVersion == 1 && product.context().babyId == context.babyId, "stored and product context differ");
    CommandMessage command; command.remainingTtlMs = 5000;
    command.request.source = v4::Source::CloudCommand; command.request.command = ProductCommand::SetTargetTemp;
    command.request.sequence = 1; command.request.temperatureC = 46;
    std::strcpy(command.request.deviceId, pair.deviceId); std::strcpy(command.request.commandId, "set-temp-test");
    check(peer.link.requestCommand(command, motion_io::now), "command not queued"); peer.step(100);
    check(peer.link.commandSendState() == CommandSendState::Complete && peer.link.commandResponse().accepted &&
          product.targetTemp() == 46 && productState.state().cloudSequence == 1, "main command acceptance missing");
    const auto persisted = fake_brain::io.disk.at("productstate").at("record").bytes;
    check(peer.link.requestCommand(command, motion_io::now), "duplicate not queued"); peer.step(100);
    check(peer.link.commandSendState() == CommandSendState::Complete && peer.link.commandResponse().accepted &&
          fake_brain::io.disk.at("productstate").at("record").bytes == persisted, "duplicate command changed durable decision");
    ResultQuery query; query.source = command.request.source; query.sequence = 1;
    std::strcpy(query.deviceId, pair.deviceId); std::strcpy(query.commandId, command.request.commandId);
    check(peer.link.requestResult(query, motion_io::now), "query not queued"); peer.step(100);
    check(peer.link.resultLookupState() == ResultLookupState::Complete && peer.link.resultQueryResponse().status == ResultQueryStatus::Known &&
          peer.link.resultQueryResponse().accepted, "main result-query callback missing");
    check(product.executionAuthorized() && !product.canStart() && productState.state().slot.kind == MotionSlotKind::Empty,
          "setting temperature bypassed initialization or started physical feeding");
}
}
#include "motion_main_http_fixture.h"
#include "motion_main_recovery_http_fixture.h"
#include "motion_main_ota_fixture.h"
#include "motion_main_prepare_fixture.h"
#include "motion_main_dual_pipe_fixture.h"

void resultDeleteFault(bool applied) {
    using namespace motion_main_dual_pipe;
    Options args;
    args.seedHistory = true;
    seed(args);
    setup();
    const auto original = productState.state();
    const auto disk = fake_brain::io.disk;
    ResultDeleteCommitFault fault;
    fault.arm(applied ? "commit-applied" : "commit");
    const auto sets = fake_brain::count(fake_brain::Op::Set);
    const auto commits = fake_brain::count(fake_brain::Op::Commit);
    CloudReceipt receipt;
    std::strcpy(receipt.deviceId, device);
    std::strcpy(receipt.eventId, original.pendingResults[0].eventId);
    check(resultDelivery.receipt(receipt), "matching archived receipt refused");
    tick();
    check(fault.hit && productState.faulted() && !productState.ready() &&
          sameMotionState(productState.state(), original), "commit fault falsely published new RAM state");
    check(!resultDelivery.receipt(receipt), "faulted owner accepted another deletion");
    tick(20);
    check(fake_brain::count(fake_brain::Op::Set) == sets + 1 &&
          fake_brain::count(fake_brain::Op::Commit) == commits + 1, "latched deletion rewrote storage");
    auto expectedDisk = disk;
    if (applied) expectedDisk["productstate"]["record"] = {fault.candidate, fake_brain::Type::Blob};
    check(fake_brain::io.disk == expectedDisk, "fault changed unrelated NVS evidence");
    MotionStateStore fresh;
    check(fresh.load(pairing()) == MotionLoad::Ready && fresh.state().pendingResultCount == (applied ? 3 : 4),
          "fresh Store did not read durable deletion truth");
    for (const auto& frame : motion_io::canTx)
        check(readOnlyQuery(frame), "historical deletion fault generated a motor action");
    fake_brain::verifyFaults();
}

void waterInputValidation() {
    using motion_main_dual_pipe::input;
    for (const auto* raw : {"{\"low_water_level\":true}", "{\"low_water_level\":2}",
                           "{\"low_water_level\":-1}", "{\"low_water_level\":\"0\"}",
                           "{\"low_water_level\":null}", "{\"low_water_level\":0,\"quit\":true}"}) {
        bool rejected = false;
        try { input(raw); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "invalid GPIO pipe input was accepted");
    }
    if (BABYTECH_LOW_WATER_PIN == 21) {
        check(input("{\"low_water_level\":0}").lowWaterLevel == LOW &&
              input("{\"low_water_level\":1}").lowWaterLevel == HIGH, "explicit GPIO pipe level changed");
    } else {
        bool rejected = false;
        try { input("{\"low_water_level\":0}"); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "default pipe allowed unconfigured GPIO input");
    }
}

void blankInstallation(bool configured) {
    if (configured) motion_main_http::savedDebugProfile();
    setup(); tick(100);
    check(!motion_io::automaticFeedback && !productBoardLink.verifiedPairing() &&
          !productState.ready() && !motor.movementGeneration(), "blank installation fixture is not blank");
    check(!demo.stationary() && !safeForCommissioning() && !productHardware.stationary(),
          "missing motor feedback became physical stop proof");
    check(safeForInstallation() && safeForRemoteInstall() && migrationMaintenance.safeToAcquire(),
          "motor-free idle board cannot install");
    check(!product.canStart(), "installation bypassed mechanical readiness");
    const auto durable = fake_brain::io.disk;
    const auto before = motion_io::canTx.size();
    check(http(HTTP_POST, "/api/queue/start", {{"program", "wait 10000"}, {"repeat", "1"}}).status == 202,
          "wait queue fixture refused");
    check(!safeForInstallation() && !safeForRemoteInstall() && !migrationMaintenance.safeToAcquire(),
          "active queue admitted installation");
    check(http(HTTP_POST, "/api/queue/cancel").status == 200 && !queue.active(), "queue cancellation failed");
    check(!motor.movementGeneration() && safeForInstallation(), "cancelled wait queue locked installation");
    check(fake_brain::io.disk == durable, "installation admission changed NVS");
    for (size_t i = before; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]) || motion_io::canTx[i].data[0] == 0xfe ||
              motion_io::canTx[i].data[0] == 0x9c, "installation admission sent a movement command");

    check(http(HTTP_POST, "/api/queue/start", {{"program", "move 1 10 deg 6 60 300 800\nwait 10000"},
          {"repeat", "1"}}).status == 202, "movement fixture refused");
    tick(20);
    check(motor.movementGeneration() != 0, "movement fixture never submitted motion");
    check(http(HTTP_POST, "/api/control/reset").status == 200, "movement reset failed");
    check(!queue.active() && !motor.operationBusy() && !controlBusy(), "reset did not clear software activity");
    check(!safeForInstallation() && !migrationMaintenance.safeToAcquire(),
          "software reset erased original physical stop requirement");
    if (configured) {
        motion_io::automaticFeedback = true;
        tick(400);
        motion_main_http::stationaryReplies();
        check(motor.movementGeneration() != 0 && safeForCommissioning(),
              "fresh stationary feedback did not restore original commissioning guard");
        check(safeForInstallation() && safeForRemoteInstall() && migrationMaintenance.safeToAcquire(),
              "settled motion permanently blocked installation");
        check(!product.canStart(),
              "restored installation guard authorized real preparation");
    }
}

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "dual-bridge")
            return motion_main_dual_pipe::run(argc - 2, argv + 2);
        check(argc == 2, "case required");
        const std::string which = argv[1];
        fake_motion_nvs::reset();
        if (which == "sdk-crypto") checks += motion_main_crypto_fixture::run();
        else if (which == "sdk-prepare-flow" || which == "sdk-prepare-home-missing" ||
                 which == "sdk-prepare-marker-missing") motion_main_prepare::run(which);
        else if (which.rfind("sdk-motor-test-", 0) == 0) {
            check(BABYTECH_V1_MOTOR_TEST == 1, "motor-test case used real-sensor mode");
            motion_main_prepare::run("sdk-prepare-" + which.substr(15));
        }
        else if (which == "sdk-water-sampling") { waterSampling(); waterInputValidation(); }
        else if (which == "sdk-result-delete-commit" || which == "sdk-result-delete-applied")
            resultDeleteFault(which == "sdk-result-delete-applied");
        else if (which == "recovery-intent" || which == "recovery-default-budget") recovery(seedPair(), which == "recovery-default-budget");
        else if (which == "http-workbench" || which == "http-partial-tx") workbench(seedPair(), which == "http-partial-tx");
        else if (which == "uart-context-command") uartContext(seedPair());
        else if (which == "http-product-guards") motion_main_http::guards(seedPair());
        else if (which == "http-history-debug") motion_main_http::history(seedPair());
        else if (which == "http-product-stop-failure") motion_main_http::cancel(seedPair(), 0, true);
        else if (which == "http-recovery-guards") motion_main_recovery_http::guards(seedPair());
        else if (which == "http-recovery-stop-failure") motion_main_recovery_http::cancel(seedPair(), 0, true);
        else if (which.rfind("http-recovery-stop-", 0) == 0)
            motion_main_recovery_http::cancel(seedPair(), unsigned(std::stoul(which.substr(19))), false);
        else if (which == "http-maintenance-usb") motion_main_http::maintenance(seedPair(), 0);
        else if (which == "http-maintenance-uart-release") motion_main_http::maintenance(seedPair(), 1);
        else if (which == "http-maintenance-uart-expiry") motion_main_http::maintenance(seedPair(), 2);
        else if (which == "http-maintenance-busy") motion_main_http::maintenanceBusy(seedPair());
        else if (which == "install-blank" || which == "install-motion-settled")
            blankInstallation(which == "install-motion-settled");
        else if (which == "http-ota-auth") { seedPair(); motion_main_ota::auth(); }
        else if (which == "http-ota-controls" || which == "http-ota-session-wrap")
            motion_main_ota::controls(seedPair(), which == "http-ota-session-wrap");
        else if (which == "http-ota-write-failure") { seedPair(); motion_main_ota::imageFailure(0); }
        else if (which == "http-ota-image-invalid") { seedPair(); motion_main_ota::imageFailure(1); }
        else if (which == "http-ota-hash-mismatch") { seedPair(); motion_main_ota::imageFailure(2); }
        else if (which == "http-ota-disconnect-abort") { seedPair(); motion_main_ota::imageFailure(3); }
        else if (which == "http-ota-start-unsafe") { seedPair(); motion_main_ota::unsafeStart(); }
        else if (which == "http-ota-challenge-expiry") { seedPair(); motion_main_ota::challengeExpiry(); }
        else if (which.rfind("http-product-stop-", 0) == 0)
            motion_main_http::cancel(seedPair(), unsigned(std::stoul(which.substr(18))), false);
        else {
        if (which == "paired-boot") seedPair();
        else check(which == "empty-boot", "unknown case");
        const auto originalPair = which == "paired-boot" ? fake_brain::io.disk.at("productpair").at("record").bytes : fake_brain::Bytes{};
        const auto originalState = which == "paired-boot" ? fake_brain::io.disk.at("productstate").at("record").bytes : fake_brain::Bytes{};
        setup();
        if (which == "paired-boot") {
            check(productBoardLink.pairingState() == PairingLoad::Ready && productState.ready() &&
                  productRecovery.loadResult() == MotionLoad::Ready &&
                  !std::strcmp(productBoardLink.deviceId(), "bt-motion-main"), "production pairing/state owner did not load");
        } else check(!productBoardLink.verifiedPairing() && !productState.ready(), "empty boot created pairing/state");
        check(canStarted && motion_io::canStarts == 1, "real CAN setup was not invoked");
        check(motion_io::canTx.empty(), "boot transmitted a motor command");
        check(product.executionAuthorized() && !product.canStart(), "boot bypassed mechanical readiness");
        tick(100);
        for (const auto& frame : motion_io::canTx)
            check(readOnlyQuery(frame), "idle loop enabled/moved/stopped a motor");
        commonHttp();
        if (which == "paired-boot") check(fake_brain::io.disk.at("productpair").at("record").bytes == originalPair &&
            fake_brain::io.disk.at("productstate").at("record").bytes == originalState, "boot modified pairing/business NVS");
        }
        fake_motion_nvs::verifyNoEraseOrInit();
        check(fake_brain::io.handles.empty(), "NVS handle leak");
        std::cout << "PASS Motion main " << which << " checks=" << checks << " CAN=" << motion_io::canTx.size() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
