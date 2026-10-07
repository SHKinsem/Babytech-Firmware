"""Host tests for BrainContextSync, real dual UART cores and Motion runtime.

Only NVS, executor/feedback, clock and byte transport are replaced. Both
mbedTLS API shims use system SHA-256. No hardware, network, broker, PIO,
downloads or installation; existing ArduinoJson 6 is required.
--link-from-head is an optional negative control: compile the committed link
core in a temporary directory without checking out or editing production files.
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
    parser.add_argument("--link-from-head", action="store_true",
                        help="Negative control using committed HEAD link core; no checkout")
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
    includes = (json_dir, root / "tests/fakes/brain_state_store",
                root / "tests/fakes/product_crypto", root / "shared/BoardProtocol/src",
                root / "shared/ProductBoardLink/src", root / "shared/BabytechDisplayCore/src",
                root / "main-controller/src", root / "device-controller/include")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp",
                 "BrainStateRecord.cpp", "BrainStateStore.cpp", "BoardPairingRecord.cpp",
                 "MotionStateRecord.cpp", "MotionStateStore.cpp", "ProductResultQuery.cpp",
                 "ProductBoardMessages.cpp", "ProductCommandResult.cpp", "ProductContextMessages.cpp",
                 "ReadOnlyBoardLink.cpp")]
    sources += [root / "device-controller/src" / name for name in
                ("MotionProductRuntime.cpp", "ProductSession.cpp", "DemoFlowController.cpp")]
    sources += [root / path for path in (
        "shared/BabytechDisplayCore/src/display_model.cpp",
        "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
        "tests/fakes/product_crypto/FakeProductCrypto.cpp",
        "tests/test_brain_context_sync.cpp")]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-brain-context-sync-") as directory:
        if args.link_from_head:
            baseline = subprocess.run(
                ["git", "show", "HEAD:shared/ProductBoardLink/src/ReadOnlyBoardLink.cpp"],
                cwd=root, check=True, capture_output=True, timeout=30).stdout
            baseline_path = Path(directory) / "ReadOnlyBoardLink.cpp"
            baseline_path.write_bytes(baseline)
            current_path = root / "shared/ProductBoardLink/src/ReadOnlyBoardLink.cpp"
            sources = [baseline_path if source == current_path else source for source in sources]
            print("Negative control: committed HEAD link core (temporary copy)", flush=True)
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("brain_context_sync_" + major)
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
            print("Brain context sync / mbedTLS " + major, flush=True)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False, timeout=60)
            failed = failed or result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
