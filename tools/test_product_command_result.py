"""Standalone host tests of the production ordinary COMMAND_RESULT codec.

Uses existing ArduinoJson 6 only; no hardware, network, or dependency install.
This does not verify UART integration or the separate Stop result path.
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
    parser.add_argument("--case", help="Run one named C++ test group")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    json_dir = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_dir / "ArduinoJson.h").is_file():
        raise SystemExit("ArduinoJson 6 required in existing motion libdeps; no dependencies installed")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    includes = (json_dir, root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src/ProductCommandResult.cpp",
                root / "shared/ProductBoardLink/test/test_command_result.cpp"]
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    for feature in ("ARDUINO_STRING", "ARDUINO_STREAM", "ARDUINO_PRINT", "PROGMEM"):
        command += ["-DARDUINOJSON_ENABLE_" + feature + "=0"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g"]
    for include in includes:
        command += ["-I", str(include)]
    command += [str(source) for source in sources]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="babytech-product-command-result-") as directory:
        binary = Path(directory) / "product_command_result"
        subprocess.run([*command, "-o", str(binary)], check=True)
        print("ProductCommandResult" + (" / ASan+UBSan" if args.sanitize else ""), flush=True)
        return subprocess.run([str(binary), *([args.case] if args.case else [])],
                              env=environment, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
