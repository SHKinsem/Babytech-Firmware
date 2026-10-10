"""Test read-only board discovery with production codecs, without Arduino or NVS."""
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
               "shared/ProductBoardLink/src/BoardDiscovery.cpp",
               "test/test_board_discovery.cpp")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in ("shared/BoardProtocol/src", "shared/ProductBoardLink/src"):
        command += ["-I", str(root / include)]
    command += [str(root / source) for source in sources]
    with tempfile.TemporaryDirectory(prefix="babytech-board-discovery-") as directory:
        binary = Path(directory) / ("discovery.exe" if os.name == "nt" else "discovery")
        subprocess.run([*command, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
