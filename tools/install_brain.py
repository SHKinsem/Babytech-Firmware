#!/usr/bin/env python3
"""First installation through one explicitly selected Brain USB port.

Requires compatible v4 boards, trusted UART/USB wiring and a stationary,
authorized bench. The operator must finish the old MQTT credential/session
handoff first; this tool cannot verify or perform that server-side operation.
Optional Wi-Fi/MQTT prompts reuse the existing network command encoders.
No flash, erase, automatic retry, FINALIZE or reset command is sent. Success
means both durable records were verified, NOT that product control is active.
Then manually power-cycle the WHOLE machine once, disconnecting USB as well.
Normal boots and NVS-preserving flashing do not need another installation.
"""
import importlib.util
from pathlib import Path
import re
import sys
import time
from typing import NamedTuple


_spec = importlib.util.spec_from_file_location(
    "_install_brain_network", Path(__file__).with_name("configure_brain_network.py"))
_network = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_network)
_capture = _network._capture

STATES = frozenset(("idle", "discovering", "reserving", "reading", "installing",
                    "verifying", "persisted_restart_required", "failed"))
REASONS = frozenset((
    "none", "activation_pending", "cancelled_outcome_unknown", "maintenance_ended",
    "discovery_unavailable", "reservation_unavailable", "records_unavailable",
    "reservation_lost_outcome_unknown", "motion_write_outcome_unknown",
    "verification_unavailable", "records_invalid", "resources_unavailable",
    "motion_import_unavailable", "motion_verification_failed", "reservation_lost_after_write",
    "existing_records_conflict", "legacy_event_pending", "import_invalid",
    "import_identity_mismatch", "import_conflict", "import_unsafe",
    "import_legacy_event_pending", "import_storage_fault", "import_state_missing",
    "import_result_unknown"))


class InstallError(ValueError):
    pass


class Status(NamedTuple):
    state: str
    reason: str
    writes_may_have_persisted: bool


def parse_status(line):
    match = re.fullmatch(
        rb"\[install\] ([a-z_]+) reason=([a-z_]+) writes_may_have_persisted=([01])", line)
    if match is None:
        raise InstallError("Unsupported installation status; preserve records and inspect locally.")
    state, reason = match[1].decode("ascii"), match[2].decode("ascii")
    if state not in STATES or reason not in REASONS:
        raise InstallError("Unsupported installation status; preserve records and inspect locally.")
    status = Status(state, reason, match[3] == b"1")
    if state == "persisted_restart_required" and (reason != "activation_pending" or not status.writes_may_have_persisted):
        raise InstallError("Incomplete durable verification; do not treat installation as successful.")
    return status


