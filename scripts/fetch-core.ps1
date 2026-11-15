# Fetch and verify the prebuilt neiro-core SDK for Windows from rheatin/neiro-core
# GitHub Releases (or resolve a local neiro-core checkout/dist when -CoreDir or
# NEIRO_CORE_DIST is provided).
#
# Prints the resolved SDK directory on the last line of stdout.
# ASCII only (AGENTS.md rule 8).

param(
    [string]$Version = "",
    [string]$Arch = "x64",
    [string]$CoreRepo = "rheatin/neiro-core",
    [string]$CoreDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$versionFile = Join-Path $repoRoot "NEIRO_CORE_VERSION"

if (-not $Version) {
    if ($env:NEIRO_CORE_VERSION_OVERRIDE) {
        $Version = $env:NEIRO_CORE_VERSION_OVERRIDE.Trim()
    } elseif (Test-Path $versionFile) {
        $Version = (Get-Content -Path $versionFile -Raw).Trim()
    } else {
        $Version = "v0.2.0"
    }
}

if (-not $CoreDir -and $env:NEIRO_CORE_DIST) {
    $CoreDir = $env:NEIRO_CORE_DIST
}

$cacheBase = Join-Path $repoRoot ".cache\neiro-core"
New-Item -ItemType Directory -Path $cacheBase -Force | Out-Null

function Set-CurrentSdk([string]$targetDir) {
    $currentDir = Join-Path $cacheBase "current"
    if (Test-Path $currentDir) { Remove-Item -Recurse -Force $currentDir }
    Copy-Item -Path $targetDir -Destination $currentDir -Recurse -Force
    Write-Output $targetDir
}

# 1. Local override (-CoreDir or NEIRO_CORE_DIST)
if ($CoreDir) {
    $resolved = (Resolve-Path $CoreDir).Path
    if ((Test-Path (Join-Path $resolved "bin\neiro_base.dll")) -and (Test-Path (Join-Path $resolved "bin\nae.dll"))) {
        Set-CurrentSdk $resolved
        exit 0
    }
    $pkgCandidate = Join-Path $resolved "dist\neiro-core-$Version-windows-$Arch"
    if (Test-Path (Join-Path $pkgCandidate "bin\neiro_base.dll")) {
        Set-CurrentSdk $pkgCandidate
        exit 0
    }
    $pkgScript = Join-Path $resolved "scripts\package-sdk.ps1"
    if (Test-Path $pkgScript) {
        Write-Host "packaging local neiro-core SDK from $resolved..." -ForegroundColor Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File $pkgScript -Version $Version -Arch $Arch | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "package-sdk.ps1 failed" }
        Set-CurrentSdk $pkgCandidate
        exit 0
    }
    throw "-CoreDir '$CoreDir' does not contain a packaged SDK or scripts/package-sdk.ps1"
}

# 2. Cached release SDK
$pkgName = "neiro-core-$Version-windows-$Arch"
$versionCache = Join-Path $cacheBase $Version
$sdkDir = Join-Path $versionCache $pkgName

if ((Test-Path (Join-Path $sdkDir "bin\neiro_base.dll")) -and (Test-Path (Join-Path $sdkDir "bin\nae.dll"))) {
    Set-CurrentSdk $sdkDir
    exit 0
}

# 3. Download from GitHub Releases (rheatin/neiro-core)
New-Item -ItemType Directory -Path $versionCache -Force | Out-Null
$zipPath = Join-Path $versionCache "$pkgName.zip"
$shaPath = "$zipPath.sha256"

$needDownload = $true
if ((Test-Path $zipPath) -and (Test-Path $shaPath)) {
    $expectedHash = ((Get-Content -Path $shaPath -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
    $actualHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($expectedHash -eq $actualHash) {
        $needDownload = $false
    }
}

if ($needDownload) {
    [Console]::Error.WriteLine("downloading $pkgName from $CoreRepo ($Version)...")
    $downloaded = $false
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        & gh release download $Version `
            --repo $CoreRepo `
            --pattern "$pkgName.zip" `
            --pattern "$pkgName.zip.sha256" `
            --dir $versionCache `
            --clobber 1>&2
        if ($LASTEXITCODE -eq 0) {
            $downloaded = $true
            break
        }
        [Console]::Error.WriteLine("attempt $attempt failed, retrying in 2s...")
        Start-Sleep -Seconds 2
    }
    if (-not $downloaded) {
        throw "gh release download failed for $CoreRepo $Version"
    }
}

$expectedHash = ((Get-Content -Path $shaPath -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
$actualHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($expectedHash -ne $actualHash) {
    throw "SHA256 mismatch for $zipPath (expected $expectedHash, got $actualHash)"
}

if (Test-Path $sdkDir) { Remove-Item -Recurse -Force $sdkDir }
Expand-Archive -Path $zipPath -DestinationPath $versionCache -Force

if (-not (Test-Path (Join-Path $sdkDir "bin\neiro_base.dll")) -or -not (Test-Path (Join-Path $sdkDir "bin\nae.dll"))) {
    throw "extracted SDK at $sdkDir is missing neiro_base.dll or nae.dll"
}

Set-CurrentSdk $sdkDir
