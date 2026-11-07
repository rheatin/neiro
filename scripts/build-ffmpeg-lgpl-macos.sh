#!/usr/bin/env bash
# Build an LGPL FFmpeg into vendor/ffmpeg-lgpl, where the engine looks first.
#
# The Homebrew package enables GPL, so linking it makes libnae.dylib GPL and
# nothing distributable. The engine only decodes, so the GPL components are
# dead weight here -- nothing above the link changes.
#
# `--disable-programs` is deliberately NOT passed: the configure step of the
# engine reads the licence out of `${prefix}/bin/ffmpeg`, and a tree with no
# executable cannot be checked.
set -euo pipefail

version="${FFMPEG_VERSION:-7.1}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="$repo_root/vendor/ffmpeg-lgpl"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "building FFmpeg $version into $prefix"
curl -L --fail "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$work/ffmpeg.tar.xz"
tar -xJf "$work/ffmpeg.tar.xz" -C "$work"
cd "$work/ffmpeg-$version"

./configure \
    --prefix="$prefix" \
    --disable-gpl \
    --disable-nonfree \
    --disable-doc \
    --disable-debug \
    --enable-shared \
    --disable-static

make -j"$(sysctl -n hw.ncpu)"
make install

"$prefix/bin/ffmpeg" -hide_banner -version | tr ' ' '\n' |
    grep -E '^--enable-(gpl|nonfree)$' && {
        echo "the build reports a GPL or nonfree switch; it is not usable" >&2
        exit 1
    }

echo "PASS: $prefix has no --enable-gpl and no --enable-nonfree."
echo "Reconfigure the engine; it picks this tree up with no flags."
