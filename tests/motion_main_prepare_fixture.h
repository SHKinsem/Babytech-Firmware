// Test SDK signals only; production HTTP, UART, sensor and execution owners run unchanged.
#pragma once

namespace motion_main_prepare {
const char* config = R"({
  "schema_version":1,"name":"host-prepare-path-not-physical-motion",
  "display":{"baby_name":"Fixture","formula_brand":"Test","water_ml":180,"temperature_c":45},
  "axes":[{"motor_id":1,"rotation_distance_mm":2}],
  "initialization":{"timeout_ms":10000,"zero_axes":[{"motor_id":1,"zero_tolerance_deg":1}],
                    "commands":["enable 1","home 1 2 await","disable 1"]},
  "stages":[
    {"id":"open_cap","timeout_ms":10000,"commands":["wait 400"]},
    {"id":"water","timeout_ms":10000,"commands":["wait 400"]},
    {"id":"powder","timeout_ms":10000,"commands":["wait 400"]},
    {"id":"close_cap","timeout_ms":10000,"commands":["wait 400"]},
    {"id":"mix","timeout_ms":10000,"commands":["wait 400"]}
  ]
})";

void run(const std::string& scenario) {
    const bool motorTest = BABYTECH_V1_MOTOR_TEST == 1;
    check(motorTest || BABYTECH_LOW_WATER_PIN == 21,
          "Real-sensor Prepare requires explicit host-only water fixture");
    const bool differentDistance = scenario == "sdk-prepare-distance-mismatch";
    if (differentDistance) {
        check(saveRotationMm(1, 8) && saveRotationMm(2, 5) && saveRotationMm(3, 10),
              "different manual distances not saved");
    }
    const auto pair = seedPair();
    motion_io::hxReady = true; motion_io::hxRaw = -12040;
    motion_io::flowFeedback = motion_io::automaticFeedback = true;
    motion_io::lowWaterLevel = LOW;
    setup();
    fake_motion_nvs::verifyNoEraseOrInit();
    check(demo.config().configured && !demoConfigJson.isEmpty() && motion_io::canTx.empty(),
          "embedded product JSON did not load without matching NVS or moved at boot");
    check(rotationMmValid[1] == differentDistance &&
          (!differentDistance || rotationMmValue[1] == 8),
          "product boot rewrote manual NVS distances");
    check(product.executionAuthorized() && !product.canStart(), "boot incorrectly ready");
    if (motorTest) {
        motion_io::hxReady = false;
        motion_io::lowWaterLevel = HIGH;
        tick(160);
        const auto resources = productResources(motion_io::now);
        check(!powderScale.snapshot().calibrated && resources.waterValid && !resources.lowWater &&
              resources.powderValid && resources.powderGrams == 300.0f && !product.canStart(),
              "motor-test resources depended on sensors or bypassed initialization");
    } else {
        tick(80);
        check(powderScale.snapshot().hasSample && powderScale.snapshot().raw == -12040 &&
              !powderScale.snapshot().calibrated && motion_io::hxSamples > 4,
              "HX711 signed data or uncalibrated state incorrect");
        check(http(HTTP_POST, "/api/scale/calibrate", {{"knownWeightG", "100"}}).status == 409,
              "calibration bypassed required tare");
        motion_io::hxRaw = 1000;
        check(http(HTTP_POST, "/api/scale/tare").status == 202, "HTTP tare rejected");
        tick(40);
        check(powderScale.tareCompleted() && powderScale.tareOffsetRaw() == 1000,
              "production tare did not consume SDK samples");
        motion_io::hxRaw = 11000; tick(40);
        check(http(HTTP_POST, "/api/scale/calibrate", {{"knownWeightG", "100"}}).status == 200,
              "HTTP calibration rejected");
        motion_io::hxRaw = 31000; tick(80);
        check(powderScale.calibrationPersisted() && powderScale.snapshot().calibrated &&
              powderScale.snapshot().stable && std::fabs(powderScale.snapshot().filteredWeightG - 300) < 0.01f,
              "calibrated GPIO input did not become 300 g");
        motion_io::hxReady = false; tick(160);
        check(powderScale.snapshot().status == motion::LoadCellStatus::Stale && !product.canStart(),
              "missing HX711 input fabricated availability");
        motion_io::hxReady = true; tick(80);
        check(lowWaterValid && !lowWater && !product.canStart(), "resources bypassed initialization/context");
    }
    const bool fiveAxes = scenario == "sdk-prepare-default-budget";
    std::string script = config;
    if (fiveAxes) {
        auto document = parseJson(script);
        auto axes = document["axes"].as<JsonArray>();
        for (unsigned id = 2; id <= 5; ++id) {
            auto axis = axes.createNestedObject();
            axis["motor_id"] = id; axis["rotation_distance_mm"] = 0;
        }
        script.clear(); serializeJson(document, script);
        check(motor.queries().config().queriesPerSecond == 10 &&
              motor.queries().config().gapMs == 100, "five-axis fixture changed query defaults");
    }
    const auto response = http(HTTP_POST, "/api/demo/config", {{"json", script}});
    if (response.status != 200) throw std::runtime_error("fixture config: " + response.body);
    check(demo.config().axes[0].rotationMm == 2 && rotationMmValid[1] == differentDistance &&
          (!differentDistance || rotationMmValue[1] == 8),
          "product Apply used or rewrote the manual conversion table");
    BrainPeer peer(pair); peer.connect();
    const auto context = fixtureContext();
    check(peer.link.requestContext(context, motion_io::now), "context not queued");
    for (unsigned i = 0; i < 300 && peer.link.contextSendState() != ContextSendState::Complete; ++i) peer.step();
    check(peer.link.contextSendState() == ContextSendState::Complete &&
          peer.link.contextResponse().status == ContextStatus::Stored && product.hasContext(),
          "actual UART context did not persist");

    CommandMessage initialize;
    initialize.remainingTtlMs = 5000;
    auto& init = initialize.request;
    init.source = v4::Source::LocalTouch; init.command = ProductCommand::Initialize; init.sequence = 1;
    std::strcpy(init.deviceId, pair.deviceId);
    auto brainPair = pair; brainPair.role = v4::Role::Brain;
    std::swap(brainPair.localPhysicalId, brainPair.peerPhysicalId);
    check(makeLocalCommandId(brainPair, init.sequence, init.commandId), "Initialize identity invalid");
    motion_io::homeCompletionReply = scenario != "sdk-prepare-home-missing";
    motion_io::markerReply = scenario != "sdk-prepare-marker-missing";
    check(peer.link.requestCommand(initialize, motion_io::now), "Initialize not queued");
    std::string initProgress, lastQueueMessage;
    for (unsigned i = 0; i < 2000 && !product.canStart() &&
         demo.stage() != babytech::display::DisplayStage::Error; ++i) {
        peer.step();
        if (lastQueueMessage != queue.message()) {
            lastQueueMessage = queue.message();
            initProgress += " " + std::to_string(motion_io::now) + ":" + lastQueueMessage;
        }
    }
    if (scenario == "sdk-prepare-home-missing" || scenario == "sdk-prepare-marker-missing") {
        check(!product.canStart() && !product.referenceValid() &&
              demo.stage() == babytech::display::DisplayStage::Error &&
              productState.state().pendingResultCount == 0 && queue.runId() == 1,
              "missing home/marker evidence fabricated Ready or feeding stages");
        return;
    }
    if (!product.canStart()) throw std::runtime_error(std::string("Initialize incomplete: ") +
        demo.reason() + " queue=" + queue.message() + " product=" + product.progress() + initProgress);
    for (unsigned i = 0; i < 800 && productState.state().slot.kind != MotionSlotKind::Empty; ++i) peer.step();
    if (peer.link.commandSendState() != CommandSendState::Complete || !peer.link.commandResponse().accepted ||
        !product.referenceValid() || productState.state().slot.kind != MotionSlotKind::Empty)
        throw std::runtime_error(std::string("Initialize completion: response=") +
            peer.link.commandResponse().reason + " state=" + std::to_string(unsigned(peer.link.commandSendState())) +
            " reference=" + std::to_string(product.referenceValid()) +
            " slot=" + std::to_string(unsigned(productState.state().slot.kind)) +
            " stationary=" + std::to_string(productHardware.stationary()) +
            " demo_stationary=" + std::to_string(demo.stationary()) +
            " affected_stationary=" + std::to_string(motor.affectedAxesStationary()) +
            " operation_busy=" + std::to_string(motor.operationBusy()) +
            " endpoint_busy=" + std::to_string(endpoint.busy()) + " motor=" + motor.statusJson(1).c_str());
    check(!motionBusy() && (motion_io::driverFlags[1] & 0x81) == 0x80,
          "actual CAN initialization did not leave a marked, disabled driver");
    if (motorTest) {
        peer.step(160);
        const auto& status = peer.link.peerStatus();
        check(product.canStart() && !powderScale.snapshot().calibrated && status.lowWaterValid &&
              !status.lowWater && status.powderValid && status.powderGrams == 300 &&
              status.snapshot.startEnabled && status.snapshot.temperatureC == 45,
              "sensor-free admission and actual UART telemetry disagree");
        if (fiveAxes) {
            check(status.stationary && !status.motionBusy && !status.activeExecutionId[0] &&
                  demo.config().axes.size() == 5, "five-axis UART status did not become ready");
            motion_io::missingId = 5; peer.step(600);
            check(!peer.link.peerStatus().stationary, "missing fifth axis became stationary");
            motion_io::missingId = 0; peer.step(400);
            check(peer.link.peerStatus().stationary, "restored fifth axis did not recover");
            motion_io::movingId = 5; peer.step(400);
            check(!peer.link.peerStatus().stationary, "moving fifth axis became stationary");
            motion_io::movingId = 0; peer.step(400);
            check(peer.link.peerStatus().stationary, "stopped fifth axis did not recover");
        }
    } else {
        motion_io::hxReady = false; peer.step(160);
        check(!product.canStart() && powderScale.snapshot().status == motion::LoadCellStatus::Stale,
              "initialized device ignored missing powder samples");
        motion_io::hxReady = true; peer.step(80);
        check(product.canStart(), "fresh restored sensor did not recover without reinitialization");
        motion_io::lowWaterLevel = HIGH; peer.step(80);
        check(!product.canStart() && peer.link.peerStatus().lowWaterValid &&
              peer.link.peerStatus().lowWater, "real-sensor low-water input did not block Prepare");
        motion_io::lowWaterLevel = LOW; peer.step(80);
        check(product.canStart(), "normal water did not restore readiness");
    }
    if (scenario.rfind("sdk-prepare-reference-", 0) == 0) {
        if (scenario == "sdk-prepare-reference-fault-wait" || scenario == "sdk-prepare-reference-fault-enable") {
            // RX is consumed in motor.poll before HTTP; the Flow has not ticked yet.
            motion_io::reply(1, {0x3a, 0x88, 0x6b});
            const auto response = scenario == "sdk-prepare-reference-fault-wait"
                ? http(HTTP_POST, "/api/queue/start", {{"program", "wait 400"}, {"repeat", "1"}})
                : http(HTTP_POST, "/api/enable", {{"id", "1"}, {"enabled", "1"}});
            check(response.status == 202 && !product.referenceValid() && !product.canStart(),
                  "same-loop non-motion action erased fault without invalidating reference");
            peer.step(100);
            check(!product.referenceValid() && !product.canStart(), "later feedback revived a faulted reference");
            return;
        }
        const auto productDisk = fake_brain::io.disk.at("productstate").at("record");
        const auto generation = motor.movementGeneration();
        const auto noEffect = [&](const char* path, WebServer::Arguments args, int expected) {
            const auto before = motion_io::canTx.size();
            const auto response = http(HTTP_POST, path, args);
            if (response.status != expected)
                throw std::runtime_error(std::string(path) + ": " + response.body);
            check(product.referenceValid() &&
                  motor.movementGeneration() == generation &&
                  fake_brain::io.disk.at("productstate").at("record") == productDisk,
                  "no-effect request invalidated reference, moved or changed product evidence");
            for (size_t i = before; i < motion_io::canTx.size(); ++i)
                check(readOnlyQuery(motion_io::canTx[i]), "no-effect request transmitted a mutation");
        };
        noEffect("/api/move", {{"id", "1"}, {"angle", "invalid"}}, 400);
        noEffect("/api/command", {{"hex", "01CD6B"}}, 400);
        noEffect("/api/queue/start", {{"program", "move 1 invalid deg 6 60 300 800"}, {"repeat", "1"}}, 400);
        noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "-1"}}, 400);
        noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "nan"}}, 400);
        noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "8"}}, 200);
        check(rotationMmValid[1] && rotationMmValue[1] == 8 && demo.config().axes[0].rotationMm == 2,
              "manual conversion changed product JSON conversion");
        for (const auto op : {fake_brain::Op::OpenRW, fake_brain::Op::Set, fake_brain::Op::Commit}) {
            fake_brain::fail(op, fake_brain::count(op) + 1);
            noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "9"}}, 500);
            check(rotationMmValue[1] == 8, "failed save published candidate RAM distance");
            fake_brain::verifyFaults();
        }
        {
            fake_motion_nvs::AllowPreferencesDeletion deletion;
            fake_brain::fail(fake_brain::Op::Erase, fake_brain::count(fake_brain::Op::Erase) + 1);
            noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "0"}}, 500);
            check(rotationMmValid[1] && rotationMmValue[1] == 8, "failed clear changed RAM distance");
            fake_brain::verifyFaults();
            noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "0"}}, 200);
        }
        check(!rotationMmValid[1] && demo.config().axes[0].rotationMm == 2,
              "manual clear erased product conversion");
        noEffect("/api/limits", {{"maxSpeedRpm", "invalid"}}, 400);
        const WebServer::Arguments limits{{"maxSpeedRpm", "600"}, {"maxAngleDeg", "720"},
            {"maxAccelRpmS", "600"}, {"maxCurrentMa", "1200"}, {"maxMoveSeconds", "120"},
            {"experimentSeconds", "10"}};
        fake_brain::fail(fake_brain::Op::Set, fake_brain::count(fake_brain::Op::Set) + 1);
        noEffect("/api/limits", limits, 500); fake_brain::verifyFaults();
        noEffect("/api/limits", limits, 200);
        noEffect("/api/move", {{"id", "1"}, {"angle", "10"}, {"speed", "6"},
            {"accel", "60"}, {"decel", "300"}, {"current", "800"}}, 409);
        check(http(HTTP_POST, "/api/command", {{"hex", "01366B"}}).status == 202 &&
              product.referenceValid(), "read command invalidated product reference");
        check(http(HTTP_POST, "/api/queue/start", {{"program", "wait 400"}, {"repeat", "1"}}).status == 202 &&
              product.referenceValid(), "wait-only queue invalidated product reference");
        const auto waitIdentity = workbenchId();
        noEffect("/api/enable", {{"id", "1"}, {"enabled", "1"}}, 409);
        noEffect("/api/motor-distance", {{"id", "1"}, {"rotationDistance", "9"}}, 409);
        check(workbenchId() == waitIdentity, "rejected request replaced wait queue identity");
        peer.step(100);
        check(product.canStart() && product.referenceValid() && !queue.active(),
              "wait-only queue required reinitialization");
        auto previousIdentity = waitIdentity;
        for (const auto* program : {"hex 01 36 6B\nwait 1000", "can ext 100 36 6B\nwait 1000",
             "hex 01 22 6B\nwait 1000", "can ext 100 22 6B\nwait 1000"}) {
            check(http(HTTP_POST, "/api/queue/start", {{"program", program}, {"repeat", "1"}}).status == 202 &&
                  product.referenceValid(), "read-only raw queue invalidated reference");
            const auto currentIdentity = workbenchId();
            check(currentIdentity != previousIdentity, "accepted read queue did not acquire a new workbench identity");
            check(peer.link.requestStop(stopTarget(previousIdentity), motion_io::now), "stale Stop fixture rejected locally");
            peer.step(20);
            check(peer.link.stopSendState() == StopSendState::Rejected && workbenchId() == currentIdentity &&
                  queue.active() && product.referenceValid(), "stale Stop cancelled the later read/wait owner");
            peer.step(100);
            check(product.canStart() && product.referenceValid(), "read-only raw queue required Initialize");
            previousIdentity = currentIdentity;
        }
        for (const auto* hex : {"0145660003206B", "0111183600646B"}) {
            check(http(HTTP_POST, "/api/command", {{"hex", hex}}).status == 202 &&
                  product.referenceValid(), "non-coordinate configuration invalidated reference");
            peer.step(100);
            check(product.canStart() && product.referenceValid(), "parameter feedback did not recover without Initialize");
        }
        if (scenario == "sdk-prepare-reference-config-failure") {
            motion_io::rejectedOpcode = 0x45;
            check(http(HTTP_POST, "/api/command", {{"hex", "0145660003206B"}}).status == 503 &&
                  !product.referenceValid() && !product.canStart(), "configuration TX fault retained reference");
            return;
        }
        if (scenario == "sdk-prepare-reference-move" || scenario == "sdk-prepare-reference-partial" ||
            scenario == "sdk-prepare-reference-enable") {
            check(http(HTTP_POST, "/api/enable", {{"id", "1"}, {"enabled", "1"}}).status == 202 &&
                  product.referenceValid(), "enable without motion invalidated reference");
            peer.step(70);
            if (scenario == "sdk-prepare-reference-enable") {
                check(product.referenceValid(), "enable-only feedback required reinitialization");
                return;
            }
        }
        if (scenario == "sdk-prepare-reference-move" || scenario == "sdk-prepare-reference-partial") {
            const bool partial = scenario == "sdk-prepare-reference-partial";
            if (partial) { motion_io::rejectedOpcode = 0xcd; motion_io::rejectedPacket = 1; }
            const auto response = http(HTTP_POST, "/api/move", {{"id", "1"}, {"angle", "10"},
                {"speed", "6"}, {"accel", "60"}, {"decel", "300"}, {"current", "800"}});
            check(response.status == (partial ? 503 : 202) && !product.referenceValid() &&
                  motor.movementGeneration() != generation && !product.canStart(),
                  "accepted/partial motion retained product reference");
            return;
        }
        if (scenario == "sdk-prepare-reference-zero") {
            check(http(HTTP_POST, "/api/command", {{"hex", "010A6D6B"}}).status == 202 &&
                  !product.referenceValid() && !product.canStart(), "origin reset retained reference");
            return;
        }
        if (scenario == "sdk-prepare-reference-queue") {
            check(http(HTTP_POST, "/api/queue/start", {{"program", "move 1 10 deg 6 60 300 800"},
                {"repeat", "1"}}).status == 202 && !product.referenceValid() && !product.canStart(),
                  "accepted motion queue retained reference");
            return;
        }
        if (scenario == "sdk-prepare-reference-can-origin" || scenario == "sdk-prepare-reference-hex-zero" ||
            scenario == "sdk-prepare-reference-unknown") {
            const char* program = scenario == "sdk-prepare-reference-can-origin" ? "can ext 100 93 88 00 6B" :
                scenario == "sdk-prepare-reference-hex-zero" ? "hex 01 0A 6D 6B" : "can std 123 DE AD";
            check(http(HTTP_POST, "/api/queue/start", {{"program", program}, {"repeat", "1"}}).status == 202 &&
                  !product.referenceValid() && !product.canStart(), "reference-changing raw queue retained reference");
            return;
        }
        check(motor.movementGeneration() == generation &&
              fake_brain::io.disk.at("productstate").at("record") == productDisk,
              "configuration/read/enable changed motion generation or product evidence");
    }
    const auto runBefore = queue.runId();
    CommandMessage prepare;
    prepare.remainingTtlMs = 5000;
    auto& request = prepare.request;
    request.source = v4::Source::CloudCommand; request.command = ProductCommand::Prepare; request.sequence = 1;
    std::strcpy(request.deviceId, pair.deviceId); std::strcpy(request.commandId, "host-new-prepare");
    std::strcpy(request.babyId, context.babyId); request.profileVersion = context.profileVersion;
    request.waterMl = 150; request.temperatureC = 42; request.powderGPer100Ml = context.powderGPer100Ml;
    check(peer.link.requestCommand(prepare, motion_io::now), "Prepare not queued");
    if (scenario == "sdk-prepare-stop") {
        for (unsigned i = 0; i < 100 && !queue.active(); ++i) peer.step();
        check(product.active() && queue.active() && productRuntime.ownsMotion(),
              "motor-test Stop fixture did not begin Prepare");
        auto stop = stopTarget(productState.state().slot.executionId);
        stop.scope = v4::StopScope::Product;
        check(peer.link.requestStop(stop, motion_io::now), "product Stop not queued");
        for (unsigned i = 0; i < 800 && productState.state().pendingResultCount == 0; ++i) peer.step();
        check(peer.link.stopSendState() == StopSendState::Received && !product.active() &&
              productState.state().pendingResultCount == 1 &&
              !productState.state().pendingResults[0].completed &&
              !std::strcmp(productState.state().pendingResults[0].reason, "stopped") &&
              queue.runId() == runBefore + 1,
              "motor-test Stop continued stages or produced a successful result");
        return;
    }
    std::array<bool, 5> stages{};
    for (unsigned i = 0; i < 800 && productState.state().pendingResultCount == 0; ++i) {
        peer.step();
        const auto stage = demo.stage();
        const std::array<babytech::display::DisplayStage, 5> expected{{
            babytech::display::DisplayStage::UnscrewingCap, babytech::display::DisplayStage::DispensingWater,
            babytech::display::DisplayStage::DispensingPowder, babytech::display::DisplayStage::ScrewingCap,
            babytech::display::DisplayStage::Mixing}};
        for (size_t n = 0; n < expected.size(); ++n) if (stage == expected[n]) stages[n] = true;
    }
    if (peer.link.commandSendState() != CommandSendState::Complete || !peer.link.commandResponse().accepted)
        throw std::runtime_error(std::string("actual UART Prepare: ") + peer.link.commandResponse().reason +
            " state=" + std::to_string(unsigned(peer.link.commandSendState())) +
            " flow=" + demo.reason() + " queue=" + queue.message());
    check(std::all_of(stages.begin(), stages.end(), [](bool seen) { return seen; }) &&
          queue.runId() == runBefore + 5, "five production stage queues did not execute exactly once");
    check(productState.state().pendingResultCount == 1 && productState.state().slot.kind == MotionSlotKind::Empty,
          "new terminal did not archive");
    const auto& result = productState.state().pendingResults[0];
    check(result.completed && sameProductRequest(result.request, request) && result.targetPowderG == 37.5f &&
          result.reason[0] == 0 && result.errorCode[0] == 0,
          "new terminal recipe/identity/outcome changed");
    const auto disk = fake_brain::io.disk;
    const auto runs = queue.runId();
    check(peer.link.requestCommand(prepare, motion_io::now), "duplicate Prepare not queued"); peer.step(100);
    check(peer.link.commandSendState() == CommandSendState::Complete && peer.link.commandResponse().accepted &&
          fake_brain::io.disk == disk && queue.runId() == runs && productState.state().pendingResultCount == 1,
          "duplicate Prepare replayed queues or changed persistent result");
    MotionStateStore readback;
    check(readback.load(pair) == MotionLoad::Ready && sameMotionState(readback.state(), productState.state()),
          "new Prepare durable state differs on reopen");
    check(!motion_io::restarts && !fake_motion_ota::update.beginCalls, "fixture wrote firmware/restarted");
    const auto firstRequest = request;

    // No Cloud receipt is supplied: one waiting result must not prevent another bottle.
    for (unsigned i = 0; i < 600 && demo.busy(); ++i) peer.step();
    check(!demo.busy(), "existing Complete display hold did not finish");
    initialize.request.sequence = 2;
    check(makeLocalCommandId(brainPair, 2, initialize.request.commandId), "second Initialize identity invalid");
    check(peer.link.requestCommand(initialize, motion_io::now), "second Initialize not queued");
    for (unsigned i = 0; i < 300 &&
        (peer.link.commandSendState() != CommandSendState::Complete ||
         productState.state().slot.kind != MotionSlotKind::Empty); ++i) peer.step();
    check(peer.link.commandResponse().accepted && product.canStart() &&
          productState.state().pendingResultCount == 1,
          "waiting terminal blocked initialization/next feeding");
    prepare.request.sequence = 2;
    std::strcpy(prepare.request.commandId, "host-next-prepare");
    prepare.request.waterMl = 180; prepare.request.temperatureC = 43;
    const auto secondRun = queue.runId();
    check(peer.link.requestCommand(prepare, motion_io::now), "second Prepare not queued");
    for (unsigned i = 0; i < 800 && productState.state().pendingResultCount != 2; ++i) peer.step();
    check(peer.link.commandSendState() == CommandSendState::Complete && peer.link.commandResponse().accepted &&
          productState.state().pendingResultCount == 2 && queue.runId() == secondRun + 5,
          "second Prepare did not archive five stages while first awaited receipt");
    const auto& second = productState.state().pendingResults[1];
    check(second.completed && sameProductRequest(second.request, prepare.request) && second.targetPowderG == 45 &&
          sameProductRequest(productState.state().pendingResults[0].request, firstRequest) &&
          std::strcmp(second.eventId, productState.state().pendingResults[0].eventId) != 0,
          "second feed overwrote first result or attribution");
    MotionStateStore secondReadback;
    check(secondReadback.load(pair) == MotionLoad::Ready &&
          sameMotionState(secondReadback.state(), productState.state()), "two-result durable reopen mismatch");
}
} // namespace motion_main_prepare
