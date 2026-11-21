# Goldfish Review Report

**Target:** `/Users/char/Repos/neiro/docs/designs/neiro-ios.md` (lines 224–320, `## Detailed Implementation Plan` through end of file)

## Reviewer Status

| Reviewer | Status |
| --- | --- |
| `egm-implementation-readiness` | completed |
| `egm-codebase-details` | completed |
| `egm-style` | completed |

## Prioritized Findings (Blockers -> Major -> Nits)

### Non-existent entry-point path `ui/src/App.tsx` - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `242` (`ui/src/App.tsx (Modify)`)
* **Discrepancy Description**: The plan marks `ui/src/App.tsx` for modification. That path does not exist; the entry point is `/Users/char/Repos/neiro/ui/App.tsx`. Every existing test opens `../App.tsx`, and `ui/tests/layout-and-formats.test.tsx` exercises the desktop/mobile split this stage changes. `ui/App.tsx` also runs `enumerateAtStartup()`, `refreshPlugins()` and `restorePolicyAtStartup()` unconditionally, none of which is gated in this task.
* **Rationale & Recommendation**: A fresh implementer following the write allowlist either fails to find the file or creates an orphan module that is never compiled. Correct the path to `ui/App.tsx` and add explicit desktop gating for enumeration, plugin refresh and policy restore.

### `EngineHandle` does not exist; the engine seam is undefined - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`, `egm-style`
* **Affected Scope:** `232–234`, `268`
* **Discrepancy Description**: Lines 232 and 234 require re-exporting and implementing `EngineHandle`. The repository exposes a concrete `pub struct Engine` at `/Users/char/Repos/neiro/ui/src/engine.rs:61`; `ui/tests/engine_shutdown.rs:10` imports `neiro_ui::engine::Engine`, and `ui/src/lib.rs` imports `Engine` directly. No `EngineHandle` symbol exists under `ui/`. The real surface used by `lib.rs` is large (`open`, `command`, `snapshot`, `remote_providers`, `list_directory`, plugin methods, queue methods, `Drop`), and the plugin methods cannot compile on iOS if the symbol set is omitted.
* **Rationale & Recommendation**: Keep `Engine` as the seam name and state the exact re-export form and cfg split, or define a trait with its methods and the shared code that is gated. Explicitly state which plugin/queue methods are stubbed or cfg-gated on iOS.

### Stage 0 module split cannot build on either target - Severity: Blocker
* **Reviewer Lens:** `egm-codebase-details`, `egm-style`
* **Affected Scope:** `232–233`
* **Discrepancy Description**: The plan says `ui/src/engine.rs` will "declare `mod desktop;` and `mod ios;`" without `#[cfg]` attributes. `desktop.rs` needs `libloading`, which line 230 removes on iOS; `ios.rs` needs the `objc2-*` crates, which line 230 adds only under the iOS target. The loader is also not separable from `Engine`: `Engine` owns `Library`, `Prepared`, `Exports`, `dll_candidates`, `symbol`/`symbol_optional` and the `Drop` implementation (`ui/src/engine.rs:58–190`, `756–943`).
* **Rationale & Recommendation**: State the exact `#[cfg(target_os = "ios")]` / `#[cfg(not(...))]` gating for both modules, what moves out of `Engine` and what stays shared. As written Stage 0 cannot leave the tree buildable.

### Stub ABI cannot satisfy the engine seam - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `234`, `237`
* **Discrepancy Description**: The temporary stub is specified to export only `NAE_BaseVersion` and `NAE_BaseHealthJson`. Neither symbol is declared by the v0.2.0 header (`/Users/char/Repos/neiro/.cache/neiro-core/v0.2.0/neiro-core-v0.2.0-macos-arm64/include/nae_base.h`) or used in `ui/src`. The host resolves `NAE_BaseStartWithLibrary`, `NAE_BaseCommand`, `NAE_BaseSubscribe`, `NAE_BaseUnsubscribe`, `NAE_BaseSnapshot`, `NAE_BaseFreeString`, `NAE_BaseShutdown`, plus the optional queue/plugin/remote set (`ui/src/engine.rs:805–885`).
* **Rationale & Recommendation**: Enumerate the pinned symbol set and have the stub export all required symbols with inert behaviour, or explicitly scope Stage 0 to `version()`/`health()` and defer the rest. The plan must also say what the ABI pin checks, since the real header exposes no version symbol.

### Static xcframework linking contradicts the undecided linking model and the absent artifact - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `231`, `235–236` (also `289`, `296`)
* **Discrepancy Description**: The plan builds toward a static `neiro_base.xcframework`, but Open Questions (line 164) say the LGPL FFmpeg linking model "is not decided." `scripts/fetch-core.sh` downloads only a macOS SDK; the cached v0.2.0 artifact contains `.dylib` files and no `.a`/`.xcframework`. Line 236 nevertheless makes the new CI job fail when the fetch fails, so it is red on day one, and the fetch script and stub both target `ui/vendor/` without a precedence rule.
* **Rationale & Recommendation**: Keep the stub as an explicit spike artifact, add a Stage 0 task that decides and documents the real linking model, and do not fail CI on a missing xcframework until a `neiro-core` release produces one.

