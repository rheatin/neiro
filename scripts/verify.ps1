# Everything that can be checked without a human and without hardware.
#
# ASCII only (AGENTS.md rule 8): PowerShell 5 reads a BOM-less script as ANSI.
#
# The checks are split in two on purpose. A CI runner has no audio endpoint, so
# anything that opens a device would fail there for a reason that has nothing to
# do with the change under test. Those run only with -WithDevices, which is what
# a developer runs before pushing.
param([switch]$WithDevices)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $repoRoot "engine"
$build = Join-Path $engine "build"
# There is no MSVC linker here; the repository's Rust side is built with the GNU
# toolchain (AGENTS.md, "Toolchain"). Left to itself cargo picks the MSVC one and
# then finds a `link.exe` that is not a linker at all.
if (-not $env:RUSTUP_TOOLCHAIN) { $env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu" }

$failures = 0

function Run-One([string]$exe, [string[]]$arguments = @()) {
    $name = Split-Path -Leaf $exe
    Write-Host "--- $name"
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) {
        $script:failures++
        Write-Host "FAILED: $name (exit $LASTEXITCODE)"
    }
}

Write-Host "=== configure and build (engine) ==="
Push-Location $engine
try {
    # Same reason as in build-lgpl.ps1: a scoop MinGW ships cc.exe, not gcc.exe,
    # so the compiler is passed by path when the environment names one.
    $compilerArgs = @()
    if (-not $env:CC -or -not $env:CXX) {
        # A fresh shell has no MinGW on PATH, and CMake then finds whatever else
        # is there -- on this machine, clang, which fails the compiler check and
        # reports it as "the C compiler is not a full path". Naming the scoop
        # MinGW is the same choice the repository's .cargo/config.toml makes.
        $mingw = Join-Path $env:USERPROFILE "scoop/apps/mingw/current/bin"
        if (Test-Path (Join-Path $mingw "cc.exe")) {
            if (-not $env:CC)  { $env:CC  = Join-Path $mingw "cc.exe" }
            if (-not $env:CXX) { $env:CXX = Join-Path $mingw "c++.exe" }
        } else {
            throw "set CC and CXX: no MinGW was found at $mingw"
        }
    }
    $compilerArgs += "-DCMAKE_C_COMPILER=$env:CC"
    $compilerArgs += "-DCMAKE_CXX_COMPILER=$env:CXX"
    # A build directory configured before the vendored tree existed keeps its
    # old cached NEIRO_FFMPEG_ROOT; CMake does not re-apply a default over a
    # cached value. Naming it here is what makes "the default is LGPL" true for
    # an existing build directory as well as a fresh one.
    $vendor = Join-Path $repoRoot "vendor/ffmpeg-lgpl"
    if (Test-Path (Join-Path $vendor "include/libavcodec/avcodec.h")) {
        $compilerArgs += "-DNEIRO_FFMPEG_ROOT=$vendor"
    }
    & cmake --preset windows @compilerArgs
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & cmake --build --preset windows
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally {
    Pop-Location
}

# These do arithmetic on signals they build themselves. None opens an output
# device, and none takes an argument -- `dsd_band_check` and `param_check` also
# need no device but do take a file, so they are not here: running them without
# one prints a usage line and exits 1, which reads as a failure of the change
# under test rather than of the fixture it was never given.
$deviceFree = @(
    "dither_check", "resample_check", "hilbert_check", "loudness_check",
    "clip_guard_check", "linear_phase_eq_check", "partitioned_convolver_check",
    "dsd_filter_check", "dsd_downrate_check",
    "analysis_check", "struct_size_check", "format_matrix"
)

Write-Host "=== engine checks (no device) ==="
foreach ($name in $deviceFree) {
    $exe = Join-Path $build "$name.exe"
    if (Test-Path $exe) { Run-One $exe } else { Write-Host "missing: $name" }
}

if ($WithDevices) {
    Write-Host "=== engine checks (need an endpoint) ==="
    Push-Location $build
    try {
        foreach ($name in @("nae_test", "mode_check", "reopen_check", "source_width_check")) {
            $exe = Join-Path $build "$name.exe"
            if (Test-Path $exe) { Run-One $exe }
        }
        $skip = Join-Path $build "skip_check.exe"
        if ((Test-Path $skip) -and (Test-Path (Join-Path $build "g1.wav"))) {
            Run-One $skip @("g1.wav", "g2.wav")
        }
    } finally {
        Pop-Location
    }
}

Write-Host "=== base ==="
Push-Location (Join-Path $repoRoot "base")
try {
    & cargo run --bin transport_check
    if ($LASTEXITCODE -ne 0) { $failures++ }
    & cargo run --bin index_probe
    if ($LASTEXITCODE -ne 0) { $failures++ }
    if ($WithDevices) {
        & cargo run --bin base_probe
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
} finally {
    Pop-Location
}

Write-Host "=== binding ==="
Push-Location (Join-Path $repoRoot "binding")
try {
    & cargo check
    if ($LASTEXITCODE -ne 0) { $failures++ }
    $probe = Join-Path $repoRoot "test/tagtmp/a.flac"
    if ((Test-Path $probe) -and $WithDevices) {
        & cargo run --bin probe -- $probe
        if ($LASTEXITCODE -ne 0) { $failures++ }
    }
} finally {
    Pop-Location
}

if ($failures -ne 0) {
    Write-Host ""
    Write-Host "FAILED: $failures step(s)"
    exit 1
}
Write-Host ""
Write-Host "PASS: every check that ran passed."
