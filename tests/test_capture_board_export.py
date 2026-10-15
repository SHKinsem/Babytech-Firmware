"""USB diagnostic tests: fake byte ports only, no pyserial, PIO or hardware.

Run: python3 -B -m unittest discover -s tests -p test_capture_board_export.py -v
Fixtures follow MaintenanceExport.cpp, RecordBytes.h, and the empty BBS1/BMS1
layouts. They do not claim to validate full business-state semantics.
"""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/capture_board_export.py"
SPEC = importlib.util.spec_from_file_location("capture_board_export", TOOL)
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)
CHALLENGE = "0123456789abcdef0123456789abcdef"
EPOCH = "fedcba9876543210fedcba9876543210"
BRAIN = "012345abcdef"
MOTION = "fedcba987654"
SECRET = "private-admin-code-DO-NOT-PRINT"


def envelope(magic, payload):
    header = struct.pack("<4sHH", magic, 1, len(payload))
    return header + struct.pack("<I", zlib.crc32(header + payload)) + payload


def repair_crc(record):
    return record[:8] + struct.pack("<I", zlib.crc32(record[:8] + record[12:])) + record[12:]


def pair_record(role="brain", device="A", epoch=EPOCH, local=None, peer=None):
    return capture._pairing.encode_record(
        1 if role == "brain" else 2, device, epoch,
        local or (BRAIN if role == "brain" else MOTION),
        peer or (MOTION if role == "brain" else BRAIN))


def state_record(role="brain", pair=None):
    pair = pair if pair is not None else pair_record(role)
    # Brain: absent context, zero localSequence, no pending request.
    # Motion: absent context, two zero watermarks, two empty results, empty slot.
    tail = b"\0" * (10 if role == "brain" else 20)
    return envelope(b"BBS1" if role == "brain" else b"BMS1", struct.pack("<H", len(pair)) + pair + tail)


def text_bytes(value):
    return struct.pack("<H", len(value)) + value


def legacy_record(device="A", cleared=True):
    data = bytes((1, int(cleared))) + text_bytes(device.encode("ascii")) + struct.pack("<I", 7)
    if not cleared:
        data += text_bytes(b"child-1") + text_bytes(b"Private name") + text_bytes(b"Private formula")
        data += struct.pack("<HBf", 100, 40, 25.0)
    return data


def exported(role="brain", ready=True, challenge=CHALLENGE, device="A"):
    pair = pair_record(role, device)
    return dict(schema=1, role=role, device_id=device,
                physical_id=BRAIN if role == "brain" else MOTION,
                boot="0000000000000001", challenge=challenge, captured_ms=123,
                pair_status="ready" if ready else "missing",
                state_status="ready" if ready else "missing",
                legacy_status="not_applicable" if role == "brain" else "missing",
                legacy_event="not_applicable" if role == "brain" else "missing",
                pair_hex=pair.hex() if ready else "",
                state_hex=state_record(role, pair).hex() if ready else "", legacy_hex="")


def wire(value):
    return capture.PREFIX + json.dumps(value, separators=(",", ":")).encode("ascii") + b"\n"


def fragment(offset, payload):
    # Independent bitwise implementation matching BoardProtocol.cpp crc16.
    crc = 0xffff
    for byte in struct.pack("<HB", offset, len(payload)) + payload:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xffff
    return f"[mx] {offset:04x}:{crc:04x}:{payload.hex()}\n".encode("ascii")


def framed(data, chunk=16, noise=b""):
    return noise.join(fragment(offset, data[offset:offset + chunk]) for offset in range(0, len(data), chunk))


def validate(value, role=None, device="A", challenge=CHALLENGE):
    return capture.validate_export(value, role=role or value["role"], device_id=device, challenge=challenge)


class Clock:
    def __init__(self, step=0.001):
        self.now = 0.0
        self.step = step

    def __call__(self):
        self.now += self.step
        return self.now


