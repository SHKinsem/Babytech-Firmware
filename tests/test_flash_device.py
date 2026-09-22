"""Offline tests: no serial ports or esptool processes are opened."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('flash_device', Path(__file__).resolve().parents[1] / 'tools/flash-device.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'package'
        self.package.mkdir()
        self.image = bytearray(128)
        self.image[0] = 0xE9
        self.image[12] = 9
        entry = lambda kind, sub, offset, size, label: struct.pack('<HBBII16sI', 0x50AA, kind, sub, offset, size, label, 0)
        self.table = (entry(1, 2, 0x9000, 0x5000, b'nvs') + entry(1, 0, 0xE000, 0x2000, b'otadata') +
                      entry(0, 0x10, 0x10000, 0x300000, b'app0')).ljust(3072, b'\xff')
        blobs = {'bootloader.bin': self.image, 'firmware.bin': self.image,
                 'partitions.bin': self.table, 'boot_app0.bin': bytes(8192)}
        for name, data in blobs.items():
            (self.package / name).write_bytes(data)
        self.hashes()
        self.old = bytearray(65536)
        self.old[0x8000:0x8C00] = self.table
        self.tool = self.root / 'esptool.py'
        self.tool.touch()
        self.calls = []

    def hashes(self):
        (self.package / 'SHA256SUMS.json').write_text(json.dumps({name: hashlib.sha256((self.package / name).read_bytes()).hexdigest() for _, name in flash.PARTS}))

    def fake_run(self, cmd, **kwargs):
        self.calls.append(cmd)
        if 'read-flash' in cmd:
            index = cmd.index('read-flash')
            offset, size = int(cmd[index + 1], 0), int(cmd[index + 2], 0)
            Path(cmd[index + 3]).write_bytes(self.old[offset:offset + size])
        return types.SimpleNamespace(returncode=0, stdout='MAC: 3c:0f:02:c5:fe:64\nDetected flash size: 16MB\n')

    def execute(self):
        return flash.main(['--package', str(self.package), '--execute', '--port', 'COM3', '--expected-mac',
                           '3c:0f:02:c5:fe:64', '--esptool', str(self.tool), '--monitor-seconds', '0'])

    def test_offline_never_starts_process(self):
        with patch.object(flash.subprocess, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(flash.main(['--package', str(self.package)]), 0)
            run.assert_not_called()

    def test_tampered_artifact_rejected(self):
        (self.package / 'firmware.bin').write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'hash'):
            flash.inspect_package(self.package)

    def test_wrong_chip_rejected_even_with_updated_hash(self):
        self.image[12] = 0
        (self.package / 'firmware.bin').write_bytes(self.image)
        self.hashes()
        with self.assertRaisesRegex(ValueError, 'ESP32-S3'):
            flash.inspect_package(self.package)

    def test_full_workflow_uses_snapshot_preserves_nvs_and_verifies(self):
        with patch.object(flash, 'ROOT', self.root), patch.object(flash.subprocess, 'run', side_effect=self.fake_run), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.execute(), 0)
        write = next(c for c in self.calls if 'write-flash' in c)
        self.assertNotIn('--erase-all', write)
        self.assertNotIn(str(self.package / 'firmware.bin'), write)
        self.assertTrue(any('verify-flash' in c for c in self.calls))
        result = json.loads(next(self.root.glob('out/flash-runs/*/result.json')).read_text())
        self.assertEqual(result['status'], 'flash-verified')
        self.assertTrue(result['nvsUnchanged'])
        self.assertFalse(result['hardwareFunctionsVerified'])

    def test_partition_change_aborts_before_write(self):
        self.old[0x8008] ^= 1
        with patch.object(flash, 'ROOT', self.root), patch.object(flash.subprocess, 'run', side_effect=self.fake_run), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, 'layout differs'):
                self.execute()
        self.assertFalse(any('write-flash' in c for c in self.calls))

    def test_mac_mismatch_aborts_before_backup_or_write(self):
        with patch.object(flash, 'ROOT', self.root), patch.object(flash.subprocess, 'run', return_value=types.SimpleNamespace(returncode=0, stdout='MAC: 00:00:00:00:00:01')) as run, contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, 'MAC mismatch'):
                self.execute()
            self.assertEqual(run.call_count, 1)

    def test_write_failure_does_not_retry(self):
        def failure(cmd, **kwargs):
            result = self.fake_run(cmd, **kwargs)
            if 'write-flash' in cmd:
                result.returncode = 2
            return result
        with patch.object(flash, 'ROOT', self.root), patch.object(flash.subprocess, 'run', side_effect=failure), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'write failed'):
                self.execute()
        self.assertEqual(sum('write-flash' in c for c in self.calls), 1)
        self.assertFalse(any('verify-flash' in c for c in self.calls))
        result = json.loads(next(self.root.glob('out/flash-runs/*/result.json')).read_text())
        self.assertEqual(result['status'], 'failed')
        self.assertTrue(result['writeStarted'])

    def test_nvs_mismatch_is_not_reported_as_success(self):
        def corrupted(cmd, **kwargs):
            result = self.fake_run(cmd, **kwargs)
            if 'read-flash' in cmd and '0x9000' in cmd:
                Path(cmd[-1]).write_bytes(b'X' * 0x5000)
            return result
        with patch.object(flash, 'ROOT', self.root), patch.object(flash.subprocess, 'run', side_effect=corrupted), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'NVS verification mismatch'):
                self.execute()
        result = json.loads(next(self.root.glob('out/flash-runs/*/result.json')).read_text())
        self.assertEqual(result['status'], 'failed')
        self.assertNotIn('nvsUnchanged', result)


if __name__ == '__main__':
    unittest.main()
