"""Test the production CAN driver with a fake TWAI hardware boundary only."""
from pathlib import Path
import os, shutil, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
compiler = shutil.which(os.environ.get('CXX', 'g++'))
if not compiler:
    raise SystemExit('Install g++ or set CXX.')
with tempfile.TemporaryDirectory(prefix='babytech-raw-can-') as folder:
    binary = Path(folder) / ('raw-can.exe' if os.name == 'nt' else 'raw-can')
    subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'tests/can_fakes'), '-I', str(root / 'device-controller/lib/XMotor/src'),
        str(root / 'tests/test_raw_can.cpp'), str(root / 'device-controller/lib/XMotor/src/X42sProtocol.cpp'),
        '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    queue_binary = Path(folder) / ('queue-wire.exe' if os.name == 'nt' else 'queue-wire')
    subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-DTEST_TWAI_RX',
        '-I', str(root / 'tests/can_fakes'), '-I', str(root / 'device-controller/lib/XMotor/src'),
        '-I', str(root / 'device-controller/include'), '-I', str(root / 'device-controller/src'),
        str(root / 'tests/test_queue_wire.cpp'), str(root / 'device-controller/lib/XMotor/src/X42sProtocol.cpp'),
        str(root / 'device-controller/src/MotorControl.cpp'), str(root / 'device-controller/src/CommandQueue.cpp'),
        '-o', str(queue_binary)], check=True)
    subprocess.run([str(queue_binary)], check=True)
