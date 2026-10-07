"""Link the real Arduino board adapter/core/codecs against host-only I/O fakes.

Uses already available ArduinoJson 6 headers; never invokes PlatformIO.
Does not emulate Flash durability, physical UART timing or FreeRTOS.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true",
                        help="Enable AddressSanitizer and UndefinedBehaviorSanitizer")
    parser.add_argument("--arduino-json", type=Path,
                        help="Existing ArduinoJson 6 src directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    json_headers = args.arduino_json or root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 headers required; pass --arduino-json PATH. No PIO run.")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    stubs = root / "shared/ProductBoardLink/test/arduino_stubs"
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("BoardDiscovery.cpp", "BoardPairingRecord.cpp", "BoardPairingStore.cpp", "ProductBoardMessages.cpp", "ProductRequest.cpp", "ReadOnlyBoardLink.cpp",
                 "BoardLinkArduino.cpp")]
    sources += [stubs / "FakeBoardIo.cpp", root / "test/test_arduino_link.cpp"]
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-DARDUINO=10819"]
    # The production codecs use byte buffers, not Arduino String/Stream/Print
    # or flash strings. Disable only these unused ArduinoJson SDK integrations.
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command += [f"-DARDUINOJSON_ENABLE_{feature}=0"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in (stubs, json_headers, root / "shared/BoardProtocol/src",
                    root / "shared/ProductBoardLink/src", root / "shared/BabytechDisplayCore/src"):
        command += ["-I", str(include)]
    with tempfile.TemporaryDirectory(prefix="babytech-board-arduino-") as temporary:
        binary = Path(temporary) / ("board_arduino.exe" if os.name == "nt" else "board_arduino")
        subprocess.run(command + [str(source) for source in sources] + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
