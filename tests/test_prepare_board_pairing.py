"""Offline manifest tests, including the actual production C++ pairing codec.

Run: python3 -B -m unittest discover -s tests -p test_prepare_board_pairing.py -v
Only temporary files are created; no SDK, PIO, network, USB or NVS is used.
The C++ tests are explicitly skipped if no C++17 compiler is available.
"""

import contextlib
from concurrent.futures import ThreadPoolExecutor
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/prepare_board_pairing.py"
SPEC = importlib.util.spec_from_file_location("prepare_board_pairing", TOOL)
pairing = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pairing)
EPOCH = "0123456789abcdef0123456789abcdef"
BRAIN = "012345abcdef"
MOTION = "fedcba987654"
GOLDEN = (
    "4254503101003b00ebdbe4ab0101"
    "3031323334353637383961626364656630313233343536373839616263646566"
    "30313233343561626364656666656463626139383736353441"
)


def manifest(device="A"):
    with mock.patch.object(pairing.secrets, "token_bytes", return_value=bytes.fromhex(EPOCH)):
        return pairing.create_manifest(device, BRAIN, MOTION)


def repair_crc(record):
    result = bytearray(record)
    result[8:12] = struct.pack("<I", zlib.crc32(result[:8] + result[12:]))
    return bytes(result)


