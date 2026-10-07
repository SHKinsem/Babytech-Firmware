"""Test production ControllerLink + Arduino adapter/session using SDK I/O fakes.

No PIO, network, MCU, or copied production logic. Includes the production PAIR
console template instantiated with ControllerLink. No CloudLink is linked, so
this suite cannot prove network-task isolation or Main/BrainNetwork integration.
Does not validate physical UART timing or MCU stacks.
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
    parser.add_argument("--arduino-json", type=Path,
                        help="Existing ArduinoJson 6 src directory; no download or PIO")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    headers = args.arduino_json or root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 headers required; pass --arduino-json PATH.")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    stubs = root / "shared/ProductBoardLink/test/arduino_stubs"
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-DARDUINO=10819", "-DBABYTECH_BOARD_LINK_V4=1"]
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command.append(f"-DARDUINOJSON_ENABLE_{feature}=0")
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    if sys.platform == "darwin":
        command += ["-Wno-deprecated-declarations"]
    for include in (stubs, headers, root / "main-controller/src",
                    root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src",
                    root / "shared/BabytechDisplayCore/src", root / "tests/fakes/product_crypto"):
        command += ["-I", str(include)]
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardDiscovery.cpp", "BoardMaintenance.cpp", "BoardExportTransfer.cpp", "MotionExportSnapshot.cpp",
                 "MotionStateRecord.cpp", "ProductContext.cpp", "ProductDigest.cpp",
                 "BoardPairingRecord.cpp", "BoardPairingStore.cpp", "ProductBoardMessages.cpp", "ProductRequest.cpp", "ReadOnlyBoardLink.cpp",
                 "BoardLinkArduino.cpp")]
    sources += [stubs / "FakeBoardIo.cpp", root / "main-controller/src/controller_link.cpp",
                root / "test/test_brain_controller.cpp", root / "tests/fakes/product_crypto/FakeProductCrypto.cpp"]
    with tempfile.TemporaryDirectory(prefix="babytech-brain-controller-") as directory:
        binary = Path(directory) / ("brain_controller.exe" if os.name == "nt" else "brain_controller")
        subprocess.run(command + [str(source) for source in sources] +
                       (["-lcrypto"] if sys.platform != "darwin" else []) + ["-o", str(binary)],
                       check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(binary)], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
