"""Dynamic host tests of production result delivery, Store, codecs and two links.

NVS, clock, byte sinks, executor/feedback and Network I/O are replaced. Network captures the
original JSON; this is not a Network SDK, broker, physical Flash/board or full
main-loop E2E test. No dependencies are installed or downloaded.
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
        raise SystemExit("ArduinoJson 6 required in existing motion libdeps; no dependencies installed")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    if sys.platform not in ("darwin", "linux"):
        raise SystemExit("System SHA-256 backend supports Apple or Linux only")
    includes = (json_dir, root / "tests/fakes/brain_state_store",
                root / "tests/fakes/product_crypto", root / "shared/BoardProtocol/src",
                root / "shared/ProductBoardLink/src", root / "shared/BabytechDisplayCore/src",
                root / "device-controller/include", root / "main-controller/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp",
                 "ProductBoardMessages.cpp", "ProductCommandResult.cpp", "ProductContextMessages.cpp",
                 "ProductResultQuery.cpp", "MotionStateRecord.cpp", "BrainStateRecord.cpp",
                 "BoardPairingRecord.cpp", "MotionStateStore.cpp", "ProductEventMessages.cpp",
                 "ReadOnlyBoardLink.cpp")]
    sources += [root / "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
                root / "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                root / "tests/test_result_delivery.cpp"]
    sources += [root / "device-controller/src" / name for name in
                ("MotionProductRuntime.cpp", "MotionStateRecovery.cpp", "ProductSession.cpp",
                 "DemoFlowController.cpp")]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-result-delivery-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("result_delivery_" + major)
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
            print("ResultDelivery / mbedTLS " + major + (" / ASan+UBSan" if args.sanitize else ""), flush=True)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False)
            failed = failed or result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
