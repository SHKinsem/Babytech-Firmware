"""Host regression for the real BrainInstaller and two isolated board NVS stores.

FakeLink adapts UART I/O and read-only MaintenanceExport snapshots. Discovery,
maintenance, Motion installation/target and both commissioning stores are the
production implementations. No network, device, PlatformIO or new dependencies.
persisted-restart adds production MAC-checked pairing/Store loads and two
ReadOnlyLink cores after simulated whole/single-board resets; it does not run
the Arduino adapter/main or prove physical power cuts, Flash or mechanical safety.
Full runs also connect install_brain.py through pipes to the production bounded
USB console/installer for four record cases; role probe and SDK I/O remain fake.
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
    if not (root / "main-controller/src/brain_installer.h").is_file():
        raise SystemExit("Production brain_installer.h is not available yet")
    includes = (json_dir, root / "tests/fakes/commissioning",
                root / "tests/fakes/brain_state_store", root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src",
                root / "shared/BabytechDisplayCore/src", root / "main-controller/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardCommissioning.cpp", "BoardPairingRecord.cpp", "BoardPairingStore.cpp",
                 "BrainStateRecord.cpp", "BrainStateStore.cpp", "MotionStateRecord.cpp",
                 "MotionStateStore.cpp", "LegacyContextStore.cpp", "ProductContext.cpp",
                 "ProductRequest.cpp", "ProductDigest.cpp", "BoardDiscovery.cpp",
                 "BoardMaintenance.cpp", "BoardInstall.cpp", "MaintenanceExport.cpp",
                 "MotionExportSnapshot.cpp", "BoardExportTransfer.cpp", "ReadOnlyBoardLink.cpp",
                 "ProductBoardMessages.cpp", "ProductCommandResult.cpp", "ProductContextMessages.cpp",
                 "ProductResultQuery.cpp", "ProductEventMessages.cpp")]
    sources += [root / path for path in (
        "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
        "tests/fakes/commissioning/FakeCommissioning.cpp",
        "tests/fakes/product_crypto/FakeProductCrypto.cpp",
        "test/test_brain_installer.cpp")]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-brain-installer-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("brain_installer_" + major)
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
            print("Brain installer regression / mbedTLS " + major, flush=True)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False)
            failed = failed or result.returncode != 0
            if not args.case and result.returncode == 0:
                print("Single Brain USB host tool / production core / mbedTLS " + major, flush=True)
                result = subprocess.run([sys.executable, str(root / "tests/check_install_brain_core.py"), str(binary)],
                                        env=environment, check=False)
                failed = failed or result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
