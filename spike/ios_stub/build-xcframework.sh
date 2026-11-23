#!/usr/bin/env bash
# Build the spike stub into a two-slice xcframework, the same layout the real
# `neiro-core` release must publish:
#
#   neiro_base.xcframework/
#     ios-arm64/libneiro_base.a
#     ios-arm64-simulator/libneiro_base.a
#
# This is a developer-machine spike, not a shipped artifact. `ui/build.rs`
# selects the slice from the Rust target, so building this once proves the
# slice selection as well as the link flags.
set -euo pipefail

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$dir/neiro_base.xcframework"
rm -rf "$out" "$dir/build"

compile() {
  local target="$1"
  local sdk="$2"
  mkdir -p "$dir/build/$target"
  xcrun --sdk "$sdk" clang -target "$target" -O2 \
    -c "$dir/nae_stub.c" -o "$dir/build/$target/nae_stub.o"
  ar qs "$dir/build/$target/libneiro_base.a" "$dir/build/$target/nae_stub.o"
}

compile arm64-apple-ios iphoneos
compile arm64-apple-ios-simulator iphonesimulator

xcodebuild -create-xcframework \
  -library "$dir/build/arm64-apple-ios/libneiro_base.a" \
  -headers "$dir/include" \
  -library "$dir/build/arm64-apple-ios-simulator/libneiro_base.a" \
  -headers "$dir/include" \
  -output "$out"

echo "built: $out"
lipo -info "$out/ios-arm64/libneiro_base.a" "$out/ios-arm64-simulator/libneiro_base.a"
nm -gU "$out/ios-arm64/libneiro_base.a" | grep -E 'NAE_Base(AbiVersion|ReinitializeAudio)' || true
