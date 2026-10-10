#!/usr/bin/env python3
"""Configure Brain v4 Wi-Fi or MQTT over explicitly selected local USB.

Usage: python3 tools/configure_brain_network.py --port /dev/cu... wifi|mqtt
Credentials are prompted, never accepted as arguments or saved in files.
Passwords are hidden unless --show-password is explicitly selected.
Requires pyserial only for the CLI. Hex on USB is NOT encryption;
use a trusted computer/cable and close other serial monitors. This writes
network settings, not pairing, product state, ACLs, or Motion settings.
MQTT can be saved before pairing; only a paired Brain starts its network worker. Saved means
read-back verified, not authenticated or connected. No reset/flash is sent;
USB adapters/drivers may still reset a board on open. No production commands.
"""
import argparse
import getpass
import importlib.util
from pathlib import Path
import re
import sys
import time
import warnings


_spec = importlib.util.spec_from_file_location(
    "_brain_network_transport", Path(__file__).with_name("capture_board_export.py"))
_capture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_capture)


class ConfigureError(ValueError):
    def __init__(self, message, *, saved=False, released=False):
        super().__init__(message)
        self.saved = saved
        self.released = released


def hidden_password(prompt):
    # getpass otherwise falls back to echoed stdin when terminal control fails.
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        try:
            return getpass.getpass(prompt)
        except getpass.GetPassWarning:
            raise ConfigureError("Terminal cannot hide password input.") from None


def read_password(prompt, show=False):
    return input(prompt) if show else hidden_password(prompt)


def _text(value, maximum, allow_empty=False):
    if not isinstance(value, str):
        raise ConfigureError("Invalid field.")
    encoded = value.encode("utf-8")
    if (not encoded and not allow_empty) or len(encoded) > maximum or any(c < 32 or c == 127 for c in encoded):
        raise ConfigureError("Invalid field length or characters.")
    return encoded.hex() or "-"


def wifi_command(ssid, password):
    ssid_hex = _text(ssid, 32)
    password_hex = _text(password, 64, True)
    size = len(password.encode("utf-8"))
    if (0 < size < 8) or (size == 64 and not re.fullmatch(r"[0-9a-fA-F]{64}", password)):
        raise ConfigureError("Invalid Wi-Fi password length.")
    return f"NET WIFI {ssid_hex} {password_hex}\n".encode("ascii")


def mqtt_command(host, port, user, password):
    host_hex, user_hex, password_hex = _text(host, 127), _text(user, 63), _text(password, 127)
    if not re.fullmatch(r"[a-zA-Z0-9.:-]+", host) or type(port) is not int or not 1 <= port <= 65535:
        raise ConfigureError("Invalid MQTT host or port.")
    return f"NET MQTT {host_hex} {port} {user_hex} {password_hex}\n".encode("ascii")


def configure(port, command, *, timeout=15, clock=time.monotonic, sleep=time.sleep):
    """Write one prepared command. Fake-port testable; never echo serial logs.

    64-byte/10ms pacing matches the firmware's 64-byte/poll bounded reader.
    Best-effort END follows any acknowledged BEGIN, including save failures.
    A timeout cannot prove whether a write persisted; do not claim rollback.
    """
    if not isinstance(command, bytes) or len(command) > 767 or command.count(b"\n") != 1 or not command.endswith(b"\n") or b"\r" in command:
        raise ConfigureError("Invalid command.")
    if command.startswith(b"NET WIFI "):
        expected = b"[network] wifi_saved"
    elif command.startswith(b"NET MQTT "):
        expected = b"[network] mqtt_saved"
    else:
        raise ConfigureError("Invalid command.")
    transport = _capture._Transport(port, _capture._timeout(timeout), clock)

    def exchange(data, expected_lines, stage, rejected=()):
        for offset in range(0, len(data), 64):
            transport.send(data[offset:offset + 64])
            if offset + 64 < len(data):
                sleep(0.01)
        while True:
            try:
                line = transport.line().removesuffix(b"\r")
            except _capture.CaptureError:
                raise ConfigureError(f"No complete reply during {stage}; saved state is unconfirmed.") from None
            if line in expected_lines:
                return line
            if line in rejected:
                raise ConfigureError(f"Board rejected {stage}.")

    port.reset_input_buffer()
    exchange(b"NET STATUS\n", (b"[network] brain_ready", b"[network] brain_unpaired"), "network status check")
    saved = False
    failure = None
    released = False
    try:
        # BEGIN may have taken effect even if its acknowledgement was lost.
        exchange(b"MAINT BEGIN\n", (b"[maint] active",), "maintenance entry",
                 (b"[maint] unsafe", b"[maint] busy"))
        exchange(command, (expected,), "configuration save",
                 (b"[network] wifi_failed", b"[network] mqtt_failed",
                  b"[network] maintenance_required", b"[network] invalid_config"))
        saved = True
    except (ConfigureError, _capture.CaptureError, OSError) as error:
        failure = error
    finally:
        # New budget, preserving buffered replies. END is cleanup, not a retry
        # of an uncertain credential write. Never send the setting twice.
        transport.deadline = clock() + 3
        transport.calls = 0
        try:
            exchange(b"\nMAINT END\n", (b"[maint] inactive",), "maintenance release",
                     (b"[maint] unsafe", b"[maint] busy"))
            released = True
        except (ConfigureError, _capture.CaptureError, OSError):
            pass
    if not released:
        message = ("Settings saved (board read-back confirmed). " if saved else
                   "Settings not confirmed; do not blindly repeat the write. ")
        if isinstance(failure, ConfigureError):
            message += str(failure) + " "
        raise ConfigureError(message + "Maintenance release unconfirmed; use MAINT END locally.",
                             saved=saved) from None
    if failure is not None:
        raise ConfigureError(str(failure) if isinstance(failure, ConfigureError) else
                             "Configuration save unconfirmed; inspect settings before retrying.",
                             released=True) from None
    return saved


