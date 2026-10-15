#!/usr/bin/env python3
"""Capture a read-only MAINT EXPORT diagnostic over explicitly selected USB.

Usage: capture_board_export.py --port /dev/tty... --role brain|motion
       --device-id ID --output NEW.json [--timeout 10]

Requires pyserial only when running the CLI, at 115200 baud. No credentials
are requested/read, and raw serial logs are never printed. The only commands
sent are MAINT BEGIN and MAINT EXPORT. No END, reset, reboot, install or NVS
write is performed, even on failure: maintenance is deliberately left active
if BEGIN succeeded. A failure before acknowledgement cannot prove lock state.
RTS/DTR are deasserted BEFORE opening, with flow control disabled. OS drivers
and USB adapters can still glitch these lines on open/close and reset a board;
this tool cannot guarantee reset-free operation or a lock surviving a reset.

Accepts ONLY CRC16-protected [mx] fragments, contiguous from offset zero,
reassembling one [maint-export] JSON line. Old unframed exports are refused.
Limits: 16384 bytes per line/reassembled export, 65536 total received bytes,
8192 I/O iterations, and one aggregate serial timeout (default 10, max 60 s).

Schema/identity/CRC checks detect corruption, NOT authenticity or migration
eligibility. State bodies are NOT fully semantically verified. Legacy context
checks cover canonical size/schema/flag/length layout and device identity only,
not UTF-8, recipe ranges, digests, semantic validity or installation readiness.
Firmware product startup and installation remain disabled.

Output can contain child/profile data: use a trusted private POSIX directory.
A complete 0600 JSON file is published exclusively using a hard link and fsync;
existing entries (including symlinks) are never replaced. A failure after link
may leave a complete output, and a crash may leave a private temporary file.
No raw response/error text or filesystem paths are printed by the CLI.
"""

import argparse
import binascii
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import secrets
import struct
import sys
import time
import zlib


# Load this trusted sibling explicitly, never a same-named module on sys.path.
# It imports only the standard library and its CLI is guarded by __main__.
_spec = importlib.util.spec_from_file_location(
    "_capture_pairing_codec", Path(__file__).resolve().with_name("prepare_board_pairing.py"))
_pairing = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_pairing)

MAX_LINE_BYTES = 16384
MAX_INPUT_BYTES = 65536
MAX_IO_CALLS = 8192
READ_SIZE = 256
DEFAULT_TIMEOUT = 10.0
MAX_TIMEOUT = 60.0
PREFIX = b"[maint-export] "
FRAGMENT = re.compile(rb"\[mx\] ([0-9a-f]{4}):([0-9a-f]{4}):((?:[0-9a-f]{2}){1,16})")
FIELDS = frozenset((
    "schema", "role", "device_id", "physical_id", "boot", "challenge", "captured_ms",
    "pair_status", "state_status", "legacy_status", "legacy_event",
    "pair_hex", "state_hex", "legacy_hex",
))
PAIR_STATUSES = frozenset(("ready", "missing", "corrupt", "io_error", "identity_mismatch"))
STATE_STATUSES = PAIR_STATUSES | {"conflict"}
LEGACY_STATUSES = frozenset(("ready", "missing", "corrupt", "io_error"))
EVENT_STATUSES = frozenset(("present", "missing", "corrupt", "io_error"))
# Production BrainStateRecord.h / MotionStateRecord.h / ProductContext.h.
STATE_LIMITS = {"brain": 1491, "motion": 4073}
SCOPE_NOTICE = "Diagnostic only; not full semantic verification or migration eligibility."
LOCK_NOTICE = "No maintenance release/reset sent; verify lock manually if the board resets."


class CaptureError(ValueError):
    """All messages are fixed diagnostics, never untrusted device data."""


def _require(condition, message):
    if not condition:
        raise CaptureError(message)


