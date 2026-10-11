#!/usr/bin/env python3
"""Prepare or check an OFFLINE pairing manifest, never credentials or NVS.

generate --output FILE --device-id ID --brain-physical-id HEX12
         --motion-physical-id HEX12
generate --output FILE --stdin  (JSON containing exactly those three identities,
                               with underscore field names)
check --input FILE              (use '-' to read a manifest from stdin)

Schema 1 has exactly: schema, device_id, pairing_epoch, brain_physical_id,
motion_physical_id, brain_record_hex, motion_record_hex. Hex is lowercase.
Physical IDs are raw ESP_MAC_WIFI_STA bytes, not colon-delimited MAC addresses.
CRC detects corruption, not authenticity. No hardware identity is discovered.
Only generate for a NEW controlled pairing; resume a migration by checking and
reusing its original manifest, never by generating another epoch at a new path.
Preparing/checking a manifest does NOT migrate boards or authorize startup.
"""

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import stat
import struct
import sys
import zlib


MAX_JSON_BYTES = 16 * 1024
IDENTITY_FIELDS = frozenset(("device_id", "brain_physical_id", "motion_physical_id"))
MANIFEST_FIELDS = IDENTITY_FIELDS | {
    "schema", "pairing_epoch", "brain_record_hex", "motion_record_hex",
}
REUSE_NOTICE = (
    "For an existing/interrupted migration, check and reuse the original "
    "manifest; do not retry generate or choose a new path to generate a new epoch."
)
SCOPE_NOTICE = "Offline manifest only: no NVS writes, migration or startup approval."


class ValidationError(ValueError):
    """Invalid data; diagnostics deliberately do not echo untrusted values."""


def _device_id(value):
    if type(value) is not str or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", value):
        raise ValidationError("device_id must be 1..64 ASCII bytes: [A-Za-z0-9][A-Za-z0-9_-]*")


def _nonzero_hex(value, length, field):
    if (type(value) is not str or not re.fullmatch(r"[0-9a-f]{%d}" % length, value)
            or value == "0" * length):
        raise ValidationError(f"{field} must be exactly {length} lowercase nonzero hex digits")


def _identities(device_id, brain_physical_id, motion_physical_id):
    _device_id(device_id)
    _nonzero_hex(brain_physical_id, 12, "brain_physical_id")
    _nonzero_hex(motion_physical_id, 12, "motion_physical_id")
    if brain_physical_id == motion_physical_id:
        raise ValidationError("physical IDs must differ")


def _fields(value, expected):
    if type(value) is not dict or value.keys() != expected:
        raise ValidationError("JSON object has missing or unknown fields")


def encode_record(role, device_id, pairing_epoch, local_physical_id, peer_physical_id):
    """Return exact production BTP1 bytes (role: integer 1=Brain, 2=Motion)."""
    if type(role) is not int or role not in (1, 2):
        raise ValidationError("role must be integer 1 or 2")
    _identities(device_id, local_physical_id, peer_physical_id)
    _nonzero_hex(pairing_epoch, 32, "pairing_epoch")
    payload = bytes((role, len(device_id))) + (
        pairing_epoch + local_physical_id + peer_physical_id + device_id
    ).encode("ascii")
    header = struct.pack("<4sHH", b"BTP1", 1, len(payload))
    return header + struct.pack("<I", zlib.crc32(header + payload)) + payload


