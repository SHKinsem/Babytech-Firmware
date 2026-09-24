"""Compatibility entry point; use package-device.py --release <identifier>."""
from pathlib import Path
import runpy

runpy.run_path(str(Path(__file__).with_name("package-device.py")), run_name="__main__")