class FakePort:
    """Respond only after a complete command, including across short writes."""
    def __init__(self, role="brain", begin=b"[maint] active\r\n", response=None,
                 chunk=256, short_write=10000, dirty=b"", initial=b""):
        self.role = role
        self.begin = begin
        self.response = response
        self.chunk = chunk
        self.short_write = short_write
        self.dirty = dirty
        self.rx = bytearray(initial)
        self.tx = bytearray()
        self.pending = bytearray()
        self.commands = []
        self.read_calls = 0
        self.reset_calls = 0
        self.closed = False
        self.opened = False
        self.timeout = None
        self.write_timeout = None

    def reset_input_buffer(self):
        self.reset_calls += 1
        self.rx.clear()

    def write(self, data):
        count = min(self.short_write, len(data))
        self.tx.extend(data[:count])
        self.pending.extend(data[:count])
        while b"\n" in self.pending:
            end = self.pending.index(b"\n")
            command = bytes(self.pending[:end])
            del self.pending[:end + 1]
            self.commands.append(command)
            if command == b"MAINT BEGIN":
                self.rx.extend(self.begin)
            elif command.startswith(b"MAINT EXPORT "):
                _, _, device, challenge = command.decode("ascii").split()
                data = exported(self.role, challenge=challenge, device=device)
                response = self.response(data) if self.response else framed(wire(data))
                self.rx.extend(self.dirty + response)
            else:
                raise AssertionError("Unexpected command")
        return count

    def read(self, size):
        self.read_calls += 1
        count = min(size, self.chunk, len(self.rx))
        data = bytes(self.rx[:count])
        del self.rx[:count]
        return data

    def open(self):
        assert self.dtr is False and self.rts is False
        self.opened = True

    def close(self):
        self.closed = True


