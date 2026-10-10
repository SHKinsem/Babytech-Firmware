"""Test standalone pre-write maintenance using the actual production v4 codec."""
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
    parser.add_argument("--case", help="Run one named C++ scenario (for reproduction)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++17 compiler is required")
    # BoardProtocol.cpp supplies the production CRC; SessionV4 supplies pairing
    # validation. No Arduino, stores, NVS, importer or action implementation.
    sources = ("shared/BoardProtocol/src/BoardProtocol.cpp",
               "shared/BoardProtocol/src/BoardProtocolV4.cpp",
               "shared/BoardProtocol/src/BoardSessionV4.cpp",
               "shared/ProductBoardLink/src/BoardMaintenance.cpp",
               "test/test_board_maintenance.cpp")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in ("shared/BoardProtocol/src", "shared/ProductBoardLink/src"):
        command += ["-I", str(root / include)]
    command += [str(root / source) for source in sources]
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = "halt_on_error=1:" + environment.get("ASAN_OPTIONS", "")
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
    with tempfile.TemporaryDirectory(prefix="babytech-board-maintenance-") as directory:
        binary = Path(directory) / ("maintenance.exe" if os.name == "nt" else "maintenance")
        subprocess.run([*command, "-o", str(binary)], check=True)
        result = subprocess.run([str(binary), *([args.case] if args.case else [])],
                                env=environment, check=False)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