def install(port, device_id, *, handoff_confirmed=False, network_commands=(),
            timeout=30, clock=time.monotonic, sleep=time.sleep):
    """One explicit attempt; status polling never retries an installation write.

    Serial/NVS uncertainty is preserved. Cleanup only cancels this attempt and
    releases transient maintenance; it cannot roll back any durable writes.
    No raw serial log or credential is returned or printed.
    """
    _capture._identity("brain", device_id)
    timeout = _capture._timeout(timeout)
    if handoff_confirmed is not True:
        raise InstallError("One-time MQTT handoff must be confirmed before installation.")
    commands = tuple(network_commands)
    if len(commands) > 2:
        raise InstallError("At most one Wi-Fi and one MQTT configuration may be supplied.")
    expected = []
    for command in commands:
        if not isinstance(command, bytes) or len(command) > 767 or command.count(b"\n") != 1 or not command.endswith(b"\n") or b"\r" in command:
            raise InstallError("Invalid network configuration command.")
        if command.startswith(b"NET WIFI "):
            reply = b"[network] wifi_saved"
        elif command.startswith(b"NET MQTT "):
            reply = b"[network] mqtt_saved"
        else:
            raise InstallError("Invalid network configuration command.")
        if reply in expected:
            raise InstallError("Duplicate network configuration command.")
        expected.append(reply)
    transport = _capture._Transport(port, timeout, clock)

    def send(data):
        for offset in range(0, len(data), 64):
            transport.send(data[offset:offset + 64])
            if offset + 64 < len(data):
                sleep(0.01)

    def receive(prefix, exact=()):
        while True:
            line = transport.line().removesuffix(b"\r")
            if line.startswith(prefix):
                if exact and line not in exact:
                    raise InstallError("Board rejected the operation; preserve existing records.")
                return line
            if line.startswith((b"[maint]", b"[network]", b"[install]", b"[simulation]")):
                raise InstallError("Unexpected board response; installation outcome is unconfirmed.")

    def exchange(data, prefix, exact=()):
        send(data)
        return receive(prefix, exact)

    def read_status():
        return parse_status(exchange(b"PAIR INSTALL STATUS\n", b"[install]"))

    port.reset_input_buffer()
    exchange(b"NET STATUS\n", b"[network]", (b"[network] brain_ready", b"[network] brain_unpaired"))
    before = read_status()
    if before.state == "persisted_restart_required":
        raise InstallError("A previous installation awaits whole-machine power cycling; no new attempt was sent.")
    if before.state not in ("idle", "failed"):
        raise InstallError("An installation is already running; no new attempt or cancellation was sent.")
    begun, attempted, persisted = False, False, False
    try:
        # Set before sending: an acknowledged BEGIN may be lost on USB.
        begun = True
        exchange(b"MAINT BEGIN\n", b"[maint]", (b"[maint] active",))
        for command, reply in zip(commands, expected):
            exchange(command, b"[network]", (reply,))
        attempted = True
        exchange(f"PAIR INSTALL {device_id} HANDOFF_CONFIRMED\n".encode("ascii"),
                 b"[install]", (b"[install] started",))
        while True:
            status = read_status()
            if status.state == "persisted_restart_required":
                persisted = True
                return status
            if status.state == "failed":
                raise InstallError("Installation failed: " + status.reason + ". Preserve records; recovery requires an explicit same-identity attempt.")
            if status.state == "idle":
                raise InstallError("Installation no longer active; durable outcome is unconfirmed.")
            sleep(0.25)
    finally:
        if begun:
            # Terminate a possibly partial input line, never resend it. A
            # separate cleanup budget cannot turn a timeout into success.
            transport = _capture._Transport(port, 3, clock)
            if attempted and not persisted:
                try:
                    send(b"\nPAIR INSTALL CANCEL\n")
                    while True:
                        line = transport.line().removesuffix(b"\r")
                        if line.startswith(b"[install]") and parse_status(line).state in ("idle", "failed", "persisted_restart_required"):
                            break
                except (ValueError, OSError):
                    pass
            transport = _capture._Transport(port, 3, clock)
            try:
                # Old cancellation/status replies may still be queued. Match
                # only END's response; do not publish raw diagnostic lines.
                send(b"\nMAINT END\n")
                while True:
                    line = transport.line().removesuffix(b"\r")
                    if line == b"[maint] inactive":
                        break
                    if line.startswith(b"[maint]") and line not in (b"[maint] unknown_command", b"[maint] active"):
                        raise InstallError("Maintenance release unconfirmed.")
            except (ValueError, OSError):
                raise InstallError("Maintenance release unconfirmed; do not erase or auto-reinstall. Check PAIR INSTALL STATUS and MAINT END locally.") from None


def main(argv=None):
    parser = _network._Parser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--port", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args(argv)
    try:
        _capture._identity("brain", args.device_id)
        _capture._timeout(args.timeout)
        if not sys.stdin.isatty():
            raise InstallError("Use an interactive terminal; passwords must not be passed as arguments.")
        print("Authorized stationary bench only. Stop the old Motion product MQTT session and revoke/replace its credentials first.")
        print("This is a one-time handoff, not a daily check. Existing records are not erased.")
        if input("Type INSTALL to confirm that handoff and install this device: ") != "INSTALL":
            raise InstallError("Not confirmed; nothing was sent to USB.")
        commands = []
        if input("Configure Wi-Fi and MQTT now? [y/N] ").strip().lower() == "y":
            commands.append(_network.wifi_command(input("Wi-Fi SSID: "),
                _network.hidden_password("Wi-Fi password (empty for open network): ")))
            commands.append(_network.mqtt_command(input("MQTT host: "),
                int(input("MQTT port [1883]: ") or "1883"), input("MQTT username: "),
                _network.hidden_password("MQTT password: ")))
        import serial
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=0.1,
                                   xonxoff=False, rtscts=False, dsrdtr=False, exclusive=True)
        try:
            connection.rts = False
            connection.dtr = False
            connection.port = args.port
            connection.open()
            install(connection, args.device_id, handoff_confirmed=True,
                    network_commands=commands, timeout=args.timeout)
        finally:
            connection.close()
        print("Both durable installation records verified; maintenance released. Activation is still pending.")
        print("Manually power off Brain AND Motion, disconnect all USB power, then power the whole machine on once. Keep NVS.")
        print("Do not repeat installation for normal boots or NVS-preserving flashing. Verify UART/MQTT operation separately.")
        return 0
    except InstallError as error:
        print(str(error), file=sys.stderr)
        print("Any settings/records already saved are retained; failure does not mean rollback. No automatic retry or reset was sent.", file=sys.stderr)
    except (ValueError, OSError, ImportError, EOFError):
        print("Installation outcome unconfirmed; settings or records may already be saved. Preserve NVS, inspect PAIR INSTALL STATUS and MAINT END locally, and do not automatically retry or power-cycle to claim success.", file=sys.stderr)
    except KeyboardInterrupt:
        print("Interrupted; records may already be saved. Preserve NVS and inspect PAIR INSTALL STATUS / MAINT END locally.", file=sys.stderr)
        return 130
    return 1


if __name__ == "__main__":
    sys.exit(main())
