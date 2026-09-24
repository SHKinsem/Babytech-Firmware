"""Flash a reviewed ESP32-S3 N16R8 package; default is offline preflight only."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
PARTS = ((0, 'bootloader.bin'), (0x8000, 'partitions.bin'),
         (0xE000, 'boot_app0.bin'), (0x10000, 'firmware.bin'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def partition_entries(data):
    entries = []
    for pos in range(0, len(data) - 31, 32):
        magic = data[pos:pos + 2]
        if magic in (b'\xff\xff', b'\xeb\xeb'):
            break
        if magic != b'\xaa\x50':
            raise ValueError('Invalid ESP partition table')
        _, kind, subtype, offset, size, label, flags = struct.unpack('<HBBII16sI', data[pos:pos + 32])
        entries.append((kind, subtype, offset, size, label.rstrip(b'\0'), flags))
    if not entries:
        raise ValueError('Empty partition table')
    return entries


def inspect_package(package):
    blobs = {name: (package / name).read_bytes() for _, name in PARTS}
    expected = {}
    if (package / 'SHA256SUMS.json').is_file():
        expected = json.loads((package / 'SHA256SUMS.json').read_text(encoding='utf-8-sig'))
    elif (package / 'SHA256SUMS.txt').is_file():
        for line in (package / 'SHA256SUMS.txt').read_text(encoding='utf-8-sig').splitlines():
            match = re.fullmatch(r'([0-9a-fA-F]{64})\s+\*?(.+)', line.strip())
            if match:
                expected[match[2]] = match[1]
    else:
        raise ValueError('Package needs SHA256SUMS.json or SHA256SUMS.txt from packaging')
    for name, data in blobs.items():
        if not data or sha(data) != str(expected.get(name, '')).lower():
            raise ValueError(f'Missing/mismatched package hash: {name}')
    for name in ('bootloader.bin', 'firmware.bin'):
        data = blobs[name]
        if len(data) < 24 or data[0] != 0xE9 or int.from_bytes(data[12:14], 'little') != 9:
            raise ValueError(f'{name} is not an ESP32-S3 image')
    if len(blobs['bootloader.bin']) > 0x8000 or len(blobs['partitions.bin']) > 0x1000 or len(blobs['boot_app0.bin']) != 0x2000:
        raise ValueError('Images exceed the supported flash regions')
    entries = partition_entries(blobs['partitions.bin'])
    if not any(e[:4] == (1, 2, 0x9000, 0x5000) for e in entries):
        raise ValueError('Expected NVS region 0x9000..0xDFFF not found')
    if not any(e[0] == 0 and e[2] == 0x10000 and e[3] >= len(blobs['firmware.bin']) for e in entries):
        raise ValueError('Application does not fit the partition at 0x10000')
    for i, entry in enumerate(entries):
        if entry[2] < 0x9000 or entry[3] <= 0 or entry[2] + entry[3] > 16 * 1024 * 1024:
            raise ValueError('Partition outside supported 16 MB layout')
        for other in entries[i + 1:]:
            if max(entry[2], other[2]) < min(entry[2] + entry[3], other[2] + other[3]):
                raise ValueError('Overlapping partitions')
    return blobs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--execute', action='store_true', help='Actually connect, back up and flash')
    parser.add_argument('--port', help='Explicit Windows COM port; never auto-selected')
    parser.add_argument('--expected-mac', help='Required device MAC, e.g. 3c:0f:02:c5:fe:64')
    parser.add_argument('--esptool', type=Path, default=Path.home() / '.platformio/packages/tool-esptoolpy/esptool.py')
    parser.add_argument('--baud', type=int, choices=[115200, 460800, 921600], default=460800)
    parser.add_argument('--monitor-seconds', type=int, choices=range(0, 61), default=10, metavar='0..60')
    args = parser.parse_args(argv)
    package = args.package.resolve()
    blobs = inspect_package(package)
    plan = {'package': str(package), 'chip': 'esp32s3', 'flashSize': '16MB',
            'parts': [{'offset': hex(offset), 'file': name, 'size': len(blobs[name]), 'sha256': sha(blobs[name])}
                      for offset, name in PARTS]}
    if not args.execute:
        print(json.dumps({'mode': 'offline-preflight', 'serialOpened': False, **plan}, indent=2))
        return 0
    if not args.port or not re.fullmatch(r'COM[1-9][0-9]*', args.port, re.I):
        raise ValueError('--execute requires an explicit COM port')
    if not args.expected_mac or not re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', args.expected_mac, re.I):
        raise ValueError('--execute requires --expected-mac')
    if not args.esptool.is_file():
        raise ValueError('esptool not found; use --esptool PATH')
    run = ROOT / 'out/flash-runs' / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])
    run.mkdir(parents=True)
    # Snapshot reviewed images: concurrent builds cannot change files mid-flash.
    for name, data in blobs.items():
        (run / name).write_bytes(data)
    report = {**plan, 'port': args.port, 'expectedMac': args.expected_mac.lower(),
              'status': 'started', 'writeStarted': False, 'hardwareFunctionsVerified': False}

    def command(label, *arguments, reset=False):
        cmd = [sys.executable, str(args.esptool), '--chip', 'esp32s3', '--port', args.port,
               '--baud', str(args.baud), '--after', 'hard_reset' if reset else 'no_reset', *map(str, arguments)]
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                encoding='utf-8', errors='replace', timeout=240)
        (run / (label + '.log')).write_text(result.stdout, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(f'{label} failed; see {run / (label + ".log")}')
        return result.stdout

    try:
        identity = command('identity', 'flash_id')
        if args.expected_mac.lower() not in identity.lower():
            raise ValueError('Device MAC mismatch: no write performed')
        if not re.search(r'flash size:\s*16\s*MB', identity, re.I):
            raise ValueError('Device does not report 16 MB flash')
        report['identityChecked'] = True
        backup = run / 'before-boot-nvs.bin'
        command('backup', 'read_flash', '0', '0x10000', backup)
        before = backup.read_bytes()
        if len(before) != 0x10000:
            raise ValueError('Incomplete backup')
        report['backupSha256'] = sha(before)
        if partition_entries(before[0x8000:0x9000]) != partition_entries(blobs['partitions.bin']):
            raise ValueError('Partition layout differs: automatic migration refused; no write performed')
        image_args = [part for offset, name in PARTS for part in (hex(offset), str(run / name))]
        report['writeStarted'] = True
        command('write', 'write_flash', '--flash_mode', 'keep', '--flash_freq', 'keep', '--flash_size', 'keep', *image_args)
        command('verify', 'verify_flash', *image_args)
        nvs = run / 'after-nvs.bin'
        command('nvs-check', 'read_flash', '0x9000', '0x5000', nvs, reset=True)
        if nvs.read_bytes() != before[0x9000:0xE000]:
            raise RuntimeError('NVS verification mismatch; backup retained; no automatic restore')
        report.update(status='flash-verified', nvsUnchanged=True)
        if args.monitor_seconds:
            try:
                import serial
                port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
                port.dtr = False
                port.rts = False
                port.port = args.port
                captured = bytearray()
                with port:
                    end = time.monotonic() + args.monitor_seconds
                    while time.monotonic() < end:
                        captured.extend(port.read(4096))
                text = captured.decode('utf-8', errors='replace')
                (run / 'monitor.log').write_text(text, encoding='utf-8')
                report['bootBannerObserved'] = 'Babytech' in text
                report['httpReadyObserved'] = '[http]' in text and '80' in text
            except Exception as exc:
                report['monitorError'] = str(exc)
        return 0
    except Exception as exc:
        report.update(status='failed', error=str(exc))
        raise
    finally:
        report['finishedUtc'] = datetime.now(timezone.utc).isoformat()
        (run / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'Report: {run / "result.json"}')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
