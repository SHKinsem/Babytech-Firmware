// Actual main boot recovery and registered HTTP handlers; SDK/intent fixtures only.
#pragma once

namespace motion_main_recovery_http {
using namespace motion_main_http;

bool exactFrame(const twai_message_t& frame, uint8_t id, std::initializer_list<uint8_t> data) {
    return frame.extd && !frame.rtr && frame.identifier == uint32_t(id) << 8 &&
        frame.data_length_code == data.size() && std::equal(data.begin(), data.end(), frame.data);
}

void recoveryFrames(size_t start, int disableIndex = -1, bool requireStop = false) {
    bool abortSeen = false, stopAfterAbort = false;
    for (size_t i = start; i < motion_io::canTx.size(); ++i) {
        const auto& frame = motion_io::canTx[i];
        const bool abort = exactFrame(frame, 0, {0x9c, 0x48, 0x6b});
        const bool stop = exactFrame(frame, 0, {0xfe, 0x98, 0, 0x6b});
        check(readOnlyQuery(frame) || abort || stop ||
              ((disableIndex == 3 || disableIndex == 4 || disableIndex == 7) &&
               exactFrame(frame, disableIndex == 4 ? 0 : 1, {0xf3, 0xab, 0, 0, 0x6b})),
              "recovery emitted wrong-target/malformed/extra CAN mutation");
        if (abort) abortSeen = true;
        if (stop && abortSeen) stopAfterAbort = true;
    }
    check(!requireStop || stopAfterAbort, "recovery omitted ordered broadcast Abort then Stop");
}

void boot() {
    setup();
    recoveryFrames(0, -1, true);
    check(queue.runId() == 0 && !product.executionAuthorized(), "recovery boot started/authorized a queue");
}

MotionExecutionSlot intent(v4::Pairing pair) {
    savedDebugProfile();
    MotionStateStore store;
    const auto context = fixtureContext();
    check(store.load(pair) == MotionLoad::Ready && store.saveContext(context) == MotionWrite::Stored,
          "recovery HTTP context fixture failed");
    ProductRequest request;
    request.source = v4::Source::CloudCommand; request.command = ProductCommand::Prepare;
    request.sequence = 1; request.profileVersion = context.profileVersion;
    request.waterMl = context.waterMl; request.temperatureC = context.temperatureC;
    request.powderGPer100Ml = context.powderGPer100Ml;
    std::strcpy(request.deviceId, pair.deviceId); std::strcpy(request.babyId, context.babyId);
    std::strcpy(request.commandId, "recovery-http-original");
    check(store.recordDecision(request, true, "accepted", "11111111111111111111111111111111") == MotionWrite::Stored,
          "recovery HTTP intent fixture failed");
    return store.state().slot;
}

void retained(const fake_brain::Database& disk, const MotionExecutionSlot& original, uint32_t generation) {
    check(productRecovery.motionPending() && productRecovery.executionPending() && motionBusy() &&
          !safeForOta() && !productHardware.stationary() && !product.canStart(),
          "unconfirmed recovery was cleared or became Ready/OTA-safe");
    check(productState.state().slot.kind == MotionSlotKind::Intent &&
          sameProductRequest(productState.state().slot.request, original.request) &&
          !std::strcmp(productState.state().slot.executionId, original.executionId) &&
          !std::strcmp(productState.state().slot.eventId, original.eventId) &&
          productState.state().cloudSequence == 1 && productState.state().pendingResultCount == 0 &&
          fake_brain::io.disk == disk && motor.movementGeneration() == generation && !queue.active(),
          "HTTP changed durable identity/watermark or replayed a recovering task");
}

void settled(v4::Pairing pair, const MotionExecutionSlot& original) {
    const auto start = motion_io::canTx.size();
    const auto generation = motor.movementGeneration();
    const auto disk = fake_brain::io.disk;
    auto expectedState = productState.state();
    motion_io::automaticFeedback = true; motion_io::missingId = 5;
    tick(400);
    check(productRecovery.motionPending() && productState.state().pendingResultCount == 0,
          "recovery HTTP ignored a missing configured axis");
    retained(disk, original, generation); recoveryFrames(start);
    motion_io::missingId = 0; motion_io::movingId = 5;
    tick(400);
    check(productRecovery.motionPending() && productState.state().pendingResultCount == 0,
          "recovery HTTP ignored a moving configured axis");
    retained(disk, original, generation); recoveryFrames(start);
    motion_io::movingId = 0;
    tick(400);
    check(!productRecovery.motionPending() && !productRecovery.executionPending() && !motionBusy() &&
          productState.state().slot.kind == MotionSlotKind::Empty && productState.state().pendingResultCount == 1,
          "fresh Stop evidence did not finish recovery");
    const auto& result = productState.state().pendingResults[0];
    check(!result.completed && sameProductRequest(result.request, original.request) &&
          !std::strcmp(result.eventId, original.eventId) && !std::strcmp(result.executionId, original.executionId) &&
          !std::strcmp(result.reason, "reboot_during_feed") && !std::strcmp(result.errorCode, "E_REBOOT_DURING_FEED"),
          "recovery changed original failed result");
    expectedState.slot = MotionExecutionSlot{};
    expectedState.pendingResultCount = 1;
    expectedState.pendingResults[0] = original;
    auto& expectedResult = expectedState.pendingResults[0];
    expectedResult.kind = MotionSlotKind::Terminal;
    expectedResult.uptimeMs = result.uptimeMs;
    std::strcpy(expectedResult.reason, "reboot_during_feed");
    std::strcpy(expectedResult.errorCode, "E_REBOOT_DURING_FEED");
    check(sameMotionState(productState.state(), expectedState),
          "recovery archival changed context/watermarks/decision/digest/target powder");
    MotionStateStore readback;
    check(readback.load(pair) == MotionLoad::Ready && readback.state().pendingResultCount == 1 &&
          sameMotionState(readback.state(), expectedState),
          "recovery failure was not persisted");
    check(!product.executionAuthorized() && !product.canStart(), "recovery bypassed default product authorization");
    recoveryFrames(start);
    check(motor.movementGeneration() == generation && queue.runId() == 0,
          "recovery wait/archival resumed a motion/queue");
    auto expectedDisk = disk;
    expectedDisk["productstate"]["record"] = fake_brain::io.disk.at("productstate").at("record");
    check(fake_brain::io.disk == expectedDisk, "recovery archival modified unrelated NVS");
}

void guards(v4::Pairing pair) {
    const auto original = intent(pair);
    fake_motion_ota::updateAvailable = true;
    boot();
    check(motor.queries().config().queriesPerSecond == 10, "recovery HTTP changed default query budget");
    const auto disk = fake_brain::io.disk;
    const auto generation = motor.movementGeneration();
    const auto start = motion_io::canTx.size();
    commonHttp(); reads();
    const auto scan = http(HTTP_GET, "/api/wifi/scan");
    check(scan.status == 200, "recovery blocked WiFi scan diagnostics"); parseJson(scan.body);
    for (const auto& route : std::array<Route, 9>{{
        {"/api/enable", {{"id", "1"}, {"enabled", "1"}}},
        {"/api/enable-all", {{"enabled", "1"}}},
        {"/api/move", {{"id", "1"}, {"angle", "10"}, {"speed", "6"},
            {"accel", "300"}, {"decel", "300"}, {"current", "800"}}},
        {"/api/command", {{"hex", "01F3AB01006B"}}},
        {"/api/limits", {{"maxSpeedRpm", "60"}, {"maxAngleDeg", "360"},
            {"maxAccelRpmS", "300"}, {"maxCurrentMa", "800"}, {"maxMoveSeconds", "60"}, {"experimentSeconds", "0"}}},
        {"/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}},
        {"/api/queue/start", {{"program", "wait 100"}, {"repeat", "1"}}},
        {"/api/polling", {{"enabled", "0"}}},
        {"/api/demo/config", {{"json", demoConfigJson.c_str()}}}
    }}) {
        expect(route, 409, "demo_busy"); retained(disk, original, generation);
    }
    expect({"/api/scale/config", {{"doutPin", "1"}, {"sckPin", "2"}}}, 409, "motion_active");
    expect({"/api/scale/tare", {}}, 409, "motion_active");
    expect({"/api/scale/calibrate", {{"knownWeightG", "100"}}}, 409, "motion_active");
    expect({"/api/query-budget", {{"queriesPerSecond", "50"}, {"gapMs", "20"},
        {"timeoutMs", "100"}, {"cooldownMs", "100"}, {"maxInflight", "2"}}}, 409, "query_budget_busy");
    expect({"/api/sync-settings", {{"progressTolerance", "0.1"}, {"timeToleranceMs", "100"},
        {"feedbackTimeoutMs", "1000"}, {"prepareTimeoutMs", "1000"}, {"stopTimeoutMs", "1000"},
        {"responseBudgetMs", "10"}, {"completionTenths", "10"}}}, 409, "sync_busy");
    expect({"/api/demo/action", {{"action", "initialize"}}}, 409, "configuration_or_wifi_busy");
    expect({"/api/ota/session", {}}, 409, "machine_not_safe");
    for (const auto* path : {"/api/wifi/connect", "/api/wifi/forget", "/api/wifi/scan"}) {
        const auto response = http(HTTP_POST, path, {{"ssid", "recovery-bench"}, {"password", "test-only-password"}});
        check(response.status == 409 && parseJson(response.body)["error"] == "busy",
              "WiFi mutation ignored unsettled recovery");
    }
    for (const auto* hex : {"bad", "01FE980000", "019C4800", "01F3AB02006B"})
        expect({"/api/command", {{"hex", hex}}}, 400);
    const auto rawStart = motion_io::canTx.size();
    expect({"/api/command", {{"hex", "011F6B"}}}, 202);
    unsigned readsSent = 0;
    for (size_t i = rawStart; i < motion_io::canTx.size(); ++i)
        if (exactFrame(motion_io::canTx[i], 1, {0x1f, 0x6b})) ++readsSent;
    check(readsSent == 1, "recovery raw read omitted/duplicated exact non-periodic query");
    retained(disk, original, generation);
    for (size_t i = start; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]) || exactFrame(motion_io::canTx[i], 1, {0x1f, 0x6b}),
              "read/rejected recovery route mutated CAN");
    check(fake_motion_ota::update.beginCalls == 0 && !motion_io::restarts,
          "recovery HTTP wrote Flash/restarted");
    settled(pair, original);
    const auto archived = fake_brain::io.disk.at("productstate").at("record").bytes;
    expect({"/api/polling", {{"enabled", "0"}}}, 200);
    reads();
    const auto trace = http(HTTP_GET, "/api/trace");
    check(trace.status == 200 && parseJson(trace.body)["frames"].size() == 48,
          "post-recovery diagnostic did not preserve the complete 48-frame trace");
    expect({"/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}}, 200);
    check(safeForOta(), "settled unreceipted recovery result blocked OTA admission");
    expect({"/api/ota/session", {}}, 400, "invalid_manifest");
    expect({"/api/queue/start", {{"program", "wait 10000"}, {"repeat", "1"}}}, 202);
    check(queue.active() && workbenchId().size() == 32, "settled recovery blocked ordinary debug queue");
    expect({"/api/queue/cancel", {}}, 200);
    check(!queue.active() && productState.state().pendingResultCount == 1 &&
          fake_brain::io.disk.at("productstate").at("record").bytes == archived,
          "post-recovery debug deleted/changed unreceipted failure");
}

void cancel(v4::Pairing pair, unsigned index, bool failStop) {
    check(index < cancellations.size(), "unknown recovery cancellation case");
    const auto original = intent(pair);
    boot();
    const auto disk = fake_brain::io.disk;
    const auto generation = motor.movementGeneration();
    const auto start = motion_io::canTx.size();
    if (failStop) motion_io::rejectedOpcode = 0xfe;
    expect(cancellations[index], failStop ? 503 : index == 8 ? 200 : 202,
           failStop ? "stop_unconfirmed" : nullptr);
    retained(disk, original, generation);
    onlyReadOrStop(start, !failStop);
    recoveryFrames(start, int(index), !failStop);
    check(frameSince(start, 0, {0x9c, 0x48, 0x6b}) &&
          frameSince(start, 0, {0xfe, 0x98, 0, 0x6b}) == !failStop,
          "recovery cancel did not submit exact Abort/Stop or hid send failure");
    if (index == 3 || index == 4 || index == 7)
        check(frameSince(start, index == 4 ? 0 : 1, {0xf3, 0xab, 0, 0, 0x6b}),
              "recovery disable omitted exact addressed/broadcast frame");
    motion_io::rejectedOpcode = -1;
    if (failStop) {
        const auto retryStart = motion_io::canTx.size();
        // The previous CanFault remains latched even when the new Stop is sent.
        expect(cancellations[index], 503, "stop_unconfirmed");
        recoveryFrames(retryStart, -1, true);
        retained(disk, original, generation);
    }
    settled(pair, original);
}
} // namespace motion_main_recovery_http
