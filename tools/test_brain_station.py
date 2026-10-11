"""Run production BrainStation against isolated NVS/WiFi fakes, without PIO.

Reuses SDK header shims only; no CloudLink/USB/Motion sources are linked.
Faults model eager individual-key persistence, not a fictional NVS transaction.
Host tests cannot establish flash power-loss durability or real radio behavior.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in ("tests/fakes/brain_network", "tests/fakes/cloud_link", "main-controller/src"):
        command += ["-I", str(root / include)]
    command += [str(root / source) for source in
                ("main-controller/src/brain_station.cpp", "tests/test_brain_station.cpp")]
    with tempfile.TemporaryDirectory(prefix="babytech-brain-station-") as directory:
        binary = Path(directory) / "brain_station"
        subprocess.run([*command, "-o", str(binary)], check=True, timeout=60)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(binary)], check=True, timeout=20, env=environment)


if __name__ == "__main__":
    main()
