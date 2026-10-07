"""Package an already-tested device controller build. Never opens a serial port."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import zipfile
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--release', required=True, help='Release identifier, e.g. 20260923-rc1')
args = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.release):
    parser.error('Release must contain only letters, digits, dot, underscore or hyphen')

root = Path(__file__).resolve().parents[1]
build = root / 'out/wsl/motion'
release = root / 'out/releases' / ('device-controller-' + args.release)
html = (root / 'device-controller/data/index.html').read_bytes()
firmware = (build / 'firmware.bin').read_bytes()
if html not in firmware:
    raise SystemExit('Firmware does not contain the exact current embedded HTML; rebuild first.')
release.mkdir(parents=True, exist_ok=False)

artifacts = ['bootloader.bin', 'partitions.bin', 'boot_app0.bin', 'firmware.bin', 'firmware.elf', 'build-info.txt']
for name in artifacts:
    shutil.copyfile(build / name, release / name)
shutil.copyfile(root / 'docs/motion-workbench-release.md', release / 'README.md')
qa = root / 'tools/motor-protocol-demo/qa'
for name in ['device-results.json', 'device-lab-1513.png', 'device-wifi-1513.png', 'device-manual-1280.png']:
    shutil.copyfile(qa / name, release / name)

layout = {
    'chip': 'esp32s3', 'flashSize': '16MB', 'hardwareVerified': False,
    'parts': [{'offset': offset, 'file': file} for offset,file in [
        ('0x0000','bootloader.bin'), ('0x8000','partitions.bin'),
        ('0xE000','boot_app0.bin'), ('0x10000','firmware.bin')]],
    'embeddedHtmlSha256': hashlib.sha256(html).hexdigest(),
}
(release / 'flash-layout.json').write_text(json.dumps(layout,ensure_ascii=False,indent=2),encoding='utf-8')

sources = set()
for folder in ['main-controller','device-controller','shared','boards','tests','test']:
    for file in (root / folder).rglob('*'):
        if not file.is_file() or any(p in {'.pio','.git','__pycache__','node_modules'} for p in file.parts):
            continue
        if file.suffix.lower() in {'.cpp','.h','.c','.ini','.json','.md','.html','.py','.cjs','.txt'}:
            sources.add(file)
sources.update((root / 'docs').glob('*.md'))
for name in ['build-wsl.ps1','build-wsl.sh','test_protocol.py','test_motion.py','package-device.py',
             'test_raw_can.py','test_demo.py','test_cloud_contract.py',
             'export_boot_app0.py','test_display_view.py','test_board_messages.py','test_board_commands.py','test_board_link.py',
             'test_pairing_record.py','test_board_arduino.py','test_board_discovery.py',
             'test_motion_export_snapshot.py','test_board_export_transfer.py','test_board_maintenance.py',
             'test_board_install.py','test_motion_install.py','test_brain_installer.py','test_brain_pending_recovery.py','test_brain_cloud_dispatcher.py',
             'test_brain_local_dispatcher.py',
             'test_brain_context_sync.py',
             'test_cloud_session.py','test_cloud_link.py',
             'test_brain_network.py','test_brain_station.py','test_brain_network_console.py',
             'configure_brain_network.py','test_brain_controller.py',
             'prepare_board_pairing.py','test_pairing_store.py',
             'test_product_context.py','test_product_context_messages.py','test_product_event_messages.py','test_legacy_context_store.py',
             'test_product_state.py','test_brain_state_store.py',
             'test_motion_state_record.py','test_motion_state_store.py','test_product_result_query.py','test_product_command_result.py','test_motion_product_runtime.py','test_motion_state_recovery.py','test_motion_result_queue.py','test_board_commissioning.py',
             'test_maintenance_console.py','test_maintenance_export.py','capture_board_export.py']:
    sources.add(root / 'tools' / name)
sources.add(root / 'README.md')
front = root / 'tools/motor-protocol-demo'
for folder in ['src','scripts','tests','worker','.openai']:
    sources.update(p for p in (front / folder).rglob('*') if p.is_file())
sources.update(p for p in (front / 'references').glob('*') if p.suffix in {'.h','.cpp'})
for name in ['package.json','package-lock.json','vite.config.js','index.html','README.md','AGENTS.md','qa-device.mjs','design-qa.md','claude-development.md']:
    if (front / name).is_file(): sources.add(front / name)
source_zip = release / 'source.zip'
with zipfile.ZipFile(source_zip, 'w', zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
    for file in sorted(sources): archive.write(file,file.relative_to(root))

hashes = {p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(release.iterdir()) if p.is_file() and p.name != 'SHA256SUMS.json'}
(release / 'SHA256SUMS.json').write_text(json.dumps(hashes,indent=2),encoding='utf-8')
bundle = release.parent / (release.name + '.zip')
with zipfile.ZipFile(bundle,'w',zipfile.ZIP_DEFLATED, strict_timestamps=False) as archive:
    for file in sorted(release.iterdir()):
        if file.is_file(): archive.write(file,release.name + '/' + file.name)
print(json.dumps({'bundle':str(bundle),'bytes':bundle.stat().st_size,'sourceFiles':len(sources),'embeddedHtmlMatchesFirmware':True},ensure_ascii=False,indent=2))
