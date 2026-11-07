# Build and run the C header check.
#
# ASCII only: powershell -File reads a BOM-less script as the system code page,
# and a backtick inside a double-quoted string is an escape rather than text.
#
# Two DLLs must sit beside the executable. neiro_base.dll is the crate and
# nae.dll is the engine it links against; Windows resolves imports when the
# process starts, so a missing one exits before main() with no message at all
# and an exit code that looks like a logic failure.
$ErrorActionPreference = 'Stop'

$base = Split-Path -Parent $PSScriptRoot
$out = Join-Path $env:TEMP 'nae-base-c-check'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$env:RUSTUP_TOOLCHAIN = 'stable-x86_64-pc-windows-gnu'
Push-Location $base
try {
    cargo build --lib
    if ($LASTEXITCODE -ne 0) { throw "cargo build --lib failed with $LASTEXITCODE" }
} finally {
    Pop-Location
}

$cc = Get-Command cc -ErrorAction SilentlyContinue
if (-not $cc) { throw 'cc (MinGW gcc) is not on PATH' }

& $cc.Source `
    -I (Join-Path $base 'include') `
    (Join-Path $base 'test/nae_base_c_check.c') `
    (Join-Path $base 'target/debug/libneiro_base.dll.a') `
    -mcrtdll=ucrt `
    -o (Join-Path $out 'base_c_check.exe')
if ($LASTEXITCODE -ne 0) { throw "cc failed with $LASTEXITCODE" }

Copy-Item (Join-Path $base 'target/debug/neiro_base.dll') $out -Force
Copy-Item (Join-Path $base 'target/debug/nae.dll') $out -Force

& (Join-Path $out 'base_c_check.exe')
exit $LASTEXITCODE