### Range-read direction is explicitly deferred, with no Stage 0 task - Severity: Blocker
* **Reviewer Lens:** `egm-style`, `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `287`, `289`
* **Discrepancy Description**: Line 287 leaves the host/core boundary as "the range-read command the core calls back into, **or** the host-side range source the core reads; the exact direction is fixed with `neiro-core` in Stage 0." Stage 0 (228–237) contains no such agreement task, and v0.2.0 exposes only whole-file `fetch_remote`. Line 289 then calls `neiro-core` external with "any new events," an unbounded dependency with no owner or fallback.
* **Rationale & Recommendation**: Choose one direction, name the C ABI function or host trait method and its signature/JSON contract, pin the required `neiro-core` release, and add the corresponding Stage 0 or Stage 4 task. Until then Stage 4 cannot be executed without an independent design decision.

### Stage 4 host work is assigned to TSX store files that cannot perform it - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `285–286`
* **Discrepancy Description**: `ui/src/state/cache.ts` is a statistics/clear UI over the Rust host's cache buckets (`engine.cacheStats()`, `engine.clearCache()`); the cache implementation and temp-file/atomic-rename logic live in `ui/src/lib.rs` (`cache_stats_in`, `clear_cache_in`, `cover_cache_write`). `ui/src/state/remote.ts` only calls `engine.fetchRemote(id)`; the download is `Base::fetch_remote` in `ui/src/lib.rs:1930`. The bullet also describes existing state as new work: the remote listing is already persisted (`ui/src/state/remote.ts:233–241`) and there is no persisted "downloaded" set today.
* **Rationale & Recommendation**: Assign the stream cache and range source to `ui/src/lib.rs` and `neiro-core`; keep `state/cache.ts` for stats and `state/remote.ts` for UI state and the device ID. Specify the byte cap value, LRU key, temp-file naming, reconciliation timing and whether the cache is per-server.

### Stage 3 bookmark/root model is assigned to the wrong file and has no lifetime contract - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `277–279`
* **Discrepancy Description**: Local roots are owned and persisted in `ui/src/state/catalog.ts` (`ROOTS_KEY = "library:roots"`, `roots` signal, `addRoot`, `removeRoot`, watcher and reconcile setup). `ui/src/state/sources.ts` only derives `localSources` and calls `addRoot()`. Changing a root from an absolute path to a bookmark plus display path also affects `sources.ts`, `sidebar.tsx`, `pages/tracks.tsx` and `pages/home.tsx`, none of which is listed. The bookmark blob contents, storage key, resolve/re-resolve triggers, `startAccessing`/`stopAccessing` lifetime and migration of existing absolute-path entries are unspecified.
* **Rationale & Recommendation**: Put the root model in `catalog.ts`, list all consumers, and define the bookmark format, storage location, access lifetime, release triggers and migration. Without that the implementer must invent the local-file data model.

### New and Home depend on data no API provides - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `252`, `254`
* **Discrepancy Description**: `pages/new.tsx` and the rewritten `pages/home.tsx` require "recently added", "newly imported", "continue listening" and "recently played". The host exposes only `list_most_played(limit)` and `record_play(path, at_ms)`; no recent-plays query, added-at timestamp or resume position exists. `ui/src/pages/user-collections.tsx:116` explicitly notes local tracks have no `addedAt`; `RemoteTrackRow` has no creation date.
* **Rationale & Recommendation**: Add the required core command/event and host/TS bridge methods with field names, ordering and persistence, or restate New and Home using existing data (most-played and the current scan diff). Also state how remote "recently added" is obtained.

### Off-main-thread engine open has no lifecycle contract and its TS bridge is out of scope - Severity: Blocker
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `267`, `269`
* **Discrepancy Description**: `Base::new` (`ui/src/lib.rs:1525`) synchronously calls `Engine::open` and `snapshot()`. If the open moves off the main thread, the constructor returns before an engine exists, so every method must queue or fail and listeners must receive the snapshot later. The plan defines none of that and never lists `ui/src/backend/engine.ts` or `ui/src/backend/types.ts`, the only TSX bridge for the engine and its `HealthStatus` (`"starting" | "ready" | "failed"`).
* **Rationale & Recommendation**: Define the opening state (including whether `HealthStatus` gains a value), what `new Base()` returns while opening, how `connect()`/`retry()` react, and how queued commands and the deferred snapshot behave. Add `backend/engine.ts` and `backend/types.ts` to the write allowlist.

### Stage dependency graph is wrong and permits conflicting edits - Severity: Blocker
* **Reviewer Lens:** `egm-style`, `egm-implementation-readiness`
* **Affected Scope:** `226`
* **Discrepancy Description**: Line 226 says "stage 2 depends on stage 0; stages 3 and 4 depend on stage 2." Stage 3 (bookmarks/scans) has no dependency on the audio session; Stage 4 depends on `neiro-core`/Stage 0, not Stage 2; Stage 5 edits files created in Stage 0 without a stated prerequisite. The file table also overlaps: Stage 1 and Stage 2 both edit `ui/src/pages/settings.tsx` (lines 256 and 273), Stage 3 edits `pages/library.tsx` and `settings.tsx` from Stage 1, and Stage 4 edits `pages/radio.tsx` from Stage 1.
* **Rationale & Recommendation**: Replace the sentence with dependencies derived from file overlap, state Stage 5's prerequisites, and sequence the `settings.tsx` edits so only one stage owns the file at a time.

### Library category routes and pages do not exist; Liked contradicts itself; Downloaded contradicts the non-goal - Severity: Blocker
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `241`, `251`, `281`
* **Discrepancy Description**: Lines 241 and 251 add `GENRES` and `DOWNLOADED` as Library categories, but `ui/src/nav.ts:14–30` has no such `Route` variants and there are no `pages/genres.tsx` or `pages/downloaded.tsx`. "Tapping a category pushes its existing page" is therefore false for both. `LIKED` is added at line 241 but omitted from the category list at line 251. A "Downloaded" category also contradicts Non-Goals line 51 (no explicit offline download library) and Stage 4's "no persisted downloaded set."
* **Rationale & Recommendation**: Either add the route variants, page files, `main.tsx` switch cases, data sources and reconcile the non-goal, or drop the categories. Resolve the Liked inconsistency.

### Unresolved either/or choices left to the implementer - Severity: Blocker
* **Reviewer Lens:** `egm-style`, `egm-implementation-readiness`, `egm-codebase-details`
* **Affected Scope:** `243`, `247`, `273`
* **Discrepancy Description**: Three bullets delegate design choices: line 243 "Remove `wideTracks()`'s hard desktop gate **or** make it width-based"; line 247 "Adapt [studio-minimal] to the five roots **or** explicitly mark it desktop-only"; line 273 "keep dither only if the engine path can apply it." For `wideTracks()` the current implementation is already width-based apart from the gate, and `studio-minimal.tsx` already reuses `MainStack`/`NavControls` and is mounted only under the desktop branch, so both options are also based on partly false premises.
* **Rationale & Recommendation**: State the chosen outcome for each with the exact change (for example, `wideTracks()` becomes a width comparison; `studio-minimal.tsx` is marked desktop-only; iOS hides dither), consistent with the Zero-Decision Rule.

### Generated-file policy and concrete plist/ATS/file-protection values are undecided - Severity: Blocker
* **Reviewer Lens:** `egm-style`, `egm-implementation-readiness`
* **Affected Scope:** `293–294`
* **Discrepancy Description**: `ui/Info.plist` / entitlements is marked "(Create or generate)", followed by "If NSH generates these files, record the generation rule." The implementer must first discover NSH's behaviour, then choose a policy. The same bullet defers "the ATS decision for LAN HTTP" and "the file-protection class" without stating either. `ui/nsh.json` currently contains no iOS keys, and the privacy manifest file is not named anywhere.
* **Rationale & Recommendation**: State whether the plist/entitlements are committed or generated, give concrete ATS keys/values and domains, name the protection class (for example `NSFileProtectionCompleteUnlessOpen`) with rationale, and name the privacy-manifest inputs and destination.

### Navigation and shell wiring is incomplete; `main.tsx` is already correct - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `241–244`, `248–249`, `259`
* **Discrepancy Description**: `ui/src/shells/main.tsx:66–84` already keeps one live `NavStack` per visited root keyed by `root.name` and hides inactive layers with `visibility: hidden`; the reset behaviour lives in `openRoot` (`ui/src/nav.ts:278–296`), not the shell. Missing changes include the `Route` union, `same()` exhaustive switch, `ROOT_ROUTES`, `main.tsx`'s `page()` switch, and a `ModalId` in `state/overlays.ts` for Settings. The folded shell (`mobile.tsx`) has no header or account avatar, yet `header.tsx` is said to own the avatar "on mobile" and is rendered only by desktop/studio shells, so Settings is unreachable folded.
* **Rationale & Recommendation**: Restate the `main.tsx` bullet as "no change needed," enumerate the actual `nav.ts`/`main.tsx`/`overlays.ts` changes, place the avatar in the folded shell, and name how it opens Settings.

### Chrome record fields diverge and most consumers are omitted - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`, `egm-style`
* **Affected Scope:** `243` (versus `84–90`, `248`, `250`)
* **Discrepancy Description**: The architecture record includes "whether a title bar exists"; the Stage 1 field list omits it and adds "content width," which `title-bar.tsx`/the mobile top inset then depends on. The plan lists only `sidebarWidth()` and `wideTracks()` as consumers, but geometry is spread across `measureShell`/`measuredShellWidth`, `barSize`, `pageEnd` (many pages), `AppSheet`'s `titleBarArea()` padding, `sidebarCollapsed` and a hardcoded `Toaster bottomOffset={96}` in `ui/App.tsx`.
* **Rationale & Recommendation**: Use one field list in both sections, name the module that defines the record type, and either add the consumer files to the allowlist or state that existing predicate names are retained and only their internals change.

