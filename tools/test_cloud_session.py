"""Test the production cloud-session state without network, hardware or PIO."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if not compiler:
        raise SystemExit("A C++ compiler is required")
    source = root / "shared/BabytechCloudLink/src"
    flags = ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="babytech-cloud-session-") as temporary:
        binary = Path(temporary) / ("session.exe" if os.name == "nt" else "session")
        subprocess.run([compiler, *flags, "-I", str(source),
                        str(source / "CloudSession.cpp"),
                        str(root / "test/test_cloud_session.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
