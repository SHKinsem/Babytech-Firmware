"""Host tests of production Motion runtime, session, flow, NVS and result query.

Uses FakeBrainNvs and system SHA-256 through both mbedTLS SDK shims. Only
clock, executor and board I/O are replaced. No hardware, broker, installs or
downloads; fake feedback/NVS cannot prove real mechanical or Flash behavior.
The manual-admission regression compiles the actual main.cpp helper in a host
fixture; HTTP handler wiring remains a separate static check, not live HTTP.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def manual_admission_source(root):
    source = (root / "device-controller/src/main.cpp").read_text()
    definitions = []
    # These two small definitions use the same balanced-body convention as
    # test_maintenance_wiring.py. Never restate their admission/ownership logic.
    for signature in ("bool demoBusy() {", "bool demoManualMutation() {"):
        start = source.index(signature)
        end = source.index("{", start) + 1
        depth = 1
        while depth and end < len(source):
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        if depth:
            raise SystemExit("Unbalanced main.cpp manual admission definition")
        definitions.append(source[start:end])
    return ("#define MOTION_HAS_PRODUCT 1\n"
            "#define MOTION_UART_PEER 2\n"
            "#define MOTION_UART_PEER_PRODUCT_BRAIN 2\n" +
            "\n".join(definitions) + "\n"
            "#undef MOTION_HAS_PRODUCT\n"
            "#undef MOTION_UART_PEER\n"
            "#undef MOTION_UART_PEER_PRODUCT_BRAIN\n")


def recovery_stationary_source(root):
    source = (root / "device-controller/src/main.cpp").read_text()
    start = source.index("bool stationary() const override", source.index("class RecoveryHardware :"))
    end = source.index("{", start) + 1
    depth = 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise SystemExit("Unbalanced main.cpp recovery stationary definition")
    return source[start:end] + "\n"


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
                 "ProductBoardMessages.cpp", "ProductCommandResult.cpp", "ProductContextMessages.cpp", "ProductResultQuery.cpp",
                 "MotionStateRecord.cpp", "BrainStateRecord.cpp", "BoardPairingRecord.cpp", "MotionStateStore.cpp",
                 "ProductEventMessages.cpp", "ReadOnlyBoardLink.cpp")]
    sources += [root / "device-controller/src" / name for name in
                ("MotionProductRuntime.cpp", "MotionStateRecovery.cpp", "ProductSession.cpp", "DemoFlowController.cpp")]
    sources += [root / "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
                root / "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                root / "test/test_motion_product_runtime.cpp"]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    failed = False
    with tempfile.TemporaryDirectory(prefix="babytech-motion-product-runtime-") as directory:
        (Path(directory) / "MotionManualAdmissionHost.inc").write_text(manual_admission_source(root))
        (Path(directory) / "MotionRecoveryStationaryHost.inc").write_text(recovery_stationary_source(root))
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("motion_product_runtime_" + major)
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
            command += ["-I", directory]
            command += [str(source) for source in sources]
            if sys.platform == "linux":
                command += ["-lcrypto"]
            subprocess.run([*command, "-o", str(binary)], check=True)
            print("MotionProductRuntime / mbedTLS " + major, flush=True)
            result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                    env=environment, check=False)
            failed = failed or result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
