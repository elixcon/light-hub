#!/bin/sh
# Packages a finished `idf.py build` as the files a release carries:
#
#   elixcon_light_hub-<version>-factory.bin  bootloader, table, updater and coordinator
#                                            in one image for a blank board, written at
#                                            0x0. The gaps are filled with 0xFF, and the
#                                            data partitions lie in them, so it erases
#                                            everything stored.
#   elixcon_light_hub-<version>-usb.zip      flash_args and the files it names, for
#                                            `esptool.py write_flash @flash_args`, which
#                                            keeps the data partitions
#   elixcon_light_hub-<version>.bin          the coordinator alone, for POST /update
#   elixcon_light_hub_updater-<version>.bin  the updater alone, for /api/updater/image or
#                                            an ESPHome conversion
#   elixcon_light_hub-<version>.elf          to decode a crash record (docs/development.md)
#   SHA256SUMS
#
#   tools/make-release.sh [build-dir] [out-dir]     defaults: build dist
#
# Needs the ESP-IDF environment, for idf.py. The version is the one the build
# put in the images (git describe), so the files are named as the firmware reports.
set -eu

build=${1:-build}
out=${2:-dist}

[ -f "$build/flash_args" ] || { echo "make-release: no $build/flash_args - run idf.py build first" >&2; exit 1; }
build=$(CDPATH='' cd -- "$build" && pwd)

# A key of the build's project_description.json.
desc() {
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' \
            "$build/project_description.json" "$1"
}
project=$(desc project_path)

rm -rf "$out"
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)

# flash_args is "<options>" on its first line, then "<offset> <file>" per image,
# relative to the build directory. The root CMakeLists.txt has already swapped in
# the full partition table and added the updater, and merge-bin reads it too.
# idf.py runs `all` before merge-bin, which can reconfigure and rebuild, so the
# version is read only after it.
idf.py -C "$project" -B "$build" merge-bin -o "$out/factory.bin"

version=$(desc project_version)
# ESP-IDF's version when git describe fails: no repository, or one git refuses
# to read, as in a container that runs as another user.
[ "$version" != 1 ] || { echo "make-release: the build has no version - git describe failed" >&2; exit 1; }
# The updater is built from the same commit in the same build, so it carries
# the same version.
name="elixcon_light_hub-$version"
mv "$out/factory.bin" "$out/$name-factory.bin"
usb="$name-usb"
mkdir "$out/$usb"

cd "$build"
cp flash_args "$out/$usb/"
sed 1d flash_args | while read -r _offset file; do
    mkdir -p "$out/$usb/$(dirname "$file")"
    cp "$file" "$out/$usb/$file"
done
cp elixcon_light_hub.bin "$out/$name.bin"
cp elixcon_light_hub.elf "$out/$name.elf"
cp updater/elixcon_light_hub_updater.bin "$out/elixcon_light_hub_updater-$version.bin"

cd "$out"
python3 -m zipfile -c "$usb.zip" "$usb"
rm -rf "$usb"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -- * > SHA256SUMS
else
    shasum -a 256 -- * > SHA256SUMS
fi
echo "make-release: $version"
ls -l