def decode_record(record):
    """Validate exact length, header, CRC and identities; return decoded fields."""
    if type(record) is not bytes or not 71 <= len(record) <= 134:
        raise ValidationError("invalid BTP1 length/type")
    magic, schema, length, crc = struct.unpack("<4sHHI", record[:12])
    if magic != b"BTP1" or schema != 1 or length != len(record) - 12:
        raise ValidationError("invalid BTP1 header")
    if crc != zlib.crc32(record[:8] + record[12:]):
        raise ValidationError("invalid BTP1 CRC")
    if not 1 <= record[13] <= 64 or len(record) != 70 + record[13]:
        raise ValidationError("invalid BTP1 device length")
    try:
        result = dict(role=record[12], device_id=record[70:].decode("ascii"),
                      pairing_epoch=record[14:46].decode("ascii"),
                      local_physical_id=record[46:58].decode("ascii"),
                      peer_physical_id=record[58:70].decode("ascii"))
    except UnicodeError as exc:
        raise ValidationError("non-ASCII BTP1 identity") from exc
    if encode_record(**result) != record:
        raise ValidationError("noncanonical BTP1 record")
    return result


def create_manifest(device_id, brain_physical_id, motion_physical_id):
    """Create a NEW pairing in memory; not a migration/resume operation."""
    _identities(device_id, brain_physical_id, motion_physical_id)
    epoch = "0" * 32
    while epoch == "0" * 32:
        epoch = secrets.token_bytes(16).hex()
    manifest = dict(schema=1, device_id=device_id, pairing_epoch=epoch,
                    brain_physical_id=brain_physical_id, motion_physical_id=motion_physical_id)
    for name, role, local, peer in (
        ("brain", 1, brain_physical_id, motion_physical_id),
        ("motion", 2, motion_physical_id, brain_physical_id),
    ):
        manifest[name + "_record_hex"] = encode_record(role, device_id, epoch, local, peer).hex()
    return manifest


def validate_manifest(manifest):
    """Validate schema/types and both CRC-protected, mirrored role records."""
    _fields(manifest, MANIFEST_FIELDS)
    if type(manifest["schema"]) is not int or manifest["schema"] != 1:
        raise ValidationError("manifest schema must be integer 1")
    _identities(*(manifest[key] for key in
                  ("device_id", "brain_physical_id", "motion_physical_id")))
    _nonzero_hex(manifest["pairing_epoch"], 32, "pairing_epoch")
    for name, role, peer_name in (("brain", 1, "motion"), ("motion", 2, "brain")):
        encoded = manifest[name + "_record_hex"]
        if (type(encoded) is not str or not 142 <= len(encoded) <= 268
                or not re.fullmatch(r"(?:[0-9a-f]{2})+", encoded)):
            raise ValidationError("record hex must encode 71..134 bytes in lowercase")
        decoded = decode_record(bytes.fromhex(encoded))
        expected = dict(role=role, device_id=manifest["device_id"],
                        pairing_epoch=manifest["pairing_epoch"],
                        local_physical_id=manifest[name + "_physical_id"],
                        peer_physical_id=manifest[peer_name + "_physical_id"])
        if decoded != expected:
            raise ValidationError("record role/device/epoch/physical IDs do not match manifest")
    return manifest


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError("duplicate JSON key")
        result[key] = value
    return result


def _invalid_constant(_value):
    raise ValidationError("non-finite JSON number")


def read_json(stream):
    """Read a strict UTF-8 JSON object, at most 16 KiB, from a binary stream."""
    data = stream.read(MAX_JSON_BYTES + 1)
    if len(data) > MAX_JSON_BYTES:
        raise ValidationError("JSON exceeds 16 KiB limit")
    try:
        result = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object,
                            parse_constant=_invalid_constant)
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise ValidationError("invalid JSON: UTF-8, unique keys and finite values required") from exc
    if type(result) is not dict:
        raise ValidationError("JSON root must be an object")
    return result