def _identity(role, device_id):
    _require(type(role) is str and role in STATE_LIMITS, "Invalid role.")
    _require(type(device_id) is str and
             re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", device_id), "Invalid device ID.")


def _nonzero_hex(value, length):
    _require(type(value) is str and re.fullmatch(r"[0-9a-f]{%d}" % length, value)
             and value != "0" * length, "Invalid nonzero hex identity.")


def _timeout(value):
    _require(type(value) in (int, float) and math.isfinite(value) and 0.1 <= value <= MAX_TIMEOUT,
             "Timeout must be finite and between 0.1 and 60 seconds.")
    return float(value)


def _blob(value, status, maximum):
    _require(type(value) is str, "Blob must be a hex string.")
    if status != "ready":
        _require(value == "", "Non-ready blob must be empty.")
        return b""
    _require(0 < len(value) <= maximum * 2 and len(value) % 2 == 0 and
             re.fullmatch(r"[0-9a-f]+", value), "Invalid blob hex or length.")
    return bytes.fromhex(value)


def _decode_pair(record, role, device_id, physical_id):
    try:
        decoded = _pairing.decode_record(record)
    except _pairing.ValidationError as exc:
        raise CaptureError("Invalid pairing envelope or identity.") from exc
    _require(decoded["role"] == (1 if role == "brain" else 2) and
             decoded["device_id"] == device_id and
             decoded["local_physical_id"] == physical_id, "Pairing identity mismatch.")
    return decoded


def _state_pair(record, role, device_id, physical_id):
    _require(len(record) >= 14, "Truncated state envelope.")
    magic, schema, length, crc = struct.unpack("<4sHHI", record[:12])
    allowed = (b"BBS1",) if role == "brain" else (b"BMS1", b"BMS2")
    _require(magic in allowed and schema == 1 and
             length == len(record) - 12, "Invalid state envelope.")
    _require(magic != b"BMS1" or len(record) <= 1728, "Invalid legacy state size.")
    _require(crc == zlib.crc32(record[:8] + record[12:]), "Invalid state CRC.")
    pair_length = struct.unpack_from("<H", record, 12)[0]
    # At least the fixed, empty-context/empty-result tail must follow the pair.
    minimum_tail = 10 if role == "brain" else (21 if magic == b"BMS2" else 20)
    _require(71 <= pair_length <= 134 and len(record) >= 14 + pair_length + minimum_tail,
             "Invalid embedded pairing length or truncated state.")
    return _decode_pair(record[14:14 + pair_length], role, device_id, physical_id)


def _legacy_layout(record, device_id):
    """Structural checks ONLY; deliberately not a ProductContext validator."""
    _require(9 <= len(record) <= 981 and record[0] == 1 and record[1] in (0, 1),
             "Invalid legacy schema, flag or size.")
    at = 2

    def text(maximum):
        nonlocal at
        _require(at + 2 <= len(record), "Truncated legacy text length.")
        length = struct.unpack_from("<H", record, at)[0]
        at += 2
        _require(length <= maximum and at + length <= len(record), "Invalid legacy text size.")
        result = record[at:at + length]
        at += length
        return result

    _require(text(64) == device_id.encode("ascii"), "Legacy device identity mismatch.")
    at += 4  # Version is structurally a uint32; no version/recipe semantics here.
    if not record[1]:
        for maximum in (96, 320, 480):
            text(maximum)
        at += 7  # water u16, temperature u8, ratio f32
    _require(at == len(record), "Invalid legacy canonical layout.")


def validate_export(value, *, role, device_id, challenge):
    """Validate exact export schema and transport evidence, not migration safety."""
    _identity(role, device_id)
    _nonzero_hex(challenge, 32)
    _require(type(value) is dict and value.keys() == FIELDS, "Invalid export fields.")
    _require(type(value["schema"]) is int and value["schema"] == 1, "Invalid export schema.")
    _require(value["role"] == role and value["device_id"] == device_id and
             value["challenge"] == challenge, "Export role/device/challenge mismatch.")
    _nonzero_hex(value["physical_id"], 12)
    _nonzero_hex(value["boot"], 16)
    _require(type(value["captured_ms"]) is int and 0 <= value["captured_ms"] <= 0xffffffff,
             "Invalid capture time.")
    for field, allowed in (
        ("pair_status", PAIR_STATUSES), ("state_status", STATE_STATUSES),
        ("legacy_status", LEGACY_STATUSES if role == "motion" else {"not_applicable"}),
        ("legacy_event", EVENT_STATUSES if role == "motion" else {"not_applicable"}),
    ):
        _require(type(value[field]) is str and value[field] in allowed, "Invalid export status.")
    pair = _blob(value["pair_hex"], value["pair_status"], 134)
    state = _blob(value["state_hex"], value["state_status"], STATE_LIMITS[role])
    legacy = _blob(value["legacy_hex"], value["legacy_status"], 981)
    decoded = _decode_pair(pair, role, device_id, value["physical_id"]) if pair else None
    if state:
        embedded = _state_pair(state, role, device_id, value["physical_id"])
        _require(decoded is None or decoded == embedded, "State and pairing records conflict.")
    if legacy:
        _legacy_layout(legacy, device_id)
    return value


def parse_export(line, *, role, device_id, challenge):
    _require(type(line) is bytes and len(line) <= MAX_LINE_BYTES and line.startswith(PREFIX),
             "Invalid export line.")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            _require(key not in result, "Duplicate JSON key.")
            result[key] = value
        return result

    def invalid_constant(_value):
        raise CaptureError("Non-finite JSON constant.")

    try:
        value = json.loads(line[len(PREFIX):].decode("utf-8"), object_pairs_hook=unique,
                           parse_constant=invalid_constant)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise CaptureError("Invalid export JSON.") from exc
    return validate_export(value, role=role, device_id=device_id, challenge=challenge)


class _Fragments:
    """CRC16-CCITT(init ffff, poly 1021) over offset u16 LE, length u8, payload.

    Only contiguous framed bytes can complete a capture. Ordinary log lines
    are handled by the caller and are never added to the reassembled JSON.
    """
    def __init__(self):
        self.data = bytearray()

    def feed(self, line):
        match = FRAGMENT.fullmatch(line)
        _require(match is not None, "Malformed export fragment.")
        offset = int(match[1], 16)
        expected_crc = int(match[2], 16)
        payload = bytes.fromhex(match[3].decode("ascii"))
        _require(offset == len(self.data), "Noncontiguous export fragment.")
        _require(offset + len(payload) <= MAX_LINE_BYTES, "Reassembled export exceeds limit.")
        protected = struct.pack("<HB", offset, len(payload)) + payload
        _require(binascii.crc_hqx(protected, 0xffff) == expected_crc, "Invalid export fragment CRC.")
        _require(b"\n" not in payload[:-1], "Injected line inside export fragment.")
        self.data.extend(payload)
        return bytes(self.data) if payload.endswith(b"\n") else None


class _Transport:
    def __init__(self, port, timeout, clock):
        self.port = port
        self.clock = clock
        self.deadline = clock() + timeout
        self.calls = 0
        self.received = 0
        self.buffer = bytearray()

    def remaining(self):
        remaining = self.deadline - self.clock()
        self.calls += 1
        _require(remaining > 0 and self.calls <= MAX_IO_CALLS, "Capture deadline or I/O budget exceeded.")
        return min(0.1, remaining)

    def send(self, command):
        remaining = command
        while remaining:
            self.port.write_timeout = self.remaining()
            count = self.port.write(remaining)
            _require(type(count) is int and 0 < count <= len(remaining), "Serial write made invalid progress.")
            remaining = remaining[count:]

    def line(self):
        while True:
            budget = self.remaining()
            end = self.buffer.find(b"\n")
            if end >= 0:
                _require(end <= MAX_LINE_BYTES, "Serial line exceeds limit.")
                line = bytes(self.buffer[:end])
                del self.buffer[:end + 1]
                return line
            _require(len(self.buffer) <= MAX_LINE_BYTES, "Serial line exceeds limit.")
            self.port.timeout = budget
            chunk = self.port.read(READ_SIZE)
            _require(type(chunk) is bytes and len(chunk) <= READ_SIZE, "Invalid serial read.")
            self.received += len(chunk)
            _require(self.received <= MAX_INPUT_BYTES, "Serial input exceeds aggregate limit.")
            self.buffer.extend(chunk)


def capture_export(port, *, role, device_id, timeout=DEFAULT_TIMEOUT, clock=time.monotonic):
    """Use an already-open byte port. No release/reset/flush/close on any path.

    The caller must supply a port honoring read/write timeouts (as pyserial
    does). The aggregate deadline also includes short writes and dirty lines.
    Discard only the HOST receive buffer before BEGIN to avoid stale replies.
    """
    _identity(role, device_id)
    transport = _Transport(port, _timeout(timeout), clock)
    port.reset_input_buffer()
    transport.send(b"MAINT BEGIN\n")
    while True:
        line = transport.line()
        if line.removesuffix(b"\r") == b"[maint] active":
            break
        _require(not line.startswith((b"[maint]", b"[maint-export]", b"[mx]")),
                 "Board did not acknowledge maintenance active.")
    # Bounded even if the entropy provider is broken or replaced by a test fake.
    for _ in range(8):
        challenge = secrets.token_bytes(16).hex()
        if challenge != "0" * 32:
            break
    else:
        raise CaptureError("Could not generate a nonzero challenge.")
    _nonzero_hex(challenge, 32)
    transport.send(f"MAINT EXPORT {device_id} {challenge}\n".encode("ascii"))
    fragments = _Fragments()
    while True:
        line = transport.line()
        if line.startswith(b"[mx]"):
            complete = fragments.feed(line)
            if complete is not None:
                return parse_export(complete, role=role, device_id=device_id, challenge=challenge)
        _require(not line.startswith((b"[maint]", b"[maint-export]")),
                 "Board refused export or sent an unsupported unframed response.")


def _output_path(path):
    value = os.fspath(path)
    _require(type(value) is str and value and value != "-" and
             not any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value), "Invalid output path.")
    _require(os.name == "posix", "Private exclusive publishing requires POSIX.")
    return Path(value)


