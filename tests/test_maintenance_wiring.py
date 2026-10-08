"""Static entry-point regression checks, not live HTTP/USB/OTA tests."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1:end - 1]


class MaintenanceWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.motion = (ROOT / "device-controller/src/main.cpp").read_text()
        cls.brain = (ROOT / "main-controller/src/main.cpp").read_text()
        cls.ota = (ROOT / "shared/WifiOta/src/WifiOta.cpp").read_text()
        cls.console = (ROOT / "shared/ProductBoardLink/src/MaintenanceUsbConsole.h").read_text()

    def test_brain_context_owner_uses_shared_store_after_urgent_io_without_global_gate(self):
        self.assertIn("contextSync(controllerLink, productState)", self.brain)
        callback = function_body(self.brain, "void receiveCloudContext(")
        self.assertIn("contextSync.receive(context)", callback)
        self.assertNotIn("saveContext", callback)
        setup = function_body(self.brain, "void setup()")
        self.assertIn("network.setContextHandler(receiveCloudContext, nullptr)", setup)
        self.assertIn("cloudDispatcher.setPrepareReadyHandler(contextReady)", setup)
        self.assertIn("localDispatcher.setPrepareReadyHandler(contextReady)", setup)
        loop = function_body(self.brain, "void loop()")
        self.assertLess(loop.index("network.poll("), loop.index("contextSync.poll("))
        self.assertLess(loop.index("controllerLink.poll(uint32_t(millis()))"), loop.index("contextSync.poll("))
        self.assertLess(loop.index("pendingRecovery.poll("), loop.index("contextSync.poll("))
        self.assertNotIn("contextReady()", loop[loop.index("intentPending = intentPending"):loop.index("view.update")])
        self.assertIn("DisplayIntent::Initialize) contextSync.yield", loop)

    def test_result_delivery_wiring_preserves_runtime_execution_slot(self):
        # Source wiring only; production Store/transport behavior is tested separately.
        setup = function_body(self.motion, "void setup()")
        self.assertIn("productBoardLink.setCloudReceiptHandler(receiptProduct)", setup)
        callback = function_body(self.motion, "bool receiptProduct(")
        self.assertIn("return resultDelivery.receipt(receipt);", callback)
        self.assertNotIn("acknowledge(", callback)
        polling = function_body(self.motion, "void pollDemo()")
        self.assertLess(polling.index("productRuntime.poll("), polling.index("resultDelivery.poll("))
        self.assertIn("!productRuntime.active() && !productRecovery.executionPending() &&", polling)
        self.assertIn("productHardware.stationary(), commissioningActive() || ota.maintenanceActive()", polling)
        brain_setup = function_body(self.brain, "void setup()")
        self.assertIn("controllerLink.setTerminalHandler(receiveMotionTerminal)", brain_setup)
        receipt = function_body(self.brain, "void receiveCloudReceipt(")
        self.assertIn("if (simulation && simulation->receipt(receipt)) return;", receipt)
        self.assertIn("resultDelivery.receipt(receipt)", receipt)
        loop = function_body(self.brain, "void loop()")
        self.assertLess(loop.index("controllerLink.poll(uint32_t(millis()))"), loop.index("resultDelivery.poll("))
        self.assertNotIn("can_start", loop[loop.index("resultDelivery.poll("):loop.index("localDispatcher.poll(")])

    def test_motion_mutating_routes_have_gate_except_safety_stop(self):
        safety = {
            "/api/stop": "handleStop",
            "/api/stop-all": "handleStopAll",
            "/api/queue/cancel": "handleQueueCancel",
        }
        mixed = {"/api/command", "/api/enable", "/api/enable-all"}
        routes = re.findall(r'server\.on\("([^"]+)", HTTP_POST, (.*?);', self.motion, re.S)
        self.assertGreaterEqual(len(routes), 19)
        for path, handler in routes:
            with self.subTest(path=path):
                if path in safety:
                    self.assertEqual(handler, safety[path] + ")")
                elif path in mixed:
                    self.assertIn("rejectDuringOta()", handler)
                else:
                    self.assertIn("rejectDuringMaintenance()", handler)
        gate = function_body(self.motion, "bool rejectDuringMaintenance() {")
        self.assertIn("commissioningActive()", gate)
        self.assertIn('sendError(409, F("commissioning_active"))', gate)

    def test_mixed_routes_gate_after_validation_before_mutation(self):
        for handler, predicate, validated, mutation in (
            ("handleEnable", "enabling", 'const bool enabling = enabledRaw == "1";', "stopDemoOwnership()"),
            ("handleEnableAll", "enabled", 'sendError(400, F("enabled must be 0 or 1"))', "stopDemoOwnership()"),
            ("handleCommand", "!stopLike && kind != motion::CommandKind::Read",
             "const bool stopLike = kind==motion::CommandKind::Stop", "stopDemoOwnership()"),
        ):
            body = function_body(self.motion, "void " + handler + "()")
            gate = "if (" + predicate + " && rejectDuringMaintenance()) return;"
            self.assertLess(body.index(validated), body.index(gate))
            self.assertLess(body.index(gate), body.index(mutation))
        raw = function_body(self.motion, "void handleCommand()")
        self.assertLess(raw.index("kind == motion::CommandKind::Invalid"), raw.index("const bool stopLike"))

    def test_motion_network_and_ota_gated(self):
        network = function_body(self.motion, "bool networkChangeBusy() {")
        self.assertIn("if (commissioningActive()) return true;", network)
        ota = function_body(self.motion, "bool safeForOta() {")
        self.assertIn("!commissioningActive()", ota)
        self.assertIn("!otaWriterBusy()", ota)
        self.assertIn("motor.otaMotionSafe()", ota)
        self.assertNotIn("controlBusy()", ota)
        writers = function_body(self.motion, "bool otaWriterBusy() {")
        self.assertNotIn("eventPending()", writers)
        self.assertNotIn("hasActiveMotion()", writers)
        wifi = (ROOT / "device-controller/src/WiFiSetup.cpp").read_text()
        for handler in ("handleConnect", "handleForget", "handleScanStart"):
            self.assertIn("isMotionBusy()", function_body(wifi, "void WiFiSetup::" + handler + "()"))
        for handler in ("handleSession", "handleUpload"):
            self.assertIn("!safeToStart_()", function_body(self.ota, "void WifiOta::" + handler + "()"))

    def test_motion_entry_requires_fresh_stationary_and_no_work(self):
        body = function_body(self.motion, "bool safeForCommissioning() {")
        for condition in ("canStarted", "!controlBusy()", "!motor.operationBusy()",
                          "!wifiSetup.busy()", "!ota.maintenanceActive()",
                          "!powderScale.tareInProgress()", "demo.stationary()"):
            self.assertIn(condition, body)
        console = function_body(self.motion, "void pollCommissioningConsole()")
        self.assertIn("commissioningSession.poll(Serial, millis(), localSafe", console)
        self.assertIn("!productBoardLink.maintenanceActive() && safeForCommissioning()", console)

    def test_remote_lease_shares_guard_without_new_release_or_daily_gates(self):
        active = function_body(self.motion, "bool commissioningActive() {")
        self.assertIn("commissioningSession.active() || productBoardLink.maintenanceActive()", active)
        target = function_body(self.motion, "bool safeToAcquire() const override")
        self.assertIn("!commissioningSession.active() && safeForCommissioning()", target)
        release = function_body(self.motion, "bool safeToRelease() const override")
        self.assertEqual(release.strip(), "return true;")
        self.assertIn("productBoardLink.setMaintenanceTarget(&migrationMaintenance)", self.motion)
        loop = function_body(self.motion, "void loop()")
        self.assertLess(loop.index("serviceBrainLink()"), loop.index("pollCommissioningConsole()"))
        self.assertLess(loop.index("serviceBrainLink()"), loop.index("server.handleClient()"))
        brain_loop = function_body(self.brain, "void loop()")
        self.assertIn("if (!commissioningSession.active()) controllerLink.releaseMaintenance(uint32_t(millis()))", brain_loop)
        for source in (self.motion, self.brain):
            self.assertNotIn("requestMaintenance(", function_body(source, "void setup()"))

    def test_v4_sole_usb_reader_preserves_ota_command(self):
        self.assertIn("#if MOTION_UART_PEER == MOTION_UART_PEER_PRODUCT_BRAIN\n"
                      "    ota.useExternalSerialReader();\n#endif", self.motion)
        body = function_body(self.ota, "void WifiOta::poll()")
        self.assertIn("if (!externalSerialReader_) pollSerialRecovery();", body)
        console = function_body(self.motion, "void pollCommissioningConsole()")
        self.assertIn("return ota.formatSerialLine(line, output, capacity);", console)
        self.assertIn("extraCommand(line, response_ + 1, sizeof(response_) - 1)", self.console)
        recovery = function_body(self.ota, "bool WifiOta::formatSerialLine")
        self.assertIn('!line || strcmp(line, "OTA CODE") != 0', recovery)
        for source in (self.motion, self.brain):
            expected_timeout = 1 if source is self.brain else 0
            self.assertIn(f"Serial.setTxTimeoutMs({expected_timeout})", source)
        self.assertNotIn("port.print", self.console)

    def test_console_bounded_before_http_and_ui(self):
        for source in (self.motion, self.brain):
            body = function_body(source, "void pollCommissioningConsole()")
            self.assertIn("commissioningSession.poll(Serial, millis(),", body)
            self.assertNotIn("Serial.println(line)", body)
        self.assertIn("n < MaintenanceLineReader::kPollBytes", self.console)
        self.assertIn("MaintenanceLineReader::Result::Line", self.console)
        loop = function_body(self.motion, "void loop()")
        self.assertLess(loop.index("pollCommissioningConsole()"), loop.index("server.handleClient()"))
        loop = function_body(self.brain, "void loop()")
        self.assertLess(loop.index("pollCommissioningConsole()"), loop.index("view.takeIntent"))
        self.assertIn("hasIntent && (touchStop.requested() || commissioningSession.active() || simulating())", loop)

    def test_brain_simulation_is_separate_from_motion_and_persistent_local_intents(self):
        command = function_body(self.brain, "void dispatchCloudCommand(")
        stop = function_body(self.brain, "void dispatchCloudStop(")
        self.assertIn("if (simulating()) { refreshSimulationContext(); simulation->command", command)
        self.assertIn("else cloudDispatcher.command", command)
        self.assertIn("if (simulating()) simulation->stop", stop)
        self.assertIn("else cloudDispatcher.stop", stop)
        loop = function_body(self.brain, "void loop()")
        self.assertIn("cloudDispatcher.busy() || localDispatcher.busy(), !simulating()", loop)
        self.assertLess(loop.index("network.poll("), loop.index("simulation->poll("))
        self.assertLess(loop.index("hasIntent && (touchStop.requested() || commissioningSession.active() || simulating())"),
                        loop.index("localDispatcher.dispatch("))
        self.assertIn("network.setReceiptHandler(receiveCloudReceipt, nullptr)", self.brain)
        receipt = function_body(self.brain, "void receiveCloudReceipt(")
        self.assertIn("simulation->receipt(receipt)", receipt)
        self.assertNotIn("clearPending", receipt)
        self.assertNotIn("controllerLink", receipt)
        self.assertIn("commissioningSession.active()) snapshot.startEnabled = false", loop)

    def test_simulation_refreshes_admission_in_receive_batch_and_observes_typed_acceptance(self):
        callback = function_body(self.brain, "void receiveCloudContext(")
        self.assertLess(callback.index("contextSync.receive(context)"), callback.index("refreshSimulationContext()"))
        command = function_body(self.brain, "void dispatchCloudCommand(")
        self.assertLess(command.index("refreshSimulationContext()"), command.index("simulation->command"))
        setup = function_body(self.brain, "void setup()")
        for owner in ("cloudDispatcher", "localDispatcher", "pendingRecovery"):
            self.assertIn(owner + ".setAcceptanceHandler(observeRealAcceptance)", setup)
        self.assertNotIn("observeAccepted(controllerLink.commandResponse()", self.brain)

    def test_console_does_not_directly_write_or_claim_network_shutdown(self):
        for source in (self.motion, self.brain):
            body = function_body(source, "void pollCommissioningConsole()")
            for forbidden in ("installFirst", "importBrain", "importMotion", "ESP.restart", "WiFi.disconnect"):
                self.assertNotIn(forbidden, body)
        self.assertIn("network.poll(", function_body(self.brain, "void loop()"))

    def test_brain_installer_explicit_owner_without_boot_autoinstall(self):
        self.assertEqual(self.brain.count("BrainStateStore productState;"), 1)
        self.assertIn("BrainInstallConsole::handle(line, installer,", self.brain)
        setup = function_body(self.brain, "void setup()")
        self.assertNotIn("installer.start(", setup)
        self.assertNotIn("importBrain(", setup)
        polling = function_body(self.brain, "void loop()")
        self.assertLess(polling.index("controllerLink.poll(nowMs)"), polling.index("installer.poll(uint32_t(millis()))"))
        self.assertLess(polling.index("pollCommissioningConsole()"), polling.index("installer.poll(uint32_t(millis()))"))
        self.assertIn("installer.busy()", function_body(self.brain, "void pollCommissioningConsole()"))

    def test_motion_install_uses_runtime_store_without_boot_or_console_writes(self):
        self.assertEqual(self.motion.count("MotionStateStore productState;"), 1)
        self.assertIn("productState, productBoardLink.maintenance(), safeForRemoteInstall, installNowMs", self.motion)
        setup = function_body(self.motion, "void setup()")
        self.assertIn("productBoardLink.install().setTarget(&migrationInstall)", setup)
        self.assertNotIn(".install().request(", setup)
        self.assertNotIn("importMotion(", setup)
        guard = function_body(self.motion, "bool safeForRemoteInstall() {")
        self.assertIn("!commissioningSession.active() && safeForCommissioning()", guard)

    def test_brain_boot_loads_existing_store_without_network_or_replay_gate(self):
        setup = function_body(self.brain, "void setup()")
        self.assertIn("if (const auto* pairing = controllerLink.verifiedPairing())", setup)
        self.assertEqual(setup.count("productState.load(*pairing)"), 1)
        self.assertLess(setup.index("controllerLink.begin()"), setup.index("productState.load(*pairing)"))
        self.assertLess(setup.index("productState.load(*pairing)"), setup.index("network.begin("))
        # Loading business state must not globally gate network diagnostics.
        self.assertIn("\n  if (!network.begin(controllerLink.deviceId()))", setup)
        for forbidden in ("installInitial(", "saveContext(", "reserveLocal(", "clearPending(",
                          "sendIntent(", "requestInstallation(", "ESP.restart("):
            self.assertNotIn(forbidden, setup)
        loop = function_body(self.brain, "void loop()")
        self.assertNotIn("productState.load(", loop)

    def test_pending_query_owner_reuses_stores_without_action_or_network_gate(self):
        self.assertIn("controllerLink, productState);", self.brain)
        loop = function_body(self.brain, "void loop()")
        self.assertIn("pendingRecovery.poll(uint32_t(millis()), commissioningSession.active() || cloudDispatcher.busy() || localDispatcher.busy())", loop)
        self.assertLess(loop.index("controllerLink.poll(nowMs)"), loop.index("pendingRecovery.poll("))
        self.assertLess(loop.index("network.poll("), loop.index("pendingRecovery.poll("))
        setup = function_body(self.motion, "void setup()")
        self.assertIn("productBoardLink.setResultQueryHandler(queryProductResult)", setup)
        self.assertLess(setup.index("productRecovery.begin("), setup.index("setResultQueryHandler("))
        handler = function_body(self.motion, "bool queryProductResult(")
        self.assertIn("queryMotionResult(productState, query, result)", handler)
        for forbidden in ("supervisedStop", "startLocal", "installInitial", "reserveLocal"):
            self.assertNotIn(forbidden, handler)

    def test_brain_cloud_owner_is_wired_without_worker_motor_or_boot_replay(self):
        setup = function_body(self.brain, "void setup()")
        self.assertIn("network.setProductHandlers(dispatchCloudCommand, dispatchCloudStop, nullptr)", setup)
        self.assertNotIn("cloudDispatcher.command(", setup)
        self.assertNotIn("cloudDispatcher.stop(", setup)
        loop = function_body(self.brain, "void loop()")
        self.assertLess(loop.index("controllerLink.poll(nowMs)"), loop.index("cloudDispatcher.poll(nowMs,"))
        self.assertLess(loop.index("cloudDispatcher.poll(nowMs,"), loop.index("pendingRecovery.poll("))
        self.assertIn("cloudDispatcher.poll(nowMs, commissioningSession.active() || productState.state().pending)", loop)
        stopDrain = loop.index("controllerLink.poll(uint32_t(millis()))", loop.index("network.poll("))
        self.assertLess(stopDrain, loop.index("installer.poll("))
        self.assertLess(stopDrain, loop.index("pendingRecovery.poll("))
        self.assertLess(loop.index("view.poll(uint32_t(millis()))"), loop.index("network.poll("))
        self.assertIn("controllerLink.lastTelemetryReceivedAtMs(), controllerLink.connected(nowMs), cloudCanStart()", loop)
        self.assertIn("network.setCanStartHandler(cloudCanStart)", setup)
        ordinary = function_body(self.brain, "const char* cloudAdmission()")
        self.assertIn("productState.ready()", ordinary)
        self.assertIn("productState.state().pending", ordinary)
        stop = function_body(self.brain, "void dispatchCloudStop(")
        self.assertIn("cloudDispatcher.stop(stop, generation, nowMs)", stop)
        for forbidden in ("productState", "commissioningSession", "pendingRecovery", "network.connected"):
            self.assertNotIn(forbidden, stop)

    def test_public_readiness_does_not_add_local_sequence_network_or_history_gates(self):
        readiness = function_body(self.brain, "bool cloudCanStart()")
        for expected in ("cloudAdmission()", "contextSync.canPrepare()", "status->snapshot.startEnabled",
                         "cloudDispatcher.resultPending()", "StopSendState::Pending",
                         "simulationModeGuard.unresolved("):
            self.assertIn(expected, readiness)
        for forbidden in ("localDispatcher.canStart", "localSequence", "network.connected",
                          "eventPending", "resultDelivery", "clearPending", "requestCommand"):
            self.assertNotIn(forbidden, readiness)
        network = (ROOT / "main-controller/src/brain_network.cpp").read_text()
        encoding = function_body(network, "bool BrainNetwork::publishStatus(")
        self.assertIn("canStartHandler_ ? canStartHandler_() : canStart", encoding)

    def test_local_touch_shares_store_uart_without_boot_replay_or_cloud_gate(self):
        self.assertEqual(self.brain.count("BrainStateStore productState;"), 1)
        self.assertIn("localDispatcher(\n  controllerLink, productState, installNowMs)", self.brain)
        setup = function_body(self.brain, "void setup()")
        self.assertIn("view.begin(true)", setup)
        self.assertNotIn("localDispatcher.dispatch(", setup)
        loop = function_body(self.brain, "void loop()")
        self.assertLess(loop.index("controllerLink.poll(uint32_t(millis()))"), loop.index("localDispatcher.poll("))
        self.assertLess(loop.index("localDispatcher.poll("), loop.index("pendingRecovery.poll("))
        self.assertIn("snapshot.startEnabled = localDispatcher.canStart(displayNowMs,", loop)
        self.assertLess(loop.index("cloudDispatcher.yieldToLocal("), loop.index("localDispatcher.dispatch("))
        local = (ROOT / "main-controller/src/brain_local_dispatcher.h").read_text()
        self.assertNotIn("network", function_body(local, "bool dispatch("))
        self.assertNotIn("requestCommand(", function_body(local, "void poll("))
        dispatch = function_body(local, "bool dispatch(")
        self.assertLess(dispatch.index("commandAvailable("), dispatch.index("store_.reserveLocal("))
        self.assertLess(dispatch.index("store_.reserveLocal("), dispatch.index("link_.requestCommand("))

    def test_v4_recovery_is_wired_to_boot_and_existing_stop_supervision(self):
        setup = function_body(self.motion, "void setup()")
        self.assertLess(setup.index("applyDemoJson("), setup.index("productRecovery.begin("))
        self.assertLess(setup.index("motor.begin("), setup.index("productRecovery.begin("))
        self.assertIn("productBoardLink.verifiedPairing()", setup)
        self.assertIn("product.recoverAfterRestart(nowMs)", self.motion)
        polling = function_body(self.motion, "void pollDemo()")
        self.assertLess(polling.index("demo.tick("), polling.index("productRecovery.poll()"))
        telemetry = function_body(self.motion, "void serviceBrainLink()")
        self.assertIn("productRecovery.project(status)", telemetry)
        # All debug motion paths share demoBusy, not only the OTA/network gates.
        busy = function_body(self.motion, "bool demoBusy() {")
        self.assertIn("recoveryMotionPending()", busy)
        self.assertNotIn("productState", busy)

    def test_recovery_stationary_uses_hardware_without_its_own_busy_gate(self):
        body = function_body(self.motion, "bool stationary() const override")
        for required in ("canStarted", "!endpoint.busy()",
                         "!motor.operationBusy()", "!queue.active()", "!demo.busy()",
                         "!product.ownsMotion()", "demoExecutor.stopConfirmed()"):
            self.assertIn(required, body)
        for recursive in ("controlBusy()", "demoBusy()", "motionPending()",
                          "hasActiveMotion()", "demo.stationary()"):
            self.assertNotIn(recursive, body)

    def test_motion_product_runtime_static_entry_wiring_and_d1_d2(self):
        # Source wiring only: this does not execute main, UART, CAN or Flash.
        self.assertIn('#include "MotionProductRuntime.h"', self.motion)
        self.assertEqual(self.motion.count("MotionStateStore productState;"), 1)
        self.assertRegex(self.motion, r"motion::MotionProductRuntime\s+productRuntime\(\s*"
                         r"productState,\s*product,\s*demo,\s*productHardware\s*\);")
        setup = function_body(self.motion, "void setup()")
        for setter in ("setCommandHandler(commandProduct)", "setStopHandler(stopProduct)"):
            self.assertIn("productBoardLink." + setter, setup)
            self.assertLess(setup.index("productRecovery.begin("), setup.index(setter))
        command = function_body(self.motion, "bool commandProduct(")
        stop = function_body(self.motion, "bool stopProduct(")
        self.assertIn("return productRuntime.command(command, nowMs, result);", command)
        self.assertIn("return productRuntime.stop(request, nowMs);", stop)
        polling = function_body(self.motion, "void pollDemo()")
        self.assertLess(polling.index("product.tick("), polling.index("productRecovery.poll()"))
        self.assertLess(polling.index("productRecovery.poll()"), polling.index("productRuntime.poll("))

        telemetry = function_body(self.motion, "void serviceBrainLink()")
        v4 = telemetry.split("#elif MOTION_UART_PEER == MOTION_UART_PEER_PRODUCT_BRAIN", 1)[1]
        v4 = v4.split("#else", 1)[0]
        self.assertIn("productRuntime.project(status, productBoardLink.link().connected(", v4)
        self.assertLess(v4.index("productRecovery.project("), v4.index("productRuntime.project("))
        self.assertLess(v4.index("productRuntime.project("), v4.index("productBoardLink.poll("))
        self.assertRegex(v4, r"productBoardLink\.poll\([^;]+\);\s*"
                         r"if\s*\(productBoardLink\.takeFailure\(\)\s*!=\s*"
                         r"babytech::v4::LinkFailure::None\s*\|\|\s*"
                         r"!productBoardLink\.link\(\)\.healthy\(\)\)\s*"
                         r"productRuntime\.linkLost\(millis\(\)\);")
        cloud = function_body(self.motion, "void serviceCloud()")
        v4_cloud = cloud.split("#else", 1)[0]
        v4_cloud = re.sub(r"//[^\n]*", "", v4_cloud)
        self.assertRegex(v4_cloud, r"^\s*#if MOTION_UART_PEER == "
                         r"MOTION_UART_PEER_PRODUCT_BRAIN\s+return;\s*$")
        self.assertIn("productSession->networkState(wifiConnected, millis())", cloud.split("#else", 1)[1])
        self.assertRegex(self.motion, r"#ifndef BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW\s+"
                         r"#define BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW 0\s+#endif")
        self.assertIn("product.setExecutionAuthorized(BABYTECH_ENABLE_NON_CONSUMABLE_PRODUCT_FLOW == 1)", setup)

        hardware = function_body(self.motion, "class ProductHardware :")
        unavailable = function_body(hardware, "const char* unavailable() const override")
        for forbidden in ("product.ownsMotion()", "demo.busy()", "demoBusy()", "cloud.", "WiFi.status()"):
            self.assertNotIn(forbidden, unavailable)
        stationary = function_body(hardware, "bool stationary() const override")
        self.assertRegex(stationary, r"const bool flowIdle\s*=\s*!demo\.busy\(\)\s*\|\|\s*"
                         r"demo\.stage\(\)\s*==\s*babytech::display::DisplayStage::Complete;")
        self.assertRegex(stationary, r"return\s+canStarted\s*&&\s*!endpoint\.busy\(\)\s*&&\s*"
                         r"!motor\.operationBusy\(\)\s*&&\s*!queue\.active\(\)\s*&&\s*flowIdle\s*&&\s*"
                         r"\(demo\.stationary\(\)\s*\|\|\s*demoExecutor\.stopConfirmed\(\)\)\s*&&\s*"
                         r"motor\.affectedAxesStationary\(\);\s*$")
        for forbidden in ("product.ownsMotion()", "product.cleaning()", "healthy()", "motor.ready()",
                          "demo.referenceValid()", "controlBusy()", "hasActiveMotion()"):
            self.assertNotIn(forbidden, stationary)

    def test_v4_context_uses_runtime_store_not_legacy_ram_cache(self):
        # Static wiring only; runtime/core persistence and correlation are tested dynamically elsewhere.
        setup = function_body(self.motion, "void setup()")
        self.assertIn("productBoardLink.setContextHandler(contextProduct)", setup)
        self.assertLess(setup.index("productRecovery.begin("), setup.index("setContextHandler("))
        self.assertIn("#if MOTION_UART_PEER != MOTION_UART_PEER_PRODUCT_BRAIN\n"
                      "    loadProductContext();\n#endif", setup)
        handler = function_body(self.motion, "bool contextProduct(")
        self.assertEqual(handler.strip(), "return productRuntime.context(context, nowMs, result);")
        for forbidden in ("saveContext(", "requestContext(", "clearContext(", "applyContext("):
            self.assertNotIn(forbidden, setup)

    def test_static_runtime_reply_waits_for_durable_busy_decision(self):
        # Source wiring only; no moving hardware or Flash timing is exercised.
        setup = function_body(self.motion, "void setup()")
        ready = "productBoardLink.setCommandReadyHandler(productResultReady)"
        self.assertIn(ready, setup)
        self.assertLess(setup.index("productRecovery.begin("), setup.index(ready))
        self.assertLess(setup.index("setCommandHandler(commandProduct)"), setup.index(ready))
        callback = function_body(self.motion, "bool productResultReady(")
        self.assertEqual(callback.strip(), "return productRuntime.resultReady(result);")
        link = (ROOT / "shared/ProductBoardLink/src/ReadOnlyBoardLink.cpp").read_text()
        polling = function_body(link, "void ReadOnlyLink::poll(")
        scheduling = function_body(link, "void ReadOnlyLink::queueMotionOutput(")
        schedule = "queueMotionOutput(nowMs, localStatus);"
        pump = "tx_.pump(nowMs, sink);"
        self.assertEqual(polling.count(schedule), 2)
        self.assertLess(polling.index(schedule), polling.rindex(pump))
        self.assertLess(polling.rindex(pump), polling.rindex(schedule))
        self.assertRegex(scheduling, r"if\s*\(\(!commandReadyHandler_\s*\|\|\s*"
                         r"commandReadyHandler_\(commandResult_\)\)\s*&&\s*"
                         r"encodeCommandResult\(commandResult_,\s*scratch_\)\s*&&\s*queue\(scratch_\)\)")
        runtime = (ROOT / "device-controller/src/MotionProductRuntime.cpp").read_text()
        result = function_body(runtime, "bool MotionProductRuntime::resultReady(")
        self.assertIn("if (deferred_) return false;", result)
        self.assertLess(result.index("if (deferred_)"), result.index("result = deferredResult_"))
        deferred = function_body(function_body(runtime, "void MotionProductRuntime::poll("),
                                 "if (deferred_ && hardware_.stationary())")
        self.assertIn("store_.recordDecision(deferredRequest_, false, deferredResult_.reason)", deferred)
        self.assertLess(deferred.index("store_.recordDecision("), deferred.index("deferred_ = false"))

    def test_static_http_stop_prioritizes_runtime_interruption(self):
        # The Interrupted mapping is inspected, not a dynamic HTTP/Flash proof.
        owner = function_body(self.motion, "StopOwnershipResult stopDemoOwnership()")
        v4 = owner.split("#if MOTION_UART_PEER == MOTION_UART_PEER_PRODUCT_BRAIN", 1)[1]
        v4 = v4.split("#endif", 1)[0]
        self.assertRegex(v4, r"if\s*\(productRuntime\.ownsMotion\(\)\)\s*"
                         r"return productRuntime\.stopOwned\(millis\(\)\)\s*\?\s*"
                         r"StopOwnershipResult::Requested\s*:\s*StopOwnershipResult::Unconfirmed;")
        self.assertLess(owner.index("productRuntime.stopOwned("), owner.index("product.stop("))
        self.assertLess(owner.index("productRuntime.stopOwned("), owner.index("demo.stop("))
        self.assertIn("stopDemoOwnership()", function_body(self.motion, "bool stopDemoIfOwned()"))
        for handler, mutation in (("handleStop", "motor.stop("),
                                  ("handleStopAll", "queue.cancel("),
                                  ("handleQueueCancel", "queue.cancel(")):
            with self.subTest(handler=handler):
                body = function_body(self.motion, "void " + handler + "()")
                self.assertIn("if (stopDemoIfOwned()) return;", body)
                self.assertLess(body.index("stopDemoIfOwned()"), body.index(mutation))
        runtime = (ROOT / "device-controller/src/MotionProductRuntime.cpp").read_text()
        stopping = function_body(runtime, "bool MotionProductRuntime::stopOwned(")
        self.assertLess(stopping.index("stopped_ = true"), stopping.index("product_.stop("))
        polling = function_body(runtime, "void MotionProductRuntime::poll(")
        self.assertIn("archiveExecution();", polling)
        archival = function_body(runtime, "void MotionProductRuntime::archiveExecution(")
        self.assertIn("stopped_ ? MotionOutcome::Interrupted : MotionOutcome::Succeeded", archival)
        self.assertIn("store_.finishOperation(execution_, outcome, true)", archival)

    def test_static_manual_admission_cannot_release_product_motion_identity(self):
        # Runtime + the actual admission helper are exercised by the host suite;
        # these checks tie rejected/configuration-only HTTP paths to that helper.
        manual = function_body(self.motion, "bool demoManualMutation()")
        self.assertNotIn("releaseMotionOwnership", manual)
        self.assertLess(manual.index("if (demoBusy())"), manual.index("demo.invalidate()"))
        for handler, guard, mutation in (
            ("handleEnable", "if (enabling && !demoManualMutation()) return;", "motor.enable("),
            ("handleEnableAll", "if (enabled && !demoManualMutation()) return;", "motor.broadcastEnable("),
            ("handleMove", "if (!demoManualMutation()) return;", "motor.move("),
            ("handleCommand", "if (!stopLike && kind != motion::CommandKind::Read && !demoManualMutation()) return;",
             "motor.command("),
            ("handleQueueStart", "if (!demoManualMutation()) return;", "queue.start("),
            ("handleLimits", "if (!demoManualMutation()) return;", "motor.setDebugLimits("),
            ("handleMotorDistance", "if (!demoManualMutation()) return;", "clearRotationMm("),
        ):
            with self.subTest(handler=handler):
                body = function_body(self.motion, "void " + handler + "()")
                self.assertIn(guard, body)
                # The raw-command invalid/read branch is not a mechanical takeover.
                self.assertLess(body.index(guard), body.rindex(mutation))
        for handler in ("handleEnable", "handleEnableAll", "handleLimits", "handleMotorDistance"):
            with self.subTest(non_execution=handler):
                self.assertNotIn("releaseMotionOwnership", function_body(self.motion, "void " + handler + "()"))

    def test_static_workbench_execution_handoff_requires_acceptance_or_partial_motion_tx(self):
        # Static HTTP/CAN outcome wiring, not a dynamic HTTP or real CAN test.
        release = "productRuntime.workbenchAccepted();"
        move = function_body(self.motion, "void handleMove()")
        self.assertIn("const motion::Result result = motor.move(request);", move)
        self.assertLess(move.rindex("sendError("), move.index("motor.move("))
        self.assertLess(move.index("motor.move("), move.index(release))
        self.assertLess(move.index("const auto movementBefore"), move.index("motor.move("))
        self.assertIn("if (result.code < 300 || motor.movementGeneration() != movementBefore)", move)
        self.assertLess(move.index(release), move.index('sendResult("move", id, result)'))
        raw = function_body(self.motion, "void handleCommand()")
        self.assertIn("const motion::Result result = motor.command(bytes, hex.length()/2);", raw)
        self.assertNotIn("workbenchAccepted", function_body(raw, "if (kind == motion::CommandKind::Invalid)"))
        self.assertLess(raw.rindex("sendError("), raw.index("const motion::Result result"))
        self.assertLess(raw.index("const auto movementBefore"), raw.index("const motion::Result result"))
        self.assertIn("(result.code < 300 || motor.movementGeneration() != movementBefore)", raw)
        for kind in ("Move", "DirectMove", "Experiment", "Home"):
            self.assertIn("kind == motion::CommandKind::" + kind, raw)
        self.assertLess(raw.index("const motion::Result result"), raw.index(release))
        self.assertLess(raw.index(release), raw.index('sendResult("command", bytes[0], result)'))
        queue = function_body(self.motion, "void handleQueueStart()")
        self.assertLess(queue.rindex("sendError("), queue.index("queue.start("))
        self.assertLess(queue.index("queue.start("), queue.index("if (started.code < 300)"))
        accepted = function_body(queue, "if (started.code < 300)")
        self.assertRegex(accepted, r"^\s*#if MOTION_UART_PEER == MOTION_UART_PEER_PRODUCT_BRAIN\s*"
                         r"productRuntime\.workbenchAccepted\(\);\s*#endif")
        self.assertLess(accepted.index(release), accepted.index("endpoint.cancelPending("))
        self.assertLess(queue.index(release), queue.index("sendQueueResult(started, true)"))
        for body in (move, raw, queue):
            self.assertEqual(body.count(release), 1)
        self.assertEqual(self.motion.count(release), 4) # Three workbench paths plus accepted Demo.

    def test_static_demo_handoff_includes_partial_submission_but_not_no_tx_rejection(self):
        release = "productRuntime.workbenchAccepted();"
        action = function_body(self.motion, "void handleDemoAction()")
        self.assertIn("if (demoBusy() ||", action)
        for launch in ("accepted = demo.initialize(", "accepted = demo.start(", "accepted = demo.single("):
            self.assertLess(action.index("const auto movementBefore"), action.index(launch))
            self.assertLess(action.index(launch), action.index(release))
        self.assertIn("if (accepted || motor.movementGeneration() != movementBefore)", action)
        self.assertLess(action.index(release), action.index("if (!accepted)"))
        self.assertLess(action.index(release), action.index("sendJson(202,"))

    def test_static_workbench_stop_and_projection_use_existing_backend(self):
        adapter = function_body(self.motion, "void stopWorkbench(uint32_t nowMs) override")
        self.assertIn("demo.stop(nowMs)", adapter)
        self.assertIn('queue.cancel("stopped")', adapter)
        self.assertNotIn("server.", adapter)
        status = function_body(self.motion, "void serviceBrainLink()")
        self.assertIn("productRecovery.motionPending()", status)
        self.assertIn("ExecutionOwner::Workbench", status)
        self.assertIn("productHardware.workbenchStationary()", status)

    def test_static_control_reset_preserves_runtime_interruption_before_clear(self):
        # Source branch/order checks, not dynamic Stop-window or Flash confirmation.
        body = function_body(self.motion, "void handleControlReset()")
        paired = body.split("#if MOTION_UART_PEER == MOTION_UART_PEER_PRODUCT_BRAIN", 1)[1]
        v4, legacy = paired.split("#else", 1)
        self.assertRegex(v4, r"^\s*const bool pairedProduct\s*=\s*productRuntime\.ownsMotion\(\);\s*"
                         r"if\s*\(pairedProduct\)\s*productRuntime\.stopOwned\(millis\(\)\);\s*$")
        self.assertRegex(legacy.split("#endif", 1)[0],
                         r"^\s*const bool pairedProduct\s*=\s*false;\s*$")
        product_stop = function_body(body, "if (!pairedProduct && product.ownsMotion())")
        self.assertIn("product.stop(millis(), wasActive);", product_stop)
        self.assertRegex(body, r"if\s*\(!pairedProduct\s*&&\s*demo\.busy\(\)\)\s*demo\.stop\(millis\(\)\);")
        self.assertEqual(body.count("productRuntime.stopOwned("), 1)
        self.assertEqual(body.count("product.stop("), 1)
        self.assertEqual(body.count("demo.stop("), 1)
        ordered = ("productRuntime.ownsMotion()", "productRuntime.stopOwned(",
                   "if (!pairedProduct && product.ownsMotion())", "demo.invalidate()",
                   "if (!pairedProduct && demo.busy())", "queue.clearControlState()")
        for before, after in zip(ordered, ordered[1:]):
            self.assertLess(body.index(before), body.index(after))

    def test_boot_inventory_precedes_executable_parameter_validation(self):
        body = function_body(self.motion, "bool applyDemoJson(")
        self.assertLess(body.index("parseDemoConfig("), body.index("configureStopAxes("))
        self.assertLess(body.index("configureStopAxes("), body.index("demoRotationMatches("))
        self.assertIn("if (boot) demoExecutor.configureStopAxes(candidate)", body)
        setup = function_body(self.motion, "void setup()")
        self.assertIn("demoJsonStart), configError,\n"
                      "                       MOTION_UART_PEER == MOTION_UART_PEER_PRODUCT_BRAIN)", setup)


if __name__ == "__main__":
    unittest.main()