def read_manifest(path):
    """Check a bounded regular file (no symlink or special-device reads)."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ValidationError("manifest input must be a regular file")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            return validate_manifest(read_json(stream))
    finally:
        os.close(fd)


def _output_path(path):
    value = os.fspath(path)
    if (type(value) is not str or not value or value == "-"
            or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value)):
        raise ValidationError("--output must name a filesystem path without control characters")
    return Path(value)


def write_manifest(path, manifest):
    """Publish 0600 bytes exclusively on POSIX; never replace a directory entry.

    The temporary inode is written and fsynced before an exclusive hard link
    publishes it. Directory operations share an open dirfd to avoid parent-path
    races. Failure before link leaves no output; failure after link may leave a
    COMPLETE manifest, which must be checked/reused, never regenerated. A crash
    can leave a private temporary file. No rename/replace or rollback unlink of
    the destination is used. Use a trusted output directory on a filesystem
    supporting hard links/fsync; this is not protection from directory owners.
    """
    path = _output_path(path)
    data = (json.dumps(validate_manifest(manifest), indent=2, sort_keys=True) + "\n").encode("ascii")
    if os.name != "posix":
        raise ValidationError("exclusive 0600 publishing requires POSIX")
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    temporary = None
    fd = None
    try:
        try:
            os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise FileExistsError("output already exists; " + REUSE_NOTICE)
        for _ in range(10):
            candidate = ".board-pairing-" + secrets.token_hex(16) + ".tmp"
            if candidate == path.name:
                continue
            try:
                fd = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                             0o600, dir_fd=directory)
            except FileExistsError:
                continue
            temporary = candidate
            break
        if fd is None:
            raise FileExistsError("could not allocate private temporary file")
        os.fchmod(fd, 0o600)
        remaining = memoryview(data)
        while remaining:
            count = os.write(fd, remaining)
            if count <= 0:
                raise OSError("manifest write made no progress")
            remaining = remaining[count:]
        os.fsync(fd)
        os.close(fd)
        fd = None
        os.link(temporary, path.name, src_dir_fd=directory, dst_dir_fd=directory,
                follow_symlinks=False)
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
        # argparse's default errors can echo unknown flags/credential values.
        self.exit(2, "Invalid arguments; use --help. " + REUSE_NOTICE + "\n")


class _Once(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error("duplicate option")
        setattr(namespace, self.dest, True if self.nargs == 0 else values)


def main(argv=None):
    parser = _Parser(description=__doc__, allow_abbrev=False)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("generate", allow_abbrev=False,
                                   description="NEW pairing only. " + REUSE_NOTICE)
    generate.add_argument("--output", required=True, action=_Once)
    generate.add_argument("--stdin", nargs=0, action=_Once,
                          help="read exactly three explicit identities as JSON")
    for field in ("device-id", "brain-physical-id", "motion-physical-id"):
        generate.add_argument("--" + field, action=_Once)
    check = commands.add_parser("check", allow_abbrev=False, description=SCOPE_NOTICE)
    check.add_argument("--input", required=True, action=_Once)
    args = parser.parse_args(argv)
    try:
        if args.command == "check":
            if args.input == "-":
                validate_manifest(read_json(sys.stdin.buffer))
            else:
                read_manifest(args.input)
            print("Manifest valid. " + SCOPE_NOTICE + " " + REUSE_NOTICE)
            return 0
        supplied = {key: getattr(args, key) for key in IDENTITY_FIELDS}
        if args.stdin:
            if any(value is not None for value in supplied.values()):
                raise ValidationError("--stdin cannot be mixed with CLI identities")
            supplied = read_json(sys.stdin.buffer)
            _fields(supplied, IDENTITY_FIELDS)
        elif any(value is None for value in supplied.values()):
            raise ValidationError("all three explicit identities are required")
        path = _output_path(args.output)
        # Refuse an existing migration before even asking the random generator.
        if os.path.lexists(path):
            raise FileExistsError("output already exists")
        write_manifest(path, create_manifest(**supplied))
        print("Manifest created. " + SCOPE_NOTICE + " " + REUSE_NOTICE)
        return 0
    except (ValidationError, OSError) as exc:
        detail = str(exc) if isinstance(exc, ValidationError) else (
            "Filesystem operation failed; no existing output was replaced. "
            "A complete output may exist: check it before proceeding."
        )
        print(detail + " " + REUSE_NOTICE, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