### Settings-as-modal migration is incomplete - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `242`, `248–249`
* **Discrepancy Description**: Settings is currently a root route rendered by `main.tsx:33–34`, reached from `sidebar.tsx:132` (a `NavRow` with `route={SETTINGS}`) and `header.tsx:104` (`switchTo(SETTINGS)`). The plan does not remove the route or those call sites, does not say how desktop opens Settings, and does not add the overlay state needed for a root-level modal.
* **Rationale & Recommendation**: Enumerate the route removal, the `switchTo`/NavRow removals or rewiring, the desktop entry point and the overlay/modal identity.

### Document-picker API and UIKit dependency are not identified - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `230`, `277`
* **Discrepancy Description**: The existing folder picker is `Base::pick_folder` using `nsh_core::file_dialog::OpenDialog::folders()` (`ui/src/lib.rs:2646–2657`); NSH's TS `Picker.pickFiles` is files-only. iOS security-scoped folder picking needs `UIDocumentPickerViewController`/`UIApplication`, i.e. UIKit, which is not among the added `objc2` crates at line 230.
* **Rationale & Recommendation**: State whether `pick_folder` is rewritten natively or NSH's picker is extended, and add the missing UIKit dependency.

### The TS wrapper layer for new native exports is absent from the file boundary - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `266–267`, `278`, `287`, `295`
* **Discrepancy Description**: `ui/src/backend/engine.ts` is the single module importing `neiro/native` and wraps every native method; `ui/src/backend/types.ts` holds the mirrored contract. Audio-session commands, bookmark resolve/scan, range reads and the Keychain bridge cannot be called from TSX without changes to both. Neither file appears in lines 224–320.
* **Rationale & Recommendation**: Add both files to every stage that adds native methods, and name the new methods and types.

