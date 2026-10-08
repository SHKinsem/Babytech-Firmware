"""Run unchanged Motion setup/loop and HTTP routes with SDK boundary fakes.

Real motor protocol, queue, product owners, stores, WiFiSetup and WifiOta are
compiled. No physical CAN/UART/Flash, broker, MCU timing or OTA install proof.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

CASES = ("empty-boot", "paired-boot", "recovery-default-budget", "recovery-intent", "http-workbench",
        "http-partial-tx", "uart-context-command", "sdk-crypto", "http-product-guards",
        "http-history-debug", "http-product-stop-failure",
        "http-maintenance-usb", "http-maintenance-uart-release", "http-maintenance-uart-expiry",
        "http-maintenance-busy",
        *tuple("http-product-stop-" + str(i) for i in range(9)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--pipe", action="store_true", help="Run actual Motion main with JSONL host SDK UART I/O")
    parser.add_argument("--seed-history", action="store_true", help="Initially archive four historical LocalTouch results")
    parser.add_argument("--state-file", type=Path, help="Persist/restore raw fake SDK NVS snapshot array")
    parser.add_argument("--context-file", type=Path, help="Initial real Cloud feeding_context JSON for bt-main-test")
    parser.add_argument("--boot-id", type=int, help="Host SDK random seed, 1..uint32_max-32 (not literal UART boot ID)")
    parser.add_argument("--build-output", type=Path, help="Build binary at PATH and return without executing any scenario")
    args = parser.parse_args()
    if args.pipe and args.case:
        parser.error("--pipe and --case are mutually exclusive")
    if not args.pipe and (args.seed_history or args.state_file is not None or
                          args.context_file is not None or args.boot_id is not None):
        parser.error("history/state/context/boot options require --pipe")
    if args.boot_id is not None and not 1 <= args.boot_id <= 2**32 - 33:
        parser.error("--boot-id must be 1..uint32_max-32")
    root = Path(__file__).resolve().parents[1]
    headers = root / "device-controller/.pio/libdeps/motion/ArduinoJson/src"
    if not (headers / "ArduinoJson.h").is_file():
        raise SystemExit("Existing ArduinoJson 6 required; no dependencies installed")
    cxx = shutil.which(os.environ.get("CXX", "c++"))
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cxx or not cc or sys.platform not in ("darwin", "linux"):
        raise SystemExit("C/C++17 and Apple/Linux crypto backend required")
    includes = ("tests/fakes/motion_main", "tests/fakes/commissioning",
                "tests/fakes/brain_state_store", "tests/fakes/cloud_link",
                "device-controller/include", "device-controller/src",
                "device-controller/lib/XMotor/src", "device-controller/lib/LoadCell/src",
                "shared/BoardProtocol/src", "shared/ProductBoardLink/src",
                "shared/BabytechDisplayCore/src", "shared/BabytechCloudLink/src",
                "shared/WifiOta/src", "tests/vendor/cjson")
    sources = ["shared/BoardProtocol/src/" + name for name in (
        "BoardProtocol.cpp", "BoardProtocolV2.cpp", "BoardEndpoint.cpp", "BoardProtocolV4.cpp",
        "BoardSessionV4.cpp", "BoardTransmitV4.cpp")]
    sources += [str(p.relative_to(root)) for p in sorted((root / "shared/ProductBoardLink/src").glob("*.cpp"))]
    sources += ["device-controller/src/" + name for name in (
        "MotorControl.cpp", "CommandQueue.cpp", "BoardMotion.cpp", "DemoFlowController.cpp",
        "DemoFlowConfig.cpp", "ProductSession.cpp", "MotionStateRecovery.cpp", "MotionProductRuntime.cpp",
        "ProductEventOutbox.cpp", "ProductEventState.cpp", "ProductEventCodec.cpp",
        "ProductContextCodec.cpp", "WiFiSetup.cpp")]
    sources += ["device-controller/lib/XMotor/src/X42sProtocol.cpp",
                "device-controller/lib/LoadCell/src/Hx711Scale.cpp",
                "device-controller/lib/LoadCell/src/LoadCellProcessor.cpp",
                "shared/BabytechDisplayCore/src/display_model.cpp",
                "shared/BabytechDisplayCore/src/display_protocol.cpp", "shared/WifiOta/src/WifiOta.cpp",
                "tests/fakes/brain_state_store/FakeBrainNvs.cpp",
                "tests/fakes/motion_main/FakeMotionNvs.cpp", "tests/fakes/motion_main/FakeMotionIo.cpp",
                "tests/test_motion_main.cpp"]
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-ffunction-sections", "-fdata-sections", "-DARDUINO=10819", "-DMOTION_UART_PEER=4",
             "-DMBEDTLS_VERSION_MAJOR=2", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
             "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
             "-DARDUINOJSON_ENABLE_PROGMEM=1"]
    if sys.platform == "darwin":
        flags += ["-Wno-deprecated-declarations", "-Wl,-dead_strip", "-framework", "Security", "-framework", "CoreFoundation"]
    else:
        flags += ["-Wl,--gc-sections"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    for include in includes:
        flags += ["-I", str(root / include)]
    flags += ["-I", str(headers)]
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="babytech-motion-main-") as tmp:
        directory = Path(tmp)
        cjson = directory / "cjson.o"
        cflags = ["-ffunction-sections", "-fdata-sections", "-Wno-deprecated-declarations"]
        if args.sanitize:
            cflags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        subprocess.run([cc, *cflags, "-c", str(root / "tests/vendor/cjson/cJSON.c"), "-o", str(cjson)], check=True, timeout=60)
        # Generate linker assets from the same source files embedded by PIO.
        assets = directory / "assets.cpp"
        content = ['#include <cstdint>', 'extern "C" {']
        for name, file in (("index_html", "index.html"), ("demo_flow_json", "demo_flow.json")):
            data = (root / "device-controller/data" / file).read_bytes() + b"\0"
            symbol = "_binary_data_" + name + "_start"
            content.append('extern const uint8_t asset_' + name + '[] asm("' + symbol + '") = {' + ','.join(map(str, data)) + '};')
            if name == "index_html":
                content.append('asm(".globl _binary_data_index_html_end\\n.set _binary_data_index_html_end, _binary_data_index_html_start+' + str(len(data)) + '");')
        content.append('}')
        assets.write_text('\n'.join(content) + '\n')
        binary = args.build_output.resolve() if args.build_output is not None else directory / "motion_main"
        command = [cxx, *flags, *[str(root / source) for source in sources], str(assets), str(cjson)]
        if sys.platform == "linux":
            command += ["-lcrypto"]
        subprocess.run([*command, "-o", str(binary)], check=True, timeout=180)
        if args.build_output is not None:
            return
        if args.pipe:
            pipe_args = [str(binary), "dual-bridge"]
            if args.seed_history:
                pipe_args.append("--seed-history")
            for name, value in (("--state-file", args.state_file), ("--context-file", args.context_file),
                                ("--boot-id", args.boot_id)):
                if value is not None:
                    pipe_args += [name, str(value)]
            subprocess.run(pipe_args, env=env, check=True, timeout=300)
            return
        for case in args.case or CASES:
            subprocess.run([str(binary), case], env=env, check=True, timeout=30)
    print("PASS production Motion main: " + str(len(args.case or CASES)) + " isolated processes")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit("Motion main test command failed (exit " + str(error.returncode) + ")") from None
