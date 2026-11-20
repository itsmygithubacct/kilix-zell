#!/bin/sh
# Build the Neuronte desktop into DEST. Packed weights are not copied.
# kilix-secretsd and the kilix-secrets units are not installed.
set -eu
dest=${1:-/usr/libexec/kilix-zell}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
prefix=${NEURONTE_PREFIX:-/usr}
jobs=${ZELL_JOBS:-$(nproc)}
libdir=
for so in "$prefix"/lib/*/libneuronte.so; do
    if [ -e "$so" ]; then
        libdir=$(dirname "$so")
        break
    fi
done
if [ -z "$libdir" ]; then
    echo "kilix-zell: the neuronte library is not installed" >&2
    exit 1
fi
objs="$root/build/main_neuronte.o $root/build/desktop.o $root/build/scene.o $root/build/draw_neuronte.o $root/build/router.o $root/build/model.o $root/build/voice.o $root/build/history.o $root/build/sprites.o $root/build/room.o $root/build/kitty_framebuffer.o $root/build/kitty_keyboard.o $root/build/kitty_keyboard_posix.o $root/build/soft_raster_neuronte.o"
for src in "$root"/src/desktop_art/*.c; do
    base=$(basename "$src" .c)
    objs="$objs $root/build/art_$base.o"
done
cd "$root"
make -j"$jobs" CC=gcc CFLAGS=-O2 \
    MODULES="$root/packaging/modules" \
    NEURONTE="$prefix" \
    NEURONTE_WEIGHTS=/usr/share/kilix-zell/weights/neuronte-split-v4.nrt \
    $objs
mkdir -p "$dest"
gcc -O2 -std=c11 -pthread -Wl,--gc-sections -o "$dest/zell-neuronte" $objs \
    -L"$libdir" -Wl,-rpath-link,"$libdir" -lneuronte -lz -lm
chmod 755 "$dest/zell-neuronte"
rm -rf "$root/build"
version=$(LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$dest/zell-neuronte" --version)
if [ "$version" != "zell-neuronte 0.1.1" ]; then
    echo "kilix-zell: unexpected version: $version" >&2
    exit 1
fi
printf '%s\n' "$version"
