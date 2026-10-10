"""Compile and execute protocol tests without PlatformIO or connected hardware."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if args.sanitize else []
compiler = shutil.which(os.environ.get('CXX', 'g++'))
if not compiler:
    raise SystemExit('A C++ compiler is required. Install g++ or set CXX.')
with tempfile.TemporaryDirectory(prefix='babytech-protocol-') as output:
    binary = Path(output) / ('protocol-test.exe' if os.name == 'nt' else 'protocol-test')
    subprocess.run([
        compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', *sanitize,
        '-I', str(root / 'shared/BoardProtocol/src'),
        str(root / 'shared/BoardProtocol/src/BoardProtocol.cpp'),
        str(root / 'shared/BoardProtocol/test/test_protocol.cpp'),
        '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    v2_binary = Path(output) / ('protocol-v2-test.exe' if os.name == 'nt' else 'protocol-v2-test')
    sources = ['BoardProtocol.cpp', 'BoardProtocolV2.cpp', 'BoardEndpoint.cpp', 'BoardClient.cpp']
    subprocess.run([
        compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', *sanitize,
        '-I', str(root / 'shared/BoardProtocol/src'),
        *[str(root / 'shared/BoardProtocol/src' / name) for name in sources],
        str(root / 'shared/BoardProtocol/test/test_v2.cpp'), '-o', str(v2_binary),
    ], check=True)
    subprocess.run([str(v2_binary)], check=True)
    v4_binary = Path(output) / ('protocol-v4-test.exe' if os.name == 'nt' else 'protocol-v4-test')
    subprocess.run([
        compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', *sanitize,
        '-I', str(root / 'shared/BoardProtocol/src'),
        str(root / 'shared/BoardProtocol/src/BoardProtocol.cpp'),
        str(root / 'shared/BoardProtocol/src/BoardProtocolV4.cpp'),
        str(root / 'shared/BoardProtocol/test/test_v4.cpp'), '-o', str(v4_binary),
    ], check=True)
    subprocess.run([str(v4_binary)], check=True)
    for name, source in [('session', 'BoardSessionV4.cpp'), ('transmit', 'BoardTransmitV4.cpp')]:
        binary = Path(output) / (f'v4-{name}.exe' if os.name == 'nt' else f'v4-{name}')
        subprocess.run([
            compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', *sanitize,
            '-I', str(root / 'shared/BoardProtocol/src'),
            str(root / 'shared/BoardProtocol/src/BoardProtocol.cpp'),
            str(root / 'shared/BoardProtocol/src/BoardProtocolV4.cpp'),
            str(root / 'shared/BoardProtocol/src' / source),
            str(root / f'shared/BoardProtocol/test/test_{name}_v4.cpp'), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
