"""Link production pairing storage/codec/session to isolated SDK/NVS/MAC fakes.

No PlatformIO, network, device or Flash access. All build products are temporary.
This exercises storage decisions, not commissioning gates or Flash durability.
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
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    sources = ("shared/BoardProtocol/src/BoardProtocol.cpp",
               "shared/BoardProtocol/src/BoardProtocolV4.cpp",
               "shared/BoardProtocol/src/BoardSessionV4.cpp",
               "shared/ProductBoardLink/src/BoardPairingRecord.cpp",
               "shared/ProductBoardLink/src/BoardPairingStore.cpp",
               "tests/fakes/pairing_store/FakePairingNvs.cpp",
               "test/test_pairing_store.cpp")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-DARDUINO=10819"]
    environment = os.environ.copy()
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g"]
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
            environment[name] = environment.get(name, "") + ":halt_on_error=1"
    for include in ("tests/fakes/pairing_store", "shared/BoardProtocol/src",
                    "shared/ProductBoardLink/src"):
        command += ["-I", str(root / include)]
    command += [str(root / source) for source in sources]
    with tempfile.TemporaryDirectory(prefix="babytech-pairing-store-") as directory:
        binary = Path(directory) / ("pairing_store.exe" if os.name == "nt" else "pairing_store")
        subprocess.run([*command, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment)


if __name__ == "__main__":
    main()
