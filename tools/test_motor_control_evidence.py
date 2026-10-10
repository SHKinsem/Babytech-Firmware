"""Narrow real-transport evidence tests plus the existing controller regression."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
requested = os.environ.get('CXX')
if requested is not None:
    compiler = shutil.which(requested)
else:
    candidates = ('clang++', 'g++', 'c++') if args.sanitize else ('g++', 'c++', 'clang++')
    compiler = None
    for name in candidates:
        compiler = shutil.which(name)
        if compiler:
            break
if not compiler:
    raise SystemExit('C++ compiler unavailable')
common = [compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror',
          '-I', str(root / 'device-controller/include'),
          '-I', str(root / 'device-controller/src'),
          '-I', str(root / 'device-controller/lib/XMotor/src')]
if args.sanitize:
    common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
with tempfile.TemporaryDirectory(prefix='babytech-motor-evidence-') as output:
    suites = [
        ('evidence-real-transport', 'tests/test_motor_control_fakes',
         ['tests/test_motor_control_evidence.cpp', 'device-controller/src/MotorControl.cpp',
          'device-controller/src/CommandQueue.cpp', 'device-controller/lib/XMotor/src/X42sProtocol.cpp']),
        ('controller-existing', 'tests/fakes',
         ['tests/test_motor_control.cpp', 'device-controller/src/MotorControl.cpp', 'tests/fakes/fake_x42s.cpp']),
    ]
    for name, include, sources in suites:
        binary = Path(output) / name
        subprocess.run(common + ['-I', str(root / include)] + [str(root / source) for source in sources]
                       + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, cwd=root)
