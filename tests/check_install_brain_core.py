"""Host tool against the compiled production USB/installer C++ fixture.

Invoked by test_brain_installer.py for each SHA SDK after compilation. Pipe I/O,
role probe, NVS/MAC/time and UART boundaries are substitutes. This does not run
Arduino main, real pyserial, credentials, a broker, Flash or physical activation.
"""
import importlib.util
import os
from pathlib import Path
import select
import subprocess
import sys


SPEC = importlib.util.spec_from_file_location("install_brain_core_tool", Path(__file__).parents[1] / "tools/install_brain.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class PipePort:
    def __init__(self, process):
        self.process = process
        self.timeout = self.write_timeout = 0.1
        self.commands = bytearray()

    def reset_input_buffer(self):
        # New fixture process has no stale host receive data.
        pass

    def write(self, data):
        chunk = data[:7]
        self.commands.extend(chunk)
        self.process.stdin.write(chunk)
        self.process.stdin.flush()
        return len(chunk)

    def read(self, size):
        if select.select([self.process.stdout], [], [], self.timeout)[0]:
            return os.read(self.process.stdout.fileno(), min(size, 9))
        return b""


def main(binary):
    for kind in range(4):
        process = subprocess.Popen([binary, "--usb-tool-fixture", str(kind)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        port = PipePort(process)
        try:
            try:
                result = tool.install(port, "Babytech_01-test", handoff_confirmed=True, timeout=15)
            except tool.InstallError as error:
                if kind != 3 or "legacy_event_pending" not in str(error):
                    raise
            else:
                if kind == 3 or result != tool.Status("persisted_restart_required", "activation_pending", True):
                    raise AssertionError("Host tool falsely accepted the fixture outcome")
            process.stdin.close()
            process.wait(timeout=10)
            errors = process.stderr.read()
            if process.returncode:
                raise AssertionError("Production installer fixture failed: " + errors.decode(errors="replace"))
            command = b"PAIR INSTALL Babytech_01-test HANDOFF_CONFIRMED\n"
            if port.commands.count(command) != 1 or b"MAINT END\n" not in port.commands:
                raise AssertionError("Expected one start and maintenance release")
            if any(word in port.commands for word in (b"FINALIZE", b"REBOOT", b"RESET", b"ERASE")):
                raise AssertionError("Forbidden lifecycle command sent")
            print("Single Brain USB tool / production core fixture " + str(kind) + ": passed", flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            for stream in (process.stdin, process.stdout, process.stderr):
                if not stream.closed:
                    stream.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
