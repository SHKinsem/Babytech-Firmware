"""Host tests of production Brain portal/state, with only SDK I/O faked.

No network, flash, credentials output or hardware acceptance. Existing local
ArduinoJson headers are used; each case runs in a fresh MCU-lifetime process.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

CASES = ["state", "mailbox", "auto", "manual", "wifi", "failed-wifi", "feeding",
         "mqtt", "prefill", "http", "invalid", "dns", "page"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--arduino-json", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    headers = args.arduino_json or root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson headers required; no downloads/builds attempted")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("C++17 compiler required")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-DARDUINO=10819", "-DMBEDTLS_VERSION_MAJOR=3", "-pthread"]
    if sys.platform == "darwin":
        command.append("-Wno-deprecated-declarations")
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command.append(f"-DARDUINOJSON_ENABLE_{feature}=0")
    command.append("-DARDUINOJSON_ENABLE_STD_STRING=1")
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in ("tests/fakes/brain_network", "tests/fakes/cloud_link", "main-controller/src",
                    "shared/BabytechCloudLink/src", "shared/BoardProtocol/src"):
        command += ["-I", str(root / include)]
    command += ["-I", str(headers)]
    for source in ("main-controller/src/brain_portal.cpp", "main-controller/src/brain_station.cpp",
                   "shared/BabytechCloudLink/src/CloudLink.cpp", "shared/BabytechCloudLink/src/CloudSession.cpp",
                   "tests/fakes/cloud_link/FakeCloudIo.cpp", "tests/fakes/brain_network/FakeBrainNvs.cpp",
                   "tests/test_brain_portal.cpp"):
        command.append(str(root / source))
    with tempfile.TemporaryDirectory(prefix="babytech-brain-portal-") as directory:
        binary = Path(directory) / "brain_portal"
        subprocess.run([*command, "-o", str(binary)], check=True, timeout=120)
        environment = os.environ.copy()
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        for case in args.case or CASES:
            subprocess.run([str(binary), case], check=True, timeout=30, env=environment)
    print(f"PASS {len(args.case or CASES)} Brain portal cases (production sources)")


if __name__ == "__main__":
    main()
