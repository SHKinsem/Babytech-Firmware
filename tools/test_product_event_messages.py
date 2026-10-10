"""Host contract tests for ProductEventMessages (TERMINAL / CLOUD_RECEIPT).

Uses existing ArduinoJson 6 and system SHA-256 through the mbedTLS 2/3
API fakes. No downloads, hardware, broker, persistence or UART integration.
The two variants exercise SDK APIs, not different SHA algorithms.

Run: python3 tools/test_product_event_messages.py --sanitize --mbedtls-major both
--emit-fixture writes only one JSON object to stdout, retaining actual encoded
terminal bytes after decode checks; compiler/progress output goes to stderr.
--receipt-fixture FILE checks each actual Cloud receipt JSONL line with both
decoder overloads, expecting bt-receipt. Empty input or any bad line fails.
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
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--mbedtls-major", choices=("2", "3", "both"), default="both")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--case", help="Run one named C++ test group")
    mode.add_argument("--emit-fixture", action="store_true",
                      help="Stdout only: four production-encoded, decoded terminal JSON fixtures")
    mode.add_argument("--emit-simulation-fixture", action="store_true",
                      help="Stdout only: Brain MQTT terminal fixtures; rejected by UART decoder")
    mode.add_argument("--receipt-fixture", type=Path, metavar="FILE",
                      help="Decode Cloud actual receipt JSONL for expected device bt-receipt")
    args = parser.parse_args()
    if args.receipt_fixture and not args.receipt_fixture.is_file():
        parser.error("Receipt fixture file does not exist: " + str(args.receipt_fixture))
    root = Path(__file__).resolve().parents[1]
    json_dir = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_dir / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 required; no dependencies installed")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    if sys.platform not in ("darwin", "linux"):
        raise SystemExit("System SHA-256 backend supports Apple or Linux only")
    includes = (json_dir, root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardPairingRecord.cpp", "MotionStateRecord.cpp", "ProductRequest.cpp",
                 "ProductDigest.cpp", "ProductContext.cpp", "ProductEventMessages.cpp")]
    sources += [root / "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                root / "shared/ProductBoardLink/test/test_event_messages.cpp"]
    for source in sources:
        if not source.is_file():
            raise SystemExit("Missing host-test source: " + str(source))
    if not (root / "shared/ProductBoardLink/src/ProductEventMessages.h").is_file():
        raise SystemExit("Missing planned ProductEventMessages.h; codec not available yet")
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    fixture_output = None
    emitting = args.emit_fixture or args.emit_simulation_fixture
    log = sys.stderr if emitting else sys.stdout
    binary_args = [args.case] if args.case else []
    if args.emit_fixture:
        binary_args = ["--emit-fixture"]
    elif args.emit_simulation_fixture:
        binary_args = ["--emit-simulation-fixture"]
    elif args.receipt_fixture:
        binary_args = ["--receipt-fixture", str(args.receipt_fixture.resolve())]
    with tempfile.TemporaryDirectory(prefix="babytech-product-event-messages-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("product_event_messages_" + major)
            print("Compiling ProductEventMessages / mbedTLS " + major +
                  (" / ASan+UBSan" if args.sanitize else ""), file=log, flush=True)
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
            subprocess.run([*command, "-o", str(binary)], check=True, timeout=120,
                           stdout=sys.stderr if emitting else None)
            print("ProductEventMessages / mbedTLS " + major, file=log, flush=True)
            result = subprocess.run([str(binary), *binary_args], env=environment,
                                    stdout=subprocess.PIPE if emitting else None,
                                    check=False, timeout=60)
            failed = failed or result.returncode != 0
            if emitting and result.returncode == 0:
                if fixture_output is not None and fixture_output != result.stdout:
                    print("Fixture bytes differ between mbedTLS API variants", file=sys.stderr)
                    failed = True
                fixture_output = result.stdout
    if emitting and not failed and fixture_output is not None:
        sys.stdout.buffer.write(fixture_output)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
