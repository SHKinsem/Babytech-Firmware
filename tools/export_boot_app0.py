"""Export boot data from the framework selected for this PlatformIO project."""
from pathlib import Path
import shutil

Import("env")

framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
output = Path(env.subst("$BUILD_DIR")) / "boot_app0.bin"
output.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(framework / "tools/partitions/boot_app0.bin", output)
