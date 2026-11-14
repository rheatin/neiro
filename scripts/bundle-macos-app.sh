#!/bin/bash
# Put the engine beside the packaged app and make the whole bundle refer to
# itself: every absolute path into the build tree becomes @loader_path/<name>.
set -euo pipefail

APP="$HOME/Repos/neiro/ui/.nsh/out/Neiro.app"
MACOS="$APP/Contents/MacOS"
VENDOR="$HOME/Repos/neiro/vendor/ffmpeg-lgpl/lib"

if [ ! -d "$MACOS" ]; then
  echo "no app at $APP" >&2
  exit 1
fi

cp "$HOME/Repos/neiro/base/target/release/libneiro_base.dylib" "$MACOS/"
cp "$HOME/Repos/neiro/engine/build-macos/libnae.dylib" "$MACOS/"
for f in "$VENDOR"/*.dylib; do cp "$f" "$MACOS/"; done

for lib in "$MACOS"/*.dylib; do
  /usr/bin/otool -L "$lib" | tail -n +2 | awk '{print $1}' | grep "^$HOME/Repos/neiro" | while read -r ref; do
    base=$(basename "$ref")
    /usr/bin/install_name_tool -change "$ref" "@loader_path/$base" "$lib" 2>/dev/null || true
  done
  /usr/bin/install_name_tool -add_rpath "@loader_path" "$lib" 2>/dev/null || true
done

/usr/bin/codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true

echo "--- Contents/MacOS ---"
ls "$MACOS"
echo "--- libnae, non-system deps ---"
/usr/bin/otool -L "$MACOS/libnae.dylib" | grep -v "^/usr/lib\|^/System"

# Run after `nsh package --target macos` in ui/: it copies the two libraries and
# the FFmpeg dylibs they need into Contents/MacOS, rewrites every reference into
# the build tree to @loader_path/<name>, signs ad hoc, and prints the result.
