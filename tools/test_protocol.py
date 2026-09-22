"""Compile and execute protocol tests without PlatformIO or connected hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which(os.environ.get('CXX', 'g++'))
if not compiler:
    raise SystemExit('A C++ compiler is required. Install g++ or set CXX.')
with tempfile.TemporaryDirectory(prefix='babytech-protocol-') as output:
    binary = Path(output) / ('protocol-test.exe' if os.name == 'nt' else 'protocol-test')
    subprocess.run([
        compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'shared/BoardProtocol/src'),
        str(root / 'shared/BoardProtocol/src/BoardProtocol.cpp'),
        str(root / 'shared/BoardProtocol/test/test_protocol.cpp'),
        '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    v2_binary = Path(output) / ('protocol-v2-test.exe' if os.name == 'nt' else 'protocol-v2-test')
    sources = ['BoardProtocol.cpp', 'BoardProtocolV2.cpp', 'BoardEndpoint.cpp', 'BoardClient.cpp']
    subprocess.run([
        compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'shared/BoardProtocol/src'),
        *[str(root / 'shared/BoardProtocol/src' / name) for name in sources],
        str(root / 'shared/BoardProtocol/test/test_v2.cpp'), '-o', str(v2_binary),
    ], check=True)
    subprocess.run([str(v2_binary)], check=True)
