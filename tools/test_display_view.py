"""Run the real LVGL view on a host display buffer; no hardware or downloads.

Uses the LVGL 8.4 dependency installed by `pio run -d main-controller -e brain`.
Optionally supply --preview /absolute/path.ppm for the rendered NotReady screen.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--preview", type=Path)
args = parser.parse_args()
lvgl = root / "main-controller/.pio/libdeps/brain/lvgl"
if not (lvgl / "lvgl.h").exists():
    raise SystemExit("Build main-controller -e brain first to install LVGL 8.4")
libraries = root / "shared"
core = libraries / "BabytechDisplayCore/src"
view = libraries / "BabytechDisplayLvgl"
flags = ["-DLV_CONF_SKIP=1", "-DLV_COLOR_DEPTH=16", "-DLV_FONT_MONTSERRAT_20=1",
         "-DLV_FONT_MONTSERRAT_28=1", "-DLV_MEM_SIZE=262144", "-I", str(lvgl)]
with tempfile.TemporaryDirectory(prefix="babytech-lvgl-test-") as directory:
    directory = Path(directory)
    sources = sorted((lvgl / "src").rglob("*.c"))
    objects = [directory / f"lvgl-{index}.o" for index in range(len(sources))]
    def compile_one(pair):
        source, output = pair
        subprocess.run(["cc", "-std=c99", "-O0", *flags, "-c", str(source), "-o", str(output)], check=True)
    with ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(compile_one, zip(sources, objects)))
    binary = directory / "display-view"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I", str(core), "-I", str(view / "src"),
                    str(core / "display_model.cpp"), str(view / "src/babytech_display_view.cpp"),
                    str(view / "tests/test_display_view.cpp"), *map(str, objects),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary), *([str(args.preview)] if args.preview else [])], check=True)
