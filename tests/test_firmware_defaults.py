"""Compile the actual selectors without Arduino or PlatformIO dependencies."""

from pathlib import Path
import os
import subprocess
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


class FirmwareDefaultsTest(unittest.TestCase):
    def compile_selection(self, defines=()):
        source = (
            '#include "main-controller/src/brain_mode.h"\n'
            '#include "device-controller/include/UartPeer.h"\n'
            'static_assert(BABYTECH_BOARD_LINK_V4 == 1, "Brain mode");\n'
            'static_assert(MOTION_UART_PEER == 4, "Motion peer");\n'
            'static_assert(MOTION_HAS_PRODUCT == 1, "Product capability");\n'
        )
        return subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++17", "-fsyntax-only", "-x", "c++", "-I", str(ROOT),
             *defines, "-"],
            input=source, text=True, capture_output=True,
        )

    def test_default_pair_uses_v4(self):
        result = self.compile_selection()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_selection_honors_explicit_compiler(self):
        with patch.dict(os.environ, {"CXX": "/custom/clang++"}), \
                patch("subprocess.run") as run:
            self.compile_selection()
            self.assertEqual(run.call_args.args[0][0], "/custom/clang++")

    def test_selection_defaults_to_system_compiler(self):
        with patch.dict(os.environ, {}, clear=True), patch("subprocess.run") as run:
            self.compile_selection()
            self.assertEqual(run.call_args.args[0][0], "c++")

    def test_explicit_v4_flags_remain_compatible(self):
        result = self.compile_selection(("-DBABYTECH_BOARD_LINK_V4=1", "-DMOTION_UART_PEER=4"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_legacy_and_invalid_brain_modes_fail_explicitly(self):
        for mode in (0, 2, 4, -1):
            with self.subTest(mode=mode):
                result = self.compile_selection((f"-DBABYTECH_BOARD_LINK_V4={mode}",))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Only Brain UART v4 is supported", result.stderr)

    def test_legacy_and_invalid_motion_peers_fail_explicitly(self):
        for peer in (0, 1, 2, 3, -1):
            with self.subTest(peer=peer):
                result = self.compile_selection((f"-DMOTION_UART_PEER={peer}",))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Only Motion UART v4", result.stderr)

    def test_production_sources_have_no_legacy_mode_branches(self):
        for relative in ("main-controller/src/main.cpp", "main-controller/src/controller_link.cpp",
                         "main-controller/src/controller_link.h", "main-controller/src/brain_network.cpp",
                         "main-controller/src/brain_network.h", "main-controller/src/brain_station.cpp",
                         "device-controller/src/main.cpp"):
            with self.subTest(source=relative):
                source = (ROOT / relative).read_text()
                self.assertNotIn("BABYTECH_BOARD_LINK_V4", source)
                self.assertNotIn("MOTION_UART_PEER", source)
        motion = (ROOT / "device-controller/src/main.cpp").read_text()
        for retired in ("DisplayLinkCore", "processCloudMessage", "cloud.begin(",
                        "brain.begin(", "brain.read(", "parser.push("):
            self.assertNotIn(retired, motion)

    def test_ci_only_builds_default_pair(self):
        workflow = (ROOT / ".github/workflows/firmware-checks.yml").read_text()
        self.assertNotIn("PLATFORMIO_BUILD_SRC_FLAGS", workflow)
        self.assertEqual(workflow.count('pio run --project-dir "$brain_dir"'), 1)
        self.assertEqual(workflow.count('pio run --project-dir "$motion_dir"'), 1)

    def test_workbench_source_and_embedded_page_do_not_offer_retired_firmware(self):
        for relative in ("tools/motor-protocol-demo/src/components/DemoPanel.jsx",
                         "device-controller/data/index.html"):
            with self.subTest(source=relative):
                source = (ROOT / relative).read_text()
                self.assertNotIn("请编译 DISPLAY 分支", source)
                self.assertIn("流程调试状态不可用，请确认 Motion 固件版本。", source)


if __name__ == "__main__":
    unittest.main()
