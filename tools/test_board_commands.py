"""Compile ordinary COMMAND/Cloud codecs with production sources and ArduinoJson 6."""
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
    parser.add_argument("--cloud-fixtures", type=Path,
                        help="Decode and UART-roundtrip every Cloud JSON line in this file")
    args = parser.parse_args()
    fixtures = None
    if args.cloud_fixtures is not None:
        fixtures = args.cloud_fixtures.resolve()
        if not fixtures.is_file():
            parser.error(f"Cloud fixtures file does not exist: {fixtures}")
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
               "shared/ProductBoardLink/src/ProductRequest.cpp",
               "shared/ProductBoardLink/src/ProductBoardMessages.cpp",
               "shared/ProductBoardLink/test/test_commands.cpp")
    with tempfile.TemporaryDirectory(prefix="babytech-board-commands-") as directory:
        binary = Path(directory) / "board_commands"
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        for include in includes:
            command += ["-I", str(include)]
        command += [str(root / source) for source in sources]
        subprocess.run([*command, "-o", str(binary)], check=True)
        run = [str(binary)]
        if fixtures is not None:
            run += ["--cloud-fixtures", str(fixtures)]
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            environment["ASAN_OPTIONS"] = "halt_on_error=1"
        subprocess.run(run, check=True, env=environment)


if __name__ == "__main__":
    main()
