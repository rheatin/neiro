# Neiro

Desktop UI host for [`rheatin/neiro-core`](https://github.com/rheatin/neiro-core), built with Native Shell (`nsh`) in TSX and Rust.

## Architecture

This repository contains **only the UI host** (`ui/`) and packaging scripts (`scripts/`). It has zero compile-time source dependency on the C++ audio engine (`engine/`) or the Rust Base layer (`base/`).

At runtime, [`ui/src/engine.rs`](ui/src/engine.rs) dynamically opens `libneiro_base.dylib` (macOS) or `neiro_base.dll` (Windows) via `libloading` and communicates over the `NAE_Base*` C ABI + JSON command/event bridge.

The target [`rheatin/neiro-core`](https://github.com/rheatin/neiro-core) SDK release version is pinned in [`NEIRO_CORE_VERSION`](NEIRO_CORE_VERSION).

| Path | Description |
|---|---|
| `NEIRO_CORE_VERSION` | Pinned `rheatin/neiro-core` GitHub Release tag (e.g. `v0.2.0`). |
| `ui/` | The Native Shell TSX + Rust host application. |
| `scripts/fetch-core.sh` / `scripts/fetch-core.ps1` | Downloads, verifies (SHA-256), and caches the prebuilt `neiro-core` SDK from GitHub Releases (or links a local `neiro-core` build via `--core-dir` / `-CoreDir`). |
| `scripts/build-app.sh` / `scripts/build-app.ps1` | End-to-end build: fetches the `neiro-core` SDK, runs `nsh check` & `nsh package`, and bundles the runtime dynamic libraries into `Neiro.app`. |
| `scripts/bundle-macos-app.sh` | Injects the macOS `neiro-core` SDK dylibs into `ui/.nsh/out/Neiro.app` and signs the bundle. |

## Building the App

### macOS (Apple Silicon)

```bash
# Downloads the pinned neiro-core release SDK (cached in .cache/neiro-core/) and builds ui/.nsh/out/Neiro.app
./scripts/build-app.sh
```

To build against a local `neiro-core` checkout (e.g. `../neiro-core`) during cross-repo development:

```bash
./scripts/build-app.sh --core-dir ../neiro-core
```

### Windows (x64 GNU / UCRT)

```powershell
# Downloads the pinned neiro-core release SDK and builds ui\.nsh\out\Neiro.app\Neiro.exe
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-app.ps1
```

To build against a local `neiro-core` checkout on Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-app.ps1 -CoreDir ..\neiro-core
```

## Updating the Core SDK Version

1. Tag and publish a new release in [`rheatin/neiro-core`](https://github.com/rheatin/neiro-core) (or run `scripts/package-sdk.sh` / `scripts/package-sdk.ps1` in `neiro-core`).
2. Update [`NEIRO_CORE_VERSION`](NEIRO_CORE_VERSION) in this repository to the new tag.
3. Run `./scripts/build-app.sh` (macOS) or `scripts/build-app.ps1` (Windows).
