"""Host decoder/exporter tests using installed ArduinoJson 6 and system SHA.

No PlatformIO, installation, network, hardware or real NVS access. Test builds
and emitted fixtures live only in a temporary directory.
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
                ("MotionExportSnapshot.cpp", "MaintenanceExport.cpp", "BoardPairingRecord.cpp",
                 "BoardPairingStore.cpp", "BrainStateRecord.cpp", "MotionStateRecord.cpp",
                 "LegacyContextStore.cpp", "ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp")]
    sources += [root / name for name in (
        "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
        "tests/fakes/commissioning/FakeCommissioning.cpp",
        "tests/fakes/product_crypto/FakeProductCrypto.cpp", "test/test_motion_export_snapshot.cpp")]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="babytech-motion-export-snapshot-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("motion_export_snapshot_" + major)
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
            print("MotionExportSnapshot / mbedTLS " + major +
                  (" / ASan+UBSan" if args.sanitize else ""), flush=True)
            subprocess.run([str(binary)], env=environment, check=True)


if __name__ == "__main__":
    main()
