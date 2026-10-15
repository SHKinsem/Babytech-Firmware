"""Deterministic fake USB tests; no model, credentials, device or pyserial needed."""
import contextlib
import importlib.util
import io
from pathlib import Path
import types
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("install_brain", Path(__file__).parents[1] / "tools/install_brain.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)
DEVICE = "bt-184DCE6E27AC"


def status(state="idle", reason="none", writes=0):
    return f"[install] {state} reason={reason} writes_may_have_persisted={writes}".encode()


DONE = status("persisted_restart_required", "activation_pending", 1)


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        self.now += 0.001
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class Port:
    def __init__(self, clock, *, before=None, stages=None, loss=None, release=True):
        self.clock = clock
        self.before = status() if before is None else before
        self.stages = list(stages if stages is not None else (
            status("discovering"), status("reading"), status("installing", writes=1),
            status("verifying", writes=1), DONE))
        self.loss, self.release = loss, release
        self.writes, self.pending, self.lines = bytearray(), bytearray(), bytearray()
        self.commands = []
        self.started, self.active, self.cancelled = False, False, False
        self.fail_network = False
        self.interrupt = False
        self.rts = self.dtr = True
        self.closed, self.opened = False, False

    def reset_input_buffer(self):
        self.lines.clear()

    def write(self, data):
        data = data[:7]
        self.writes.extend(data)
        self.pending.extend(data)
        while b"\n" in self.pending:
            at = self.pending.index(b"\n")
            command = bytes(self.pending[:at])
            del self.pending[:at + 1]
            self.commands.append(command)
            if command == b"NET STATUS":
                reply = b"[network] brain_unpaired"
            elif command == b"PAIR INSTALL STATUS":
                if self.interrupt and self.started:
                    self.interrupt = False
                    raise KeyboardInterrupt()
                reply = self.stages.pop(0) if self.started and self.stages else self.before
            elif command == b"MAINT BEGIN":
                self.active = True
                reply = b"[maint] active"
            elif command == b"MAINT END":
                self.active = not self.release
                reply = b"[maint] inactive" if self.release else b"[maint] unsafe"
            elif command == b"PAIR INSTALL CANCEL":
                self.cancelled = True
                reply = status("failed", "cancelled_outcome_unknown", int(self.started))
            elif command.startswith(b"PAIR INSTALL "):
                valid = command == f"PAIR INSTALL {DEVICE} HANDOFF_CONFIRMED".encode()
                self.started |= valid
                reply = b"[install] started" if valid else b"[install] not_started"
            elif command.startswith(b"NET WIFI "):
                reply = b"[network] wifi_failed" if self.fail_network else b"[network] wifi_saved"
            elif command.startswith(b"NET MQTT "):
                reply = b"[network] mqtt_failed" if self.fail_network else b"[network] mqtt_saved"
            else:
                reply = b"[maint] unknown_command"
            if command != self.loss:
                self.lines.extend(b"private-secret raw log\n" + reply + b"\r\n")
        return len(data)

    def read(self, size):
        self.clock.sleep(0.01)
        chunk = bytes(self.lines[:min(size, 9)])
        del self.lines[:len(chunk)]
        return chunk

    def open(self):
        assert self.rts is False and self.dtr is False
        self.opened = True

    def close(self):
        self.closed = True


class InstallTest(unittest.TestCase):
    def run_install(self, port, **kwargs):
        return tool.install(port, DEVICE, handoff_confirmed=True, clock=port.clock,
                            sleep=port.clock.sleep, **kwargs)

    def test_success_is_durable_only_one_start_one_usb(self):
        port = Port(Clock())
        result = self.run_install(port)
        self.assertEqual(result, tool.Status("persisted_restart_required", "activation_pending", True))
        self.assertEqual(port.commands.count(f"PAIR INSTALL {DEVICE} HANDOFF_CONFIRMED".encode()), 1)
        self.assertEqual(port.commands[:3], [b"NET STATUS", b"PAIR INSTALL STATUS", b"MAINT BEGIN"])
        self.assertFalse(port.active)
        self.assertFalse(port.cancelled)
        for command in port.commands:
            self.assertFalse(any(word in command for word in (b"FINALIZE", b"REBOOT", b"RESET", b"ERASE", b"EXPORT", b"MOVE")))

    def test_optional_network_settings_same_attempt_and_maintenance(self):
        port = Port(Clock())
        wifi = tool._network.wifi_command("Home", "secret123")
        mqtt = tool._network.mqtt_command("localhost", 1883, "brain", "private-password")
        self.run_install(port, network_commands=(wifi, mqtt))
        self.assertEqual(port.commands[3:5], [wifi.strip(), mqtt.strip()])
        self.assertEqual(port.commands.count(b"MAINT BEGIN"), 1)
        self.assertEqual(port.commands.count(b"MAINT END"), 1)

    def test_already_running_never_cancelled_or_released(self):
        for state in ("discovering", "reserving", "reading", "installing", "verifying"):
            with self.subTest(state=state):
                port = Port(Clock(), before=status(state))
                with self.assertRaisesRegex(tool.InstallError, "already running"):
                    self.run_install(port)
                self.assertEqual(port.commands, [b"NET STATUS", b"PAIR INSTALL STATUS"])

    def test_previous_persisted_not_reinstalled_or_claimed_for_new_device(self):
        port = Port(Clock(), before=DONE)
        with self.assertRaisesRegex(tool.InstallError, "previous installation"):
            self.run_install(port)
        self.assertFalse(port.started)

    def test_failed_previous_attempt_can_be_explicitly_retried_once(self):
        port = Port(Clock(), before=status("failed", "motion_write_outcome_unknown", 1))
        self.run_install(port)
        self.assertTrue(port.started)

    def test_rejected_start_never_retried(self):
        port = Port(Clock())
        original_write = port.write
        def reject(data):
            result = original_write(data)
            port.lines[:] = port.lines.replace(b"[install] started", b"[install] not_started")
            return result
        port.write = reject
        with self.assertRaises(tool.InstallError):
            self.run_install(port)
        self.assertEqual(sum(c.startswith(b"PAIR INSTALL " + DEVICE.encode()) for c in port.commands), 1)
        self.assertTrue(port.cancelled)
        self.assertFalse(port.active)

    def test_failed_write_keeps_uncertainty_and_releases(self):
        port = Port(Clock(), stages=[status("failed", "motion_write_outcome_unknown", 1)])
        with self.assertRaisesRegex(tool.InstallError, "motion_write_outcome_unknown"):
            self.run_install(port)
        self.assertTrue(port.cancelled)
        self.assertFalse(port.active)

    def test_start_ack_lost_cancel_not_replay(self):
        command = f"PAIR INSTALL {DEVICE} HANDOFF_CONFIRMED".encode()
        port = Port(Clock(), loss=command)
        with self.assertRaises(ValueError):
            self.run_install(port, timeout=2)
        self.assertTrue(port.started and port.cancelled)
        self.assertEqual(port.commands.count(command), 1)
        self.assertFalse(port.active)

    def test_begin_ack_lost_still_releases_without_start(self):
        port = Port(Clock(), loss=b"MAINT BEGIN")
        with self.assertRaises(ValueError):
            self.run_install(port, timeout=2)
        self.assertFalse(port.started or port.cancelled or port.active)
        self.assertIn(b"MAINT END", port.commands)

    def test_io_budget_exhaustion_does_not_exhaust_cleanup_budget(self):
        port = Port(Clock())
        original_write = port.write
        def lost_status(data):
            result = original_write(data)
            if port.started and port.commands[-1] == b"PAIR INSTALL STATUS":
                port.lines.clear()
            return result
        port.write = lost_status
        with self.assertRaisesRegex(ValueError, "I/O budget"):
            tool.install(port, DEVICE, handoff_confirmed=True, clock=lambda: 0, sleep=lambda _: None)
        self.assertTrue(port.cancelled)
        self.assertFalse(port.active)
        self.assertEqual(port.commands.count(f"PAIR INSTALL {DEVICE} HANDOFF_CONFIRMED".encode()), 1)

    def test_partial_start_write_exception_or_zero_progress_is_not_replayed(self):
        for zero in (False, True):
            port = Port(Clock())
            original_write = port.write
            triggered = False
            def broken_write(data):
                nonlocal triggered
                if not triggered and port.pending.startswith(b"PAIR INSTALL " + DEVICE[:5].encode()):
                    triggered = True
                    if zero:
                        return 0
                    raise OSError("private-secret serial error")
                return original_write(data)
            port.write = broken_write
            with self.subTest(zero=zero), self.assertRaises((ValueError, OSError)):
                self.run_install(port)
            self.assertTrue(triggered and port.cancelled)
            self.assertFalse(port.active or port.started)
            self.assertEqual(sum(c.startswith(b"PAIR INSTALL " + DEVICE[:5].encode()) for c in port.commands), 1)

    def test_lost_cancel_reply_still_attempts_end_and_never_success(self):
        port = Port(Clock(), stages=[status("failed", "motion_write_outcome_unknown", 1)], loss=b"PAIR INSTALL CANCEL")
        with self.assertRaisesRegex(tool.InstallError, "motion_write_outcome_unknown"):
            self.run_install(port)
        self.assertTrue(port.cancelled)
        self.assertIn(b"MAINT END", port.commands)
        self.assertFalse(port.active)
        self.assertLess(port.clock.now, 7)

    def test_lost_end_reply_reports_release_uncertain_not_success(self):
        port = Port(Clock(), loss=b"MAINT END")
        with self.assertRaisesRegex(tool.InstallError, "release unconfirmed"):
            self.run_install(port)
        self.assertFalse(port.cancelled)
        self.assertLess(port.clock.now, 7)

    def test_wifi_saved_mqtt_failed_or_ack_lost_does_not_start_or_roll_back(self):
        wifi = tool._network.wifi_command("Home", "secret123")
        mqtt = tool._network.mqtt_command("localhost", 1883, "brain", "private-password")
        for lost in (False, True):
            port = Port(Clock(), loss=mqtt.strip() if lost else None)
            original_write = port.write
            def failed_mqtt(data):
                result = original_write(data)
                if not lost:
                    port.lines[:] = port.lines.replace(b"[network] mqtt_saved", b"[network] mqtt_failed")
                return result
            port.write = failed_mqtt
            with self.subTest(lost=lost), self.assertRaises(ValueError):
                self.run_install(port, network_commands=(wifi, mqtt), timeout=3)
            self.assertEqual(port.commands.count(wifi.strip()), 1)
            self.assertEqual(port.commands.count(mqtt.strip()), 1)
            self.assertFalse(port.started or port.cancelled or port.active)

    def test_keyboard_interrupt_attempt_cancelled_and_released(self):
        port = Port(Clock())
        port.interrupt = True
        with self.assertRaises(KeyboardInterrupt):
            self.run_install(port)
        self.assertTrue(port.cancelled)
        self.assertFalse(port.active)

    def test_failed_network_save_never_starts_and_releases(self):
        port = Port(Clock())
        port.fail_network = True
        with self.assertRaises(tool.InstallError):
            self.run_install(port, network_commands=(tool._network.wifi_command("Home", ""),))
        self.assertFalse(port.started or port.cancelled or port.active)

    def test_release_failure_does_not_claim_active_or_complete(self):
        port = Port(Clock(), release=False)
        with self.assertRaisesRegex(tool.InstallError, "release unconfirmed"):
            self.run_install(port)
        self.assertTrue(port.started)
        self.assertFalse(port.cancelled)

    def test_invalid_inputs_no_serial_io(self):
        for device in ("", "-bad", "a\nMAINT END", "a" * 65, "你好"):
            port = Port(Clock())
            with self.subTest(device=device), self.assertRaises(ValueError):
                tool.install(port, device, handoff_confirmed=True)
            self.assertFalse(port.writes)
        for timeout in (True, 0, float("nan"), 61):
            port = Port(Clock())
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                self.run_install(port, timeout=timeout)
            self.assertFalse(port.writes)
        port = Port(Clock())
        with self.assertRaises(tool.InstallError):
            tool.install(port, DEVICE)
        self.assertFalse(port.writes)

    def test_injected_or_duplicate_configuration_no_io(self):
        wifi = tool._network.wifi_command("Home", "")
        for commands in ((b"NET WIFI a b\nMAINT END\n",), (b"NET WIFI a b\rMAINT END\n",),
                         (b"RESET\n",), (wifi, wifi), (wifi, wifi, wifi)):
            port = Port(Clock())
            with self.subTest(commands=commands), self.assertRaises(ValueError):
                self.run_install(port, network_commands=commands)
            self.assertFalse(port.writes)

    def test_malformed_or_unverified_terminal_status_never_success(self):
        for reply in (status("persisted_restart_required", "none", 1),
                      status("persisted_restart_required", "activation_pending", 0),
                      status("unknown"), status("failed", "private_password"),
                      b"[install] failed reason=none writes_may_have_persisted=2", DONE + b" extra"):
            port = Port(Clock(), stages=[reply])
            with self.subTest(reply=reply), self.assertRaises(tool.InstallError) as error:
                self.run_install(port)
            self.assertNotIn("private_password", str(error.exception))
            self.assertTrue(port.cancelled)

    def test_raw_logs_never_echoed(self):
        port = Port(Clock())
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            self.run_install(port)
        self.assertEqual(output.getvalue(), "")

    def test_cli_requires_terminal_and_never_echoes_mistaken_secret_argument(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output), self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/test", "--device-id", DEVICE, "--password", "private-value"])
        self.assertNotIn("private-value", output.getvalue())
        with mock.patch.object(tool.sys.stdin, "isatty", return_value=False), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(tool.main(["--port", "/dev/test", "--device-id", DEVICE]), 1)

    def test_cli_unconfirmed_does_not_open_usb(self):
        constructor = mock.Mock()
        with mock.patch.object(tool.sys.stdin, "isatty", return_value=True), mock.patch("builtins.input", return_value="no"), \
                mock.patch.dict("sys.modules", {"serial": types.SimpleNamespace(Serial=constructor)}), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(tool.main(["--port", "/dev/test", "--device-id", DEVICE]), 1)
        constructor.assert_not_called()

    def test_cli_success_closes_port_and_requires_manual_whole_power_cycle(self):
        port = Port(Clock(), stages=[DONE])
        output = io.StringIO()
        with mock.patch.object(tool.sys.stdin, "isatty", return_value=True), mock.patch("builtins.input", side_effect=["INSTALL", "n"]), \
                mock.patch.dict("sys.modules", {"serial": types.SimpleNamespace(Serial=mock.Mock(return_value=port))}), \
                contextlib.redirect_stdout(output):
            self.assertEqual(tool.main(["--port", "/dev/test", "--device-id", DEVICE]), 0)
        self.assertTrue(port.opened and port.closed)
        self.assertIn("Activation is still pending", output.getvalue())
        self.assertIn("disconnect all USB power", output.getvalue())
        self.assertTrue(port.started)
        self.assertFalse(port.active)

    def test_cli_open_close_and_install_failures_close_and_do_not_echo(self):
        for failure in ("open", "close", "install", "release", "interrupt"):
            port = Port(Clock(), stages=[DONE], release=failure != "release")
            if failure == "open":
                port.open = mock.Mock(side_effect=OSError("private-secret open error"))
            elif failure == "close":
                def close():
                    port.closed = True
                    raise OSError("private-secret close error")
                port.close = close
            elif failure == "install":
                port.stages = [status("failed", "import_storage_fault", 1)]
            elif failure == "interrupt":
                port.interrupt = True
            out, err = io.StringIO(), io.StringIO()
            with self.subTest(failure=failure), mock.patch.object(tool.sys.stdin, "isatty", return_value=True), \
                    mock.patch("builtins.input", side_effect=["INSTALL", "n"]), \
                    mock.patch.dict("sys.modules", {"serial": types.SimpleNamespace(Serial=mock.Mock(return_value=port))}), \
                    contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(tool.main(["--port", "/dev/test", "--device-id", DEVICE]), 130 if failure == "interrupt" else 1)
            self.assertTrue(port.closed)
            self.assertNotIn("records verified", out.getvalue())
            self.assertNotIn("private-secret", out.getvalue() + err.getvalue())

    def test_cli_actual_network_path_hidden_credentials_and_partial_failure(self):
        for fail in (False, True):
            port = Port(Clock(), stages=[DONE])
            original_write = port.write
            def fail_mqtt(data):
                result = original_write(data)
                if fail:
                    port.lines[:] = port.lines.replace(b"[network] mqtt_saved", b"[network] mqtt_failed")
                return result
            port.write = fail_mqtt
            out, err = io.StringIO(), io.StringIO()
            with self.subTest(fail=fail), mock.patch.object(tool.sys.stdin, "isatty", return_value=True), \
                    mock.patch("builtins.input", side_effect=["INSTALL", "y", "Home", "localhost", "1883", "brain"]), \
                    mock.patch.object(tool._network, "hidden_password", side_effect=["secret123", "private-password"]), \
                    mock.patch.dict("sys.modules", {"serial": types.SimpleNamespace(Serial=mock.Mock(return_value=port))}), \
                    contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(tool.main(["--port", "/dev/test", "--device-id", DEVICE]), int(fail))
            self.assertTrue(port.closed)
            self.assertFalse(port.active)
            self.assertEqual(port.started, not fail)
            for secret in ("secret123", "private-password", "private-secret", "private-password".encode().hex()):
                self.assertNotIn(secret, out.getvalue() + err.getvalue())


if __name__ == "__main__":
    unittest.main()
