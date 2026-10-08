// Actual registered HTTP handlers and product UART owner; SDK I/O only.
#pragma once
#include <Update.h>
#include <esp_partition.h>

namespace motion_main_http {
struct Route {
    const char* path;
    WebServer::Arguments args;
};
const std::array<Route, 9> cancellations{{
    {"/api/stop", {{"id", "1"}}},
    {"/api/stop-all", {}},
    {"/api/queue/cancel", {}},
    {"/api/enable", {{"id", "1"}, {"enabled", "0"}}},
    {"/api/enable-all", {{"enabled", "0"}}},
    {"/api/command", {{"hex", "01FE98006B"}}},
    {"/api/command", {{"hex", "019C486B"}}},
    {"/api/command", {{"hex", "01F3AB00006B"}}},
    {"/api/control/reset", {}}
}};
void savedDebugProfile() {
    // Explicit saved bench calibration matching the embedded draft, not a
    // production default, reference proof or physical movement simulation.
    check(saveRotationMm(1, 2) && saveRotationMm(3, 40), "bench rotation fixture save");
}
bool cancellationFrame(const twai_message_t& frame) {
    return frame.data_length_code > 0 &&
        (frame.data[0] == 0xfe || frame.data[0] == 0x9c ||
         (frame.data[0] == 0xf3 && frame.data_length_code > 2 && frame.data[2] == 0));
}
void onlyReadOrStop(size_t before, bool requireStop = false) {
    unsigned stopped = 0;
    for (size_t i = before; i < motion_io::canTx.size(); ++i) {
        const auto& frame = motion_io::canTx[i];
        check(readOnlyQuery(frame) || cancellationFrame(frame), "HTTP guard enabled/moved a motor");
        if (cancellationFrame(frame)) ++stopped;
    }
    if (requireStop) check(stopped != 0, "HTTP cancellation transmitted no Stop/disable");
}
bool frameSince(size_t before, uint8_t id, std::initializer_list<uint8_t> data) {
    for (size_t i = before; i < motion_io::canTx.size(); ++i) {
        const auto& frame = motion_io::canTx[i];
        if (frame.extd && !frame.rtr && frame.identifier == uint32_t(id) << 8 &&
            frame.data_length_code == data.size() && std::equal(data.begin(), data.end(), frame.data))
            return true;
    }
    return false;
}
void expect(const Route& route, int code, const char* error = nullptr) {
    const auto response = http(HTTP_POST, route.path, route.args);
    if (response.status != code) throw std::runtime_error(std::string(route.path) +
        " status=" + std::to_string(response.status) + " body=" + response.body);
    if (error) {
        const auto key = !std::strcmp(route.path, "/api/ota/session") ? "error" : "message";
        if (parseJson(response.body)[key] != error)
            throw std::runtime_error(std::string(route.path) + " wrong rejection: " + response.body);
    }
}
void reads() {
    for (const auto* path : {"/api/status", "/api/limits", "/api/can-debug", "/api/config-result",
            "/api/trace", "/api/logs", "/api/scale", "/api/queue", "/api/query-budget",
            "/api/sync-settings", "/api/motor-distance", "/api/demo", "/api/demo/config",
            "/api/wifi", "/api/ota/status"}) {
        const auto response = http(HTTP_GET, path,
            !std::strcmp(path, "/api/status") || !std::strcmp(path, "/api/motor-distance")
                ? WebServer::Arguments{{"id", "1"}} : WebServer::Arguments{});
        if (response.status != 200) throw std::runtime_error(std::string(path) + " read status=" +
            std::to_string(response.status) + " body=" + response.body);
        try { parseJson(response.body); }
        catch (const std::exception&) {
            throw std::runtime_error(std::string(path) + " invalid JSON: " + response.body.substr(0, 512));
        }
    }
}
void clean(BrainPeer& peer) {
    CommandMessage message;
    message.remainingTtlMs = 5000;
    auto& request = message.request;
    request.source = v4::Source::CloudCommand; request.command = ProductCommand::Clean;
    request.sequence = 1;
    std::strcpy(request.deviceId, "bt-motion-main");
    std::strcpy(request.commandId, "http-matrix-clean");
    check(peer.link.requestCommand(message, motion_io::now), "Clean request not queued");
    for (unsigned i = 0; i < 100 && peer.link.commandSendState() != CommandSendState::Complete; ++i)
        peer.step();
    check(peer.link.commandSendState() == CommandSendState::Complete &&
          peer.link.commandResponse().accepted, "actual UART Clean was not accepted");
    check(productRuntime.ownsMotion() && productRuntime.active() &&
          productState.state().slot.kind != MotionSlotKind::Empty &&
          productState.state().cloudSequence == 1, "accepted Clean did not retain product ownership");
    check(!product.executionAuthorized() && !productHardware.stationary() && !safeForOta(),
          "Clean fixture bypassed non-consumable default or fabricated stationary evidence");
}
void guards(v4::Pairing pair) {
    fake_motion_ota::updateAvailable = true; // Metadata only, never enable Flash.
    savedDebugProfile();
    setup(); BrainPeer peer(pair); peer.connect(); clean(peer);
    const auto saved = fake_brain::io.disk;
    const auto execution = std::string(productState.state().slot.executionId);
    const auto generation = motor.movementGeneration();
    const auto before = motion_io::canTx.size();
    reads();
    for (const auto& route : std::array<Route, 7>{{
            {"/api/enable", {{"id", "1"}, {"enabled", "1"}}},
            {"/api/enable-all", {{"enabled", "1"}}},
            {"/api/move", {{"id", "1"}, {"angle", "10"}, {"speed", "6"},
                {"accel", "300"}, {"decel", "300"}, {"current", "800"}}},
            {"/api/command", {{"hex", "01F3AB01006B"}}},
            {"/api/limits", {{"maxSpeedRpm", "60"}, {"maxAngleDeg", "360"},
                {"maxAccelRpmS", "300"}, {"maxCurrentMa", "800"},
                {"maxMoveSeconds", "60"}, {"experimentSeconds", "0"}}},
            {"/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}},
            {"/api/queue/start", {{"program", "wait 100"}, {"repeat", "1"}}}
        }}) expect(route, 409, "demo_busy");
    expect({"/api/scale/config", {{"doutPin", "1"}, {"sckPin", "2"}}}, 409, "motion_active");
    expect({"/api/scale/tare", {}}, 409, "motion_active");
    expect({"/api/scale/calibrate", {{"knownWeightG", "100"}}}, 409, "motion_active");
    expect({"/api/query-budget", {{"queriesPerSecond", "10"}, {"gapMs", "100"},
        {"timeoutMs", "500"}, {"cooldownMs", "500"}, {"maxInflight", "2"}}}, 409, "query_budget_busy");
    expect({"/api/sync-settings", {{"progressTolerance", "0.1"}, {"timeToleranceMs", "100"},
        {"feedbackTimeoutMs", "1000"}, {"prepareTimeoutMs", "1000"}, {"stopTimeoutMs", "1000"},
        {"responseBudgetMs", "10"}, {"completionTenths", "10"}}}, 409, "sync_busy");
    expect({"/api/polling", {{"enabled", "0"}}}, 409, "demo_busy");
    expect({"/api/demo/config", {{"json", demoConfigJson.c_str()}}}, 409, "demo_busy");
    expect({"/api/demo/action", {{"action", "initialize"}}}, 409, "configuration_or_wifi_busy");
    expect({"/api/ota/session", {}}, 409, "machine_not_safe");
    // Invalid raw input must not take ownership away as an accidental Stop.
    expect({"/api/command", {{"hex", "bad"}}}, 400);
    for (const auto* hex : {"01FE980000", "019C4800", "01F3AB02006B"})
        expect({"/api/command", {{"hex", hex}}}, 400);
    expect({"/api/command", {{"hex", "01366B"}}}, 202);
    check(fake_brain::io.disk == saved && productRuntime.ownsMotion() &&
          execution == productState.state().slot.executionId &&
          motor.movementGeneration() == generation && !queue.active() &&
          product.ownsMotion() && !product.cleaning() && !std::strcmp(demo.reason(), "stop_requested"),
          "read/rejected HTTP route changed product identity, persistent state or motion");
    for (size_t i = before; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "read/rejected HTTP route transmitted cancellation/mutation");
    check(fake_motion_ota::update.beginCalls == 0 && !motion_io::restarts,
          "unsafe OTA route wrote Flash/restarted");
}
void cancel(v4::Pairing pair, unsigned index, bool failStop) {
    check(index < cancellations.size(), "unknown cancellation case");
    savedDebugProfile();
    setup(); BrainPeer peer(pair); peer.connect(); clean(peer);
    const auto execution = std::string(productState.state().slot.executionId);
    const auto persisted = fake_brain::io.disk.at("productstate").at("record").bytes;
    const auto before = motion_io::canTx.size();
    const auto generation = motor.movementGeneration();
    if (failStop) motion_io::rejectedOpcode = 0xfe;
    expect(cancellations[index], failStop ? 503 : index == 8 ? 200 : 202,
           failStop ? "stop_unconfirmed" : nullptr);
    check(productRuntime.ownsMotion() && execution == productState.state().slot.executionId &&
          !productHardware.stationary() && !safeForOta(),
          "HTTP cancellation erased unresolved owner or became physical stop/OTA proof");
    check(productState.state().pendingResultCount == 0 && motor.movementGeneration() == generation,
          "HTTP cancellation invented feeding record/new motion");
    check(fake_brain::io.disk.at("productstate").at("record").bytes == persisted,
          "unsettled cancellation changed durable request/decision/consumption evidence");
    onlyReadOrStop(before, !failStop);
    // The existing queue backend broadcasts Abort + Stop, not five addressed
    // commands. Driver feedback for all configured axes is a separate proof.
    check(frameSince(before, 0, {0x9c, 0x48, 0x6b}), "cancel omitted broadcast Abort");
    check(frameSince(before, 0, {0xfe, 0x98, 0, 0x6b}) == !failStop,
          "cancel broadcast Stop submission disagrees with injected CAN failure");
    if (index == 3 || index == 4 || index == 7)
        check(frameSince(before, index == 4 ? 0 : 1, {0xf3, 0xab, 0, 0, 0x6b}),
              "disable route omitted its exact target/broadcast disable frame");
    if (index == 8) {
        const auto response = parseJson(server.responses.back().body);
        check(response["stateCleared"] == true && response["stopSent"] == true,
              "reset response lost software-clear/Stop-submitted distinction");
    }
}
void history(v4::Pairing pair) {
    fake_motion_ota::updateAvailable = true; // Admission metadata, no Flash writes.
    savedDebugProfile();
    MotionStateStore saved;
    const auto context = fixtureContext();
    check(saved.load(pair) == MotionLoad::Ready && saved.saveContext(context) == MotionWrite::Stored,
          "history fixture context save");
    ProductRequest request;
    request.source = v4::Source::CloudCommand; request.command = ProductCommand::Prepare;
    request.sequence = 1; request.profileVersion = context.profileVersion;
    request.waterMl = context.waterMl; request.temperatureC = context.temperatureC;
    request.powderGPer100Ml = context.powderGPer100Ml;
    std::strcpy(request.deviceId, pair.deviceId); std::strcpy(request.babyId, context.babyId);
    std::strcpy(request.commandId, "historical-http-fixture");
    const char execution[] = "11111111111111111111111111111111";
    check(saved.recordDecision(request, true, "accepted", execution) == MotionWrite::Stored &&
          saved.finishFeeding(execution, true, "", "", 100) == MotionWrite::Stored &&
          saved.archiveFeeding(true) == MotionWrite::Stored, "history fixture archive");
    const auto original = fake_brain::io.disk.at("productstate").at("record").bytes;
    setup();
    check(productState.state().pendingResultCount == 1 && !productRuntime.ownsMotion() &&
          !productRecovery.executionPending() && !productRecovery.motionPending(),
          "settled history acquired motion ownership");
    const auto beforeConfig = motion_io::canTx.size();
    expect({"/api/polling", {{"enabled", "0"}}}, 200);
    reads();
    check(safeForOta(), "settled historical result blocked OTA admission");
    expect({"/api/ota/session", {}}, 400, "invalid_manifest");
    expect({"/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}}, 200);
    expect({"/api/limits", {{"maxSpeedRpm", "60"}, {"maxAngleDeg", "360"},
        {"maxAccelRpmS", "300"}, {"maxCurrentMa", "800"}, {"maxMoveSeconds", "60"},
        {"experimentSeconds", "0"}}}, 200);
    double storedDistance = 0;
    Preferences prefs;
    check(prefs.begin(kMotorDistanceNamespace, true) &&
          prefs.getBytes("d1", &storedDistance, sizeof(storedDistance)) == sizeof(storedDistance),
          "distance write was not persisted");
    prefs.end();
    check(storedDistance == 8 && rotationMmValid[1] && rotationMmValue[1] == 8 &&
          parseJson(http(HTTP_GET, "/api/motor-distance", {{"id", "1"}}).body)["rotationDistance"] == 8,
          "distance RAM/NVS/HTTP readback differ");
    check(motor.debugLimits().maxSpeedTenths == 600 && motor.debugLimits().maxAngleTenths == 3600,
          "accepted limits did not update RAM");
    motion::DebugLimits storedLimits;
    check(prefs.begin("debug-limits", true) &&
          prefs.getBytes("config", &storedLimits, sizeof(storedLimits)) == sizeof(storedLimits),
          "accepted limits were not persisted");
    prefs.end();
    check(storedLimits.maxSpeedTenths == 600 && storedLimits.maxAngleTenths == 3600 &&
          storedLimits.maxAccelRpmS == 300 && storedLimits.maxCurrentMa == 800 &&
          storedLimits.maxMoveDurationMs == 60000 && storedLimits.experimentDurationMs == 0,
          "persisted limits differ from accepted parameters");
    for (size_t i = beforeConfig; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "configuration/read route sent a CAN mutation");
    expect({"/api/queue/start", {{"program", "wait 10000"}, {"repeat", "1"}}}, 202);
    check(queue.active() && workbenchId().size() == 32, "history blocked ordinary debug queue");
    const auto beforeCancel = motion_io::canTx.size();
    expect({"/api/queue/cancel", {}}, 200);
    onlyReadOrStop(beforeCancel, true);
    check(!queue.active() && productState.state().pendingResultCount == 1 &&
          fake_brain::io.disk.at("productstate").at("record").bytes == original,
          "ordinary debugging deleted or changed unreceipted history");
    check(fake_motion_ota::update.beginCalls == 0 && !motion_io::restarts,
          "historical OTA admission wrote Flash/restarted");
}
} // namespace motion_main_http
