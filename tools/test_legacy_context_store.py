"""Test the production legacy STRING loader and codec with isolated SDK fakes.

No PlatformIO, network, device or Flash access; build products are temporary.
Requires the existing local ArduinoJson 6 dependency, never downloads it.
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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    arduino_json = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (arduino_json / "ArduinoJson.h").is_file():
        raise SystemExit("Run `pio run -e motion` in device-controller before this test")
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    sources = ("shared/BoardProtocol/src/BoardProtocol.cpp",
               "shared/BoardProtocol/src/BoardProtocolV4.cpp",
               "shared/BoardProtocol/src/BoardSessionV4.cpp",
               "shared/ProductBoardLink/src/ProductContext.cpp",
               "shared/ProductBoardLink/src/LegacyContextStore.cpp",
               "tests/fakes/legacy_context_store/FakeLegacyNvs.cpp",
               "test/test_legacy_context_store.cpp")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
               "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
               "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0"]
    environment = os.environ.copy()
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g"]
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
            environment[name] = environment.get(name, "") + ":halt_on_error=1"
    for include in (root / "tests/fakes/legacy_context_store", arduino_json,
                    root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src"):
        command += ["-I", str(include)]
    command += [str(root / source) for source in sources]
    with tempfile.TemporaryDirectory(prefix="babytech-legacy-context-") as directory:
        binary = Path(directory) / ("legacy_context.exe" if os.name == "nt" else "legacy_context")
        subprocess.run([*command, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment)


if __name__ == "__main__":
    main()
