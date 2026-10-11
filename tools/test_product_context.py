"""Compile production ProductContext with real ArduinoJson 6 (C++17)."""
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
    parser.add_argument("--case", help="Run one named C++ test group (default: all)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    arduino_json = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (arduino_json / "ArduinoJson.h").is_file():
        raise SystemExit("ArduinoJson 6 is required in the existing motion libdeps; "
                         "this runner does not install dependencies")
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if not cxx:
        raise SystemExit("A C++17 compiler is required")
    includes = (arduino_json, root / "shared/BoardProtocol/src",
                root / "shared/BabytechDisplayCore/src", root / "shared/ProductBoardLink/src")
    sources = ("shared/BoardProtocol/src/BoardProtocol.cpp",
               "shared/BoardProtocol/src/BoardProtocolV4.cpp",
               "shared/ProductBoardLink/src/ProductContext.cpp",
               "shared/ProductBoardLink/test/test_context.cpp")
    with tempfile.TemporaryDirectory(prefix="babytech-product-context-") as directory:
        binary = Path(directory) / "product_context"
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer", "-g"]
        for include in includes:
            command += ["-I", str(include)]
        command += [str(root / source) for source in sources]
        subprocess.run([*command, "-o", str(binary)], check=True)
        subprocess.run([str(binary), *([args.case] if args.case else [])], check=True)


if __name__ == "__main__":
    main()
