#!/usr/bin/env bash
# Resolve the iOS core xcframework for the pinned neiro-core release and link
# it at ui/vendor/neiro_base.xcframework for ui/build.rs.
#
# A candidate is accepted only when both device and simulator slices contain
# libneiro_base.a and Info.plist is present.
#
# Prints the resolved xcframework path on stdout.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="$repo_root/NEIRO_CORE_VERSION"
default_version="v0.3.0"
if [ -f "$version_file" ]; then
    default_version="$(tr -d '[:space:]' < "$version_file")"
fi

version="${NEIRO_CORE_VERSION_OVERRIDE:-$default_version}"
core_repo="${NEIRO_CORE_REPO:-rheatin/neiro-core}"
local_dir="${NEIRO_CORE_IOS_DIST:-}"
out_dir="$repo_root/ui/vendor"
output="$out_dir/neiro_base.xcframework"
archive_name="neiro-core-${version}-ios.xcframework.tar.gz"

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

validate_xcframework() {
    local candidate="$1"
    local slice
    for slice in ios-arm64 ios-arm64-simulator; do
        if [ ! -d "$candidate/$slice" ] \
            || [ ! -f "$candidate/$slice/libneiro_base.a" ]; then
            return 1
        fi
    done
    [ -f "$candidate/Info.plist" ]
}

install_xcframework() {
    local source="$1"
    mkdir -p "$out_dir"
    rm -rf "$output"
    ln -s "$source" "$output"
    printf "%s\n" "$source"
}

if [ -n "$local_dir" ]; then
    local_dir="$(cd "$local_dir" && pwd)"
    checkout_candidate="$local_dir/dist/ios/neiro_base.xcframework"
    packaged_candidate="$local_dir/neiro_base.xcframework"
    if [ -d "$checkout_candidate" ]; then
        candidate="$checkout_candidate"
    elif [ -d "$packaged_candidate" ]; then
        candidate="$packaged_candidate"
    elif [ "$(basename "$local_dir")" = "neiro_base.xcframework" ]; then
        candidate="$local_dir"
    else
        echo "error: '$local_dir' contains no neiro_base.xcframework" >&2
        exit 1
    fi
    if ! validate_xcframework "$candidate"; then
        echo "error: xcframework at $candidate is missing a required slice, libneiro_base.a, or Info.plist" >&2
        exit 1
    fi
    candidate="$(cd "$candidate" && pwd)"
    install_xcframework "$candidate"
    exit 0
fi

cache_base="$repo_root/.cache/neiro-core"
version_cache="$cache_base/$version"
cached_xcframework="$version_cache/neiro_base.xcframework"
if [ -d "$cached_xcframework" ] && validate_xcframework "$cached_xcframework"; then
    install_xcframework "$cached_xcframework"
    exit 0
fi

mkdir -p "$version_cache"
archive="$version_cache/$archive_name"

echo "downloading $archive_name from $core_repo ($version)..." >&2
if ! command -v gh >/dev/null 2>&1; then
    echo "error: gh CLI is required to download release assets from $core_repo" >&2
    exit 1
fi

if ! gh release download "$version" \
    --repo "$core_repo" \
    --pattern "$archive_name" \
    --pattern "${archive_name}.sha256" \
    --dir "$version_cache" \
    --clobber >&2; then
    echo "error: $core_repo release $version has no iOS asset '$archive_name' yet." >&2
    echo "Pass --core-dir or run neiro-core/scripts/build-ios-xcframework.sh." >&2
    exit 1
fi

(
    cd "$version_cache"
    shasum -a 256 -c "${archive_name}.sha256" >&2
)

rm -rf "$cached_xcframework"
tar -xzf "$archive" -C "$version_cache"

if ! validate_xcframework "$cached_xcframework"; then
    echo "error: extracted xcframework at $cached_xcframework is missing a required slice, libneiro_base.a, or Info.plist" >&2
    exit 1
fi

cached_xcframework="$(cd "$cached_xcframework" && pwd)"
install_xcframework "$cached_xcframework"