### Stage 2 capability/event API has no wire contract and parsing is put in an iOS-only module - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `268`
* **Discrepancy Description**: No event names, JSON shape, payload fields, defaults or fail-closed behaviour are given. `WireEvent` and `parse()` are target-agnostic and live in `ui/src/engine.rs:1504–1662`; `Engine::open`/`Exports` never parse events, so placing parsing in `engine/ios.rs` duplicates the shared parser. The plan also does not say how `audio_session.rs` obtains the engine handle, receives track changes for `MPNowPlayingInfoCenter`, or routes remote commands to the same functions the UI calls.
* **Rationale & Recommendation**: Specify the command/event schema, keep parsing in the shared module, and define the session-to-engine ownership and callback path.

### `ui/src/state/engine.ts` is not where engine state lives - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `269`
* **Discrepancy Description**: `ui/src/state/engine.ts` is a thin health accessor; the mirrored engine signals (`healthSig`, `renderPathSig`, `devicesSig`, etc.) are in `ui/src/backend/engine.ts:98–136`, and contract types are in `ui/src/backend/types.ts`. Neither is in the target list.
* **Rationale & Recommendation**: Add the capability report and route state to `backend/engine.ts` signals and `backend/types.ts`, and correct the file in the plan.

### Output-surface list omits `dsp-matrix.tsx` and the "WASAPI" premise is wrong - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `271`
* **Discrepancy Description**: No UI code assumes "WASAPI"; the app reads `renderPath`/`resolvedPath().backend`, which already enumerates `"coreaudio"`. The desktop assumptions are the exclusive controls in `output-interface.tsx:311–329` and the bit-perfect action in `master-bus.tsx:184–207`. The device/rate policy UI most affected by "no device enumeration on iOS" is `surface/dsp-matrix.tsx`, which is absent from the list.
* **Rationale & Recommendation**: Add `dsp-matrix.tsx` and describe the actual desktop assumptions rather than a non-existent WASAPI path.

