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
        while b"\n" in self.pending:
            end = self.pending.index(b"\n") + 1
            command = bytes(self.pending[:end])
            del self.pending[:end]
            if command == b"\n":
                continue
            if command == b"NET STATUS\n":
                reply = b"[network] brain_ready" if self.ready else b"[network] brain_unpaired"
            elif command == b"MAINT BEGIN\n":
                reply = b"[maint] active"
            elif command == b"MAINT END\n":
                reply = b"[maint] inactive" if self.release else b"[maint] unsafe"
            elif command == b"NET DIAG\n":
                reply = b"[network] diag wifi=3 cfg=1 mqtt=1 state=0 n=1 ok=2 fail=0"
            elif self.fail:
                reply = b"[network] wifi_failed" if command.startswith(b"NET WIFI") else b"[network] mqtt_failed"
            else:
                reply = b"[network] wifi_saved" if command.startswith(b"NET WIFI") else b"[network] mqtt_saved"
            self.lines.extend(b"unrelated private log\n" + reply + b"\n")
        return len(data)

    def read(self, size):
        chunk = bytes(self.lines[:min(size, 9)])
        del self.lines[:len(chunk)]
        return chunk


class ConfigureTest(unittest.TestCase):
    def test_visible_password_requires_explicit_selection(self):
        with mock.patch.object(tool, "hidden_password", return_value="private") as hidden, mock.patch("builtins.input", return_value="visible") as visible:
            self.assertEqual(tool.read_password("Password: "), "private")
            hidden.assert_called_once()
            visible.assert_not_called()
            self.assertEqual(tool.read_password("Password: ", show=True), "visible")
            visible.assert_called_once()

    def test_failed_save_reports_stage_without_secret(self):
        with self.assertRaisesRegex(tool.ConfigureError, "configuration save") as failure:
            tool.configure(Port(fail=True), tool.mqtt_command("localhost", 1883, "user", "private"))
        self.assertNotIn("private", str(failure.exception))

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
        self.assertEqual(port.writes, b"NET STATUS\nMAINT BEGIN\n" + command + b"\nMAINT END\n")

    def test_failed_save_releases_without_claiming_success(self):
        port = Port(fail=True)
        with self.assertRaises(tool.ConfigureError):
            tool.configure(port, tool.wifi_command("Home", "secret123"))
        self.assertTrue(port.writes.endswith(b"MAINT END\n"))

    def test_failed_release_reported(self):
        with self.assertRaisesRegex(tool.ConfigureError, "release unconfirmed"):
            tool.configure(Port(release=False), tool.wifi_command("Home", ""))

    def test_unpaired_mqtt_saved_in_maintenance_without_echo(self):
        port = Port(ready=False)
        command = tool.mqtt_command("localhost", 1883, "user", "private")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            self.assertTrue(tool.configure(port, command, sleep=lambda _: None))
        self.assertEqual(port.writes, b"NET STATUS\nMAINT BEGIN\n" + command + b"\nMAINT END\n")
        self.assertEqual(output.getvalue(), "")

    def test_unpaired_mqtt_failed_save_releases_and_retries_without_echo(self):
        port = Port(ready=False, fail=True)
        command = tool.mqtt_command("localhost", 1883, "user", "private")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            with self.assertRaises(tool.ConfigureError) as failure:
                tool.configure(port, command, sleep=lambda _: None)
            self.assertNotIn("private", str(failure.exception))
            self.assertNotIn("70726976617465", str(failure.exception))
            self.assertTrue(port.writes.endswith(b"MAINT END\n"))
            port.fail = False
            self.assertTrue(tool.configure(port, command, sleep=lambda _: None))
        self.assertEqual(port.writes, (b"NET STATUS\nMAINT BEGIN\n" + command + b"\nMAINT END\n") * 2)
        self.assertEqual(output.getvalue(), "")

    def test_read_only_check(self):
        port = Port()
        result = tool.inspect_network(port)
        self.assertEqual(result["diagnostics"]["mqtt"], 1)
        self.assertEqual(port.writes, b"NET STATUS\nNET DIAG\n")

    def test_saved_state_survives_release_failure(self):
        with self.assertRaises(tool.ConfigureError) as failure:
            tool.configure(Port(release=False), tool.wifi_command("Home", ""))
        self.assertTrue(failure.exception.saved)
        self.assertFalse(failure.exception.released)
        self.assertIn("Settings saved", str(failure.exception))

    def test_lost_begin_ack_still_releases_without_saving(self):
        port = Port()
        original_read = port.read
        def read(size):
            port.lines[:] = port.lines.replace(b"[maint] active\n", b"lost\n")
            return original_read(size)
        port.read = read
        with self.assertRaises(tool.ConfigureError) as failure:
            tool.configure(port, tool.wifi_command("Home", ""))
        self.assertTrue(failure.exception.released)
        self.assertNotIn(b"NET WIFI", port.writes)
        self.assertIn(b"MAINT END\n", port.writes)

    def test_save_ack_lost_does_not_retry_write(self):
        port = Port()
        original_read = port.read
        def read(size):
            port.lines[:] = port.lines.replace(b"[network] wifi_saved\n", b"lost\n")
            return original_read(size)
        port.read = read
        with self.assertRaises(tool.ConfigureError) as failure:
            tool.configure(port, tool.wifi_command("Home", ""))
        self.assertFalse(failure.exception.saved)
        self.assertTrue(failure.exception.released)
        self.assertEqual(port.writes.count(b"NET WIFI"), 1)

    def test_save_and_release_failures_both_reported(self):
        with self.assertRaises(tool.ConfigureError) as error:
            tool.configure(Port(fail=True, release=False), tool.wifi_command("Home", ""))
        self.assertIn("configuration save", str(error.exception))
        self.assertIn("release unconfirmed", str(error.exception))
        self.assertFalse(error.exception.saved)

    def test_malformed_diagnostics_never_echoed(self):
        port = Port()
        original_read = port.read
        def read(size):
            port.lines[:] = port.lines.replace(b"wifi=3 cfg=1 mqtt=1 state=0 n=1 ok=2 fail=0", b"test-secret")
            return original_read(size)
        port.read = read
        with self.assertRaises((tool.ConfigureError, ValueError)) as error:
            tool.inspect_network(port)
        self.assertNotIn("test-secret", str(error.exception))

    def test_stale_reply_does_not_reject_next_stage(self):
        port = Port()
        original_read = port.read
        injected = False
        def read(size):
            nonlocal injected
            if not injected and b"[maint] active\n" in port.lines:
                port.lines[:] = port.lines.replace(b"[maint] active\n", b"[network] brain_ready\n[maint] active\n")
                injected = True
            return original_read(size)
        port.read = read
        self.assertTrue(tool.configure(port, tool.wifi_command("Home", "")))

    def test_unpaired_mqtt_failed_release_reports_uncertainty_without_secret(self):
        port = Port(ready=False, release=False)
        with self.assertRaisesRegex(tool.ConfigureError, "release unconfirmed") as failure:
            tool.configure(port, tool.mqtt_command("localhost", 1883, "user", "private"), sleep=lambda _: None)
        self.assertTrue(port.writes.endswith(b"MAINT END\n"))
        self.assertNotIn("private", str(failure.exception))

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
