"""Run standalone display and demo suites without hardware or network."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cxx = shutil.which(os.environ.get("CXX", "c++"))
cc = shutil.which(os.environ.get("CC", "cc"))
if not cxx or not cc:
    raise SystemExit("C and C++17 compilers required")
core = root / "shared/BabytechDisplayCore"
with tempfile.TemporaryDirectory(prefix="babytech-demo-") as directory:
    directory = Path(directory)
    cjson = directory / "cjson.o"
    subprocess.run([cc, "-c", str(root / "tests/vendor/cjson/cJSON.c"), "-o", str(cjson)], check=True)
    flags = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror"]
    for include in ["device-controller/include", "device-controller/src", "device-controller/lib/XMotor/src", "tests/fakes", "tests/vendor/cjson", "shared/BabytechDisplayCore/src"]:
        flags.extend(["-I", str(root / include)])
    model = str(core / "src/display_model.cpp")
    protocol = str(core / "src/display_protocol.cpp")
    hardware = [str(root / name) for name in ["device-controller/src/CommandQueue.cpp", "device-controller/src/MotorControl.cpp", "tests/fakes/fake_x42s.cpp"]]
    suites = {
        "display-model": [model, str(core / "tests/test_display_model.cpp")],
        "display-protocol": [model, protocol, str(core / "tests/test_display_protocol.cpp")],
        "demo-flow": [model, protocol, str(root / "device-controller/src/DemoFlowController.cpp"), str(root / "tests/test_demo_flow.cpp")],
        "demo-config": hardware + [str(cjson), str(root / "device-controller/src/DemoFlowConfig.cpp"), str(root / "tests/test_demo_config.cpp")],
        "demo-motor": hardware + [str(root / "device-controller/src/DeviceAPI.cpp"), str(cjson), str(root / "device-controller/src/DemoFlowConfig.cpp"), str(root / "device-controller/src/DemoFlowController.cpp"), str(root / "tests/test_demo_motor.cpp")],
    }
    for name, sources in suites.items():
        output = directory / name
        subprocess.run(flags + sources + ["-o", str(output)], check=True)
        args = [str(root / "device-controller/data/demo_flow.json")] if name == "demo-config" else []
        subprocess.run([str(output), *args], check=True)
