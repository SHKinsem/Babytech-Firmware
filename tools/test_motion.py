"""Run pure motion core and actual controller tests without connected hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
compiler = shutil.which(os.environ.get('CXX', 'g++'))
if not compiler:
    raise SystemExit('Install g++ or set CXX.')
with tempfile.TemporaryDirectory(prefix='babytech-motion-') as output:
    common = [compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'device-controller/include'), '-I', str(root / 'device-controller/lib/XMotor/src')]
    suites = [
        ('config-recovery', ['-I', str(root / 'tests/fakes'), '-I', str(root / 'device-controller/src'),
            str(root / 'tests/test_config_recovery.cpp'), str(root / 'tests/fakes/fake_x42s.cpp'),
            str(root / 'device-controller/src/MotorControl.cpp'), str(root / 'device-controller/src/CommandQueue.cpp')]),
        ('core', [str(root / 'tests/test_motion_core.cpp')]),
        ('debug-log', ['-I', str(root / 'device-controller/src'),
            str(root / 'tests/test_debug_log.cpp')]),
        ('load-cell', ['-I', str(root / 'device-controller/lib/LoadCell/src'),
            str(root / 'tests/test_load_cell.cpp'),
            str(root / 'device-controller/lib/LoadCell/src/LoadCellProcessor.cpp')]),
        ('controller', ['-I', str(root / 'tests/fakes'), '-I', str(root / 'device-controller/src'),
            str(root / 'tests/test_motor_control.cpp'), str(root / 'tests/fakes/fake_x42s.cpp'),
            str(root / 'device-controller/src/MotorControl.cpp')]),
        ('board-motion', ['-I', str(root / 'tests/fakes'), '-I', str(root / 'device-controller/src'),
            '-I', str(root / 'shared/BoardProtocol/src'),
            str(root / 'tests/test_board_motion.cpp'), str(root / 'tests/fakes/fake_x42s.cpp'),
            str(root / 'device-controller/src/MotorControl.cpp'), str(root / 'device-controller/src/BoardMotion.cpp'),
            *[str(root / 'shared/BoardProtocol/src' / name) for name in
              ['BoardProtocol.cpp', 'BoardProtocolV2.cpp', 'BoardEndpoint.cpp']]]),
        ('command-queue', ['-I', str(root / 'tests/fakes'), '-I', str(root / 'device-controller/src'),
            str(root / 'tests/test_command_queue.cpp'), str(root / 'tests/fakes/fake_x42s.cpp'),
            str(root / 'device-controller/src/MotorControl.cpp'), str(root / 'device-controller/src/CommandQueue.cpp')]),
        ('queue-uart', ['-I', str(root / 'tests/fakes'), '-I', str(root / 'device-controller/src'),
            '-I', str(root / 'shared/BoardProtocol/src'),
            str(root / 'tests/test_queue_uart.cpp'), str(root / 'tests/fakes/fake_x42s.cpp'),
            str(root / 'device-controller/src/MotorControl.cpp'), str(root / 'device-controller/src/CommandQueue.cpp'),
            str(root / 'device-controller/src/BoardMotion.cpp'),
            *[str(root / 'shared/BoardProtocol/src' / name) for name in
              ['BoardProtocol.cpp', 'BoardProtocolV2.cpp', 'BoardEndpoint.cpp']]]),
    ]
    for name, sources in suites:
        binary = Path(output) / (name + ('.exe' if os.name == 'nt' else ''))
        subprocess.run(common + sources + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
