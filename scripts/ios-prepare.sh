#!/usr/bin/env bash
# Finish the iOS package NSH writes.
#
#   ios-prepare.sh <path/to/Neiro.app>            # verify, inject, re-sign
#   ios-prepare.sh --check <path/to/Neiro.app>    # verify only, no writes
#
# What NSH already carries from `ui/nsh.json` (`ios.infoPlist`): background
# audio, the local-network prompt and the ATS exemption for the LAN. What it
# has no notion of: the privacy manifest. This script places
# `ui/ios/PrivacyInfo.xcprivacy` beside the binary and re-signs the bundle,
# because a file added after signing is not covered by the signature.
#
# Signing: the simulator build is ad hoc (`-`). A device build passes
#   IOS_SIGN_IDENTITY="Apple Distribution: …" IOS_ENTITLEMENTS=<file> ios-prepare.sh …
# and the profile must already be at Neiro.app/embedded.mobileprovision
# (nsh package copies it there when --sign and --profile are given).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
manifest="$ROOT/ui/ios/PrivacyInfo.xcprivacy"

check=0
case "${1:-}" in
    --check)
        check=1
        shift
        ;;
esac

app="${1:-}"
if [ -z "$app" ] || [ ! -d "$app" ]; then
    echo "usage: $(basename "$0") [--check] <path/to/Neiro.app>" >&2
    exit 2
fi
app="$(cd "$app" && pwd)"
plist="$app/Info.plist"
if [ ! -f "$plist" ]; then
    echo "not an app bundle (no Info.plist): $app" >&2
    exit 2
fi
if [ ! -f "$manifest" ]; then
    echo "missing privacy manifest: $manifest" >&2
    exit 2
fi

fail=0
expect() {
    local key="$1" want="$2" got
    got="$(plutil -extract "$key" json -o - "$plist" 2>/dev/null || echo '<missing>')"
    if [ "$got" != "$want" ]; then
        echo "Info.plist $key = $got, expected $want" >&2
        fail=1
    fi
}

# The three plist promises the iOS build depends on. `plutil -extract` takes
# one key, not a path, so the ATS dictionary is read whole.
expect UIBackgroundModes '["audio"]'
ats="$(plutil -extract NSAppTransportSecurity json -o - "$plist" 2>/dev/null || echo '<missing>')"
case "$ats" in
    *'"NSAllowsLocalNetworking":true'*) ;;
    *)
        echo "Info.plist NSAppTransportSecurity.NSAllowsLocalNetworking is missing" >&2
        fail=1
        ;;
esac
plutil -extract NSLocalNetworkUsageDescription raw -o - "$plist" >/dev/null 2>&1 || {
    echo "Info.plist NSLocalNetworkUsageDescription is missing" >&2
    fail=1
}

if [ "$check" = 1 ]; then
    if [ -f "$app/PrivacyInfo.xcprivacy" ]; then
        echo "privacy manifest: present"
    else
        echo "privacy manifest: missing (run without --check to add it)" >&2
        fail=1
    fi
    exit "$fail"
fi

# A copy added after signing leaves the signature incomplete; re-sign after.
cp "$manifest" "$app/PrivacyInfo.xcprivacy"

executable="$(plutil -extract CFBundleExecutable raw -o - "$plist")"
out_dir="$(dirname "$app")"
entitlements="$out_dir/${executable}.entitlements"

sign_args=(--force --sign -)
if [ -n "${IOS_SIGN_IDENTITY:-}" ]; then
    sign_args=(--force --sign "$IOS_SIGN_IDENTITY")
fi
if [ -n "${IOS_ENTITLEMENTS:-}" ]; then
    entitlements="$IOS_ENTITLEMENTS"
fi
if [ -f "${entitlements}" ]; then
    sign_args+=(--entitlements "${entitlements}")
fi

codesign "${sign_args[@]}" "$app"
echo "prepared $app (privacy manifest injected, signature refreshed)"

exit "$fail"