### `isStreamBitPerfect()`'s iOS meaning is not a precise predicate - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`, `egm-style`
* **Affected Scope:** `270`
* **Discrepancy Description**: The current predicate (`ui/src/state/resolved.ts:176–183`) checks `renderPath.bitPerfect`, `opraActive()`, `renderPath.crossfadeEnabled` and `settings.crossfadeEnabled`. The plan names no new capability field, no distinction from `rp.bitPerfect`, no rule for the existing crossfade/OPRA checks, and "the first release never sets that flag" has no antecedent (the subject is a function).
* **Rationale & Recommendation**: State the exact predicate and its source fields, or say unconditionally that iOS returns false in the first release. Align with the AGENTS.md single-owner invariant.

### Bottom-sheet detents are not supported by the pinned NSH version - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `259`
* **Discrepancy Description**: NSH 0.6.1's `Sheet` exposes `side`, `closeOnSwipe`, `popupClass`/`popupStyle` and backdrop options but no detents (`ui/src/.nsh/types/nsh/base.d.ts:4621–4652`); `AppSheet` exposes no detent prop either (`ui/src/components/ui/overlays.tsx:54–74`). The plan depends on a capability that does not exist.
* **Rationale & Recommendation**: Specify a custom implementation or drop the detent requirement.

### Keychain work has no module boundary and the migration is under-specified - Severity: Major
* **Reviewer Lens:** `egm-style`, `egm-implementation-readiness`
* **Affected Scope:** `295`
* **Discrepancy Description**: No Rust module is created for the Keychain bridge (a 7000-line `lib.rs` is the only named home). Credentials live in `state/sources.ts` under `remote:servers` as the whole roster (including non-secret `id`, `label`, `kind`, `baseUrl`) and in `state/remote.ts` under `remote:config`. "Deletes the old `Storage` blobs" would destroy non-secret fields and break the legacy fallback; `remote.ts` is not in the Stage 5 file list, and migration order, idempotency and rollback are unspecified.
* **Rationale & Recommendation**: Create a dedicated `keychain.rs` with its public functions, name both storage keys, split secrets from non-secret records, and define idempotent migration and rollback.

### Per-install Jellyfin device ID change only touches a probe - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `285`
* **Discrepancy Description**: `Device="desktop", DeviceId="neiro-base"` occurs only in `probeRemoteConfig` (`ui/src/state/remote.ts:111,128`). Listings, artwork and streaming go through `engine.setRemote`/`listRemote`/`fetchRemote` into `neiro-core`, where the live request identity is built. Changing `remote.ts` alone will not fix cross-install collisions.
* **Rationale & Recommendation**: Add a `device_id` to `set_remote`, or state that the core owns device identity; specify the format and storage location.

### Tests exercise native behaviour from TSX with no seam, and iOS Rust code is untested - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `302–311`
* **Discrepancy Description**: `audio-session.test.tsx`, `ios-import.test.tsx` and `remote-stream.test.tsx` target behaviour implemented in `audio_session.rs`, `lib.rs` and `neiro-core`, not TSX; `nsh test` is headless TSX with no `AVAudioSession` or filesystem-cache surface. No fake/injection bridge is defined, no harness files are listed although every multi-step test in this repo uses one, and `cargo test --lib` on a macOS runner does not compile `#[cfg(target_os = "ios")]` modules.
* **Rationale & Recommendation**: State where each behaviour is tested (Rust unit tests with fakes for `audio_session.rs`, `engine/ios.rs`, bookmark helpers; thin TSX tests over an injected bridge), list the harness files, and add an iOS-target test command or an explicit manual verification step.

