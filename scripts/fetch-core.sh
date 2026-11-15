#!/usr/bin/env bash
# Fetch and verify the prebuilt neiro-core SDK for macOS from rheatin/neiro-core
# GitHub Releases (or resolve a local neiro-core checkout/dist when --core-dir
# or NEIRO_CORE_DIST is provided).
#
# Prints the resolved SDK directory on stdout.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="$repo_root/NEIRO_CORE_VERSION"
default_version="v0.2.0"
if [ -f "$version_file" ]; then
    default_version="$(tr -d '[:space:]' < "$version_file")"
fi

version="${NEIRO_CORE_VERSION_OVERRIDE:-$default_version}"
arch="${NEIRO_CORE_ARCH:-arm64}"
core_repo="${NEIRO_CORE_REPO:-rheatin/neiro-core}"
local_dir="${NEIRO_CORE_DIST:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --version)
            version="$2"
            shift 2
            ;;
        --arch)
            arch="$2"
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

cache_base="$repo_root/.cache/neiro-core"
mkdir -p "$cache_base"

link_current() {
    local target="$1"
    ln -sfn "$target" "$cache_base/current"
    printf "%s\n" "$target"
}

# 1. Local override (--core-dir or NEIRO_CORE_DIST)
if [ -n "$local_dir" ]; then
    local_dir="$(cd "$local_dir" && pwd)"
    if [ -f "$local_dir/lib/libneiro_base.dylib" ] && [ -f "$local_dir/lib/libnae.dylib" ]; then
        link_current "$local_dir"
        exit 0
    fi
    pkg_candidate="$local_dir/dist/neiro-core-${version}-macos-${arch}"
    if [ -d "$pkg_candidate/lib" ]; then
        link_current "$pkg_candidate"
        exit 0
    fi
    if [ -x "$local_dir/scripts/package-sdk.sh" ]; then
        echo "packaging local neiro-core SDK from $local_dir..." >&2
        "$local_dir/scripts/package-sdk.sh" "$version" "$arch" >&2
        link_current "$pkg_candidate"
        exit 0
    fi
    echo "error: --core-dir '$local_dir' does not contain a packaged SDK or scripts/package-sdk.sh" >&2
    exit 1
fi

# 2. Cached release SDK
pkg_name="neiro-core-${version}-macos-${arch}"
version_cache="$cache_base/$version"
sdk_dir="$version_cache/$pkg_name"

if [ -f "$sdk_dir/lib/libneiro_base.dylib" ] && [ -f "$sdk_dir/lib/libnae.dylib" ]; then
    link_current "$sdk_dir"
    exit 0
fi

# 3. Download from GitHub Releases (rheatin/neiro-core)
mkdir -p "$version_cache"
archive="$version_cache/${pkg_name}.tar.gz"
sha_file="${archive}.sha256"

echo "downloading $pkg_name from $core_repo ($version)..." >&2
if command -v gh >/dev/null 2>&1; then
    gh release download "$version" \
        --repo "$core_repo" \
        --pattern "${pkg_name}.tar.gz" \
        --pattern "${pkg_name}.tar.gz.sha256" \
        --dir "$version_cache" \
        --clobber >&2
else
    echo "error: 'gh' CLI is required to download release assets from private repo $core_repo" >&2
    exit 1
fi

(
    cd "$version_cache"
    shasum -a 256 -c "${pkg_name}.tar.gz.sha256" >&2
)

rm -rf "$sdk_dir"
tar -xzf "$archive" -C "$version_cache"

if [ ! -f "$sdk_dir/lib/libneiro_base.dylib" ] || [ ! -f "$sdk_dir/lib/libnae.dylib" ]; then
    echo "error: extracted SDK at $sdk_dir is missing libneiro_base.dylib or libnae.dylib" >&2
    exit 1
fi

link_current "$sdk_dir"