class CodecTest(unittest.TestCase):
    def test_fixed_vector_and_crc(self):
        self.assertEqual(zlib.crc32(b"123456789"), 0xcbf43926)
        self.assertEqual(manifest()["brain_record_hex"], GOLDEN)
        record = bytes.fromhex(GOLDEN)
        self.assertEqual(struct.unpack("<I", record[8:12])[0], 0xabe4dbeb)
        self.assertEqual(repair_crc(record), record)

    def test_all_device_lengths_and_role_mirrors(self):
        for length in range(1, 65):
            device = ("aZ09_-" * 11)[:length]
            data = manifest(device)
            self.assertIs(pairing.validate_manifest(data), data)
            for role, name, local, peer in ((1, "brain", BRAIN, MOTION),
                                          (2, "motion", MOTION, BRAIN)):
                record = bytes.fromhex(data[name + "_record_hex"])
                self.assertEqual(len(record), 70 + length)
                self.assertEqual(pairing.decode_record(record), dict(
                    role=role, device_id=device, pairing_epoch=EPOCH,
                    local_physical_id=local, peer_physical_id=peer))

    def test_identity_boundaries_and_types(self):
        for device in ("", "_A", "-A", "A" * 65, " A", "A ", "A/B", "A.B",
                       "A+", "A#", "设备", "A\n", "A\x00", "A\x7f", "A\x85",
                       None, True, 1, 1.0, [], {}):
            with self.subTest(device=device), self.assertRaises(pairing.ValidationError):
                pairing.create_manifest(device, BRAIN, MOTION)
        for physical in ("", "0" * 12, "a" * 11, "a" * 13, BRAIN.upper(),
                         "01:23:45:ab:cd:ef", "g" * 12, "a" * 11 + "\n",
                         "a" * 11 + "\x00", "a" * 11 + "é", None, True, 1, [], {}):
            for first, second in ((physical, MOTION), (BRAIN, physical)):
                with self.subTest(physical=physical), self.assertRaises(pairing.ValidationError):
                    pairing.create_manifest("A", first, second)
        with self.assertRaises(pairing.ValidationError):
            pairing.create_manifest("A", BRAIN, BRAIN)
        for epoch in ("0" * 32, "a" * 31, "a" * 33, EPOCH.upper(), "g" * 32, None, 1):
            with self.assertRaises(pairing.ValidationError):
                pairing.encode_record(1, "A", epoch, BRAIN, MOTION)
        for role in (True, False, 0, 3, "1", 1.0, None):
            with self.assertRaises(pairing.ValidationError):
                pairing.encode_record(role, "A", EPOCH, BRAIN, MOTION)
        for epoch in ("0" * 31 + "1", "f" * 32):
            record = pairing.encode_record(2, "9_-", epoch, "0" * 11 + "1", "f" * 12)
            self.assertEqual(pairing.decode_record(record)["pairing_epoch"], epoch)

    def test_random_nonzero_epoch_and_validation_before_entropy(self):
        with mock.patch.object(pairing.secrets, "token_bytes", side_effect=[
                b"\0" * 16, bytes.fromhex(EPOCH)]) as random:
            self.assertEqual(pairing.create_manifest("A", BRAIN, MOTION)["pairing_epoch"], EPOCH)
            self.assertEqual(random.call_args_list, [mock.call(16), mock.call(16)])
        with mock.patch.object(pairing.secrets, "token_bytes") as random:
            with self.assertRaises(pairing.ValidationError):
                pairing.create_manifest("bad/id", BRAIN, MOTION)
            random.assert_not_called()
        first = pairing.create_manifest("A", BRAIN, MOTION)
        second = pairing.create_manifest("A", BRAIN, MOTION)
        self.assertNotEqual(first["pairing_epoch"], second["pairing_epoch"])

    def test_corruption_truncation_and_trailing_bytes(self):
        for device in ("A", "Z" * 64):
            original = bytes.fromhex(manifest(device)["brain_record_hex"])
            for offset in range(len(original)):
                damaged = bytearray(original)
                damaged[offset] ^= 1
                with self.subTest(offset=offset), self.assertRaises(pairing.ValidationError):
                    pairing.decode_record(bytes(damaged))
            for length in range(len(original)):
                with self.assertRaises(pairing.ValidationError):
                    pairing.decode_record(original[:length])
            for suffix in (b"\0", b"garbage", b"\0" * 256):
                with self.assertRaises(pairing.ValidationError):
                    pairing.decode_record(original + suffix)

    def test_semantic_tampering_with_recomputed_crc(self):
        original = bytes.fromhex(GOLDEN)
        replacements = ((0, b"X"), (4, b"\x02"), (6, b"\x00"), (12, b"\x03"),
                        (13, b"\x00"), (13, b"\x40"), (14, b"0" * 32),
                        (14, b"A"), (14, b"\0"), (46, b"0" * 12), (46, b"G"),
                        (58, BRAIN.encode()), (70, b"\0"), (70, b"_"), (70, b"\xff"))
        for offset, replacement in replacements:
            damaged = bytearray(original)
            damaged[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaises(pairing.ValidationError):
                pairing.decode_record(repair_crc(damaged))

    def test_manifest_exact_fields_types_and_consistency(self):
        good = manifest()
        for field in good:
            bad = copy.deepcopy(good)
            del bad[field]
            with self.assertRaises(pairing.ValidationError):
                pairing.validate_manifest(bad)
            for value in (None, True, False, 1.0, 2, [], {}, "", "\n", "\x00"):
                bad = dict(good, **{field: value})
                with self.subTest(field=field, value=value), self.assertRaises(pairing.ValidationError):
                    pairing.validate_manifest(bad)
        for bad in ([], None, dict(good, password="secret"), dict(good, schema=2),
                    dict(good, device_id="B"), dict(good, pairing_epoch="f" * 32),
                    dict(good, brain_physical_id=MOTION), dict(good, motion_physical_id="1" * 12)):
            with self.assertRaises(pairing.ValidationError):
                pairing.validate_manifest(bad)
        for role, device, epoch, local, peer in (
            (1, "A", EPOCH, MOTION, BRAIN), (2, "B", EPOCH, MOTION, BRAIN),
            (2, "A", "f" * 32, MOTION, BRAIN), (2, "A", EPOCH, BRAIN, MOTION),
            (2, "A", EPOCH, MOTION, "1" * 12),
        ):
            bad = dict(good, motion_record_hex=pairing.encode_record(
                role, device, epoch, local, peer).hex())
            with self.assertRaises(pairing.ValidationError):
                pairing.validate_manifest(bad)
        for encoded in (GOLDEN.upper(), GOLDEN + "00", GOLDEN[:-1], " " + GOLDEN,
                        GOLDEN[:10] + "\n" + GOLDEN[10:]):
            with self.assertRaises(pairing.ValidationError):
                pairing.validate_manifest(dict(good, brain_record_hex=encoded))

    def test_strict_json(self):
        good = json.dumps(manifest()).encode()
        self.assertEqual(pairing.validate_manifest(pairing.read_json(io.BytesIO(good))), manifest())
        for data in (b'{"schema":1,"schema":1}', b'{"schema":1,"schem\\u0061":1}',
                     b'{"x":{"a":1,"a":2}}', b'{"x":NaN}', b'{"x":Infinity}',
                     b'{"x":-Infinity}', b'{"x":"a\x00b"}', b'{"x":"a\nb"}',
                     b'\xff', b'\xef\xbb\xbf' + good, good + b'{}',
                     b'[' * 2000 + b']' * 2000,
                     b' ' * (pairing.MAX_JSON_BYTES + 1)):
            with self.subTest(data=data[:30]), self.assertRaises(pairing.ValidationError):
                pairing.read_json(io.BytesIO(data))
        for control in ("\0", "\n", "\t", "\x7f", "\x85", "\u2028", "\ud800"):
            for field in manifest():
                data = json.dumps(dict(manifest(), **{field: control})).encode()
                with self.assertRaises(pairing.ValidationError):
                    pairing.validate_manifest(pairing.read_json(io.BytesIO(data)))
        exact = good + b" " * (pairing.MAX_JSON_BYTES - len(good))
        self.assertEqual(pairing.read_json(io.BytesIO(exact)), manifest())

    def test_read_json_requires_object_without_schema_validation(self):
        for data in (b"0", b"-1", b"1.5", b"1e3", b"1" * 30, b"1" * 5000,
                     b"null", b"true", b"false", b'"A"', b"[]", b"[{}]"):
            with self.subTest(data=data[:30]), self.assertRaises(pairing.ValidationError):
                pairing.read_json(io.BytesIO(data))
        self.assertEqual(pairing.read_json(io.BytesIO(b"{}")), {})


@unittest.skipUnless(os.name == "posix", "atomic 0600 publishing is POSIX-only")
class FilesystemTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pairing-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = self.root / "pairing.json"
        self.data = manifest("Z" * 64)

    def test_exclusive_permissions_even_with_restrictive_umask(self):
        for mask in (0, 0o022, 0o777):
            old = os.umask(mask)
            try:
                pairing.write_manifest(self.path, self.data)
            finally:
                os.umask(old)
            self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
            self.assertEqual(pairing.read_manifest(self.path), self.data)
            self.assertEqual(list(self.root.iterdir()), [self.path])
            self.path.unlink()

    def test_existing_files_directories_and_symlinks_protected(self):
        target = self.root / "target"
        target.write_bytes(b"old migration evidence")
        for kind in ("file", "directory", "symlink", "dangling", "hardlink"):
            if kind == "file":
                self.path.write_bytes(b"original")
            elif kind == "directory":
                self.path.mkdir()
            elif kind == "hardlink":
                os.link(target, self.path)
            else:
                self.path.symlink_to(target if kind == "symlink" else self.root / "missing")
            before = self.path.lstat()
            with self.subTest(kind=kind), self.assertRaises(FileExistsError):
                pairing.write_manifest(self.path, self.data)
            self.assertEqual(self.path.lstat(), before)
            self.assertEqual(target.read_bytes(), b"old migration evidence")
            if kind == "file":
                self.assertEqual(self.path.read_bytes(), b"original")
            self.path.rmdir() if kind == "directory" else self.path.unlink()

    def test_before_publish_failures_leave_no_partial_output(self):
        for function in ("write", "fsync", "fchmod", "link"):
            with self.subTest(function=function), mock.patch.object(
                    pairing.os, function, side_effect=OSError("injected failure")):
                with self.assertRaises(OSError):
                    pairing.write_manifest(self.path, self.data)
            self.assertEqual(list(self.root.iterdir()), [])
        real_write = os.write
        calls = 0

        def partial_then_fail(fd, data):
            nonlocal calls
            calls += 1
            if calls == 1:
                return real_write(fd, data[:17])
            raise OSError("disk full")

        with mock.patch.object(pairing.os, "write", side_effect=partial_then_fail):
            with self.assertRaises(OSError):
                pairing.write_manifest(self.path, self.data)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_short_writes_and_zero_progress(self):
        real_write = os.write
        with mock.patch.object(pairing.os, "write", side_effect=lambda fd, data: real_write(fd, data[:7])):
            pairing.write_manifest(self.path, self.data)
        self.assertEqual(pairing.read_manifest(self.path), self.data)
        self.path.unlink()
        with mock.patch.object(pairing.os, "write", return_value=0):
            with self.assertRaises(OSError):
                pairing.write_manifest(self.path, self.data)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_late_failure_preserves_complete_manifest(self):
        real_fsync = os.fsync

        def fail_directory(fd):
            if stat.S_ISDIR(os.fstat(fd).st_mode):
                raise OSError("directory sync failed")
            return real_fsync(fd)

        with mock.patch.object(pairing.os, "fsync", side_effect=fail_directory):
            with self.assertRaises(OSError):
                pairing.write_manifest(self.path, self.data)
        self.assertEqual(pairing.read_manifest(self.path), self.data)
        self.assertEqual(list(self.root.iterdir()), [self.path])

    def test_atomic_publish_race_does_not_replace_winner(self):
        real_link = os.link

        def race(source, destination, **kwargs):
            self.assertFalse(self.path.exists())
            temporary = self.root / source
            self.assertEqual(pairing.read_manifest(temporary), self.data)
            self.assertEqual(stat.S_IMODE(temporary.stat().st_mode), 0o600)
            self.path.symlink_to(self.root / "missing-winner")
            return real_link(source, destination, **kwargs)

        with mock.patch.object(pairing.os, "link", side_effect=race):
            with self.assertRaises(FileExistsError):
                pairing.write_manifest(self.path, self.data)
        self.assertTrue(self.path.is_symlink())
        self.assertEqual(os.readlink(self.path), str(self.root / "missing-winner"))
        self.assertEqual(list(self.root.iterdir()), [self.path])

    def test_read_rejects_symlink_special_file_and_oversize(self):
        self.path.symlink_to(self.root / "missing")
        with self.assertRaises(OSError):
            pairing.read_manifest(self.path)
        self.path.unlink()
        os.mkfifo(self.path)
        with self.assertRaises(pairing.ValidationError):
            pairing.read_manifest(self.path)
        self.path.unlink()
        self.path.write_bytes(b" " * (pairing.MAX_JSON_BYTES + 1))
        with self.assertRaises(pairing.ValidationError):
            pairing.read_manifest(self.path)
        with self.assertRaises(pairing.ValidationError):
            pairing.read_manifest(self.root)

    def test_concurrent_publishers_have_exactly_one_winner(self):
        candidates = [manifest("Device_" + str(index)) for index in range(8)]

        def publish(data):
            try:
                pairing.write_manifest(self.path, data)
                return data
            except FileExistsError:
                return None

        with ThreadPoolExecutor(max_workers=8) as pool:
            winners = [data for data in pool.map(publish, candidates) if data is not None]
        self.assertEqual(len(winners), 1)
        self.assertEqual(pairing.read_manifest(self.path), winners[0])
        self.assertEqual(list(self.root.iterdir()), [self.path])

    def test_temporary_name_collisions_never_write_the_destination(self):
        collision = "f" * 32
        target = self.root / (".board-pairing-" + collision + ".tmp")
        with mock.patch.object(pairing.secrets, "token_hex", return_value=collision):
            with self.assertRaises(FileExistsError):
                pairing.write_manifest(target, self.data)
        self.assertEqual(list(self.root.iterdir()), [])
        target.write_bytes(b"unrelated temporary evidence")
        with mock.patch.object(pairing.secrets, "token_hex", return_value=collision):
            with self.assertRaises(FileExistsError):
                pairing.write_manifest(self.path, self.data)
        self.assertEqual(target.read_bytes(), b"unrelated temporary evidence")
        self.assertFalse(self.path.exists())

    def test_parent_path_swap_cannot_redirect_publication(self):
        parent = self.root / "original"
        parent.mkdir()
        moved = self.root / "moved"
        other = self.root / "other"
        other.mkdir()
        real_link = os.link

        def replace_parent(source, destination, **kwargs):
            parent.rename(moved)
            parent.symlink_to(other, target_is_directory=True)
            return real_link(source, destination, **kwargs)

        with mock.patch.object(pairing.os, "link", side_effect=replace_parent):
            pairing.write_manifest(parent / "pairing.json", self.data)
        self.assertEqual(pairing.read_manifest(moved / "pairing.json"), self.data)
        self.assertEqual(list(other.iterdir()), [])
        self.assertEqual([path.name for path in moved.iterdir()], ["pairing.json"])

    def test_invalid_manifest_missing_parent_and_invalid_output(self):
        with self.assertRaises(pairing.ValidationError):
            pairing.write_manifest(self.path, dict(self.data, schema=True))
        with self.assertRaises(OSError):
            pairing.write_manifest(self.root / "missing" / "pair.json", self.data)
        for path in ("", "-", "x\0y", "x\ny", "x\x7fy"):
            with self.assertRaises(pairing.ValidationError):
                pairing.write_manifest(path, self.data)
        self.assertEqual(list(self.root.iterdir()), [])


class CliTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="pairing-cli-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "pairing.json"
        self.identities = ["--device-id", "A", "--brain-physical-id", BRAIN,
                           "--motion-physical-id", MOTION]

    def run_cli(self, *args, data=b""):
        return subprocess.run([sys.executable, "-B", str(TOOL), *args], input=data,
                              capture_output=True, timeout=10)

    def test_generate_check_and_stdin(self):
        result = self.run_cli("generate", "--output", str(self.path), *self.identities)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"no NVS writes, migration or startup approval", result.stdout)
        self.assertIn(b"reuse", result.stdout)
        saved = self.path.read_bytes()
        for arguments, data in ((["check", "--input", str(self.path)], b""),
                                (["check", "--input", "-"], saved)):
            result = self.run_cli(*arguments, data=data)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.path.read_bytes(), saved)
        self.path.unlink()
        result = self.run_cli("generate", "--output", str(self.path), "--stdin", data=json.dumps(dict(
            device_id="A" * 64, brain_physical_id=BRAIN, motion_physical_id=MOTION)).encode())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(pairing.read_manifest(self.path)["device_id"], "A" * 64)

    def test_existing_output_never_requests_new_epoch(self):
        for kind in ("file", "dangling"):
            if kind == "file":
                self.path.write_bytes(b"existing migration")
            else:
                self.path.symlink_to(self.path.parent / "missing")
            with mock.patch.object(pairing.secrets, "token_bytes") as random:
                with contextlib.redirect_stderr(io.StringIO()) as stderr:
                    code = pairing.main(["generate", "--output", str(self.path), *self.identities])
                self.assertEqual(code, 1)
                self.assertIn("reuse", stderr.getvalue())
                random.assert_not_called()
            self.path.unlink()

    def test_reject_unknown_abbreviated_duplicate_and_incomplete_arguments(self):
        base = ["generate", "--output", str(self.path)]
        cases = [[], ["generate", *self.identities], base, base + self.identities[:-2],
                 base + self.identities + ["--device-id", "B"],
                 base + self.identities + ["--output", str(self.path)],
                 base + self.identities + ["--stdin"],
                 base + ["--stdin", "--stdin"],
                 base + self.identities + ["--pairing-epoch", EPOCH],
                 base + self.identities + ["--force"],
                 base + ["--device", "A", *self.identities[2:]],
                 ["check"], ["check", "--input", "-", "--input", "-"],
                 ["check", "--input", "-", "--output", str(self.path)]]
        for args in cases:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"reuse", result.stderr)
                self.assertFalse(self.path.exists())

    def test_invalid_stdin_identity_and_no_secret_echo(self):
        identities = dict(device_id="A", brain_physical_id=BRAIN, motion_physical_id=MOTION)
        cases = [b"{}", b"[]", b"", b" " * (pairing.MAX_JSON_BYTES + 1),
                 json.dumps(dict(identities, password="do-not-echo-secret")).encode(),
                 json.dumps(dict(identities, pairing_epoch=EPOCH)).encode(),
                 json.dumps(dict(identities, device_id=True)).encode(),
                 json.dumps(dict(identities, device_id="A\n")).encode(),
                 json.dumps(dict(identities, motion_physical_id=BRAIN)).encode(),
                 (json.dumps(identities)[:-1] + ',"device_id":"A"}').encode()]
        for data in cases:
            result = self.run_cli("generate", "--output", str(self.path), "--stdin", data=data)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"do-not-echo-secret", result.stderr + result.stdout)
            self.assertFalse(self.path.exists())
        result = self.run_cli("generate", "--output", str(self.path), *self.identities,
                              "--password", "do-not-echo-secret")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(b"do-not-echo-secret", result.stderr + result.stdout)
        for device in ("A" * 65, "A\n", "_A", "设备"):
            result = self.run_cli("generate", "--output", str(self.path),
                                  "--device-id", device, *self.identities[2:])
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(self.path.exists())

    def test_check_rejects_tampering_and_duplicate_key_without_writing(self):
        for content in (json.dumps(dict(manifest(), device_id="B")).encode(),
                        (json.dumps(manifest())[:-1] + ',"schema":1}').encode()):
            self.path.write_bytes(content)
            result = self.run_cli("check", "--input", str(self.path))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.path.read_bytes(), content)