def write_capture(path, value):
    """Publish validated JSON only, not a serial transcript; never overwrite."""
    path = _output_path(path)
    _require(type(value) is dict and value.keys() == FIELDS, "Invalid export fields.")
    validate_export(value, role=value["role"], device_id=value["device_id"], challenge=value["challenge"])
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("ascii")
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    temporary = None
    fd = None
    try:
        try:
            os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise FileExistsError("Output already exists.")
        for _ in range(10):
            candidate = ".board-export-" + secrets.token_hex(16) + ".tmp"
            if candidate == path.name:
                continue
            try:
                fd = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=directory)
            except FileExistsError:
                continue
            temporary = candidate
            break
        _require(fd is not None, "Could not allocate private temporary output.")
        os.fchmod(fd, 0o600)
        remaining = memoryview(data)
        while remaining:
            count = os.write(fd, remaining)
            _require(type(count) is int and 0 < count <= len(remaining), "Output write made invalid progress.")
            remaining = remaining[count:]
        os.fsync(fd)
        os.close(fd)
        fd = None
        os.link(temporary, path.name, src_dir_fd=directory, dst_dir_fd=directory, follow_symlinks=False)
        os.unlink(temporary, dir_fd=directory)
        temporary = None
        os.fsync(directory)
    finally:
        try:
            if fd is not None:
                os.close(fd)
            if temporary is not None:
                os.unlink(temporary, dir_fd=directory)
        finally:
            os.close(directory)


