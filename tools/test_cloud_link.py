"""Link real CloudLink.cpp/CloudSession.cpp to deterministic host I/O fakes.

No PIO, device, broker, network or git operations. Each case gets a fresh
process for the production MCU-lifetime singleton; the captured task runs until
a test-only exception from vTaskDelay. This tests production control flow, not
real FreeRTOS scheduling, packet allocation, Flash persistence or stack usage.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


CASES = [
    *(f"alloc-{slot}" for slot in range(1, 7)),
    "packet", "task", "ownership", "legacy", "session", "inbound", "outbound",
    *(f"disconnect-{mode}" for mode in ("wifi", "socket", "loop", "settings", "expiry", "request")),
    "send-time-generation", "send-time-expiry", "publish-failure",
    *(f"connect-{mode}" for mode in ("auth", "subscribe-command", "subscribe-config", "random")),
    "routes", "settings-load",
    *(f"status-boundary-{mode}" for mode in ("normal", "zero", "wrap")),
    "status-expiry-periodic", "status-expiry-probe", "status-deferred-probe", "status-latest",
    *(f"status-disconnect-{mode}" for mode in ("wifi", "socket", "loop", "settings", "expiry", "request")),
    "status-limits", "status-publish-failure", "status-send-generation",
    "network-service", "network-service-retry",
    *(f"status-ordering-{mode}" for mode in ("fault", "wrap", "stale", "deferred", "recovery", "newer-probe")),
    "status-order-full", "status-order-stale-fence",
    "configure-validation", "configure-lifecycle", "configure-worker", "configure-connect-race",
    "configure-prestart-validation",
    *(f"configure-prestart-failure-{mode}" for mode in
      ("open", "write", "read-open", "length", "read", "magic", "host", "user", "password", "port")),
    *(f"configure-prestart-begin-{mode}" for mode in
      (*(f"alloc-{slot}" for slot in range(1, 7)), "packet", "task")),
    *(f"configure-failure-{mode}" for mode in
      ("open", "write", "read-open", "length", "read", "magic", "host", "user", "password", "port")),
    *(f"configure-threads-{mode}" for mode in ("api", "http", "reader", "failure")),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true",
                        help="Enable AddressSanitizer and UndefinedBehaviorSanitizer")
    parser.add_argument("--arduino-json", type=Path,
                        help="Existing ArduinoJson 6 src directory; nothing is downloaded")
    parser.add_argument("--case", action="append", choices=CASES,
                        help="Run only selected cases (repeatable)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    json_headers = args.arduino_json or root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 headers required; pass --arduino-json PATH. No PIO run.")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    production = root / "shared/BabytechCloudLink/src"
    fakes = root / "tests/fakes/cloud_link"
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread", "-DARDUINO=10819"]
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command.append(f"-DARDUINOJSON_ENABLE_{feature}=0")
    command.append("-DARDUINOJSON_ENABLE_STD_STRING=1")
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in (fakes, production, json_headers):
        command += ["-I", str(include)]
    command += [str(production / "CloudLink.cpp"), str(production / "CloudSession.cpp"),
                str(fakes / "FakeCloudIo.cpp"), str(root / "tests/test_cloud_link.cpp")]
    with tempfile.TemporaryDirectory(prefix="babytech-cloud-link-") as temporary:
        binary = Path(temporary) / ("cloud_link.exe" if os.name == "nt" else "cloud_link")
        subprocess.run(command + ["-o", str(binary)], check=True, timeout=120)
        failures = []
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        for case in args.case or CASES:
            try:
                result = subprocess.run([str(binary), case], env=environment, timeout=15, check=False)
                if result.returncode:
                    failures.append(case)
            except subprocess.TimeoutExpired:
                failures.append(case)
                print(f"FAIL cloud-link {case}: worker timeout", flush=True)
        if failures:
            raise SystemExit("Failed cases: " + ", ".join(failures))
        print(f"PASS {len(args.case or CASES)} cloud-link cases (real production sources)")


if __name__ == "__main__":
    main()
