"""Host tests for the read-only UART Motion-records pull.

Compiles production transfer, snapshot decoder, MaintenanceExport and record
codecs with existing NVS/MAC/crypto fakes, for both SDK SHA-256 APIs. A one-slot
TX model exercises queue acceptance and short writes, not actual UART drivers.
No PlatformIO, dependency installation, hardware, network or real NVS access.
Missing forthcoming implementations are an error, never a passing/skipped test.
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
    parser.add_argument("--case", choices=("records", "decoder", "frames", "timing", "bounds"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    json_dir = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    includes = (json_dir, root / "tests/fakes/commissioning",
                root / "tests/fakes/brain_state_store", root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardDiscovery.cpp", "BoardExportTransfer.cpp", "MotionExportSnapshot.cpp",
                 "MaintenanceExport.cpp", "BoardPairingRecord.cpp", "BoardPairingStore.cpp",
                 "BrainStateRecord.cpp", "MotionStateRecord.cpp", "LegacyContextStore.cpp",
                 "ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp")]
    sources += [root / path for path in (
        "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
        "tests/fakes/commissioning/FakeCommissioning.cpp",
        "tests/fakes/product_crypto/FakeProductCrypto.cpp", "test/test_board_export_transfer.cpp")]
    required = [*sources, json_dir / "ArduinoJson.h"]
    required += [root / "shared/ProductBoardLink/src" / name for name in
                 ("BoardExportTransfer.h", "MotionExportSnapshot.h", "BoardExportSource.h")]
    missing = [str(path.relative_to(root)) for path in required if not path.is_file()]
    if missing:
        print("BoardExportTransfer tests NOT RUN; missing dependencies:\n  " +
              "\n  ".join(missing), file=sys.stderr)
        return 2
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        parser.error("A C++17 compiler is required")
    if sys.platform not in ("darwin", "linux"):
        parser.error("System SHA-256 backend supports Apple or Linux only")
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-board-export-transfer-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("board_export_transfer_" + major)
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
            try:
                subprocess.run([*command, "-o", str(binary)], check=True)
                print("BoardExportTransfer + production MaintenanceExport / mbedTLS " + major, flush=True)
                subprocess.run([str(binary), *([args.case] if args.case else [])],
                               env=environment, check=True)
            except (OSError, subprocess.CalledProcessError) as exc:
                print("BoardExportTransfer verification failed: " + str(exc), file=sys.stderr)
                failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
