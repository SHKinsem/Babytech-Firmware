"""Exercise source archive contents with synthetic, non-flashable artifacts."""
import configparser
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]


class SourceArchiveTest(unittest.TestCase):
    def test_packaged_controllers_contain_their_shared_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ("main-controller", "device-controller", "shared", "boards", "tests", "test"):
                shutil.copytree(ROOT / folder, root / folder,
                                ignore=shutil.ignore_patterns(".pio", "__pycache__"))
            (root / "tools").mkdir()
            for path in (ROOT / "tools").iterdir():
                if path.is_file():
                    shutil.copyfile(path, root / "tools" / path.name)
            files = {
                "device-controller/data/index.html": b"test-only embedded page",
                "docs/motion-workbench-release.md": b"test fixture",
                "README.md": b"test fixture",
            }
            for name in ("device-results.json", "device-lab-1513.png",
                         "device-wifi-1513.png", "device-manual-1280.png"):
                files[f"tools/motor-protocol-demo/qa/{name}"] = b"test fixture"
            for name in ("bootloader.bin", "partitions.bin", "boot_app0.bin",
                         "firmware.elf", "build-info.txt"):
                files[f"out/wsl/motion/{name}"] = b"not flashable"
            files["out/wsl/motion/firmware.bin"] = files["device-controller/data/index.html"]
            for name, data in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            subprocess.run([sys.executable, str(root / "tools/package-device.py"),
                            "--release", "test-only"], check=True, capture_output=True)
            with zipfile.ZipFile(root / "out/releases/device-controller-test-only/source.zip") as archive:
                extracted = root / "extracted"
                archive.extractall(extracted)
            config = configparser.ConfigParser(interpolation=None)
            project = extracted / "main-controller"
            config.read(project / "platformio.ini")
            board_dir = project / config["platformio"]["boards_dir"]
            self.assertTrue((board_dir / (config["env:brain"]["board"] + ".json")).is_file())
            script = config["env:brain"]["extra_scripts"].removeprefix("pre:")
            self.assertTrue((project / script).is_file())
            for folder, environment in (("main-controller", "brain"), ("device-controller", "motion")):
                project = extracted / folder
                config.read(project / "platformio.ini")
                dependencies = config[f"env:{environment}"]["lib_deps"]
                self.assertIn("BabytechCloudLink=symlink://../shared/BabytechCloudLink", dependencies)
                for dependency in dependencies.splitlines():
                    if "symlink://" in dependency:
                        library = project / dependency.split("symlink://", 1)[1]
                        self.assertTrue((library / "library.json").is_file())
                for name in ("CloudLink.h", "CloudLink.cpp", "CloudCommandPriority.h",
                             "CloudInboundOrder.h", "CloudConfigKind.h", "RetryDeadline.h"):
                    for local in ("src", "include"):
                        self.assertFalse((project / local / name).exists())
            shared = extracted / "shared/BabytechCloudLink"
            manifest = json.loads((shared / "library.json").read_text())
            self.assertEqual(manifest["name"], "BabytechCloudLink")
            self.assertIn("knolleary/PubSubClient", manifest["dependencies"])
            self.assertIn("bblanchon/ArduinoJson", manifest["dependencies"])
            self.assertNotIn("srcFilter", manifest.get("build", {}))
            for name in ("CloudLink.h", "CloudLink.cpp", "CloudIdentity.h", "CloudCommandPriority.h",
                         "CloudInboundOrder.h", "CloudConfigKind.h", "RetryDeadline.h",
                         "CloudSession.h", "CloudSession.cpp"):
                self.assertTrue((shared / "src" / name).is_file())
            for name in ("tests/test_cloud_identity.cpp", "tests/test_product_event_outbox.cpp",
                         "tests/fakes/outbox/Arduino.h", "test/test_cloud_session.cpp"):
                self.assertTrue((extracted / name).is_file())
            self.assertTrue((extracted / "tools/test_display_view.py").is_file())
            for name in ("test_protocol.py", "test_board_messages.py", "test_board_link.py",
                         "test_pairing_record.py", "test_board_arduino.py", "test_board_discovery.py",
                         "test_motion_export_snapshot.py", "test_board_export_transfer.py",
                         "test_board_maintenance.py",
                         "test_board_install.py", "test_motion_install.py", "test_brain_installer.py",
                         "test_cloud_contract.py", "test_cloud_session.py", "test_cloud_link.py",
                         "test_brain_network.py", "test_brain_station.py", "test_brain_controller.py",
                         "prepare_board_pairing.py", "test_pairing_store.py",
                         "test_product_context.py", "test_legacy_context_store.py",
                         "test_product_state.py", "test_brain_state_store.py",
                         "test_motion_state_record.py", "test_motion_state_store.py", "test_product_result_query.py", "test_motion_result_queue.py", "test_board_commissioning.py",
                         "test_maintenance_console.py", "test_maintenance_export.py", "capture_board_export.py",
                         "test_brain_network_console.py", "configure_brain_network.py"):
                self.assertTrue((extracted / "tools" / name).is_file())
            self.assertTrue((extracted / "shared/ProductBoardLink/src/ReadOnlyBoardLink.cpp").is_file())
            self.assertTrue((extracted / "shared/ProductBoardLink/src/ProductResultQuery.cpp").is_file())
            self.assertTrue((extracted / "shared/ProductBoardLink/src/ProductResultQuery.h").is_file())
            self.assertTrue((extracted / "shared/BabytechDisplayCore/src/generated/feeding_flow_ui.h").is_file())


if __name__ == "__main__":
    unittest.main()
