"""Compile production board message codecs with real ArduinoJson 6 (C++17)."""
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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    arduino_json = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (arduino_json / "ArduinoJson.h").is_file():
        raise SystemExit("Run `pio run -e motion` in device-controller before this test")
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if not cxx:
        raise SystemExit("A C++17 compiler is required")
    includes = (arduino_json, root / "shared/BoardProtocol/src",
                root / "shared/BabytechDisplayCore/src", root / "shared/ProductBoardLink/src")
    sources = ("shared/BoardProtocol/src/BoardProtocol.cpp",
               "shared/BoardProtocol/src/BoardProtocolV4.cpp",
               "shared/BoardProtocol/src/BoardSessionV4.cpp",
               "shared/ProductBoardLink/src/ProductBoardMessages.cpp",
               "shared/ProductBoardLink/src/ProductRequest.cpp",
               "shared/ProductBoardLink/test/test_messages.cpp")
    with tempfile.TemporaryDirectory(prefix="babytech-board-messages-") as directory:
        binary = Path(directory) / "board_messages"
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        for include in includes:
            command += ["-I", str(include)]
        command += [str(root / source) for source in sources]
        subprocess.run([*command, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
