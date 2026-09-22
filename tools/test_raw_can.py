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
        '-I', str(root / 'tests/can_fakes'), '-I', str(root / 'motion/lib/XMotor/src'),
        str(root / 'tests/test_raw_can.cpp'), str(root / 'motion/lib/XMotor/src/X42sProtocol.cpp'),
        '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