### The verification plan omits the integration test it modifies and has no link/ABI check - Severity: Major
* **Reviewer Lens:** `egm-style`, `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `311`, `315–320`
* **Discrepancy Description**: `cargo test --manifest-path ui/Cargo.toml --lib` does not run `ui/tests/engine_shutdown.rs`; the macOS workflow runs it explicitly with `--test engine_shutdown` (`.github/workflows/macos.yml:44–46`). `cargo check` also does not link, so nothing verifies the pinned symbols are present or that the stub is not shipped. Separately, "off-main-thread open" is implemented in `Base::new`, not `Engine::open`, which `engine_shutdown.rs` calls directly.
* **Rationale & Recommendation**: Add `cargo test --manifest-path ui/Cargo.toml --test engine_shutdown` and an `nm`/`otool` symbol check (plus a release check that the real artifact, not the stub, is linked). Clarify which layer owns the off-main-thread open.

### "The four engine-dependent failures that predate this work" are uncalibrated - Severity: Major
* **Reviewer Lens:** `egm-style`, `egm-implementation-readiness`
* **Affected Scope:** `317`
* **Discrepancy Description**: The plan asserts an exact count with no test names, failure modes, baseline command or captured output, and "separated" names no mechanism (skip list, quarantine or baseline). A fresh session cannot verify the count or apply the separation.
* **Rationale & Recommendation**: Enumerate the four tests and their expected failures, or include the baseline command and captured output, and state how they are excluded.

### The iOS CI job is not executable as written - Severity: Major
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `236`, `297`
* **Discrepancy Description**: The job names no runner, `rustup target add aarch64-apple-ios aarch64-apple-ios-sim`, `nsh component add ios` runtime step, simulator boot, or the `CORE_RELEASE_TOKEN` secret used by `macos.yml`. `docs/ios/phase0-s1.md` records that the NSH iOS runtime is not installed and the simulator list is empty. The existing workflow only runs `nsh check` and only if `nsh` is present. `nsh run --target ios` is a hot-reload dev command, not a CI assertion.
* **Rationale & Recommendation**: Specify the full job (runner, targets, runtime component, simulator boot, secret) and a deliberate stub fallback or skip until the real artifact exists.

### `build.rs` slice selection, `ui/vendor/` policy and the version pin are underspecified - Severity: Major
* **Reviewer Lens:** `egm-style`, `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `231`, `235`, `296`
* **Discrepancy Description**: "Emit `cargo:rustc-link-search` for the device or simulator slice" gives no rule for selecting the slice (Rust sees only `TARGET`/`CARGO_CFG_TARGET_ARCH`/`CARGO_CFG_TARGET_ABI`), no expected `.xcframework` layout, no missing-artifact behaviour, no `cargo:rerun-if-changed`, and no `ui/.gitignore` entry for `vendor/`. "This is the only place that knows where the core artifact lives" conflicts with the fetch script writing `ui/vendor/`. Staging pins the version in Stage 0 (line 235) and Stage 5 (line 296) without stating it; `scripts/fetch-core.sh` already reads `NEIRO_CORE_VERSION`.
* **Rationale & Recommendation**: Name artifact paths, the slice-selection rule, the version source (`NEIRO_CORE_VERSION`), the gitignore decision and one stage that owns the pin.

### "One network trust path" has no task - Severity: Major
* **Reviewer Lens:** `egm-implementation-readiness`
* **Affected Scope:** `283–289`
* **Discrepancy Description**: The architecture requires probing, listings and streams to share one client and one ATS/local-network policy. Today `state/remote.ts:91–189` probes with `fetch`, while listings/streams go through Base's Rust client (`set_remote`, `list_remote`, `fetch_remote`), so a server can probe successfully and then fail to stream. No Stage 4 bullet addresses this.
* **Rationale & Recommendation**: Add a task that names the single client and policy surface, and list the files involved (`state/remote.ts`, `backend/engine.ts`, `lib.rs`).

### Stage 2 introduces a desktop behaviour change that contradicts G7 - Severity: Major
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `268` (versus `43`)
* **Discrepancy Description**: G7 promises desktop behaviour and the runtime-loaded path remain unchanged "except where shared code is refactored behind a target seam." Line 268 requires "the desktop loader parses the same events when the core emits them," which is new desktop behaviour, not a seam, and the plan does not say whether desktop output surfaces consume those events.
* **Rationale & Recommendation**: Either scope the parsing behind the iOS target and leave the desktop loader untouched, or record it as an explicit desktop change with its test updates.

### Plugin-manager gating names three different mechanisms - Severity: Major
* **Reviewer Lens:** `egm-style`, `egm-implementation-readiness`
* **Affected Scope:** `234`, `242` (versus `103`)
* **Discrepancy Description**: Line 234 relies on symbols being undeclared so the manager "cannot report them" (compile-time absence); line 242 says to move the desktop-only plugin manager "behind a desktop check" (runtime); the architecture line 103 says "compiled out on iOS." `ui/src/state/plugins.ts` also calls `engine.listPlugins`, `reloadPlugins` and `setPluginEnabled`, which must be gated for the crate to compile.
* **Rationale & Recommendation**: State one mechanism and its exact form (`#[cfg(not(target_os = "ios"))]` on the module and call sites) and how `state/plugins.ts` is gated.

### Evicted-file UI is assigned to the category-list page - Severity: Major
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `281` (versus `251`)
* **Discrepancy Description**: Stage 1 defines `pages/library.tsx` as the category list; Stage 3 says that same page should "show evicted or missing files as unavailable, with a re-pick action." A category list does not render files.
* **Rationale & Recommendation**: Assign the unavailable state to the category detail page (for example Tracks or Downloaded) or name the exact component that renders file rows.

### The spike directory is a glob with no removal gate - Severity: Major
* **Reviewer Lens:** `egm-implementation-readiness`, `egm-style`
* **Affected Scope:** `237`
* **Discrepancy Description**: `spike/neiro_base_stub/` is a hand-written-source directory glob, which the standard permits only for generated output. It gives no exact file names or output paths, no stage that deletes the stub, and no CI guard against shipping it; `spike/` does not exist in the repository.
* **Rationale & Recommendation**: List the exact files (for example `spike/neiro_base_stub/nae_stub.c` and `build-ios.sh`), their outputs, and add a Stage 5 removal task or CI assertion.