def inspect_network(port, *, timeout=5, clock=time.monotonic):
    """Read-only status and diagnostics. Never enters maintenance or saves."""
    transport = _capture._Transport(port, _capture._timeout(timeout), clock)
    result = {}
    port.reset_input_buffer()
    for command, prefix, key in ((b"NET STATUS\n", b"[network] brain_", "status"),
                                 (b"NET DIAG\n", b"[network] diag ", "diagnostics")):
        transport.send(command)
        while True:
            line = transport.line().removesuffix(b"\r")
            if line.startswith(prefix):
                if key == "status":
                    if line not in (b"[network] brain_ready", b"[network] brain_unpaired"):
                        raise ConfigureError("Unsupported network status.")
                    result[key] = line.decode().split()[-1]
                else:
                    try:
                        fields = line[len(prefix):].decode("ascii").split()
                        values = dict(field.split("=", 1) for field in fields)
                        if len(fields) != 7 or set(values) != {"wifi", "cfg", "mqtt", "state", "n", "ok", "fail"}:
                            raise ValueError
                        result[key] = {name: int(value) for name, value in values.items()}
                    except (ValueError, UnicodeError):
                        raise ConfigureError("Unsupported network diagnostics.") from None
                break
    return result


class _Parser(argparse.ArgumentParser):
    def error(self, _message):
        self.exit(2, "Invalid arguments; use --help. Credentials must be entered interactively.\n")


def main(argv=None):
    parser = _Parser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--port", required=True)
    parser.add_argument("--show-password", action="store_true",
                        help="Echo password input locally; keep terminal output private.")
    parser.add_argument("mode", choices=("wifi", "mqtt", "check"))
    args = parser.parse_args(argv)
    try:
        if args.mode != "check" and not sys.stdin.isatty():
            raise ConfigureError("Use an interactive terminal for hidden credential input.")
        if args.show_password:
            print("Warning: password input is visible in this terminal. Do not share screenshots or output.", file=sys.stderr)
        if args.mode == "wifi":
            command = wifi_command(input("Wi-Fi SSID: "), read_password("Wi-Fi password (empty for open network): ", args.show_password))
        elif args.mode == "mqtt":
            host = input("MQTT host: ")
            port = int(input("MQTT port [1883]: ") or "1883")
            user = input("MQTT username: ")
            command = mqtt_command(host, port, user, read_password("MQTT password: ", args.show_password))
        import serial
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=0.1,
                                   xonxoff=False, rtscts=False, dsrdtr=False, exclusive=True)
        try:
            connection.rts = False
            connection.dtr = False
            connection.port = args.port
            connection.open()
            if args.mode == "check":
                import json
                print(json.dumps(inspect_network(connection), sort_keys=True))
                return 0
            configure(connection, command)
        finally:
            connection.close()
        print("Settings saved and maintenance released. Verify Wi-Fi/MQTT connection separately.")
        return 0
    except ConfigureError as error:
        print(str(error), file=sys.stderr)
        if error.released:
            print("Maintenance released. Save was not confirmed; no automatic write retry.", file=sys.stderr)
        return 1
    except (ValueError, OSError, ImportError, EOFError):
        print("Configuration failed; some settings may already be saved. Check pairing/USB, "
              "inspect with check before retrying; use MAINT END if the local UI remains locked.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; verify settings and use MAINT END if needed.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