class ValidationTest(unittest.TestCase):
    def test_ready_and_missing_both_roles(self):
        for role in ("brain", "motion"):
            for ready in (True, False):
                value = exported(role, ready)
                self.assertIs(validate(value), value)
                self.assertEqual(capture.parse_export(wire(value).rstrip(b"\n"), role=role,
                                 device_id="A", challenge=CHALLENGE), value)

    def test_exact_fields_and_root_types(self):
        for root in (None, [], True, 1, "", {}):
            with self.assertRaises(capture.CaptureError):
                capture.validate_export(root, role="brain", device_id="A", challenge=CHALLENGE)
        for field in capture.FIELDS:
            value = exported()
            del value[field]
            with self.subTest(field=field), self.assertRaises(capture.CaptureError):
                validate(value, role="brain")
        value = exported()
        value["credentials"] = SECRET
        with self.assertRaises(capture.CaptureError):
            validate(value)

    def test_schema_identity_and_timestamp_rejections(self):
        cases = {
            "schema": (True, 1.0, "1", 0, 2),
            "role": ("motion", "Brain", None, []),
            "device_id": ("a", "A\n", "", True, {}),
            "challenge": ("0" * 32, CHALLENGE.upper(), "f" * 32, CHALLENGE[:-1], []),
            "physical_id": ("0" * 12, BRAIN.upper(), BRAIN + "\n", "ff:ff:ff:ff:ff:ff", None),
            "boot": ("0" * 16, "a" * 15, "A" * 16, 1),
            "captured_ms": (True, False, 1.0, -1, 2**32, "12", None),
        }
        for field, alternatives in cases.items():
            for bad in alternatives:
                value = exported()
                value[field] = bad
                with self.subTest(field=field, value=bad), self.assertRaises(capture.CaptureError):
                    validate(value, role="brain")
        for ms in (0, 2**32 - 1):
            value = exported()
            value["captured_ms"] = ms
            validate(value)

    def test_all_diagnostic_statuses(self):
        for role in ("brain", "motion"):
            for field, allowed in (("pair_status", capture.PAIR_STATUSES),
                                   ("state_status", capture.STATE_STATUSES)):
                for status in allowed:
                    value = exported(role)
                    value[field] = status
                    if status != "ready":
                        value[field.replace("_status", "_hex")] = ""
                    validate(value)
            for field in ("pair_status", "state_status", "legacy_status", "legacy_event"):
                for bad in ("", "READY", "future_status", None, [], {}, True):
                    value = exported(role)
                    value[field] = bad
                    with self.assertRaises(capture.CaptureError):
                        validate(value)
        for field in ("legacy_status", "legacy_event"):
            value = exported("motion")
            value[field] = "not_applicable"
            with self.assertRaises(capture.CaptureError):
                validate(value)
            for status in (("missing", "corrupt", "io_error") if field == "legacy_status"
                           else capture.EVENT_STATUSES):
                value[field] = status
                validate(value)
        value = exported()
        value["pair_status"] = "conflict"
        value["pair_hex"] = ""
        with self.assertRaises(capture.CaptureError):
            validate(value)

    def test_nonready_blobs_must_be_empty(self):
        for role in ("brain", "motion"):
            for field in ("pair_hex", "state_hex", "legacy_hex"):
                for bad in ("00", " ", None, [], 0):
                    value = exported(role, ready=False)
                    value[field] = bad
                    with self.assertRaises(capture.CaptureError):
                        validate(value)

    def test_ready_hex_format_and_maximum(self):
        for field, limit in (("pair_hex", 134), ("state_hex", 4073), ("legacy_hex", 981)):
            for bad in ("", "0", "AA", "0g", "00\n", "00 00", "00" * (limit + 1), [], None):
                value = exported("motion")
                value[field.replace("_hex", "_status")] = "ready"
                value[field] = bad
                with self.subTest(field=field, bad=str(bad)[:10]), self.assertRaises(capture.CaptureError):
                    validate(value)

    def test_pair_crc_header_and_identity(self):
        original = pair_record()
        for offset in (0, 4, 6, 8, 12, len(original) - 1):
            record = bytearray(original)
            record[offset] ^= 1
            value = exported()
            value["pair_hex"] = record.hex()
            with self.assertRaises(capture.CaptureError):
                validate(value)
        for record in (original[:-1], original + b"\0", pair_record("motion"),
                       pair_record(device="Other"), pair_record(local="112233445566")):
            value = exported()
            value["pair_hex"] = record.hex()
            with self.assertRaises(capture.CaptureError):
                validate(value)

    def test_state_envelope_crc_role_and_embedded_pair(self):
        for role in ("brain", "motion"):
            original = state_record(role)
            bad_records = [original[:13], original[:-1], original + b"\0",
                           state_record("motion" if role == "brain" else "brain")]
            for offset in (0, 4, 6, 8, len(original) - 1):
                data = bytearray(original)
                data[offset] ^= 1
                bad_records.append(bytes(data))
            for pair in (pair_record(role, device="Wrong"), pair_record(role, local="112233445566"),
                         pair_record("motion" if role == "brain" else "brain")):
                bad_records.append(state_record(role, pair))
            damaged_pair = bytearray(pair_record(role))
            damaged_pair[8] ^= 1
            bad_records.append(state_record(role, damaged_pair))  # Good outer CRC, bad inner CRC.
            for length in (0, 70, 135, 65535):
                bad_records.append(repair_crc(original[:12] + struct.pack("<H", length) + original[14:]))
            for data in bad_records:
                value = exported(role)
                value["state_hex"] = data.hex()
                with self.subTest(role=role, prefix=data[:14].hex()), self.assertRaises(capture.CaptureError):
                    validate(value)

    def test_state_pair_must_exactly_match_epoch_and_peer(self):
        for role in ("brain", "motion"):
            for pair in (pair_record(role, epoch="1" * 32), pair_record(role, peer="112233445566")):
                value = exported(role)
                value["state_hex"] = state_record(role, pair).hex()
                with self.assertRaises(capture.CaptureError):
                    validate(value)
                value["pair_status"] = "missing"
                value["pair_hex"] = ""
                validate(value)  # Export diagnostics can describe interrupted installation.

    def test_legacy_canonical_structural_scope(self):
        value = exported("motion")
        value["legacy_status"] = "ready"
        for cleared in (False, True):
            value["legacy_hex"] = legacy_record(cleared=cleared).hex()
            validate(value)
        original = legacy_record()
        for bad in (b"", original[:-1], original + b"\0", b"\2" + original[1:],
                    original[:1] + b"\2" + original[2:], original[:2] + b"\xff\xff" + original[4:],
                    legacy_record(device="Wrong")):
            value["legacy_hex"] = bad.hex()
            with self.assertRaises(capture.CaptureError):
                validate(value)
        # Structurally valid but semantically invalid data is deliberately NOT certified.
        data = b"\1\0" + text_bytes(b"A") + struct.pack("<I", 0)
        data += text_bytes(b"\xff") + text_bytes(b"") + text_bytes(b"") + struct.pack("<HBf", 0, 255, float("nan"))
        value["legacy_hex"] = data.hex()
        validate(value)
        self.assertIn("not full semantic verification", capture.SCOPE_NOTICE)

    def test_device_boundaries(self):
        for device in ("A", "9_-", "Z" * 64):
            validate(exported(device=device), device=device)
        for device in ("", "_A", "-A", "A" * 65, "A\nMAINT END", "A/B", "\u00e9", None, []):
            with self.assertRaises(capture.CaptureError):
                capture.validate_export(exported(), role="brain", device_id=device, challenge=CHALLENGE)

    def test_strict_json_duplicate_nonfinite_nested_and_utf8(self):
        good = wire(exported()).rstrip(b"\n")
        alternatives = [good.replace(b'"schema":1', b'"schema":1,"schema":1'),
                        good.replace(b'"schema":1', b'"schema":NaN'),
                        good.replace(b'"schema":1', b'"schema":Infinity'),
                        good + b"garbage", capture.PREFIX + b"\xff", capture.PREFIX + b"[]",
                        capture.PREFIX + b"[" * 2000 + b"]" * 2000,
                        b"dirty" + good, good.replace(capture.PREFIX, b"[maint-export]"),
                        capture.PREFIX + b" " * capture.MAX_LINE_BYTES]
        for data in alternatives:
            with self.assertRaises(capture.CaptureError):
                capture.parse_export(data, role="brain", device_id="A", challenge=CHALLENGE)


