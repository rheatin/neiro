# Build and verify the engine against an LGPL FFmpeg.
#
# The engine links FFmpeg, and a GPL build makes nae.dll GPL -- fine to develop
# against, not fine to distribute. The swap is a link-time change: the engine
# only uses decoding APIs (avformat_open_input, av_read_frame,
# avcodec_receive_frame), so nothing above the link moves.
#
# This script does the whole thing end to end, because "an LGPL build is a
# link-time change" is a claim that has to be demonstrated rather than asserted:
#
#   1. download a shared LGPL build from BtbN (the gyan.dev builds are all GPL);
#   2. refuse it if its configuration still enables GPL or nonfree;
#   3. configure a separate build directory against it;
#   4. build, run the checks, and read the licence the DLL reports.
#
# ASCII only. PowerShell 5 reads a BOM-less script as ANSI and a backtick inside
# a double-quoted string is an escape, not text (AGENTS.md rule 8).

param(
    [string]$Prefix = "",
    [string]$BuildDir = "build-lgpl",
    [switch]$SkipDownload
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$engineDir = Join-Path $repoRoot "engine"
$workDir = Join-Path $env:TEMP "neiro-lgpl"

if (-not $Prefix) {
    if ($SkipDownload) {
        Write-Error "-SkipDownload needs -Prefix pointing at an existing LGPL FFmpeg."
    }

    $zip = Join-Path $env:TEMP "neiro-lgpl.zip"
    $url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"

    Write-Host "downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

    if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
    New-Item -ItemType Directory -Path $workDir | Out-Null
    Expand-Archive -Path $zip -DestinationPath $workDir -Force

    $inner = Get-ChildItem -Path $workDir -Directory | Select-Object -First 1
    $Prefix = $inner.FullName
}

Write-Host "FFmpeg prefix: $Prefix"

$ffmpeg = Join-Path $Prefix "bin\ffmpeg.exe"
if (-not (Test-Path $ffmpeg)) {
    Write-Error "no ffmpeg.exe under $Prefix\bin"
}

# The build has to actually be LGPL. A directory called "lgpl" proves nothing;
# the configuration string is what decides the licence.
$version = & $ffmpeg -hide_banner -version 2>&1 | Out-String
if ($version -match "--enable-gpl") {
    Write-Error "this FFmpeg build enables GPL; it cannot be used for a distributable nae.dll."
}
if ($version -match "--enable-nonfree") {
    Write-Error "this FFmpeg build enables nonfree components; it cannot be distributed at all."
}
Write-Host "configuration has no --enable-gpl and no --enable-nonfree"

$buildPath = Join-Path $engineDir $BuildDir
if (Test-Path $buildPath) { Remove-Item -Recurse -Force $buildPath }

$toolchain = Join-Path $engineDir "cmake/mingw-ucrt.cmake"

& cmake -S $engineDir -B $buildPath -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DNEIRO_FFMPEG_ROOT=$Prefix"
if ($LASTEXITCODE -ne 0) { Write-Error "configure failed" }

& cmake --build $buildPath
if ($LASTEXITCODE -ne 0) { Write-Error "build failed" }

# The shared FFmpeg DLLs have to sit next to the executables, or the loader
# fails before any check runs.
Get-ChildItem -Path (Join-Path $Prefix "bin") -Filter *.dll |
    Copy-Item -Destination $buildPath -Force

# The answer that matters: what does the DLL say about itself?
$caps = & (Join-Path $buildPath "neiro_cli.exe") caps 2>&1 | Out-String
Write-Host $caps
if ($caps -notmatch '"ffmpegLicense":"LGPL') {
    Write-Error "the engine did not report an LGPL licence. It said: $caps"
}

Write-Host ""
Write-Host "PASS: nae.dll reports an LGPL FFmpeg and links against one."
Write-Host "Run the checks from $buildPath as usual."
