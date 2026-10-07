"""Exercise production BrainNetwork/BrainStation/status + CloudLink/session/codecs.

Only SDK I/O is faked. Reuses the CloudLink captured-worker scheduler and MQTT
capture; each case has a fresh process for MCU-lifetime ownership. No private
access, production copies, PIO, git, broker, hardware or network operations.
This does not validate real FreeRTOS races, Wi-Fi SDK behavior, Flash or stacks.
With --cloud-fixtures and --ack-output, run only external JSONL commands and
export captured, verified production ACK JSONL without rewriting the inputs.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


CASES = [
    "configure-wifi", "configure-mqtt", "configure-mqtt-failure", "configure-unpaired",
    "configure-unpaired-failure", "configure-unpaired-begin-failure", "configure-other-owner",
    *(f"station-{kind}" for kind in (
        "missing", "open-error", "ssid-missing", "pass-missing", "ssid-error", "pass-error",
        "ssid-empty", "ssid-max", "ssid-long", "pass-open", "pass-short", "pass-min",
        "pass-max", "pass-hex", "pass-not-hex", "pass-long", "ssid-nul", "pass-nul",
        "ssid-unterminated", "pass-unterminated", "ssid-length", "pass-length",
        "length-zero", "length-oversize")),
    "station-retry", "station-rollover", "station-connected", "station-wrong-ssid", "station-no-ip",
    "identity", "identity-max", "no-cloudcfg", "offline", "periodic", "periodic-rollover",
    "reconnect", "expiry", "send-expiry", "probe", "probe-reject", "probe-budget",
    "probe-old-generation", "readonly", "queue-full", "failure-throttle", "failure-throttle-rollover",
    "publish-failure", "status-safety", "status-size", "status-control-characters",
    "encoder-boundary", "status-payload", "motion-expiry", "motion-expiry-wrap",
    "motion-expiry-probe", "motion-receipt-zero",
    *(f"command-{action}" for action in (
        "prepare", "clean", "set_target_temp", "reset_error", "check_firmware_update", "stop")),
    "command-maxseq-prepare", "command-maxseq-stop",
    *(f"command-fresh-{action}-{kind}" for action in ("clean", "stop") for kind in (
        "age-4999", "age-5000", "age-5001", "future", "pre-session", "queued-expired",
        "wrap-current", "wrap-expired", "uptime-zero")),
    *(f"command-reject-{action}" for action in ("prepare", "clean", "set_target_temp", "stop")),
    *(f"command-generation-{action}-{kind}" for action in ("clean", "stop") for kind in (
        "inbound-queued", "inbound-deferred", "ack-deferred")),
    "command-topics", "command-budget", "command-stop-priority",
    "command-size-clean", "command-size-stop",
    "command-disconnected-clean", "command-disconnected-stop",
    *(f"handler-{action}-current" for action in (
        "prepare", "clean", "set_target_temp", "reset_error", "check_firmware_update", "stop")),
    *(f"handler-{action}-{kind}" for action in ("clean", "stop") for kind in (
        "age5000", "expired", "wrong", "missing", "unregister", "deadline")),
    *(f"handler-reject-{action}" for action in ("prepare", "clean", "set_target_temp", "stop")),
    *(f"handler-generation-{action}-{kind}" for action in ("clean", "stop") for kind in ("queued", "deferred")),
    "ack-validation",
    *(f"ack-runtime-{kind}" for kind in ("accepted", "rejected", "boolean", "reconnect", "offline", "full", "deferred")),
    "status-flags",
    *(f"status-flags-{kind}" for kind in (
        "enabled", "operations", "disabled", "stale", "absent", "age1499", "age1500", "wrap", "send-expiry")),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arduino-json", type=Path,
                        help="Existing ArduinoJson 6 src directory; nothing is downloaded")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--cloud-fixtures", type=Path, help="External Cloud command payloads, one JSON object per line")
    parser.add_argument("--ack-output", type=Path, help="Write only real, verified Brain ACK payloads as JSONL")
    args = parser.parse_args()
    if (args.cloud_fixtures is None) != (args.ack_output is None):
        parser.error("--cloud-fixtures and --ack-output must be specified together")
    if args.cloud_fixtures is not None:
        if args.case:
            parser.error("--case cannot be combined with external fixture mode")
        if args.cloud_fixtures.resolve() == args.ack_output.resolve():
            parser.error("Cloud fixture input and ACK output must be different files")
        if not args.cloud_fixtures.is_file():
            parser.error("Cloud fixture input must be an existing JSONL file")
    root = Path(__file__).resolve().parents[1]
    headers = args.arduino_json or root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 headers required; pass --arduino-json PATH. No PIO run.")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-DARDUINO=10819", "-DBABYTECH_BOARD_LINK_V4=1", '-DFIRMWARE_VERSION="host-test"']
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command.append(f"-DARDUINOJSON_ENABLE_{feature}=0")
    command.append("-DARDUINOJSON_ENABLE_STD_STRING=1")
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in (root / "tests/fakes/brain_network", root / "tests/fakes/cloud_link",
                    root / "main-controller/src", root / "shared/BabytechCloudLink/src",
                    root / "shared/ProductBoardLink/src", root / "shared/BoardProtocol/src",
                    root / "shared/BabytechDisplayCore/src", headers):
        command += ["-I", str(include)]
    for source in ("main-controller/src/brain_network.cpp", "main-controller/src/brain_station.cpp",
                   "main-controller/src/brain_status.cpp", "shared/BabytechCloudLink/src/CloudLink.cpp",
                   "shared/BabytechCloudLink/src/CloudSession.cpp", "tests/fakes/cloud_link/FakeCloudIo.cpp",
                   "shared/BoardProtocol/src/BoardProtocol.cpp", "shared/BoardProtocol/src/BoardProtocolV4.cpp",
                   "shared/BoardProtocol/src/BoardSessionV4.cpp", "shared/ProductBoardLink/src/ProductBoardMessages.cpp",
                   "shared/ProductBoardLink/src/ProductRequest.cpp",
                   "tests/fakes/brain_network/FakeBrainNvs.cpp", "tests/test_brain_network.cpp"):
        command.append(str(root / source))
    with tempfile.TemporaryDirectory(prefix="babytech-brain-network-") as directory:
        binary = Path(directory) / ("brain_network.exe" if os.name == "nt" else "brain_network")
        subprocess.run([*command, "-o", str(binary)], check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        if args.cloud_fixtures is not None:
            subprocess.run([str(binary), "--cloud-fixtures", str(args.cloud_fixtures),
                            "--ack-output", str(args.ack_output)], env=environment, timeout=15, check=True)
            return
        failures = []
        for case in args.case or CASES:
            try:
                result = subprocess.run([str(binary), case], env=environment, timeout=15, check=False)
                if result.returncode:
                    failures.append(case)
            except subprocess.TimeoutExpired:
                failures.append(case)
                print(f"FAIL brain-network {case}: worker timeout", flush=True)
        if failures:
            raise SystemExit("Failed cases: " + ", ".join(failures))
        print(f"PASS {len(args.case or CASES)} brain-network cases (real production sources)")


if __name__ == "__main__":
    main()
