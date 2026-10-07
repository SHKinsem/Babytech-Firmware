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
        self.assertIn("if (!commissioningSession.active()) controllerLink.releaseMaintenance(nowMs)", brain_loop)
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
        self.assertIn("hasIntent && commissioningSession.active()", loop)
        self.assertIn("commissioningSession.active()) snapshot.startEnabled = false", loop)

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
        self.assertLess(polling.index("controllerLink.poll(nowMs)"), polling.index("installer.poll(nowMs)"))
        self.assertLess(polling.index("pollCommissioningConsole()"), polling.index("installer.poll(nowMs)"))
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
