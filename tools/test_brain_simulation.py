"""Pure Brain RAM simulation host tests, not main/UART/network integration.

Uses the existing ArduinoJson 6 headers and system SHA-256 API fakes, just
like test_product_event_messages.py. No installs or persistent test outputs.
Both mbedTLS API variants compile and run serially, with ASan/UBSan by default.

Run: python3 tools/test_brain_simulation.py
Run one group: python3 tools/test_brain_simulation.py --case timer
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
    parser.add_argument("--mbedtls-major", choices=("2", "3", "both"), default="both")
    parser.add_argument("--case", help="Run one named C++ test group")
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--sanitize", dest="sanitize", action="store_true")
    sanitizer.add_argument("--no-sanitize", dest="sanitize", action="store_false")
    parser.set_defaults(sanitize=True)
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
    includes = (json_dir, root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src",
                root / "main-controller/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardPairingRecord.cpp", "MotionStateRecord.cpp", "ProductRequest.cpp",
                 "ProductDigest.cpp", "ProductContext.cpp", "ProductEventMessages.cpp")]
    sources += [root / "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                root / "tests/test_brain_simulation.cpp"]
    for source in (*sources, root / "main-controller/src/brain_simulation.h",
                   root / "main-controller/src/brain_simulation_console.h"):
        if not source.is_file():
            raise SystemExit("Missing host-test source: " + str(source))
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-brain-simulation-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("brain_simulation_" + major)
            print("Compiling BrainSimulation / mbedTLS " + major +
                  (" / ASan+UBSan" if args.sanitize else ""), flush=True)
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
            subprocess.run([*command, "-o", str(binary)], check=True, timeout=120)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False, timeout=60)
            failed = failed or result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