class TransportTest(unittest.TestCase):
    def run_capture(self, port, **kwargs):
        with mock.patch.object(capture.secrets, "token_bytes", return_value=bytes.fromhex(CHALLENGE)):
            return capture.capture_export(port, role=port.role, device_id="A", clock=Clock(), **kwargs)

    def assert_only_capture_commands(self, port):
        self.assertEqual(port.commands[0], b"MAINT BEGIN")
        self.assertLessEqual(len(port.commands), 2)
        if len(port.commands) == 2:
            self.assertEqual(port.commands[1], b"MAINT EXPORT A " + CHALLENGE.encode())
        for forbidden in (b"END", b"RESET", b"REBOOT", b"OTA", b"INSTALL"):
            self.assertNotIn(forbidden, port.tx)

    def test_fragmentation_dirty_lines_and_short_writes(self):
        for role in ("brain", "motion"):
            for size in (1, 2, 17, 256):
                port = FakePort(role, chunk=size, short_write=size,
                                begin=b"boot chatter\n\xff\x00\n[maint] active\r\n",
                                dirty=SECRET.encode() + b"\n\n")
                self.assertEqual(self.run_capture(port), exported(role))
                self.assert_only_capture_commands(port)
                self.assertEqual(port.reset_calls, 1)
                self.assertFalse(port.closed)
                self.assertLessEqual(port.timeout, 0.1)
                self.assertLessEqual(port.write_timeout, 0.1)

    def test_active_required_and_stale_buffer_discarded(self):
        for begin in (b"[maint] unsafe\n", b"[maint] inactive\n", b"[maint] active extra\n",
                      wire(exported()), b"", b"prefix [maint] active\n"):
            port = FakePort(begin=begin, initial=b"[maint] active\n")
            with mock.patch.object(capture.secrets, "token_bytes") as entropy:
                with self.assertRaises(capture.CaptureError):
                    capture.capture_export(port, role="brain", device_id="A", timeout=0.1, clock=Clock())
                entropy.assert_not_called()
            self.assertEqual(port.commands, [b"MAINT BEGIN"])

    def test_zero_challenge_retry_is_bounded_and_nonzero(self):
        port = FakePort()
        with mock.patch.object(capture.secrets, "token_bytes", side_effect=[b"\0" * 16, bytes.fromhex(CHALLENGE)]) as entropy:
            capture.capture_export(port, role="brain", device_id="A", clock=Clock())
            self.assertEqual(entropy.call_count, 2)
        self.assert_only_capture_commands(port)
        port = FakePort()
        with mock.patch.object(capture.secrets, "token_bytes", return_value=b"\0" * 16) as entropy:
            with self.assertRaises(capture.CaptureError):
                capture.capture_export(port, role="brain", device_id="A", clock=Clock())
            self.assertEqual(entropy.call_count, 8)
        self.assertEqual(port.commands, [b"MAINT BEGIN"])

    def test_wrong_challenge_role_and_device_fail_without_release(self):
        for field, bad in (("challenge", "f" * 32), ("role", "motion"), ("device_id", "Wrong")):
            def reply(data):
                data[field] = bad
                return framed(wire(data))
            port = FakePort(response=reply)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port)
            self.assert_only_capture_commands(port)

    def test_refusal_aborted_export_no_response_and_partial_line(self):
        for response in (b"[maint] unsafe\n", b"[maint] export_failed\n", b"[maint] export_aborted\n",
                         b'[maint-export] {"schema":1,\n[maint] export_aborted\n',
                         b"", wire(exported()).rstrip(b"\n")):
            port = FakePort(response=lambda _data: response)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port, timeout=0.1)
            self.assert_only_capture_commands(port)

    def test_huge_unterminated_or_terminated_lines_are_bounded(self):
        for suffix in (b"", b"\n"):
            port = FakePort(response=lambda _data: b"x" * 100000 + suffix)
            with self.assertRaisesRegex(capture.CaptureError, "line exceeds"):
                self.run_capture(port)
            self.assertLessEqual(port.read_calls * capture.READ_SIZE,
                                 capture.MAX_LINE_BYTES + 2 * capture.READ_SIZE)
            self.assert_only_capture_commands(port)

    def test_line_limit_boundary(self):
        port = FakePort(dirty=b"x" * capture.MAX_LINE_BYTES + b"\n")
        self.run_capture(port)
        port = FakePort(dirty=b"x" * (capture.MAX_LINE_BYTES + 1) + b"\n")
        with self.assertRaises(capture.CaptureError):
            self.run_capture(port)

    def test_aggregate_dirty_input_limit(self):
        port = FakePort(response=lambda _data: b"x" * 255 + b"\n")
        port.read = mock.Mock(return_value=b"x" * 255 + b"\n")
        with self.assertRaisesRegex(capture.CaptureError, "aggregate"):
            self.run_capture(port)
        self.assertLessEqual(port.read.call_count, capture.MAX_INPUT_BYTES // capture.READ_SIZE + 1)

    def test_empty_reads_are_bounded_even_with_frozen_clock(self):
        port = FakePort(begin=b"")
        with mock.patch.object(capture, "MAX_IO_CALLS", 20):
            with self.assertRaisesRegex(capture.CaptureError, "budget"):
                capture.capture_export(port, role="brain", device_id="A", clock=lambda: 0)
        self.assertLessEqual(port.read_calls, 20)

    def test_deadline_includes_short_writes(self):
        port = FakePort(short_write=1)
        with self.assertRaises(capture.CaptureError):
            capture.capture_export(port, role="brain", device_id="A", timeout=0.1, clock=Clock(0.02))
        self.assertEqual(port.commands, [])
        self.assertGreater(port.write_timeout, 0)
        self.assertLessEqual(port.write_timeout, 0.1)

    def test_invalid_progress_and_io_failures(self):
        for returned in (0, -1, None, True, 999):
            port = FakePort()
            port.write = mock.Mock(return_value=returned)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port)
        for returned in (None, "text", b"x" * (capture.READ_SIZE + 1)):
            port = FakePort()
            port.read = mock.Mock(return_value=returned)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port)
        for operation in ("write", "read", "reset_input_buffer"):
            port = FakePort()
            setattr(port, operation, mock.Mock(side_effect=OSError(SECRET)))
            with self.assertRaises(OSError):
                self.run_capture(port)
            self.assertNotIn(b"END", port.tx)

    def test_bad_parameters_do_not_touch_port(self):
        for timeout in (0, -1, 0.09, 61, True, float("nan"), float("inf"), "10"):
            port = FakePort()
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port, timeout=timeout)
            self.assertEqual(port.reset_calls, 0)
            self.assertFalse(port.tx)

    def test_noise_between_every_fragment(self):
        for size in (1, 7, 16):
            port = FakePort(response=lambda data: framed(wire(data), chunk=size,
                            noise=SECRET.encode() + b"\n\xffinvalid UTF-8 log\n"))
            self.assertEqual(self.run_capture(port), exported())
            self.assert_only_capture_commands(port)

    def test_legacy_unframed_export_is_never_success(self):
        port = FakePort(response=wire)
        with self.assertRaises(capture.CaptureError):
            self.run_capture(port)

    def test_chunk_crc_offset_order_duplicates_and_injection(self):
        valid = framed(wire(exported())).splitlines(keepends=True)
        corrupt = valid[0][:10] + (b"0" if valid[0][10:11] != b"0" else b"1") + valid[0][11:]
        alternatives = [corrupt + b"".join(valid[1:]), b"".join(valid[1:]),
                        valid[0] + b"".join(valid), valid[1] + valid[0] + b"".join(valid[2:]),
                        fragment(1, b"[maint-export] ") + b"".join(valid),
                        fragment(0, b"injected\n") + b"".join(valid),
                        fragment(0, b"injected\nextra") + b"".join(valid),
                        valid[0] + fragment(0, b"replay") + b"".join(valid[1:])]
        for data in alternatives:
            port = FakePort(response=lambda _value: data)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port)
            self.assert_only_capture_commands(port)

    def test_malformed_frame_lines_rejected(self):
        good = fragment(0, b"abc")
        cases = [b"[mx]\n", b"[mx]garbage\n", good.rstrip(b"\n") + b" \n",
                 good.replace(b"[mx] ", b"[mx]  "), good[:-1] + b"\r\n",
                 b"[mx] 0000:ffff:\n", b"[mx] 0000:ffff:0\n", b"[mx] 0000:ffff:gg\n",
                 b"[mx] 0000:FFFF:aa\n", b"[mx] 0000:ffff:AA\n",
                 b"[mx] 000A:ffff:aa\n", b"[mx] 00000:ffff:aa\n",
                 fragment(0, b"a" * 17), good + b"\0"]
        for data in cases:
            port = FakePort(response=lambda _value: data)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port, timeout=0.5)

    def test_partial_framed_stream_and_frame_before_begin(self):
        frames = framed(wire(exported())).splitlines(keepends=True)
        for data in (b"".join(frames[:-1]), b"".join(frames)[:-1]):
            port = FakePort(response=lambda _value: data)
            with self.assertRaises(capture.CaptureError):
                self.run_capture(port, timeout=0.5)
        port = FakePort(begin=frames[0] + b"[maint] active\n")
        with self.assertRaises(capture.CaptureError):
            self.run_capture(port)
        self.assertEqual(port.commands, [b"MAINT BEGIN"])

    def test_framed_reassembly_bound(self):
        port = FakePort(response=lambda _data: framed(b"a" * (capture.MAX_LINE_BYTES + 1)))
        with self.assertRaisesRegex(capture.CaptureError, "Reassembled"):
            self.run_capture(port)
        fragments = capture._Fragments()
        data = b"a" * (capture.MAX_LINE_BYTES - 1) + b"\n"
        complete = None
        for line in framed(data).splitlines():
            complete = fragments.feed(line)
        self.assertEqual(complete, data)

    def test_crc_includes_offset_length_and_payload(self):
        self.assertEqual(capture.binascii.crc_hqx(b"123456789", 0xffff), 0x29b1)
        value = fragment(0, b"abc")
        for bad in (value.replace(b"0000:", b"0001:"), value[:-3] + b"64\n",
                    value[:-3] + b"\n"):
            with self.assertRaises(capture.CaptureError):
                capture._Fragments().feed(bad.rstrip(b"\n"))


