# End-to-end Windows App build:
# 1. Fetch/verify the pinned neiro-core SDK release (or -CoreDir local override)
# 2. Typecheck and package the UI via 'nsh check' + 'nsh package .'
# 3. Copy the Core SDK DLLs beside Neiro.exe in ui/.nsh/out/Neiro.app/
#
# ASCII only (AGENTS.md rule 8).

param(
    [string]$Version = "",
    [string]$Arch = "x64",
    [string]$CoreRepo = "rheatin/neiro-core",
    [string]$CoreDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $env:RUSTUP_TOOLCHAIN) { $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu" }

$fetchArgs = @("-Arch", $Arch, "-CoreRepo", $CoreRepo)
if ($Version) { $fetchArgs += @("-Version", $Version) }
if ($CoreDir) { $fetchArgs += @("-CoreDir", $CoreDir) }

$sdkDir = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "fetch-core.ps1") @fetchArgs | Select-Object -Last 1).Trim()
if (-not $sdkDir -or -not (Test-Path (Join-Path $sdkDir "bin\neiro_base.dll"))) {
    throw "fetch-core.ps1 did not return a valid SDK directory (got '$sdkDir')"
}
Write-Host "=== Using neiro-core SDK: $sdkDir ===" -ForegroundColor Cyan

Push-Location (Join-Path $repoRoot "ui")
try {
    Write-Host "=== Typechecking UI (nsh check) ===" -ForegroundColor Cyan
    & nsh check
    if ($LASTEXITCODE -ne 0) { throw "nsh check failed" }

    Write-Host "=== Packaging UI (nsh package --target windows App.tsx) ===" -ForegroundColor Cyan
    & nsh package --target windows App.tsx
    if ($LASTEXITCODE -ne 0) { throw "nsh package failed" }
} finally {
    Pop-Location
}

$appOut = Join-Path $repoRoot "ui\.nsh\out\Neiro.app"
if (-not (Test-Path $appOut)) {
    throw "missing packaged app directory at $appOut"
}

Get-ChildItem -Path (Join-Path $sdkDir "bin") -Filter *.dll |
    Copy-Item -Destination $appOut -Force

Write-Host "PASS: built $appOut\Neiro.exe with Core SDK DLLs" -ForegroundColor Green
Get-ChildItem -Path $appOut -Filter *.dll | Select-Object Name, Length
