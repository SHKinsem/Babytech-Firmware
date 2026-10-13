"""Pre-build hook tests without PlatformIO, parent checkout, or hardware."""
import configparser
import importlib.util
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/sync_feeding_flow_ui.py"
SPEC = importlib.util.spec_from_file_location("sync_feeding_flow_ui", SCRIPT)
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class SyncFeedingUiTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="feeding ui ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.firmware = self.root / "Embeded_System/babytech-firmware"
        self.project = self.firmware / "main-controller"
        self.project.mkdir(parents=True)
        self.header = self.firmware / "shared/BabytechDisplayCore/src/generated/feeding_flow_ui.h"
        self.header.parent.mkdir(parents=True)
        self.header.write_text("committed header\n")

    def parent_generator(self):
        generator = self.root / "Tools/generate_feeding_flow_ui.py"
        generator.parent.mkdir()
        generator.write_text("# Fixture; subprocess is mocked.\n")
        return generator

    def test_parent_uses_current_python_and_cli_with_spaced_paths(self):
        generator = self.parent_generator()
        with mock.patch.object(tool.subprocess, "run") as run:
            tool.sync_feeding_flow_ui(self.project)
        run.assert_called_once_with([sys.executable, str(generator)], cwd=self.root, check=True)

    def test_standalone_nested_checkout_keeps_committed_header(self):
        before = self.header.stat().st_mtime_ns
        with mock.patch.object(tool.subprocess, "run") as run:
            tool.sync_feeding_flow_ui(self.project)
        run.assert_not_called()
        self.assertEqual(self.header.read_text(), "committed header\n")
        self.assertEqual(self.header.stat().st_mtime_ns, before)

    def test_unrelated_parent_assets_are_not_used(self):
        self.parent_generator()
        standalone = self.root / "standalone/main-controller"
        standalone.mkdir(parents=True)
        with mock.patch.object(tool.subprocess, "run") as run:
            tool.sync_feeding_flow_ui(standalone)
        run.assert_not_called()

    def test_parent_generation_failure_is_not_silently_skipped(self):
        self.parent_generator()
        with mock.patch.object(tool.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "generator")), \
             self.assertRaises(subprocess.CalledProcessError):
            tool.sync_feeding_flow_ui(self.project)
        self.assertEqual(self.header.read_text(), "committed header\n")

    def test_parent_source_without_generator_does_not_fall_back(self):
        (self.root / "Shared/feeding_flow_ui").mkdir(parents=True)
        with mock.patch.object(tool.subprocess, "run", side_effect=subprocess.CalledProcessError(2, "generator")) as run, \
             self.assertRaises(subprocess.CalledProcessError):
            tool.sync_feeding_flow_ui(self.project)
        run.assert_called_once()

    def test_declared_parent_without_any_assets_does_not_fall_back(self):
        (self.root / ".gitmodules").write_text(
            '[submodule "firmware"]\npath = Embeded_System/babytech-firmware\n')
        with mock.patch.object(tool.subprocess, "run", side_effect=subprocess.CalledProcessError(2, "generator")) as run, \
             self.assertRaises(subprocess.CalledProcessError):
            tool.sync_feeding_flow_ui(self.project)
        run.assert_called_once()

    def test_scons_entry_runs_the_hook(self):
        self.parent_generator()
        env = SimpleNamespace(subst=lambda expression: str(self.project))
        importer = mock.Mock()
        with mock.patch.object(subprocess, "run") as run:
            runpy.run_path(str(SCRIPT), init_globals={"env": env, "Import": importer})
        importer.assert_called_once_with("env")
        run.assert_called_once()

    def test_only_brain_has_the_prebuild_hook(self):
        for project, environment in (("main-controller", "brain"), ("device-controller", "motion")):
            config = configparser.ConfigParser(interpolation=None)
            config.read(ROOT / project / "platformio.ini")
            scripts = config.get(f"env:{environment}", "extra_scripts", fallback="").splitlines()
            self.assertEqual("pre:../tools/sync_feeding_flow_ui.py" in scripts, environment == "brain")


if __name__ == "__main__":
    unittest.main()
