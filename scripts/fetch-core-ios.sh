#!/usr/bin/env bash
# Fetch and verify the prebuilt iOS core artifact for a pinned neiro-core
# release. The artifact layout is normative in docs/ios/core-contract.md:
#
#   neiro_base.xcframework/
#     ios-arm64/libneiro_base.a            (+ FFmpeg.framework)
#     ios-arm64-simulator/libneiro_base.a  (+ FFmpeg.framework)
#
# Unlike fetch-core.sh (desktop dylibs), this downloads into ui/vendor/ so
# `ui/build.rs` can find it without an environment variable.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="$repo_root/NEIRO_CORE_VERSION"
default_version="v0.2.0"
if [ -f "$version_file" ]; then
    default_version="$(tr -d '[:space:]' < "$version_file")"
fi

version="${NEIRO_CORE_VERSION_OVERRIDE:-$default_version}"
core_repo="${NEIRO_CORE_REPO:-rheatin/neiro-core}"
local_dir="${NEIRO_CORE_IOS_DIST:-}"
out_dir="$repo_root/ui/vendor"
pkg_name="neiro-core-${version}-ios"

while [ $# -gt 0 ]; do
    case "$1" in
        --version)
            version="$2"
            shift 2
            ;;
        --core-dir)
            local_dir="$2"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

install_from() {
    local source="$1"
    mkdir -p "$out_dir"
    rm -rf "$out_dir/neiro_base.xcframework"
    cp -R "$source" "$out_dir/neiro_base.xcframework"
    echo "$out_dir/neiro_base.xcframework"
}

# 1. Local override: a directory that is either the xcframework itself or a
#    parent containing it.
if [ -n "$local_dir" ]; then
    local_dir="$(cd "$local_dir" && pwd)"
    if [ -d "$local_dir/neiro_base.xcframework" ]; then
        install_from "$local_dir/neiro_base.xcframework"
        exit 0
    fi
    if [ -d "$local_dir" ] && [ "$(basename "$local_dir")" = "neiro_base.xcframework" ]; then
        install_from "$local_dir"
        exit 0
    fi
    echo "error: --core-dir '$local_dir' does not contain neiro_base.xcframework" >&2
    exit 1
fi

# 2. Already vendored.
if [ -d "$out_dir/neiro_base.xcframework/ios-arm64" ] \
    && [ -d "$out_dir/neiro_base.xcframework/ios-arm64-simulator" ]; then
    echo "$out_dir/neiro_base.xcframework"
    exit 0
fi

# 3. Download from GitHub Releases.
cache_base="$repo_root/.cache/neiro-core"
version_cache="$cache_base/$version"
archive="$version_cache/${pkg_name}.zip"
mkdir -p "$version_cache"

echo "downloading $pkg_name from $core_repo ($version)..." >&2
if command -v gh >/dev/null 2>&1; then
    if ! gh release download "$version" \
        --repo "$core_repo" \
        --pattern "${pkg_name}.zip" \
        --dir "$version_cache" \
        --clobber >&2; then
        echo "error: $core_repo has no iOS asset '${pkg_name}.zip' for $version." >&2
        echo "The iOS artifact is specified in docs/ios/core-contract.md and is not published yet." >&2
        exit 1
    fi
else
    echo "error: 'gh' CLI is required to download release assets from $core_repo" >&2
    exit 1
fi

rm -rf "$version_cache/$pkg_name"
unzip -q "$archive" -d "$version_cache"

if [ ! -d "$version_cache/$pkg_name/neiro_base.xcframework" ]; then
    echo "error: $archive does not contain neiro_base.xcframework" >&2
    exit 1
fi

install_from "$version_cache/$pkg_name/neiro_base.xcframework"
