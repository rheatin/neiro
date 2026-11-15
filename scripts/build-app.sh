#!/usr/bin/env bash
# End-to-end macOS App build:
# 1. Fetch/verify the pinned neiro-core SDK release (or --core-dir local override)
# 2. Typecheck and package the UI via 'nsh check' + 'nsh package .'
# 3. Bundle the Core SDK dylibs into Neiro.app and code-sign the bundle
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CARGO_PROFILE_RELEASE_STRIP=none
chmod +x "$ROOT/scripts/fetch-core.sh" "$ROOT/scripts/bundle-macos-app.sh"

SDK_DIR="$("$ROOT/scripts/fetch-core.sh" "$@")"
echo "=== Using neiro-core SDK: $SDK_DIR ==="

cd "$ROOT/ui"
echo "=== Typechecking UI (nsh check) ==="
nsh check

echo "=== Packaging UI (nsh package --target macos App.tsx) ==="
nsh package --target macos App.tsx

echo "=== Injecting neiro-core SDK into Neiro.app ==="
"$ROOT/scripts/bundle-macos-app.sh" "$@"

echo "PASS: built $ROOT/ui/.nsh/out/Neiro.app"
