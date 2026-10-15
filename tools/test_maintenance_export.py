"""Host tests for the production read-only maintenance exporter and USB runtime.

Uses installed ArduinoJson and existing MAC/NVS/mbedTLS shims, with both SDK
SHA-256 APIs. No PlatformIO, hardware, network, installation or real NVS access.
Host faults do not establish USB driver timing, Flash or mechanical safety.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def check_interop(root, binary, environment, directory, major):
    # Import only the trusted sibling, without creating files in its directory.
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location(
            "_maintenance_export_capture", root / "tools/capture_board_export.py")
        capture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(capture)
    finally:
        sys.dont_write_bytecode = previous
    for role in ("brain", "motion"):
        result = subprocess.run([str(binary), "--emit-" + role], env=environment,
                                capture_output=True, check=False)
        if result.returncode:
            sys.stderr.buffer.write(result.stderr)
            raise RuntimeError("C++ interoperability fixture failed: " + role)
        path = Path(directory) / ("framed_" + role + "_" + major + ".txt")
        path.write_bytes(result.stdout)
        wire = path.read_bytes()
        if not wire or not wire.endswith(b"\n"):
            raise RuntimeError("Incomplete fixture output")
        lines = wire.splitlines()
        if any(line and not line.startswith(b"[mx] ") for line in lines):
            raise RuntimeError("Fixture stdout contains non-frame output")
        # Also exercise capture-tool CRC verification with ordinary logs between frames.
        with_logs = [entry for line in lines for entry in (line, b"[runtime] ordinary log")]
        for stream in (lines, with_logs):
            fragments = capture._Fragments()
            complete = None
            for line in stream:
                if not line.startswith(b"[mx]"):
                    continue
                if complete is not None:
                    raise RuntimeError("Unexpected frame after completed export")
                complete = fragments.feed(line)
            if complete is None or not complete.endswith(b"\n"):
                raise RuntimeError("Fixture did not complete export")
            value = capture.parse_export(complete, role=role, device_id="D" * 64,
                                         challenge="0123456789abcdef0123456789abcdef")
            expected = {"pair_status": "ready", "state_status": "ready",
                        "legacy_status": "ready" if role == "motion" else "not_applicable",
                        "legacy_event": "missing" if role == "motion" else "not_applicable",
                        "physical_id": "012345abcdef", "boot": "fedcba9876543210",
                        "captured_ms": 123}
            if any(value[key] != item for key, item in expected.items()):
                raise RuntimeError("Unexpected interoperability fixture metadata")
            if role == "motion" and len(bytes.fromhex(value["legacy_hex"])) != 981:
                raise RuntimeError("Fixture lost maximum legacy context")
        print("C++ frames -> Python capture: " + role + " max context, clean/interleaved logs passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--mbedtls-major", choices=("2", "3", "both"), default="both")
    parser.add_argument("--case", help="Run one named C++ test group")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    json_dir = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_dir / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 required; no dependencies installed")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    if sys.platform not in ("darwin", "linux"):
        raise SystemExit("System SHA-256 backend supports Apple or Linux only")
    includes = (json_dir, root / "tests/fakes/commissioning",
                root / "tests/fakes/brain_state_store", root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("MaintenanceExport.cpp", "BoardPairingRecord.cpp", "BoardPairingStore.cpp",
                 "BrainStateRecord.cpp", "MotionStateRecord.cpp", "LegacyContextStore.cpp",
                 "ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp")]
    sources += [root / path for path in (
        "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
        "tests/fakes/commissioning/FakeCommissioning.cpp",
        "tests/fakes/product_crypto/FakeProductCrypto.cpp",
        "shared/ProductBoardLink/test/test_maintenance_export.cpp")]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-maintenance-export-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("maintenance_export_" + major)
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-DARDUINO=10819", "-DMBEDTLS_VERSION_MAJOR=" + major]
            for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
                command += ["-DARDUINOJSON_ENABLE_" + feature + "=0"]
            if sys.platform == "darwin":
                command += ["-Wno-deprecated-declarations"]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
            for include in includes:
                command += ["-I", str(include)]
            command += [str(source) for source in sources]
            if sys.platform == "linux":
                command += ["-lcrypto"]
            subprocess.run([*command, "-o", str(binary)], check=True)
            print("MaintenanceExport + MaintenanceUsbConsole / mbedTLS " + major, flush=True)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False)
            failed = failed or result.returncode != 0
            try:
                check_interop(root, binary, environment, directory, major)
            except (OSError, ValueError, RuntimeError) as exc:
                print("Interoperability failure: " + str(exc), file=sys.stderr)
                failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
