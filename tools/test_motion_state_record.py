"""Test production MotionStateRecord with real system SHA and both mbedTLS APIs.

Only temporary host test binaries are built; no firmware, NVS, devices, network,
dependency installation or Git operations. This does not prove Flash durability.
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
    implementation = root / "shared/ProductBoardLink/src/MotionStateRecord.cpp"
    if not implementation.is_file():
        raise SystemExit("MotionStateRecord.cpp is not present yet; no tests run")
    json_dir = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_dir / "ArduinoJson.h").is_file():
        raise SystemExit("ArduinoJson 6 required in existing motion libdeps; no dependencies installed")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    if sys.platform not in ("darwin", "linux"):
        raise SystemExit("System SHA-256 backend supports Apple or Linux only")
    includes = (json_dir, root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src",
                root / "shared/BabytechDisplayCore/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp",
                 "BoardPairingRecord.cpp", "MotionStateRecord.cpp")]
    sources += [root / "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                root / "shared/ProductBoardLink/test/test_motion_state_record.cpp"]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="babytech-motion-state-record-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("motion_state_record_" + major)
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-DMBEDTLS_VERSION_MAJOR=" + major]
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
            print("Testing MotionStateRecord / mbedTLS " + major, flush=True)
            subprocess.run([str(binary), *([args.case] if args.case else [])],
                           check=True, env=environment)


if __name__ == "__main__":
    main()