@unittest.skipUnless(os.name == "posix", "0600 hard-link publication requires POSIX")
class OutputTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.path = self.directory / "capture.json"

    def assert_clean(self):
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_private_json_with_permissive_umask(self):
        previous = os.umask(0)
        try:
            capture.write_capture(self.path, exported())
        finally:
            os.umask(previous)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertEqual(json.loads(self.path.read_bytes()), exported())
        self.assertEqual(list(self.directory.iterdir()), [self.path])

    def test_existing_file_directory_and_symlink_never_replaced(self):
        for kind in ("file", "directory", "symlink"):
            if kind == "file":
                self.path.write_bytes(b"existing")
            elif kind == "directory":
                self.path.mkdir()
            else:
                self.path.symlink_to(self.directory / "absent")
            with self.assertRaises(FileExistsError):
                capture.write_capture(self.path, exported())
            if kind == "file":
                self.assertEqual(self.path.read_bytes(), b"existing")
            if kind == "directory":
                self.path.rmdir()
            else:
                self.path.unlink()
            self.assert_clean()

    def test_publish_race_never_overwrites_winner(self):
        original = capture.os.link

        def race(*args, **kwargs):
            self.path.write_bytes(b"winner")
            return original(*args, **kwargs)

        with mock.patch.object(capture.os, "link", side_effect=race):
            with self.assertRaises(FileExistsError):
                capture.write_capture(self.path, exported())
        self.assertEqual(self.path.read_bytes(), b"winner")
        self.assertEqual(list(self.directory.iterdir()), [self.path])

    def test_short_file_writes(self):
        original = capture.os.write
        with mock.patch.object(capture.os, "write", side_effect=lambda fd, data: original(fd, data[:3])):
            capture.write_capture(self.path, exported())
        self.assertEqual(json.loads(self.path.read_bytes()), exported())

    def test_write_chmod_sync_and_link_failures_before_publication(self):
        for operation in ("write", "fchmod", "fsync", "link"):
            with mock.patch.object(capture.os, operation, side_effect=OSError(SECRET)):
                with self.assertRaises(OSError):
                    capture.write_capture(self.path, exported())
            self.assert_clean()
        for count in (0, -1, None, True, 100000):
            with mock.patch.object(capture.os, "write", return_value=count):
                with self.assertRaises(capture.CaptureError):
                    capture.write_capture(self.path, exported())
            self.assert_clean()

    def test_second_write_failure_leaves_no_partial_output(self):
        original = capture.os.write
        calls = 0

        def fail_after_partial(fd, data):
            nonlocal calls
            calls += 1
            if calls == 1:
                return original(fd, data[:5])
            raise OSError(SECRET)

        with mock.patch.object(capture.os, "write", side_effect=fail_after_partial):
            with self.assertRaises(OSError):
                capture.write_capture(self.path, exported())
        self.assert_clean()

    def test_private_complete_inode_synced_before_link_and_directory_after(self):
        original_link, original_sync = capture.os.link, capture.os.fsync
        steps = []

        def sync(fd):
            steps.append("directory" if stat.S_ISDIR(os.fstat(fd).st_mode) else "file")
            original_sync(fd)

        def link(source, destination, **kwargs):
            self.assertEqual(steps, ["file"])
            info = os.stat(source, dir_fd=kwargs["src_dir_fd"])
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
            self.assertEqual(json.loads((self.directory / source).read_bytes()), exported())
            self.assertEqual(kwargs["src_dir_fd"], kwargs["dst_dir_fd"])
            self.assertFalse(kwargs["follow_symlinks"])
            steps.append("link")
            original_link(source, destination, **kwargs)

        with mock.patch.object(capture.os, "fsync", side_effect=sync), mock.patch.object(capture.os, "link", side_effect=link):
            capture.write_capture(self.path, exported())
        self.assertEqual(steps, ["file", "link", "directory"])

    def test_directory_sync_failure_leaves_complete_output(self):
        original = capture.os.fsync

        def sync(fd):
            if stat.S_ISDIR(os.fstat(fd).st_mode):
                raise OSError(SECRET)
            original(fd)

        with mock.patch.object(capture.os, "fsync", side_effect=sync):
            with self.assertRaises(OSError):
                capture.write_capture(self.path, exported())
        self.assertEqual(json.loads(self.path.read_bytes()), exported())
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertEqual(list(self.directory.iterdir()), [self.path])

    def test_temporary_name_collisions_are_bounded_and_never_reused(self):
        name = ".board-export-" + "a" * 32 + ".tmp"
        temporary = self.directory / name
        temporary.symlink_to(self.directory / "absent")
        with mock.patch.object(capture.secrets, "token_hex", return_value="a" * 32) as entropy:
            with self.assertRaises(capture.CaptureError):
                capture.write_capture(self.path, exported())
            self.assertEqual(entropy.call_count, 10)
        self.assertTrue(temporary.is_symlink())
        self.assertFalse(self.path.exists())
        self.assertEqual(list(self.directory.iterdir()), [temporary])

    def test_postlink_cleanup_error_does_not_delete_published_capture(self):
        original = capture.os.unlink
        calls = 0

        def fail_once(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise OSError(SECRET)
            return original(*args, **kwargs)

        with mock.patch.object(capture.os, "unlink", side_effect=fail_once):
            with self.assertRaises(OSError):
                capture.write_capture(self.path, exported())
        self.assertEqual(json.loads(self.path.read_bytes()), exported())
        self.assertEqual(list(self.directory.iterdir()), [self.path])

    def test_invalid_value_path_and_missing_parent(self):
        value = exported()
        value["secret"] = SECRET
        with self.assertRaises(capture.CaptureError):
            capture.write_capture(self.path, value)
        for path in ("-", "", "bad\npath"):
            with self.assertRaises(capture.CaptureError):
                capture.write_capture(path, exported())
        with self.assertRaises(OSError):
            capture.write_capture(self.directory / "absent" / "capture.json", exported())
        self.assert_clean()


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "capture.json"
        self.args = ["--port", "/dev/fake", "--role", "brain", "--device-id", "A", "--output", str(self.path)]

    def run_cli(self, port=None, args=None):
        port = port or FakePort(dirty=SECRET.encode() + b"\n")
        factory = mock.Mock(return_value=port)
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=factory)}), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), \
                mock.patch.object(capture.secrets, "token_bytes", return_value=bytes.fromhex(CHALLENGE)):
            result = capture.main(self.args if args is None else args)
        output = stdout.getvalue() + stderr.getvalue()
        self.assertNotIn(SECRET, output)
        self.assertNotIn(CHALLENGE, output)
        self.assertNotIn(EPOCH, output)
        self.assertNotIn(str(self.path), output)
        self.assertNotIn(BRAIN, output)
        return result, output, factory

    def test_success_no_default_reset_signals_and_metadata_only(self):
        port = FakePort(dirty=SECRET.encode() + b"\n")
        result, output, factory = self.run_cli(port)
        self.assertEqual(result, 0)
        factory.assert_called_once_with(port=None, baudrate=115200, timeout=0.1, write_timeout=0.1,
                                        xonxoff=False, rtscts=False, dsrdtr=False, exclusive=True)
        self.assertTrue(port.opened)
        self.assertTrue(port.closed)
        self.assertEqual(len(port.commands), 2)
        self.assertIn("role=brain, schema=1", output)
        self.assertIn("not full semantic verification", output)
        self.assertEqual(json.loads(self.path.read_bytes()), exported())

    def test_no_port_open_on_existing_output_or_bad_arguments(self):
        self.path.write_bytes(b"existing")
        result, _, factory = self.run_cli()
        self.assertEqual(result, 1)
        factory.assert_not_called()
        self.assertEqual(self.path.read_bytes(), b"existing")
        self.path.unlink()
        for extra in (["--timeout", "nan"], ["--timeout", "inf"], ["--timeout", "0"], ["--timeout", "61"]):
            result, _, factory = self.run_cli(args=self.args + extra)
            self.assertEqual(result, 1)
            factory.assert_not_called()

    def test_serial_failures_close_without_publish_or_raw_errors(self):
        for operation in ("open", "write", "read", "close", "reset_input_buffer"):
            port = FakePort()
            setattr(port, operation, mock.Mock(side_effect=OSError(SECRET)))
            result, _, _ = self.run_cli(port)
            self.assertEqual(result, 1)
            self.assertFalse(self.path.exists())
            if operation != "close":
                self.assertTrue(port.closed)
            self.assertNotIn(b"END", port.tx)

    def test_bad_json_never_published_or_printed(self):
        port = FakePort(response=lambda _data: framed(capture.PREFIX + SECRET.encode() + b"\n"))
        result, _, _ = self.run_cli(port)
        self.assertEqual(result, 1)
        self.assertTrue(port.closed)
        self.assertFalse(self.path.exists())
        self.assertNotIn(b"END", port.tx)

    def test_output_failure_and_interrupt_do_not_release_lock(self):
        port = FakePort()
        with mock.patch.object(capture, "write_capture", side_effect=OSError(SECRET)):
            result, _, _ = self.run_cli(port)
        self.assertEqual(result, 1)
        self.assertTrue(port.closed)
        self.assertNotIn(b"END", port.tx)
        port = FakePort()
        port.read = mock.Mock(side_effect=KeyboardInterrupt)
        result, _, _ = self.run_cli(port)
        self.assertEqual(result, 130)
        self.assertTrue(port.closed)
        self.assertNotIn(b"END", port.tx)

    def test_missing_pyserial_fails_sanitized(self):
        stderr = io.StringIO()
        with mock.patch.dict(sys.modules, {"serial": None}), contextlib.redirect_stderr(stderr):
            self.assertEqual(capture.main(self.args), 1)
        self.assertFalse(self.path.exists())
        self.assertNotIn(str(self.path), stderr.getvalue())

    def test_argument_errors_do_not_echo_values(self):
        for args in (["--unknown", SECRET], self.args + ["--timeout", SECRET],
                     self.args + ["--role", SECRET], self.args + ["--port", SECRET]):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as error:
                capture.main(args)
            self.assertEqual(error.exception.code, 2)
            self.assertNotIn(SECRET, stderr.getvalue())

    def test_import_is_safe_without_pyserial_or_trusting_module_search_path(self):
        script = '''
import importlib.util, sys, types
sys.modules['serial'] = None
sys.modules['prepare_board_pairing'] = types.SimpleNamespace(decode_record=None)
spec = importlib.util.spec_from_file_location('capture_test', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert callable(module._pairing.decode_record)
assert sys.modules['serial'] is None
'''
        result = subprocess.run([sys.executable, "-B", "-c", script, str(TOOL)],
                                capture_output=True, timeout=5, check=False)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_help_documents_limits_and_scope_without_serial_dependency(self):
        output = io.StringIO()
        with mock.patch.dict(sys.modules, {"serial": None}), contextlib.redirect_stdout(output), \
                self.assertRaises(SystemExit) as error:
            capture.main(["--help"])
        self.assertEqual(error.exception.code, 0)
        for text in ("RTS/DTR", "reset-free", "migration", "0600", "semantically", "disabled", "0.1..60"):
            self.assertIn(text, output.getvalue())


if __name__ == "__main__":
    unittest.main()
