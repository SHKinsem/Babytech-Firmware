"""Fake USB tests; no dependency installation, hardware, or credentials."""
import importlib.util
import contextlib
import io
from pathlib import Path
import unittest
from unittest import mock
import warnings

SPEC = importlib.util.spec_from_file_location("configure_brain_network", Path(__file__).parents[1] / "tools/configure_brain_network.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class Port:
    def __init__(self, ready=True, fail=False, release=True):
        self.writes = bytearray()
        self.pending = bytearray()
        self.lines = bytearray()
        self.ready, self.fail, self.release = ready, fail, release

    def reset_input_buffer(self):
        self.lines.clear()

    def write(self, data):
        data = data[:7]  # Exercise short writes.
        self.writes.extend(data)
        self.pending.extend(data)
        if self.pending.endswith(b"\n"):
            command = bytes(self.pending)
            self.pending.clear()
            if command == b"NET STATUS\n":
                reply = b"[network] brain_ready" if self.ready else b"[network] brain_unpaired"
            elif command == b"MAINT BEGIN\n":
                reply = b"[maint] active"
            elif command == b"MAINT END\n":
                reply = b"[maint] inactive" if self.release else b"[maint] unsafe"
            elif self.fail:
                reply = b"[network] wifi_failed"
            else:
                reply = b"[network] wifi_saved" if command.startswith(b"NET WIFI") else b"[network] mqtt_saved"
            self.lines.extend(b"unrelated private log\n" + reply + b"\n")
        return len(data)

    def read(self, size):
        chunk = bytes(self.lines[:min(size, 9)])
        del self.lines[:len(chunk)]
        return chunk


class ConfigureTest(unittest.TestCase):
    def test_mistaken_secret_argument_not_echoed(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output), self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/test", "mqtt", "--password", "private-value"])
        self.assertNotIn("private-value", output.getvalue())

    def test_requires_interactive_terminal(self):
        with mock.patch.object(tool.sys.stdin, "isatty", return_value=False), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(tool.main(["--port", "/dev/test", "wifi"]), 1)

    def test_password_never_falls_back_to_echo(self):
        def unsupported(_prompt):
            warnings.warn("echo fallback", tool.getpass.GetPassWarning)
            self.fail("fallback was allowed to read a password")
        with mock.patch.object(tool.getpass, "getpass", side_effect=unsupported):
            with self.assertRaises(tool.ConfigureError):
                tool.hidden_password("Password: ")

    def test_password_hidden_input(self):
        with mock.patch.object(tool.getpass, "getpass", return_value="private"):
            self.assertEqual(tool.hidden_password("Password: "), "private")

    def test_wifi_utf8_spaces_and_open(self):
        self.assertEqual(tool.wifi_command("奶机 home", ""), b"NET WIFI " + "奶机 home".encode().hex().encode() + b" -\n")

    def test_maximum_commands(self):
        for command in (tool.wifi_command("s" * 32, "a" * 64), tool.mqtt_command("h" * 127, 65535, "u" * 63, "p" * 127)):
            self.assertLess(len(command), 768)
            self.assertTrue(tool.configure(Port(), command, sleep=lambda _: None))

    def test_save_and_release(self):
        port = Port()
        command = tool.wifi_command("Home", "secret123")
        self.assertTrue(tool.configure(port, command, sleep=lambda _: None))
        self.assertEqual(port.writes, b"NET STATUS\nMAINT BEGIN\n" + command + b"MAINT END\n")

    def test_failed_save_releases_without_claiming_success(self):
        port = Port(fail=True)
        with self.assertRaises(tool.ConfigureError):
            tool.configure(port, tool.wifi_command("Home", "secret123"))
        self.assertTrue(port.writes.endswith(b"MAINT END\n"))

    def test_failed_release_reported(self):
        with self.assertRaisesRegex(tool.ConfigureError, "release unconfirmed"):
            tool.configure(Port(release=False), tool.wifi_command("Home", ""))

    def test_unpaired_mqtt_does_not_send_secret(self):
        port = Port(ready=False)
        with self.assertRaises(tool.ConfigureError):
            tool.configure(port, tool.mqtt_command("localhost", 1883, "user", "private"))
        self.assertEqual(port.writes, b"NET STATUS\n")

    def test_unpaired_wifi_can_be_saved(self):
        self.assertTrue(tool.configure(Port(ready=False), tool.wifi_command("Home", "")))

    def test_validation_before_io(self):
        for ssid, password in (("", ""), ("s" * 33, ""), ("a\n", ""), ("a", "short"), ("a", "z" * 64), ("a", "p" * 65)):
            with self.subTest(ssid_length=len(ssid), password_length=len(password)), self.assertRaises(tool.ConfigureError):
                tool.wifi_command(ssid, password)
        for host, port, user, password in (("", 1883, "u", "p"), ("url/path", 1883, "u", "p"), ("host", 0, "u", "p"), ("host", 65536, "u", "p"), ("host", 1883, "", "p"), ("host", 1883, "u", "\x00")):
            with self.assertRaises(tool.ConfigureError):
                tool.mqtt_command(host, port, user, password)

    def test_no_injected_command(self):
        port = Port()
        for command in (b"NET WIFI a b\nMAINT END\n", b"OTA CODE\n", b"NET WIFI " + b"a" * 800 + b"\n"):
            with self.assertRaises(tool.ConfigureError):
                tool.configure(port, command)
        self.assertFalse(port.writes)


if __name__ == "__main__":
    unittest.main()
