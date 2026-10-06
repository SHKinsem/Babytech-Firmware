"""Exercise the shared read-only board link over bounded in-memory byte sinks."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

arguments = argparse.ArgumentParser(description=__doc__)
arguments.add_argument("--sanitize", action="store_true")
args = arguments.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = shutil.which(os.environ.get("CXX", "c++"))
json_headers = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
if not compiler or not json_headers.is_dir():
    raise SystemExit("A C++17 compiler and `pio run -d device-controller -e motion` are required")

with tempfile.TemporaryDirectory(prefix="babytech-board-link-") as temporary:
    executable = Path(temporary) / ("link.exe" if os.name == "nt" else "link")
    includes = [json_headers, root / "shared/BoardProtocol/src",
                root / "shared/ProductBoardLink/src", root / "shared/BabytechDisplayCore/src"]
    sources = [root / "shared/BoardProtocol/src" / name for name in
               ("BoardProtocol.cpp", "BoardProtocolV4.cpp", "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources.extend(root / "shared/ProductBoardLink/src" / name for name in
                   ("ProductBoardMessages.cpp", "ReadOnlyBoardLink.cpp"))
    sources.append(root / "shared/ProductBoardLink/test/test_readonly_link.cpp")
    command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    for include in includes:
        command.extend(["-I", str(include)])
    subprocess.run(command + [str(source) for source in sources] + ["-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
