"""Sync parent-owned UI copy before Brain builds; standalone builds use the header."""
import configparser
from pathlib import Path
import subprocess
import sys


def sync_feeding_flow_ui(project_dir):
    firmware = Path(project_dir).resolve().parent
    root = firmware.parent.parent
    if (root / "Embeded_System/babytech-firmware").resolve() != firmware:
        return
    generator = root / "Tools/generate_feeding_flow_ui.py"
    source = root / "Shared/feeding_flow_ui/feeding_flow_ui.json"
    if not generator.is_file() and not source.parent.is_dir():
        modules = configparser.ConfigParser(interpolation=None)
        modules.read(root / ".gitmodules")
        if not any(modules.get(section, "path", fallback="") == "Embeded_System/babytech-firmware"
                   for section in modules.sections()):
            return
    # Run as a CLI so the generator resolves its own root, not SCons PROJECT_DIR.
    subprocess.run([sys.executable, str(generator)], cwd=root, check=True)


if "Import" in globals():
    Import("env")
    sync_feeding_flow_ui(env.subst("$PROJECT_DIR"))
