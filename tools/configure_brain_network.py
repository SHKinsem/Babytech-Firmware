#!/usr/bin/env python3
"""Configure Brain v4 Wi-Fi or MQTT over explicitly selected local USB.

Usage: python3 tools/configure_brain_network.py --port /dev/cu... wifi|mqtt
Credentials are prompted, never accepted as arguments, saved in files, or
printed. Requires pyserial only for the CLI. Hex on USB is NOT encryption;
use a trusted computer/cable and close other serial monitors. This writes
network settings, not pairing, product state, ACLs, or Motion settings.
MQTT requires an already paired/running Brain network worker. Saved means
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
    pass


def hidden_password(prompt):
    # getpass otherwise falls back to echoed stdin when terminal control fails.
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        try:
            return getpass.getpass(prompt)
        except getpass.GetPassWarning:
            raise ConfigureError("Terminal cannot hide password input.") from None


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
    if not isinstance(command, bytes) or len(command) > 767 or command.count(b"\n") != 1 or not command.endswith(b"\n"):
        raise ConfigureError("Invalid command.")
    if command.startswith(b"NET WIFI "):
        expected = b"[network] wifi_saved"
    elif command.startswith(b"NET MQTT "):
        expected = b"[network] mqtt_saved"
    else:
        raise ConfigureError("Invalid command.")
    transport = _capture._Transport(port, _capture._timeout(timeout), clock)

    def exchange(data, expected_lines):
        for offset in range(0, len(data), 64):
            transport.send(data[offset:offset + 64])
            if offset + 64 < len(data):
                sleep(0.01)
        while True:
            line = transport.line().removesuffix(b"\r")
            if line in expected_lines:
                return line
            if line.startswith((b"[network]", b"[maint]")):
                raise ConfigureError("Board rejected the operation.")

    port.reset_input_buffer()
    role = exchange(b"NET STATUS\n", (b"[network] brain_ready", b"[network] brain_unpaired"))
    if role == b"[network] brain_unpaired" and expected == b"[network] mqtt_saved":
        raise ConfigureError("Pair Brain before configuring MQTT.")
    exchange(b"MAINT BEGIN\n", (b"[maint] active",))
    saved = False
    try:
        exchange(command, (expected,))
        saved = True
    finally:
        # Configuration only: no commissioning/product-state import took place.
        # Give release its own bounded deadline after a failed save/timeout.
        transport.deadline = clock() + 3
        try:
            exchange(b"MAINT END\n", (b"[maint] inactive",))
        except (ConfigureError, _capture.CaptureError, OSError):
            raise ConfigureError("Maintenance release unconfirmed; use MAINT END locally.") from None
    return saved


class _Parser(argparse.ArgumentParser):
    def error(self, _message):
        self.exit(2, "Invalid arguments; use --help. Credentials must be entered interactively.\n")


def main(argv=None):
    parser = _Parser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--port", required=True)
    parser.add_argument("mode", choices=("wifi", "mqtt"))
    args = parser.parse_args(argv)
    try:
        if not sys.stdin.isatty():
            raise ConfigureError("Use an interactive terminal for hidden credential input.")
        if args.mode == "wifi":
            command = wifi_command(input("Wi-Fi SSID: "), hidden_password("Wi-Fi password (empty for open network): "))
        else:
            host = input("MQTT host: ")
            port = int(input("MQTT port [1883]: ") or "1883")
            user = input("MQTT username: ")
            command = mqtt_command(host, port, user, hidden_password("MQTT password: "))
        import serial
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=0.1,
                                   xonxoff=False, rtscts=False, dsrdtr=False, exclusive=True)
        try:
            connection.rts = False
            connection.dtr = False
            connection.port = args.port
            connection.open()
            configure(connection, command)
        finally:
            connection.close()
        print("Settings saved and maintenance released. Verify Wi-Fi/MQTT connection separately.")
        return 0
    except (ValueError, OSError, ImportError, EOFError):
        print("Configuration failed; some settings may already be saved. Check pairing/USB, "
              "retry configuration, and use MAINT END if the local UI remains locked.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; verify settings and use MAINT END if needed.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
