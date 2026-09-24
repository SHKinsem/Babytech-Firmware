#!/usr/bin/env bash
set -euo pipefail

source_root=$(realpath "$(wslpath -u "$1")")
target=${2:-all}
case "$target" in main-controller) target=brain;; device-controller) target=motion;; esac
jobs=${3:-8}
case "$target" in all|brain|motion|test) ;; *) echo 'Invalid build target' >&2; exit 2;; esac
[[ "$jobs" =~ ^[0-9]+$ ]] && ((jobs >= 1 && jobs <= 32)) || exit 2
test -f "$source_root/main-controller/platformio.ini"
test -f "$source_root/device-controller/platformio.ini"
test -f "$source_root/shared/BoardProtocol/library.json"

export PATH="$HOME/.local/bin:$HOME/.platformio/penv/bin:$HOME/.venvs/platformio/bin:$PATH"
command -v pio >/dev/null || { echo 'Install PlatformIO in WSL first.' >&2; exit 1; }
command -v rsync >/dev/null
command -v flock >/dev/null

# Keep build output and the PlatformIO toolchain off /mnt/c and /mnt/d.
cache_root="$HOME/.cache/babytech-firmware"
mkdir -p "$cache_root/workspaces"
cache_root=$(realpath "$cache_root")
case "$cache_root" in /mnt/*) echo 'Cache must be on the Linux filesystem.' >&2; exit 1;; esac
export PLATFORMIO_CORE_DIR="$cache_root/platformio"
workspace_id=$(printf '%s' "$source_root" | sha256sum | cut -c1-16)
workspace="$cache_root/workspaces/$workspace_id"
mkdir -p "$workspace"
exec 9>"$workspace/build.lock"
flock 9

if [[ -f "$workspace/source.path" ]]; then
    [[ "$(cat "$workspace/source.path")" == "$source_root" ]] || exit 2
else
    [[ ! -e "$workspace/source" ]] || { echo 'Unmanaged build directory; refusing to sync.' >&2; exit 2; }
    printf '%s\n' "$source_root" > "$workspace/source.path"
fi
mkdir -p "$workspace/source"
build_root=$(realpath "$workspace/source")
[[ "$build_root" == "$cache_root/workspaces/$workspace_id/source" ]] || exit 2
[[ "$build_root" != "$source_root" ]] || exit 2

echo "Source: $source_root"
echo "Linux build: $build_root"
# Deletion is restricted to our marked build mirror; excluded caches are protected.
# Checksums avoid rebuilding unchanged files even if a Windows editor touches timestamps.
rsync -rc --delete --exclude='.git/' --exclude='.pio/' --exclude='out/' \
    --exclude='node_modules/' --exclude='dist/' --exclude='qa/' --exclude='.deepseek-runs/' --exclude='references/' \
    --exclude='.vscode/' --exclude='__pycache__/' --exclude='*.pyc' \
    "$source_root/" "$build_root/"

cd "$build_root"
python3 tools/test_protocol.py
python3 tools/test_motion.py
python3 tools/test_raw_can.py
python3 tools/test_demo.py
if [[ "$target" == test ]]; then exit 0; fi
if [[ "$target" == all ]]; then projects=(motion brain); else projects=("$target"); fi

for project in "${projects[@]}"; do
    started=$(date +%s)
    if [[ "$project" == brain ]]; then project_dir=main-controller; else project_dir=device-controller; fi
    pio run -d "$project_dir" -e "$project" -j "$jobs"
    elapsed=$(( $(date +%s) - started ))
    destination="$source_root/out/wsl/$project"
    mkdir -p "$destination"
    for artifact in firmware.bin firmware.elf bootloader.bin partitions.bin; do
        cp -- "$project_dir/.pio/build/$project/$artifact" "$destination/$artifact"
    done
    cp -- "$PLATFORMIO_CORE_DIR/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$destination/boot_app0.bin"
    printf 'project=%s\nbuilt_utc=%s\nbuild_seconds=%s\nlinux_build=%s\n' \
        "$project" "$(date -u +%FT%TZ)" "$elapsed" "$build_root" > "$destination/build-info.txt"
    echo "Exported $project build to $destination ($elapsed seconds)"
done
