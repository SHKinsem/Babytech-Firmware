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
    check(!product.canStart() && !productHardware.stationary() && !safeForOta(),
          "Clean fixture allowed Prepare or fabricated stationary evidence");
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

void usb(const char* line, const char* response) {
    const size_t before = motion_io::usbTx.size();
    const std::string input = std::string(line) + "\n";
    motion_io::usbRx.insert(motion_io::usbRx.end(), input.begin(), input.end());
    tick(12);
    if (!motion_io::usbRx.empty() ||
        motion_io::usbTx.substr(before).find(std::string("[maint] ") + response + "\n") == std::string::npos)
        throw std::runtime_error(std::string(line) + " expected=" + response + " output=" + motion_io::usbTx.substr(before));
}

// A support peer uses actual discovery/maintenance codecs over the SDK UART.
// No direct assignment to Motion's reservation state or manual maintenance ACK.
class MaintenancePeer {
    v4::Parser parser_;
    BoardDiscovery discovery_;
    BoardMaintenance maintenance_;
    size_t offset_ = 0;
    unsigned renewals_ = 0;
    uint32_t lastLeaseAt_ = 0;
    static size_t send(const v4::Frame& frame) {
        uint8_t bytes[v4::kMaxFrame];
        const auto size = v4::encode(frame, bytes, sizeof bytes);
        check(size != 0, "support frame encoding failed");
        motion_io::uartRx.insert(motion_io::uartRx.end(), bytes, bytes + size);
        return size;
    }
public:
    explicit MaintenancePeer(v4::Pairing pair) {
        pair.role = v4::Role::Brain;
        std::swap(pair.localPhysicalId, pair.peerPhysicalId);
        check(discovery_.begin(pair.role, pair.localPhysicalId, 0x200000006ULL,
              DiscoveryPairState::Ready, &pair) &&
              maintenance_.begin(pair.role, pair.localPhysicalId, 0x200000006ULL, &pair),
              "support peer begin failed");
    }
    void step() {
        bool leaseSent = false;
        size_t leaseSize = 0;
        v4::Frame frame;
        while (offset_ < motion_io::uartTx.size()) {
            if (!parser_.push(motion_io::uartTx[offset_++], motion_io::now, frame)) continue;
            if (frame.kind == v4::Kind::Discovery) discovery_.receive(frame, motion_io::now);
            else if (frame.kind == v4::Kind::MigrationMaintenance) maintenance_.receive(frame, motion_io::now);
        }
        discovery_.poll(motion_io::now);
        maintenance_.poll(motion_io::now);
        if (const auto* outgoing = discovery_.outgoing()) { send(*outgoing); discovery_.queued(); }
        if (const auto* outgoing = maintenance_.outgoing()) {
            if (outgoing->payload[0] == 2) ++renewals_;
            leaseSent = outgoing->payload[0] == 1 || outgoing->payload[0] == 2;
            leaseSize = send(*outgoing); maintenance_.queued();
        }
        const auto readsBefore = motion_io::uartReadCount;
        tick();
        if (leaseSent) {
            check(motion_io::uartRx.empty() && motion_io::uartReadCount - readsBefore == leaseSize,
                  "lease frame not consumed exactly once in its recorded loop");
            lastLeaseAt_ = motion_io::lastUartReadAt;
        }
    }
    void acquire(bool allowed) {
        check(discovery_.request("bt-motion-main", motion_io::now), "discovery request failed");
        for (unsigned i = 0; i < 100 && discovery_.result().state != DiscoveryState::Found; ++i) step();
        check(discovery_.result().state == DiscoveryState::Found &&
              discovery_.result().pairingState == DiscoveryPairState::Ready,
              "actual main discovery response missing");
        check(maintenance_.request("bt-motion-main", discovery_.result(),
              "0123456789abcdef0123456789abcdef", motion_io::now), "UART reservation request failed");
        for (unsigned i = 0; i < 100 && maintenance_.state() == BoardMaintenanceState::Pending; ++i) step();
        check(maintenance_.state() == (allowed ? BoardMaintenanceState::Active : BoardMaintenanceState::Unsafe) &&
              productBoardLink.maintenanceActive() == allowed, "actual main reservation decision mismatch");
    }
    void release() {
        check(maintenance_.release(motion_io::now), "UART reservation release request failed");
        for (unsigned i = 0; i < 100 && maintenance_.state() == BoardMaintenanceState::Releasing; ++i) step();
        check(maintenance_.state() == BoardMaintenanceState::Released &&
              !productBoardLink.maintenanceActive(), "UART release did not unlock Motion");
    }
    void keepAlive() {
        for (unsigned i = 0; i < BoardMaintenance::kLeaseMs / 10 + 50; ++i) {
            step();
            check(maintenance_.state() == BoardMaintenanceState::Active &&
                  productBoardLink.maintenanceActive(), "healthy renewing lease expired");
        }
        check(renewals_ >= 5, "keepalive did not send actual UART renewals");
    }
    void expire() {
        check(lastLeaseAt_ && productBoardLink.maintenanceActive() && motion_io::uartRx.empty(),
              "expiry lacks last consumed lease frame");
        // Freeze only the host SDK clock for each boundary iteration; XMotor
        // delays and loop's delay(1) otherwise move time during the observation.
        // No peer poll or release is sent, and ordinary tests keep advancing time.
        check(uint32_t(motion_io::now - lastLeaseAt_) < BoardMaintenance::kLeaseMs - 1,
              "fixture already passed exact expiry boundary");
        motion_io::now = lastLeaseAt_ + BoardMaintenance::kLeaseMs - 1;
        motion_io::freezeClock = true;
        loop();
        check(productBoardLink.maintenanceActive() &&
              motion_io::now == lastLeaseAt_ + BoardMaintenance::kLeaseMs - 1,
              "reservation released before 3000 ms");
        ++motion_io::now;
        loop();
        check(!productBoardLink.maintenanceActive(), "reservation not released at 3000 ms");
        motion_io::freezeClock = false;
    }
};

