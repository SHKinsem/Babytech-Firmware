"""Test production durable codecs with system SHA-256 and both mbedTLS APIs."""
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
    parser.add_argument("--mbedtls-major", choices=("2", "3", "both"), default="both")
    parser.add_argument("--case", help="Run one C++ test group")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    json_dir = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (json_dir / "ArduinoJson.h").is_file():
        raise SystemExit("ArduinoJson 6 required in existing motion libdeps; no dependencies installed")
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    if not cxx:
        raise SystemExit("A C++17 compiler is required")
    if sys.platform not in ("darwin", "linux"):
        raise SystemExit("System SHA-256 backend supports Apple or Linux only")
    includes = (json_dir, root / "tests/fakes/product_crypto",
                root / "shared/BoardProtocol/src", root / "shared/ProductBoardLink/src",
                root / "shared/BabytechDisplayCore/src")
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp")]
    sources += [root / "shared/ProductBoardLink/src" / name for name in
                ("ProductContext.cpp", "ProductRequest.cpp", "ProductDigest.cpp",
                 "BrainStateRecord.cpp", "BoardPairingRecord.cpp")]
    sources += [root / "tests/fakes/product_crypto/FakeProductCrypto.cpp",
                root / "shared/ProductBoardLink/test/test_state_record.cpp"]
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = env.get("UBSAN_OPTIONS", "") + ":halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="babytech-product-state-") as directory:
        for major in (("2", "3") if args.mbedtls_major == "both" else (args.mbedtls_major,)):
            binary = Path(directory) / ("product_state_" + major)
            command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-DMBEDTLS_VERSION_MAJOR=" + major]
            if sys.platform == "darwin":
                command += ["-Wno-deprecated-declarations"]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
            for include in includes:
                command += ["-I", str(include)]
            command += [str(source) for source in sources]
            if sys.platform == "linux":
                command += ["-lcrypto"]
            subprocess.run([*command, "-o", str(binary)], check=True)
            print("Testing mbedTLS " + major, flush=True)
            subprocess.run([str(binary), *([args.case] if args.case else [])], check=True, env=env)


if __name__ == "__main__":
    main()
