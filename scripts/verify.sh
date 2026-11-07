#!/usr/bin/env bash
# Everything that can be checked without a human and without hardware.
#
# The same split as scripts/verify.ps1: a CI runner has no audio endpoint, so
# anything that opens a device runs only with --with-devices.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# .cargo/config.toml pins CC/CXX to the Windows MinGW because cargo has no
# per-target env table. Overriding them here is what makes the same checkout
# build on macOS; Cargo.toml's comment says so and this is the other half of it.
export CC="${CC:-/usr/bin/cc}"
export CXX="${CXX:-/usr/bin/c++}"
engine="$repo_root/engine"
build="$engine/build-macos"
with_devices=0
[ "${1:-}" = "--with-devices" ] && with_devices=1

failures=0
run_one() {
    local exe="$1"; shift
    echo "--- $(basename "$exe")"
    "$exe" "$@" || { failures=$((failures + 1)); echo "FAILED: $(basename "$exe")"; }
}

echo "=== configure and build (engine) ==="
cd "$engine" || exit 1
vendor="$repo_root/vendor/ffmpeg-lgpl"
ffmpeg_arg=()
# A build directory configured before the vendored tree existed keeps its old
# cached root, so name it rather than relying on the default.
[ -f "$vendor/include/libavcodec/avcodec.h" ] && ffmpeg_arg=("-DNEIRO_FFMPEG_ROOT=$vendor")
cmake --preset macos -DNEIRO_WIP_ENGINE_MACOS=ON "${ffmpeg_arg[@]}" || exit 1
cmake --build --preset macos || exit 1

export DYLD_FALLBACK_LIBRARY_PATH="$build:$engine/../vendor/ffmpeg-lgpl/lib"

echo "=== engine checks (no device) ==="
# The macOS preset builds the platform-neutral core and the CoreAudio checks;
# a name it does not build is skipped rather than reported as a failure.
for name in dither_check resample_check hilbert_check loudness_check \
            clip_guard_check linear_phase_eq_check partitioned_convolver_check \
            dsd_filter_check dsd_downrate_check analysis_check param_check \
            struct_size_check policy_check routing_check; do
    exe="$build/$name"
    if [ -x "$exe" ]; then run_one "$exe"; else echo "not built: $name"; fi
done

if [ "$with_devices" = 1 ]; then
    echo "=== engine checks (need an endpoint) ==="
    for name in macos_smoke macos_play_check output_stream_check; do
        exe="$build/$name"
        [ -x "$exe" ] || continue
        if [ "$name" = "macos_play_check" ]; then
            # It needs a file and audio fixtures are not in the repository, so
            # make one from the FFmpeg this build already links. Skipping it left
            # the one check that opens a real stream out of the default run.
            fixture="$build/g1.wav"
            if [ ! -f "$fixture" ] && [ -x "$repo_root/vendor/ffmpeg-lgpl/bin/ffmpeg" ]; then
                "$repo_root/vendor/ffmpeg-lgpl/bin/ffmpeg" -y -loglevel error \
                    -f lavfi -i "sine=frequency=1000:duration=1" -ar 44100 "$fixture"
            fi
            if [ -f "$fixture" ]; then
                run_one "$exe" "$fixture"
            else
                echo "not run: macos_play_check needs a file and none could be made"
            fi
        else
            run_one "$exe"
        fi
    done
fi

echo "=== base ==="
cd "$repo_root/base" || exit 1
cargo run --bin transport_check || failures=$((failures + 1))
cargo run --bin index_probe || failures=$((failures + 1))
if [ "$with_devices" = 1 ]; then
    cargo run --bin base_probe || failures=$((failures + 1))
fi

echo "=== binding ==="
cd "$repo_root/binding" || exit 1
cargo check || failures=$((failures + 1))
if [ "$with_devices" = 1 ] && [ -f "$repo_root/test/tagtmp/a.flac" ]; then
    cargo run --bin probe -- "$repo_root/test/tagtmp/a.flac" || failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    echo
    echo "FAILED: $failures step(s)"
    exit 1
fi
echo
echo "PASS: every check that ran passed."