class _Parser(argparse.ArgumentParser):
    def error(self, _message):
        self.exit(2, "Invalid arguments; use --help.\n")


class _Once(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error("Duplicate option.")
        setattr(namespace, self.dest, values)


def main(argv=None):
    parser = _Parser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                     allow_abbrev=False)
    for name in ("port", "role", "device-id", "output"):
        parser.add_argument("--" + name, required=True, action=_Once)
    parser.add_argument("--timeout", type=float, action=_Once, help="aggregate serial timeout: 0.1..60 seconds")
    args = parser.parse_args(argv)
    try:
        _identity(args.role, args.device_id)
        timeout = _timeout(DEFAULT_TIMEOUT if args.timeout is None else args.timeout)
        path = _output_path(args.output)
        _require(args.port and not any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in args.port),
                 "Invalid explicit serial port.")
        if os.path.lexists(path):
            raise FileExistsError("Output already exists.")
        # Optional dependency: importing this module and all fake-port tests need no pyserial.
        import serial

        port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=0.1,
                             xonxoff=False, rtscts=False, dsrdtr=False, exclusive=True)
        try:
            port.rts = False
            port.dtr = False
            port.port = args.port
            port.open()
            value = capture_export(port, role=args.role, device_id=args.device_id, timeout=timeout)
        finally:
            port.close()
        write_capture(path, value)
        print(f"Capture saved: role={args.role}, schema=1. {SCOPE_NOTICE} {LOCK_NOTICE}")
        return 0
    except (CaptureError, OSError, ImportError):
        # Even serial/OS exception messages can contain raw logs or sensitive paths.
        print("Capture failed (arguments, dependency, serial data or output I/O). "
              "A complete output may exist; existing files were not replaced. " + LOCK_NOTICE,
              file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Capture interrupted. " + LOCK_NOTICE, file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
