"""Compile and run production maintenance parser/session host tests in C++11.

No PlatformIO, dependencies, hardware, OTA actions or commissioning imports.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
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
    with tempfile.TemporaryDirectory(prefix="babytech-maintenance-console-") as directory:
        binary = Path(directory) / "test_maintenance_console"
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
        command += ["-I", str(root / "shared/ProductBoardLink/src"),
                    str(root / "shared/ProductBoardLink/test/test_maintenance_console.cpp"),
                    "-o", str(binary)]
        subprocess.run(command, check=True)
        print("C++11 strict warnings" + (" + ASan/UBSan" if args.sanitize else ""), flush=True)
        subprocess.run([str(binary)], env=environment, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