### The iOS data-root branch is missing - Severity: Major
* **Reviewer Lens:** `egm-implementation-readiness`
* **Affected Scope:** `277–278`, `295`
* **Discrepancy Description**: `data_root()` (`ui/src/lib.rs:523–549`) handles Windows and macOS and falls through to the generic unix branch on iOS, returning `XDG_DATA_HOME`, `HOME/.local/share` or `"."`. The design requires Application Support for `library.db`, bookmarks and settings, and Caches excluded from backup. The plan never mentions `data_root()`, `library_path()`, `remote_cache_dir()` or `local_cover_cache_dir()`.
* **Rationale & Recommendation**: Add the iOS container branch explicitly and state backup-exclusion behaviour for Caches.

### No stage has acceptance criteria, and tests are not mapped to stages - Severity: Major
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `226`, `301–320`
* **Discrepancy Description**: Line 226 asserts each stage has a verifiable result, but no stage defines an exit condition. The `Tests` and `Verification Plan` lists are global and unnamed relative to stages, so a reviewer cannot tell which test proves a stage complete. The test bullets are topic lists, not cases with expected outcomes (for example "safe areas respected", "Cache cap and eviction" with no cap value).
* **Rationale & Recommendation**: Add a "Done when" line per stage naming the tests and observable result, and restate each test as input → expected outcome.

### The existing Phase 0 plan is not reconciled and contradicts Stage 0 - Severity: Major
* **Reviewer Lens:** `egm-implementation-readiness`
* **Affected Scope:** `228–238`
* **Discrepancy Description**: `docs/ios/phase0-s1.md` already specifies the toolchain (`rustup target add`, `nsh component add ios`, simulator), the stub, the build.rs link flags and the simulator run, and describes the internal seam as "at least `version()` and `health()`" with a temporary `layout()` change. Stage 0 instead says `EngineHandle`, omits the toolchain/runtime setup, and defers `layout()` "mobile" to Stage 1. A fresh implementer has two contradictory Stage 0 specifications.
* **Rationale & Recommendation**: Cite and either incorporate or explicitly supersede `docs/ios/phase0-s1.md`, aligning seam names, toolchain steps and stub scope.

### `title-bar.tsx` is already desktop-only - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `250`
* **Discrepancy Description**: `TitleBar` draws only when `titleBarArea().height > 0`, and `drawsTitleBar` is NSH's desktop `isAvailable` (`ui/src/components/ui/title-bar.tsx:13,19–22,71`). "Draw only on desktop" needs no change; only the mobile top inset is new work.
* **Rationale & Recommendation**: Reword the bullet to the actual change.

### The desktop search field already lives in the title bar; desktop entry path is unspecified - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `249`, `255`
* **Discrepancy Description**: `SearchField` is rendered by `TrailingControls`, which `desktop.tsx:28–33` places inside `TitleBar` (`ui/src/components/header.tsx:98–107`). The plan moves the field to `pages/search.tsx` but does not say how desktop users reach it once the header stops rendering it, or which change removes it.
* **Rationale & Recommendation**: Scope the statement to mobile and state the desktop entry path and the header change.

### `selection-bar.tsx` does not match the described problem - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `261`
* **Discrepancy Description**: `ui/src/components/library/selection-bar.tsx:30–77` is a visible action bar of buttons; it has no hover-only play buttons or `onContextMenu`-only actions. Only `cards.tsx`, `remote-cards.tsx` and `tracks.tsx` match the described problem.
* **Rationale & Recommendation**: Remove `selection-bar.tsx` from that bullet.

### Settings has no filesystem-watcher control to hide - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `256`
* **Discrepancy Description**: `pages/settings.tsx` contains no watcher toggle; the watcher is enabled unconditionally in `catalog.ts:80`.
* **Rationale & Recommendation**: Reword the bullet to the iOS behaviour actually being removed.

### `fetch-core-ios.sh` should reuse the existing version file - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`, `egm-implementation-readiness`
* **Affected Scope:** `235`, `296`
* **Discrepancy Description**: `scripts/fetch-core.sh:10–14` already reads `NEIRO_CORE_VERSION` (present at the repository root) with an override; a second pin mechanism in a new script is avoidable drift.
* **Rationale & Recommendation**: Reuse `NEIRO_CORE_VERSION` and keep a single pin owner.

### The mini-player mode should reuse `NowPlayingSummary` - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `257`
* **Discrepancy Description**: `ui/src/components/player/parts.tsx` already exports the cover/title summary used by `now-playing.tsx:18`; the plan does not mention reuse, risking a second cover/title rendering path.
* **Rationale & Recommendation**: Name the existing component and reuse it.

