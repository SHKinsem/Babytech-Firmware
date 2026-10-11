"""Run production Brain setup/loop, UART adapter and network with SDK I/O fakes.

UI objects, USB, UART, NVS and Wi-Fi/MQTT I/O are replaced. Business owners,
stores, parsers and scheduling are production code. Each case uses a fresh
process. This is not physical timing, a broker, or full Motion Arduino main.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


CASES = ("simulation-complete", "simulation-stop", "simulation-context",
         "simulation-receipt", "simulation-offline", "simulation-offline-ack-lost", "panel-failure",
         "bridge-input-validation", "real-readiness", "real-results", "real-results-write-failure",
         "real-auth-offline", "real-pipe-input-validation", "portal-guard", "portal-unpaired")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--compile-timeout", type=int, default=120,
                        help="Compiler timeout in seconds (default 120); case timeouts/flags unchanged")
    parser.add_argument("--case", action="append", choices=CASES)
    pipes = parser.add_mutually_exclusive_group()
    pipes.add_argument("--pipe", action="store_true",
                        help="Expose host SDK MQTT I/O to an isolated broker driver over JSON lines")
    pipes.add_argument("--real-pipe", action="store_true",
                       help="Expose actual Brain main MQTT and raw UART SDK I/O (DUAL_BRAIN= JSON lines)")
    parser.add_argument("--state-file", type=Path,
                        help="Initial raw fake NVS snapshot JSON; only valid with --real-pipe")
    parser.add_argument("--boot-id", type=int,
                        help="SDK random counter seed (1..UINT32_MAX-32), not an actual boot ID")
    parser.add_argument("--build-output", type=Path,
                        help="Build the host binary at this path and return without running")
    args = parser.parse_args()
    if args.compile_timeout <= 0:
        parser.error("--compile-timeout must be positive")
    if (args.pipe or args.real_pipe) and args.case:
        parser.error("pipe modes and --case are mutually exclusive")
    if (args.state_file is not None or args.boot_id is not None) and not args.real_pipe:
        parser.error("--state-file and --boot-id require --real-pipe")
    if args.boot_id is not None and not 1 <= args.boot_id <= 2**32 - 1 - 32:
        parser.error("--boot-id must be in 1..UINT32_MAX-32")
    root = Path(__file__).resolve().parents[1]
    headers = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 required; no dependencies installed")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler or sys.platform not in ("darwin", "linux"):
        raise SystemExit("C++17 and Apple/Linux SHA-256 backend required")
    includes = ("tests/fakes/brain_main", "tests/fakes/commissioning",
                "tests/fakes/brain_state_store", "tests/fakes/cloud_link",
                "tests/fakes/product_crypto", "shared/ProductBoardLink/test/arduino_stubs",
                "main-controller/src", "device-controller/include", "shared/BoardProtocol/src", "shared/ProductBoardLink/src",
                "shared/BabytechDisplayCore/src", "shared/BabytechCloudLink/src")
    sources = ["shared/BoardProtocol/src/" + name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += ["shared/ProductBoardLink/src/" + name for name in (
        "BoardDiscovery.cpp", "BoardMaintenance.cpp", "BoardInstall.cpp", "BoardExportTransfer.cpp",
        "MotionExportSnapshot.cpp", "MotionStateRecord.cpp", "MotionStateStore.cpp", "BrainStateRecord.cpp",
        "BrainStateStore.cpp", "ProductContext.cpp", "ProductDigest.cpp", "BoardPairingRecord.cpp",
        "BoardPairingStore.cpp", "BoardCommissioning.cpp", "MaintenanceExport.cpp", "LegacyContextStore.cpp",
        "ProductBoardMessages.cpp", "ProductRequest.cpp", "ProductResultQuery.cpp", "ProductCommandResult.cpp",
        "ReadOnlyBoardLink.cpp", "ProductContextMessages.cpp", "ProductEventMessages.cpp", "BoardLinkArduino.cpp")]
    sources += ["main-controller/src/" + name for name in
                ("controller_link.cpp", "brain_network.cpp", "brain_station.cpp", "brain_portal.cpp", "brain_status.cpp")]
    sources += ["shared/BabytechCloudLink/src/" + name for name in ("CloudLink.cpp", "CloudSession.cpp")]
    sources += ["shared/BabytechDisplayCore/src/display_model.cpp",
                "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
                "tests/fakes/commissioning/FakeCommissioning.cpp",
                "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                "tests/fakes/cloud_link/FakeCloudIo.cpp", "tests/fakes/brain_main/FakeMainIo.cpp",
                "tests/test_brain_main.cpp"]
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-DARDUINO=10819", "-DMBEDTLS_VERSION_MAJOR=3",
               '-DFIRMWARE_VERSION="main-host-test"']
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command += ["-DARDUINOJSON_ENABLE_" + feature + "=0"]
    if sys.platform == "darwin":
        command += ["-Wno-deprecated-declarations"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    for include in includes:
        command += ["-I", str(root / include)]
    command += ["-I", str(headers), *[str(root / source) for source in sources]]
    if sys.platform == "linux":
        command += ["-lcrypto"]
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    if args.build_output is not None:
        subprocess.run([*command, "-o", str(args.build_output)], check=True, timeout=args.compile_timeout)
        return
    with tempfile.TemporaryDirectory(prefix="babytech-brain-main-") as directory:
        binary = Path(directory) / "brain_main"
        subprocess.run([*command, "-o", str(binary)], check=True, timeout=args.compile_timeout)
        if args.pipe:
            subprocess.run([str(binary), "broker-bridge"], env=env, check=True, timeout=300)
            return
        if args.real_pipe:
            options = []
            if args.state_file is not None:
                options += ["--state-file", str(args.state_file)]
            if args.boot_id is not None:
                options += ["--boot-id", str(args.boot_id)]
            subprocess.run([str(binary), "real-broker-bridge", *options],
                           env=env, check=True, timeout=300)
            return
        process_count = 0
        for case in args.case or CASES:
            subprocess.run([str(binary), case], env=env, check=True, timeout=30)
            process_count += 1
            if case == "real-readiness":
                # Separate boot: max sequence is a valid persisted fixture, not
                # a mutation of the production Store's live/private state.
                subprocess.run([str(binary), case, "max-sequence"], env=env, check=True, timeout=30)
                process_count += 1
            if case == "real-auth-offline":
                for failure in ("command-subscribe-failed", "config-subscribe-failed"):
                    subprocess.run([str(binary), case, failure], env=env, check=True, timeout=30)
                    process_count += 1
    print("PASS production Brain main: " + str(len(args.case or CASES)) +
          " cases, " + str(process_count) + " isolated processes")


if __name__ == "__main__":
    main()
