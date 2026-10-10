"""Run real BrainNetworkConsole and MaintenanceLineReader host tests in C++11.

Only the network collaborator is faked, recording calls with injected results.
No third-party dependencies, PIO, git, hardware, broker or network operations.
This does not validate BrainNetwork/CloudLink storage, RTOS or real USB timing.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true",
                        help="Enable AddressSanitizer and UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++11 compiler is required (set CXX to its executable)")
    root = Path(__file__).resolve().parents[1]
    environment = os.environ.copy()
    command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
               "-pedantic-errors", "-Wconversion", "-Wsign-conversion"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g"]
        for key, options in (
            ("ASAN_OPTIONS", "halt_on_error=1:abort_on_error=1"),
            ("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1"),
        ):
            environment[key] = ":".join(filter(None, (environment.get(key), options)))
    command += ["-I", str(root / "main-controller/src"),
                "-I", str(root / "shared/ProductBoardLink/src"),
                str(root / "tests/test_brain_network_console.cpp")]
    with tempfile.TemporaryDirectory(prefix="babytech-brain-network-console-") as directory:
        binary = Path(directory) / ("brain_network_console.exe" if os.name == "nt"
                                    else "brain_network_console")
        print("C++11 strict warnings" + (" + ASan/UBSan" if args.sanitize else ""), flush=True)
        subprocess.run([*command, "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
