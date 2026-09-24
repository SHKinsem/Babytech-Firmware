"""Sign one reviewed ESP32-S3 application image for browser OTA.

The private key defaults to ignored out/ota/signing-key.pem. Never ship that
file with the release. Use an offline production key before field deployment.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BOARDS = ('brain', 'motion')
PROJECT_DIRS = {'brain': 'main-controller', 'motion': 'device-controller'}
BOARD_ALIASES = {value: key for key, value in PROJECT_DIRS.items()}


def identity(board):
    header = (ROOT / PROJECT_DIRS[board] / 'include/ota_identity.h').read_text(encoding='utf-8')
    values = {}
    for key in ('BOARD', 'HARDWARE', 'VERSION', 'BUILD'):
        match = re.search(rf'^#define BABYTECH_OTA_{key} (.+)$', header, re.M)
        if not match:
            raise ValueError(f'Missing OTA {key} for {board}')
        raw = match.group(1).strip()
        values[key.lower()] = int(raw) if key == 'BUILD' else json.loads(raw)
    if values['board'] != board or values['hardware'] != 'esp32-s3-n16r8':
        raise ValueError('Board identity does not match the release target')
    return values


def ota_slot_size(path):
    data = path.read_bytes()
    slots = []
    for pos in range(0, len(data) - 31, 32):
        entry = data[pos:pos + 32]
        if entry[:2] != b'\xaa\x50':
            break
        _, kind, subtype, _offset, size, _label, _flags = struct.unpack('<HBBII16sI', entry)
        if kind == 0 and subtype in (0x10, 0x11):
            slots.append(size)
    if len(slots) != 2 or slots[0] != slots[1]:
        raise ValueError('Build must have two equal OTA application slots')
    return slots[0]


def canonical(manifest):
    return ('BABYTECH-OTA-V1\n' + '\n'.join(str(manifest[key]) for key in
            ('board', 'hardware', 'build', 'version', 'size', 'sha256')) + '\n').encode('ascii')


def run_openssl(*args, input_bytes=None):
    result = subprocess.run(['openssl', *map(str, args)], input=input_bytes,
                            capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError('OpenSSL failed: ' + result.stderr.decode('utf-8', 'replace'))
    return result.stdout


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', required=True, choices=(*BOARDS, *BOARD_ALIASES))
    parser.add_argument('--build-dir', type=Path, help='Directory containing firmware.bin and partitions.bin')
    parser.add_argument('--signing-key', type=Path, default=ROOT / 'out/ota/signing-key.pem')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    args.board = BOARD_ALIASES.get(args.board, args.board)

    fields = identity(args.board)
    build_dir = args.build_dir or ROOT / 'out/wsl' / args.board
    image_path = build_dir / 'firmware.bin'
    partition_path = build_dir / 'partitions.bin'
    image = image_path.read_bytes()
    if len(image) < 1024 or image[0] != 0xE9 or int.from_bytes(image[12:14], 'little') != 9:
        raise ValueError('Not an ESP32-S3 application image')
    if len(image) > ota_slot_size(partition_path):
        raise ValueError('Firmware exceeds one OTA application slot')
    image_id = (f"BABYTECH-OTA-IMAGE-V1|{fields['board']}|{fields['hardware']}|"
                f"{fields['build']}|{fields['version']}").encode('ascii')
    if image_id not in image:
        raise ValueError('Firmware does not contain the expected board and build identity')
    source_roots = [ROOT / PROJECT_DIRS[args.board] / folder for folder in ('src', 'include', 'data', 'lib')]
    source_roots += [ROOT / 'shared/WifiOta/src', ROOT / 'shared/BoardProtocol/src',
                     ROOT / 'shared/BabytechDisplayCore/src']
    newest_source = max(path.stat().st_mtime for folder in source_roots
                        for path in folder.rglob('*') if path.is_file())
    if image_path.stat().st_mtime < newest_source:
        raise ValueError('Image predates project sources; rebuild first')

    manifest = {**fields, 'size': len(image), 'sha256': hashlib.sha256(image).hexdigest()}
    if not args.signing_key.is_file():
        raise ValueError('Signing key missing; supply --signing-key (never commit the private key)')
    public = run_openssl('pkey', '-in', args.signing_key, '-pubout').decode('ascii')
    committed = (ROOT / 'shared/WifiOta/src/OtaPublicKey.h').read_text(encoding='utf-8')
    if not all(line in committed for line in public.splitlines() if not line.startswith('-----')):
        raise ValueError('Signing key does not match the public key embedded in firmware')
    manifest['signature'] = run_openssl('dgst', '-sha256', '-sign', args.signing_key,
                                        input_bytes=canonical(manifest)).hex()

    output = args.output or ROOT / 'out/ota/releases' / f"{args.board}-{fields['version']}-build{fields['build']}"
    output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(image_path, output / 'firmware.bin')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'release': str(output), 'board': args.board, 'version': fields['version'],
                      'build': fields['build'], 'bytes': len(image)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
