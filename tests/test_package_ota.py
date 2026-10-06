"""Reject stale OTA images using synthetic artifacts and no signing key."""

import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_ota", ROOT / "tools/package-ota.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class ImageFreshnessTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.output = self.root / "release"
        header = self.root / "device-controller/include/ota_identity.h"
        header.parent.mkdir(parents=True)
        header.write_text(
            '#define BABYTECH_OTA_BOARD "motion"\n'
            '#define BABYTECH_OTA_HARDWARE "esp32-s3-n16r8"\n'
            '#define BABYTECH_OTA_VERSION "test"\n'
            '#define BABYTECH_OTA_BUILD 2\n', encoding="utf-8")
        os.utime(header, (100, 100))
        image = bytearray(1024)
        image[0] = 0xE9
        image[12:14] = (9).to_bytes(2, "little")
        identity = b"BABYTECH-OTA-IMAGE-V1|motion|esp32-s3-n16r8|2|test"
        image[64:64 + len(identity)] = identity
        self.image = self.build / "firmware.bin"
        self.image.write_bytes(image)
        os.utime(self.image, (200, 200))
        (self.build / "partitions.bin").write_bytes(b"".join(
            struct.pack("<HBBII16sI", 0x50AA, 0, subtype, offset, 4096, label, 0)
            for subtype, offset, label in ((0x10, 4096, b"app0"), (0x11, 8192, b"app1"))))
        root_patch = mock.patch.object(PACKAGE, "ROOT", self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        openssl_patch = mock.patch.object(PACKAGE, "run_openssl")
        self.openssl = openssl_patch.start()
        self.addCleanup(openssl_patch.stop)

    def source(self, relative, modified):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// synthetic source\n", encoding="utf-8")
        os.utime(path, (modified, modified))
        return path

    def run_package(self, board="motion"):
        PACKAGE.main(["--board", board, "--build-dir", str(self.build),
                      "--output", str(self.output)])

    def test_newer_board_link_source_rejects_image_before_signing(self):
        for name in ("BoardMessages.h", "ArduinoBoardLink.cpp"):
            with self.subTest(source=name):
                path = self.source(f"shared/ProductBoardLink/src/nested/{name}", 201)
                try:
                    with self.assertRaisesRegex(ValueError, "Image predates project sources"):
                        self.run_package()
                    self.openssl.assert_not_called()
                    self.assertFalse(self.output.exists())
                finally:
                    path.unlink()

    def test_older_or_equal_board_link_source_passes_freshness(self):
        for modified in (199, 200):
            for board in ("motion", "device-controller"):
                with self.subTest(modified=modified, board=board):
                    self.source("shared/ProductBoardLink/src/BoardMessages.h", modified)
                    with self.assertRaisesRegex(ValueError, "Signing key missing"):
                        self.run_package(board)
                    self.openssl.assert_not_called()
                    self.assertFalse(self.output.exists())

    def test_existing_project_and_shared_sources_still_reject_old_image(self):
        for relative in ("device-controller/src/main.cpp", "shared/BoardProtocol/src/Protocol.h"):
            with self.subTest(source=relative):
                path = self.source(relative, 201)
                try:
                    with self.assertRaisesRegex(ValueError, "Image predates project sources"):
                        self.run_package()
                finally:
                    path.unlink()


if __name__ == "__main__":
    unittest.main()
