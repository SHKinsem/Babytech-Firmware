"""Test production cloud codecs, shared transport helpers and the outbox adapter."""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
arduino_json = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
if not arduino_json.is_dir():
    raise SystemExit("Run `pio run -e motion` in device-controller before this test")
cxx = shutil.which(os.environ.get("CXX", "c++"))
if not cxx:
    raise SystemExit("A C++17 compiler is required")
parser = argparse.ArgumentParser()
parser.add_argument("--sanitize", action="store_true",
                    help="Enable AddressSanitizer and UndefinedBehaviorSanitizer")
parser.add_argument("--emit-fixture", action="store_true",
                    help="Print status, ACK and terminal events from the production codecs")
parser.add_argument("--context-fixture", type=Path,
                    help="Decode the parent Cloud feeding-context fixture with the Motion parser")
args = parser.parse_args()
sanitizer_flags = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
                   if args.sanitize else [])

with tempfile.TemporaryDirectory(prefix="babytech-cloud-contract-") as directory:
    if args.emit_fixture:
        binary = Path(directory) / "emit_product_contract"
        subprocess.run([
            cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            *sanitizer_flags,
            "-I", str(arduino_json),
            "-I", str(root / "device-controller/include"),
            "-I", str(root / "shared/BabytechDisplayCore/src"),
            *(str(root / f"device-controller/src/Product{name}Codec.cpp")
              for name in ("Status", "Ack", "Event")),
            str(root / "tests/emit_product_contract.cpp"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        raise SystemExit(0)
    names = ("context",) if args.context_fixture else (
        "event", "status", "ack", "command_priority", "command_gate",
        "command_dispatcher", "inbound_order", "context", "retry_deadline", "event_state",
        "event_outbox", "identity")
    for name in names:
        binary = Path(directory) / f"product_{name}_codec"
        source = (root / f"device-controller/src/Product{name.title()}Codec.cpp")
        sources = [source]
        test = root / f"tests/test_product_{name}_codec.cpp"
        if name == "command_priority":
            sources = []
            test = root / "tests/test_cloud_command_priority.cpp"
        if name == "command_gate":
            sources = []
            test = root / "tests/test_cloud_command_gate.cpp"
        if name == "inbound_order":
            sources = []
            test = root / "tests/test_cloud_inbound_order.cpp"
        if name == "command_dispatcher":
            sources = [root / path for path in (
                "shared/BabytechDisplayCore/src/display_model.cpp",
                "shared/BabytechDisplayCore/src/display_protocol.cpp",
                "device-controller/src/DemoFlowController.cpp",
                "device-controller/src/ProductSession.cpp",
                "device-controller/src/ProductCommandDispatcher.cpp",
            )]
            test = root / "tests/test_product_command_dispatcher.cpp"
        if name == "retry_deadline":
            sources = []
            test = root / "tests/test_retry_deadline.cpp"
        if name == "event_state":
            sources = [root / f"device-controller/src/{name}.cpp"
                       for name in ("ProductEventState", "ProductEventCodec")]
            test = root / "tests/test_product_event_state.cpp"
        platform_flags = []
        if name == "identity":
            sources = []
            test = root / "tests/test_cloud_identity.cpp"
            platform_flags = ["-I", str(root / "tests/fakes/outbox")]
        if name == "event_outbox":
            sources = [root / path for path in (
                "shared/BabytechDisplayCore/src/display_model.cpp",
                "device-controller/src/DemoFlowController.cpp",
                "device-controller/src/ProductSession.cpp",
                "device-controller/src/ProductEventCodec.cpp",
                "device-controller/src/ProductEventState.cpp",
                "device-controller/src/ProductEventOutbox.cpp",
            )]
            test = root / "tests/test_product_event_outbox.cpp"
            platform_flags = ["-I", str(root / "tests/fakes/outbox")]
        subprocess.run([
            cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            *sanitizer_flags,
            *platform_flags,
            "-I", str(arduino_json),
            "-I", str(root / "device-controller/include"),
            "-I", str(root / "device-controller/src"),
            "-I", str(root / "shared/BabytechCloudLink/src"),
            "-I", str(root / "shared/BabytechDisplayCore/src"),
            *(str(source) for source in sources),
            str(test),
            "-o", str(binary),
        ], check=True)
        command = [str(binary)]
        if name == "context" and args.context_fixture:
            command.append(str(args.context_fixture))
        subprocess.run(command, check=True)