void commissioningGuards() {
    const auto saved = fake_brain::io.disk;
    const auto before = motion_io::canTx.size();
    const auto generation = motor.movementGeneration();
    reads();
    for (const auto& route : std::array<Route, 16>{{
            {"/api/enable", {{"id", "1"}, {"enabled", "1"}}},
            {"/api/enable-all", {{"enabled", "1"}}},
            {"/api/command", {{"hex", "01F3AB01006B"}}},
            {"/api/move", {{"id", "1"}, {"angle", "10"}, {"speed", "6"}}},
            {"/api/limits", {}}, {"/api/polling", {{"enabled", "0"}}},
            {"/api/scale/config", {}}, {"/api/scale/tare", {}}, {"/api/scale/calibrate", {}},
            {"/api/control/reset", {}}, {"/api/query-budget", {}}, {"/api/sync-settings", {}},
            {"/api/queue/start", {{"program", "wait 100"}, {"repeat", "1"}}},
            {"/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}},
            {"/api/demo/config", {{"json", demoConfigJson.c_str()}}},
            {"/api/demo/action", {{"action", "initialize"}}}
        }}) expect(route, 409, "commissioning_active");
    expect({"/api/command", {{"hex", "01FE980000"}}}, 400);
    // 1F is supported read-only firmware info; idle probing never emits it.
    const auto beforeRead = motion_io::canTx.size();
    expect({"/api/command", {{"hex", "011F6B"}}}, 202);
    unsigned explicitReads = 0;
    for (size_t i = beforeRead; i < motion_io::canTx.size(); ++i) {
        const auto& f = motion_io::canTx[i];
        if (f.extd && !f.rtr && f.identifier == (1u << 8) &&
            f.data_length_code == 2 && f.data[0] == 0x1f && f.data[1] == 0x6b) ++explicitReads;
    }
    check(explicitReads == 1, "explicit raw Read was missing/duplicated or replaced by idle queries");
    expect({"/api/ota/session", {}}, 409, "machine_not_safe");
    check(commissioningActive() && !safeForOta() && fake_brain::io.disk == saved &&
          motor.movementGeneration() == generation && !queue.active(),
          "commissioning guards changed reservation, NVS or motion");
    for (size_t i = before; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]) ||
              (motion_io::canTx[i].extd && !motion_io::canTx[i].rtr &&
               motion_io::canTx[i].identifier == (1u << 8) && motion_io::canTx[i].data_length_code == 2 &&
               motion_io::canTx[i].data[0] == 0x1f && motion_io::canTx[i].data[1] == 0x6b),
              "commissioning read/rejection sent mutation or cancellation");
}

