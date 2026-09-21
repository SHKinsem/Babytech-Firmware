"""Package an already-tested Motion build. Never opens a serial port."""
from pathlib import Path
import hashlib
import json
import shutil
import zipfile

root = Path(__file__).resolve().parents[1]
build = root / 'out/wsl/motion'
release = root / 'out/releases/motion-workbench-20260921'
release.mkdir(parents=True, exist_ok=True)
html = (root / 'motion/data/index.html').read_bytes()
firmware = (build / 'firmware.bin').read_bytes()
if html not in firmware:
    raise SystemExit('Firmware does not contain the exact current embedded HTML; rebuild first.')

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
for folder in ['brain','motion','shared','tests']:
    for file in (root / folder).rglob('*'):
        if not file.is_file() or any(p in {'.pio','.git','__pycache__','node_modules'} for p in file.parts):
            continue
        if file.suffix.lower() in {'.cpp','.h','.c','.ini','.json','.md','.html','.py','.cjs','.txt'}:
            sources.add(file)
sources.update((root / 'docs').glob('*.md'))
for name in ['build-wsl.ps1','build-wsl.sh','test_protocol.py','test_motion.py','package-motion.py']:
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
