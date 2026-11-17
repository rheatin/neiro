#!/usr/bin/env bash
# Bundle the prebuilt neiro-core SDK dylibs into the packaged Neiro.app bundle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${APP:-$ROOT/ui/.nsh/out/Neiro.app}"
MACOS="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"

if [ ! -d "$MACOS" ]; then
    echo "no app at $APP (run 'nsh package .' in ui/ first)" >&2
    exit 1
fi

SDK_DIR="$("$ROOT/scripts/fetch-core.sh" "$@")"
SDK_LIB="$SDK_DIR/lib"

if [ ! -d "$SDK_LIB" ]; then
    echo "error: no lib/ directory in SDK '$SDK_DIR'" >&2
    exit 1
fi

mkdir -p "$FRAMEWORKS"
rm -f "$FRAMEWORKS"/lib*.dylib "$MACOS"/lib*.dylib

for dylib in "$SDK_LIB"/*.dylib; do
    [ -f "$dylib" ] || continue
    base="$(basename "$dylib")"
    cp "$dylib" "$FRAMEWORKS/$base"
    chmod u+w "$FRAMEWORKS/$base"
done

for bin in "$MACOS/neiro" "$MACOS/Neiro"; do
    if [ -f "$bin" ]; then
        otool -l "$bin" | grep -q "@executable_path/../Frameworks" || \
            install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin" 2>/dev/null || true
    fi
done

for lib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$lib" ] || continue
    codesign --force --sign - "$lib" >/dev/null 2>&1 || true
done

codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true

echo "--- Bundled Core SDK from: $SDK_DIR ---"
ls -1 "$FRAMEWORKS"/*.dylib
