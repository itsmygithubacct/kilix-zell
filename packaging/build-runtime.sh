#!/bin/sh
# Build the Neuronte desktop into DEST from the published source commit.
# Packed weights are not copied. kilix-secretsd and the kilix-secrets
# units are not installed.
set -eu
export GIT_TERMINAL_PROMPT=0
dest=${1:-/usr/libexec/kilix-zell}
pin=b374ed0a131570088e1f5eda841986d407232814
prefix=${NEURONTE_PREFIX:-/usr}
jobs=${ZELL_JOBS:-$(nproc)}
url=https://github.com/itsmygithubacct/kilix-zell.git
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
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$work" init -q
git -C "$work" remote add origin "$url"
git -C "$work" fetch --depth 1 origin "$pin"
git -C "$work" checkout -q --detach FETCH_HEAD
got=$(git -C "$work" rev-parse HEAD)
if [ "$got" != "$pin" ]; then
    echo "kilix-zell: unexpected source revision" >&2
    exit 1
fi
root=$work
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
version=$(LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$dest/zell-neuronte" --version)
if [ "$version" != "zell-neuronte 0.1.1" ]; then
    rm -f "$dest/zell-neuronte"
    echo "kilix-zell: unexpected version: $version" >&2
    exit 1
fi
printf '%s\n' "$version"