### `studio-minimal.tsx` already has no separate navigation model - Severity: Nit
* **Reviewer Lens:** `egm-codebase-details`
* **Affected Scope:** `247`
* **Discrepancy Description**: It imports `MainStack` and `NavControls` (`ui/src/shells/studio-minimal.tsx:11–17`) and is mounted only under the desktop branch (`ui/App.tsx:134–138`), so the warning about a silently broken second model does not correspond to the code.
* **Rationale & Recommendation**: Keep the chosen outcome but drop the false premise.

### `Tests` and `Verification Plan` are formatted as unnumbered stages - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `301`, `313`
* **Discrepancy Description**: Both use `###`, the same level as `### Stage N — Title`, so they read as Stage 6; Stage 5's title also duplicates "Verification Plan."
* **Rationale & Recommendation**: Number them or demote them to `####` under a non-stage heading.

### Path form is inconsistent in multi-file bullets - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `261`, `288`
* **Discrepancy Description**: Line 288 writes `ui/src/pages/radio.tsx` / `pages/remote.tsx` (second path not repository-relative); line 261 groups four filenames after a single directory. The standard asks for exact repository-relative paths.
* **Rationale & Recommendation**: Spell out each path.

### Article error - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `234`
* **Discrepancy Description**: "a `EngineHandle` implementation."
* **Rationale & Recommendation**: "an `EngineHandle` implementation" (or, after the seam fix, "an `Engine` implementation").

### Subjective emphasis and unverified universals - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `231`, `232`, `241`
* **Discrepancy Description**: "This is the only place that knows where the core artifact lives" is contradicted by `fetch-core-ios.sh`; "Every current call site keeps working" is unverifiable and contradicted by the same bullet's `EngineHandle`; "This is the centre of the IA change; every navigation test depends on it" is opinion plus an unverified universal. Impacted harnesses such as `page-scroll-harness.tsx`, `virtual-grid-harness.tsx` and others are not listed.
* **Rationale & Recommendation**: Replace with the concrete facts and name the affected tests/harnesses.

### "pop only the active tab" is ambiguous - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `241`
* **Discrepancy Description**: The architecture (line 65) says re-tapping "pops its stack to the root," but "pop only the active tab on re-tap" can be read as popping a single entry or as popping other tabs.
* **Rationale & Recommendation**: Use the architecture wording.

### Hidden-stack mechanism is vague and Now Playing is placed in a stack - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `244`
* **Discrepancy Description**: "Hidden stacks stay hidden by the shell, not by route reset" names no mechanism (visibility, `display`, unmount), and "This preserves Library → album → Now Playing → Library" treats Now Playing, a sheet, as a stack route.
* **Rationale & Recommendation**: State the visibility mechanism and drop Now Playing from the stack chain.

### "right-click-only removal" has no object - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `248`
* **Discrepancy Description**: Removal of what (sidebar entries, roots, tracks, sources) is not stated, and the nearby long-press conversion at line 261 reads as a duplicate.
* **Rationale & Recommendation**: Name the removed affordance (for example, removing a source root).

### Glyph, platform and API terms wander - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `251`, `258–261`, `271`
* **Discrepancy Description**: `"..."` and `⋯` denote the same control; "on iOS" and "on mobile" alternate; "WASAPI" is used but defined nowhere and does not match the repository's share-mode vocabulary.
* **Rationale & Recommendation**: Use `⋯` consistently, reserve "mobile" for the layout value, and replace "WASAPI" with the actual API in use.

### "neutral path state" is an undefined label - Severity: Nit
* **Reviewer Lens:** `egm-style`
* **Affected Scope:** `272`
* **Discrepancy Description**: The architecture (line 116) calls it the "system mixer in path" state; the plan gives no literal text or enum value.
* **Rationale & Recommendation**: Give the exact label or the enum/value that supplies it.

### objc2 dependencies have no versions or features - Severity: Nit
* **Reviewer Lens:** `egm-implementation-readiness`
* **Affected Scope:** `230`
* **Discrepancy Description**: `objc2`, `objc2-foundation`, `objc2-avf-audio`, `objc2-media-player` and `objc2-security` are listed without versions or required features, unlike the pinned `libloading = "0.8"` and `ureq = "3.5.1"`.
* **Rationale & Recommendation**: Pin versions and state the features needed for `AVAudioSession`, `MPNowPlayingInfoCenter` and the Security APIs.

### "Make shutdown idempotent" does not address the deliberate leak on timeout - Severity: Nit
* **Reviewer Lens:** `egm-implementation-readiness`
* **Affected Scope:** `267`
* **Discrepancy Description**: `Engine::drop` (`ui/src/engine.rs:894–931`) waits 500 ms and deliberately forgets the `Library` on timeout; `Base::shutdown` just drops the handle. The plan says shutdown should "not leak on timeout" but only specifies idempotence.
* **Rationale & Recommendation**: State the intended timeout/leak policy and what replaces the current safety behaviour.