void commissioningStops() {
    const auto saved = fake_brain::io.disk;
    const auto before = motion_io::canTx.size();
    const auto generation = motor.movementGeneration();
    for (size_t i = 0; i < cancellations.size() - 1; ++i) {
        const auto start = motion_io::canTx.size();
        // Without a product owner queue/cancel is the ordinary debug 200 route.
        expect(cancellations[i], i == 2 ? 200 : 202);
        check(commissioningActive(), "HTTP cancellation silently released maintenance");
        struct Expected { uint8_t id; std::vector<uint8_t> bytes; };
        std::vector<Expected> expected;
        if (i == 1 || i == 2 || i == 4) {
            expected.push_back({0, {0x9c, 0x48, 0x6b}});
            expected.push_back({0, {0xfe, 0x98, 0, 0x6b}});
        } else if (i == 0 || i == 5) expected.push_back({1, {0xfe, 0x98, 0, 0x6b}});
        else if (i == 6) {
            expected.push_back({1, {0x9c, 0x48, 0x6b}});
            expected.push_back({1, {0xfe, 0x98, 0, 0x6b}});
        }
        if (i == 3 || i == 4 || i == 7) expected.push_back({uint8_t(i == 4 ? 0 : 1), {0xf3, 0xab, 0, 0, 0x6b}});
        size_t seen = 0;
        for (size_t n = start; n < motion_io::canTx.size(); ++n) {
            const auto& f = motion_io::canTx[n];
            if (readOnlyQuery(f)) continue;
            check(seen < expected.size(), "maintenance cancellation emitted extra non-query frame");
            const auto& e = expected[seen++];
            check(f.extd && !f.rtr && f.identifier == uint32_t(e.id) << 8 &&
                  f.data_length_code == e.bytes.size() && std::equal(e.bytes.begin(), e.bytes.end(), f.data),
                  "maintenance cancellation frame target/DLC/payload/order differ");
        }
        check(seen == expected.size(), "maintenance cancellation omitted a required frame");
    }
    onlyReadOrStop(before, true);
    check(fake_brain::io.disk == saved && motor.movementGeneration() == generation &&
          !safeForOta() && !fake_motion_ota::update.beginCalls && !motion_io::restarts,
          "maintenance Stop changed durable state/new motion or admitted OTA");
}

void stationaryReplies() {
    // Fresh SDK CAN packets, not directly assigned controller safety flags.
    for (uint8_t id = 1; id <= 5; ++id) {
        motion_io::reply(id, {0x36, 0, 0, 0, 0, 0, 0x6b});
        motion_io::reply(id, {0x35, 0, 0, 0, 0x6b});
        motion_io::reply(id, {0x3a, 1, 0x6b});
    }
    tick();
}
void maintenance(v4::Pairing pair, unsigned mode) {
    fake_motion_ota::updateAvailable = true;
    savedDebugProfile(); setup(); stationaryReplies();
    const auto durable = fake_brain::io.disk;
    const auto enteringCan = motion_io::canTx.size();
    const auto generation = motor.movementGeneration();
    MaintenancePeer peer(pair);
    if (mode == 0) { usb("MAINT BEGIN", "active"); check(commissioningSession.active(), "USB session not active"); }
    else peer.acquire(true);
    check(fake_brain::io.disk == durable && motor.movementGeneration() == generation,
          "maintenance acquisition changed durable state or motion");
    for (size_t i = enteringCan; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "maintenance acquisition cancelled or moved a motor");
    commissioningGuards(); commissioningStops();
    const auto exitingCan = motion_io::canTx.size();
    if (mode == 0) {
        tick(310);
        check(commissioningSession.active(), "USB session incorrectly expired as UART lease");
        usb("MAINT END", "unsafe");
        check(commissioningSession.active(), "unsafe USB END silently cleared session");
        // Explicit post-Stop CAN SDK replies, not elapsed time, establish safety.
        stationaryReplies(); usb("MAINT END", "inactive");
    } else if (mode == 1) { peer.keepAlive(); peer.release(); }
    else {
        peer.keepAlive(); peer.expire();
    }
    check(!commissioningActive() && fake_brain::io.disk == durable,
          "maintenance exit changed durable state or left routes locked");
    check(motor.movementGeneration() == generation, "maintenance exit replayed motion");
    for (size_t i = exitingCan; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "maintenance exit emitted cancellation/mutation");
    const auto persisted = fake_brain::io.disk;
    expect({"/api/polling", {{"enabled", "0"}}}, 200);
    check(fake_brain::io.disk == persisted && !motor.autoQueriesEnabled(),
          "maintenance exit failed harmless debug restoration");
}

void maintenanceBusy(v4::Pairing pair) {
    savedDebugProfile(); setup(); BrainPeer productPeer(pair); productPeer.connect(); clean(productPeer);
    const auto saved = fake_brain::io.disk;
    const auto execution = std::string(productState.state().slot.executionId);
    const auto before = motion_io::canTx.size();
    usb("MAINT BEGIN", "unsafe");
    MaintenancePeer support(pair); support.acquire(false);
    check(!commissioningActive() && productRuntime.ownsMotion() &&
          execution == productState.state().slot.executionId && fake_brain::io.disk == saved,
          "refused maintenance stole product ownership/evidence");
    for (size_t i = before; i < motion_io::canTx.size(); ++i)
        check(readOnlyQuery(motion_io::canTx[i]), "refused maintenance emitted Stop/mutation");
}
} // namespace motion_main_http