DRIVER = r'''
#include "BoardPairingRecord.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using babytech::v4::Pairing;
using babytech::v4::Role;
using babytech::boardlink::decodePairingRecord;
using babytech::boardlink::encodePairingRecord;
int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string operation;
        in >> operation;
        Pairing pair{};
        bool valid = true;
        if (operation == "D") {
            std::string hex;
            in >> hex;
            std::vector<uint8_t> data;
            if (hex.size() % 2) valid = false;
            for (size_t i = 0; valid && i < hex.size(); i += 2) {
                const std::string alphabet = "0123456789abcdef";
                auto hi = alphabet.find(hex[i]);
                auto lo = alphabet.find(hex[i + 1]);
                if (hi == std::string::npos || lo == std::string::npos) valid = false;
                else data.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
            valid = valid && decodePairingRecord(data.data(), data.size(), pair);
        } else if (operation == "E") {
            unsigned role;
            std::string device, epoch, local, peer;
            valid = bool(in >> role >> device >> epoch >> local >> peer);
            valid = valid && role <= 255 && device.size() <= 64 && epoch.size() == 32
                && local.size() == 12 && peer.size() == 12;
            if (valid) {
                pair.role = static_cast<Role>(role);
                std::strcpy(pair.deviceId, device.c_str());
                std::strcpy(pair.epoch, epoch.c_str());
                std::strcpy(pair.localPhysicalId, local.c_str());
                std::strcpy(pair.peerPhysicalId, peer.c_str());
            }
        } else valid = false;
        uint8_t output[256]{};
        const size_t size = valid ? encodePairingRecord(pair, output, sizeof(output)) : 0;
        if (!size) { std::cout << "INVALID\n"; continue; }
        std::cout << unsigned(pair.role) << ' ' << pair.deviceId << ' ' << pair.epoch
                  << ' ' << pair.localPhysicalId << ' ' << pair.peerPhysicalId << ' ';
        const char* digits = "0123456789abcdef";
        for (size_t i = 0; i < size; ++i)
            std::cout << digits[output[i] >> 4] << digits[output[i] & 15];
        std::cout << '\n';
    }
}
'''


class ProductionCppInteropTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if not compiler:
            raise unittest.SkipTest("C++17 compiler missing: production codec interoperability NOT tested")
        temporary = tempfile.TemporaryDirectory(prefix="pairing-cpp-")
        cls.addClassCleanup(temporary.cleanup)
        root = Path(temporary.name)
        driver = root / "driver.cpp"
        driver.write_text(DRIVER, encoding="ascii")
        cls.binary = root / "codec"
        sources = ["shared/BoardProtocol/src/BoardProtocol.cpp",
                   "shared/BoardProtocol/src/BoardProtocolV4.cpp",
                   "shared/BoardProtocol/src/BoardSessionV4.cpp",
                   "shared/ProductBoardLink/src/BoardPairingRecord.cpp"]
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(ROOT / "shared/BoardProtocol/src"),
                   "-I", str(ROOT / "shared/ProductBoardLink/src"),
                   *(str(ROOT / source) for source in sources), str(driver), "-o", str(cls.binary)]
        subprocess.run(command, check=True, capture_output=True, timeout=60)

    def run_driver(self, lines):
        result = subprocess.run([str(self.binary)], input="\n".join(lines) + "\n",
                                text=True, capture_output=True, check=True, timeout=15)
        return result.stdout.splitlines()

    def test_both_directions_all_lengths_roles_and_golden(self):
        lines = []
        expected = []
        for length in range(1, 65):
            device = ("aZ09_-" * 11)[:length]
            for role, name, local, peer in ((1, "brain", BRAIN, MOTION),
                                          (2, "motion", MOTION, BRAIN)):
                record = manifest(device)[name + "_record_hex"]
                fields = f"{role} {device} {EPOCH} {local} {peer}"
                lines.extend(("D " + record, "E " + fields))
                expected.extend((fields + " " + record,) * 2)
        lines += ["D " + GOLDEN, f"E 1 A {EPOCH} {BRAIN} {MOTION}"]
        expected += [f"1 A {EPOCH} {BRAIN} {MOTION} {GOLDEN}"] * 2
        outputs = self.run_driver(lines)
        self.assertEqual(outputs, expected)
        for line in outputs:
            role, device, epoch, local, peer, record = line.split()
            self.assertEqual(pairing.decode_record(bytes.fromhex(record)), dict(
                role=int(role), device_id=device, pairing_epoch=epoch,
                local_physical_id=local, peer_physical_id=peer))

    def test_shared_rejection_of_damaged_and_crc_valid_invalid_records(self):
        original = bytes.fromhex(GOLDEN)
        invalid = [original[:length] for length in range(len(original))]
        invalid.extend((original + b"\0", original + b"extra"))
        for offset in range(len(original)):
            bad = bytearray(original)
            bad[offset] ^= 1
            invalid.append(bytes(bad))
        for offset, replacement in ((12, b"\x00"), (13, b"\x41"), (14, b"0" * 32),
                                    (46, b"0" * 12), (58, BRAIN.encode()), (70, b"_"),
                                    (70, b"\0"), (70, b"\xff")):
            bad = bytearray(original)
            bad[offset:offset + len(replacement)] = replacement
            invalid.append(repair_crc(bad))
        for record in invalid:
            with self.assertRaises(pairing.ValidationError):
                pairing.decode_record(record)
        self.assertEqual(self.run_driver(["D " + record.hex() for record in invalid]),
                         ["INVALID"] * len(invalid))


if __name__ == "__main__":
    unittest.main()
