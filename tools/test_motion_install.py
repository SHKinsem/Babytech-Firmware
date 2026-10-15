"""Integration tests of the real Motion installation channel, lease and stores.

Uses the commissioning runner's production sources and existing fake NVS/system
SHA-256 backend, plus BoardDiscovery, BoardMaintenance and BoardInstall. The
actual MotionInstallTarget header is compiled, not replaced with a fake target.
Fixtures own separate long-lived RX assembler/scratch buffers bound before begin.
Abrupt cuts cover every observed NVS I/O boundary for all three context kinds,
then reboot into fresh channel/target/store objects with a new real lease.
No device, dependency installation, network or actual Flash access. Simulated
I/O failures do not establish real Flash atomicity or mechanical safety.
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
    for name in ("BoardInstall.h", "BoardInstall.cpp", "MotionInstallTarget.h"):
        if not (root / "shared/ProductBoardLink/src" / name).is_file():
            raise SystemExit("Production " + name + " is not available yet; rerun when ready")
    includes = (json_dir, root / "tests/fakes/commissioning",
                root / "tests/fakes/brain_state_store", root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src",
                root / "shared/BabytechDisplayCore/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardCommissioning.cpp", "BoardPairingRecord.cpp", "BoardPairingStore.cpp",
                 "BrainStateRecord.cpp", "BrainStateStore.cpp", "MotionStateRecord.cpp",
                 "MotionStateStore.cpp", "LegacyContextStore.cpp", "ProductContext.cpp",
                 "ProductRequest.cpp", "ProductDigest.cpp", "BoardDiscovery.cpp",
                 "BoardMaintenance.cpp", "BoardInstall.cpp")]
    sources += [root / path for path in (
        "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
        "tests/fakes/commissioning/FakeCommissioning.cpp",
        "tests/fakes/product_crypto/FakeProductCrypto.cpp",
        "shared/ProductBoardLink/test/test_motion_install.cpp")]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-motion-install-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("motion_install_" + major)
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
            print("Motion install integration / mbedTLS " + major, flush=True)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False)
            failed = failed or result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
