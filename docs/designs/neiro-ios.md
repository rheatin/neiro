# Neiro iOS and Foldable Support

*   **Author:** Char ONE
*   **Status:** Draft (revised after review)
*   **Date:** 2026-11-20

--------------------------------------------------------------------------------

## Overview

Neiro today is a desktop host: a C++ audio core (`neiro_base`) loaded at run time through its C ABI, a desktop shell with a sidebar, a floating player bar, and full-screen Now Playing, queue and output sheets. This design adds an iOS client for two screen shapes: a normal iPhone, and a book-style foldable with an outer cover screen and a large inner screen (the iPhone Duo simulator profile).

The first release is a **mobile-only vertical slice**. It adds four tabs — Home, Library, Servers, Search — a mini player, Now Playing, queue and lyrics, one local-folder model built on security-scoped bookmarks, and one remote protocol (Jellyfin) using the existing whole-file fetch. The desktop application keeps its current navigation, sidebar, search placement, settings route and player bar without change. A shared sidebar across desktop and mobile is a later design, not part of this one.

The information architecture follows Apple Music where it applies: folded, a labelled bottom tab bar with a mini player above it; unfolded, a left sidebar with the same four entries. Library is a category list — Playlists, Artists, Albums, Songs, Liked — and each category opens its own page. Now Playing is a sheet raised from the mini player, not a tab and not a route. Settings opens from a gear button in the mobile top bar; there is no account system, so there is no account avatar.

The core is linked statically on iOS. The host keeps one C ABI. The system owns the output route and the mixer on iOS, so the output surface reports the real route, format and channels and does not claim bit-perfect playback. Background audio, lock-screen metadata, interruptions and route changes are first-release concerns.

## Terms

*   **NSH (Native Shell):** the TSX UI toolkit and build tool this app is written in. It provides `View`, `Text`, signals, `TabBar`, `Sheet`, `safeArea`, `keyboardFrame`, `Picker.pickFiles`, and the `nsh run` / `nsh test` / `nsh check` commands. It has no folder picker and no Keychain API.
*   **Core (`neiro_base`):** the C++ engine library, shipped prebuilt by the `neiro-core` repository. It decodes through FFmpeg, which is built LGPL and shared-only today.
*   **Engine (`Engine`):** the Rust host wrapper in `ui/src/engine.rs` that owns the C ABI function table and parses engine events.
*   **Audio route:** the physical or virtual output path (speaker, headphones, Bluetooth, USB, AirPlay, CarPlay). This design always says "audio route"; a navigation destination is a "page".
*   **ATS:** App Transport Security, the iOS policy enforced by URLSession for HTTPS and local-network access.
*   **DoP:** DSD over PCM. Not offered on iOS in this release.
*   **OPRA:** a headphone-correction profile database used by the desktop EQ. Desktop only in this release.

## The Problem

Neiro cannot run on iOS today, and the parts that would make it run are not merely missing UI.

1.  **No iOS product and no iOS core artifact.** `layout()` explicitly returns `"unsupported"` for iOS (`ui/src/components/layout.tsx:29-32`). `ui/src/engine.rs` loads the core at run time with `libloading`. `scripts/fetch-core.sh` fetches only a macOS SDK; the cached v0.2.0 artifact contains dylibs and no static library or xcframework. There is no iOS CI job.

2.  **Audio ownership inverts on iOS.** The macOS backend owns device enumeration and exclusive/shared ownership. On iOS the system owns interruptions, route changes and the mixer, and the app must configure `AVAudioSession` and own the render path. The engine protocol has no interruption, route-change, audio-format or capability event. `isStreamBitPerfect()` would report either a permanent failure or a badge while the system mixer is in the path.

3.  **Files and remote audio have no iOS acquisition model.** Local music enters through a folder picker, a recursive filesystem watcher and a 600-second reconcile loop; iOS allows none of those across an arbitrary tree. Local tracks carry no added-at timestamp. Remote playback already downloads a whole track before playing, keeps its cache index in the host, and leaves orphan temp files on a mid-download kill.

4.  **Mobile navigation does not exist, and the desktop one does not transfer.** There are nine roots and no `library` route. Liked, Albums, Artists and Tracks are reachable only through the desktop sidebar's Library panel. `openRoot` resets other roots' stacks when switching, which is sidebar behaviour, not tab behaviour. Reusing it on mobile would break per-tab history.

5.  **Mobile lifecycle and secrets are absent.** Nothing configures an audio session, declares background audio, updates the lock screen or handles remote commands. Remote credentials sit in the general key-value `Storage`, which is not encrypted; NSH exposes no Keychain API.

## Goals

*   **G1 — Build and run on iOS.** The app builds for `aarch64-apple-ios` and `aarch64-apple-ios-sim`, links the core statically, and launches in the NSH iOS host on the simulator and on one physical device.
*   **G2 — Play local files through security-scoped bookmarks.** The user picks folders; the app stores bookmarks, resolves them on launch and foreground, scans them, and plays through the core. Stale bookmarks and missing files show explicit states.
*   **G3 — One remote protocol.** Jellyfin is the only remote source in this release. Playback uses the existing whole-file fetch. Navidrome and WebDAV are deferred.
*   **G4 — A mobile shell for both screen widths.** Folded uses a labelled bottom tab bar with four entries; unfolded uses a left sidebar with the same entries. Each tab preserves its own navigation stack. Now Playing is a sheet from the mini player.
*   **G5 — Truthful output reporting.** The iOS output surface shows the real audio route, render format, channel count and system volume, and never claims exclusive or bit-perfect playback. Desktop semantics do not change.
*   **G6 — Background and lock-screen playback.** Playback continues when backgrounded, the lock screen shows metadata and artwork, and interruptions and route changes preserve or pause the stream correctly.
*   **G7 — No desktop regression.** Desktop behaviour, navigation, tests and the runtime-loaded core path remain unchanged. Shared code may gain target branches, but no desktop screen changes.

## Non-Goals

*   New, Radio, Genres, Downloaded and the account avatar. None ships on iOS in this release.
*   Desktop navigation changes. The desktop keeps its nine roots, sidebar, title-bar search and Settings route.
*   Progressive (byte-range) remote streaming. It depends on an unshipped core API and a second cache owner; it is a follow-up design.
*   Navidrome and WebDAV.
*   EQ/OPRA, lyrics editing, and sort/grid controls on iOS. Lyrics display through the existing player is in scope; the rest is not. EQ is desktop-only because iOS has no output console.
*   Runtime plugins and shell switching on iOS (the plugin manager is compiled out).
*   Native DSD and DoP on iOS. No exception is defined.
*   Exclusive mode, device enumeration, the output console and the desktop mastering console on iOS.
*   Synchronisation of favourites, playlists, folders or EQ/OPRA profiles between desktop and iOS.
*   An offline download library; the remote cache is not user-visible.
*   Two- or three-column content layouts, a right-edge icon dock, or a Now Playing tab.
*   Android or other platforms.

## Technical Plan (Architecture)

### 1. Information architecture

Four mobile roots: `HOME`, `LIBRARY`, `REMOTE` (labelled **Servers**), `SEARCH`. The folded tab bar shows all four with labels; the unfolded sidebar shows the same four plus a gear that opens Settings. This table is the single authority for the IA; the prose above and the stage plan refer to it rather than restating it.

| Route | Desktop today | Mobile destination | How it is reached | Status |
| --- | --- | --- | --- | --- |
| `home` | root | Home tab root | tab | reused; content narrowed to most-played and source summary |
| `library` | does not exist | Library tab root | tab | **new** category list |
| `albums` | root | Library → Albums | push | reused page |
| `artists` | root | Library → Artists | push | reused page |
| `tracks` | root | Library → Songs | push | reused page; label "Songs" maps to `TRACKS` |
| `liked` | root | Library → Liked | push | reused page |
| `playlists` | root | Library → Playlists | push | reused page |
| `playlist` | page | Library → playlist | push | reused page |
| `album`, `artist` | page | Library → detail | push | reused pages |
| `remote` | root | Servers tab root | tab | reused page; Jellyfin only |
| `remoteAlbum`, `remoteArtist`, `remotePlaylist` | page | Servers → detail | push | reused pages |
| `search` | root | Search tab root | tab | reused; the page owns its input on mobile |
| `settings` | root | modal | gear in the mobile top bar | desktop route unchanged; mobile modal |
| `eq` | page | — | — | desktop only |
| `genres`, `downloaded`, `new`, `radio` | — | — | — | deferred |
| account | — | — | — | does not exist |

Tab-switch semantics on mobile: switching tabs keeps every tab's stack; re-tapping the active tab pops that tab's stack to its root. This is a new `activateTab()` used only when `layout() === "mobile"`. `openRoot()` and its reset behaviour are untouched on desktop.

```text
folded (phone, < 700 pt)              unfolded (inner screen / iPad, >= 700 pt)
┌─────────────────────────────┐       ┌────────────┬─────────────────────┐
│  top bar            [gear]  │       │ Home       │  content            │
│  content (one column)       │       │ Library    │  (one column)       │
│                             │       │ Servers    │                     │
├─────────────────────────────┤       │ Search     │                     │
│  mini player                │       │            │                     │
├─────────────────────────────┤       │ [gear]     │                     │
│ Home Library Servers Search │       └────────────┴─────────────────────┘
└─────────────────────────────┘
```

### 2. Shells and the width contract

`layout()` gains a third value, and its type becomes `"desktop" | "mobile" | "unsupported"`. iOS returns `"mobile"`; desktop returns `"desktop"` exactly as today; web and terminal keep `"unsupported"`.

Shell selection uses measured content width, not a hinge API (NSH has none, and no public API exposes a fold state):

*   unfold when the measured content width is **>= 700 pt**;
*   fold when it is **< 660 pt**;
*   keep the current shell inside the 660–700 pt band (40 pt hysteresis);
*   measure on layout; debounce a change by **250 ms** so a fold drag does not rebuild the shell mid-gesture;
*   height and hinge angle are ignored.

This maps the iPhone Duo cover screen and an iPhone to the folded shell, and the inner screen and an iPad to the unfolded shell. No hinge detection is implemented.

Geometry stays in the existing helpers rather than a second "chrome record". `pageEnd()` gains the mobile case: mini-player height plus tab-bar height plus the bottom safe-area inset. `barSize()` gains a mobile case derived from the content width. `wideTracks()` loses its hard `layout() === "desktop"` gate and becomes a width comparison. Safe-area values come from NSH's `safeArea()`; they are not republished. Every literal `pb-28` / `pb-36` is replaced by `pageEnd()` — `ui/src/pages/albums.tsx`, `remote.tsx`, `tracks.tsx`, `settings.tsx`, and the `Toaster` offset in `ui/App.tsx`.

### 3. Engine seam and the core contract

The desktop loader is unchanged. On iOS the same `Engine` type is filled from statically linked symbols.

**Build seam.**

*   `ui/Cargo.toml`: `libloading` moves to `[target.'cfg(not(target_os = "ios"))'.dependencies]`. `objc2`, `objc2-foundation`, `objc2-avf-audio`, `objc2-media-player`, `objc2-security` and `objc2-ui-kit` are added under `[target.'cfg(target_os = "ios")'.dependencies]`, with versions pinned.
*   `ui/src/engine.rs` keeps `Engine`, `Exports`, `WireEvent` and `parse()` shared. The loader moves into two cfg-gated modules: `engine/desktop.rs` (`#[cfg(not(target_os = "ios"))]`, `libloading`, optional probing, `-2` fallback) and `engine/ios.rs` (`#[cfg(target_os = "ios")]`, `extern "C"` statics, no probing).
*   Plugin methods are compiled out on iOS (`#[cfg(not(target_os = "ios"))]` on the module and its call sites); `ui/src/state/plugins.ts` is gated the same way.
*   `ui/build.rs` emits the link search path and `cargo:rustc-link-lib` for the xcframework slice. The slice is selected from `CARGO_CFG_TARGET_ARCH` plus `CARGO_CFG_TARGET_ABI` (`sim` vs device), not from a user path.
*   The engine opens synchronously in `Base::new` on iOS. A static link has no `dlopen` cost, so no opening state is added in this release.

**Artifact contract.** `neiro-core` must publish `neiro_base.xcframework` with two slices (`ios-arm64`, `ios-arm64-simulator`). Each slice contains:

*   `libneiro_base.a` — the engine, statically linked;
*   `FFmpeg.framework` — FFmpeg as a dynamic framework with `@rpath` install names, because the current FFmpeg build is shared-only and LGPL requires a replaceable library.

The app links both. If `neiro-core` cannot produce the FFmpeg framework, the fallback is a static FFmpeg plus published relinkable object files; that fallback requires legal sign-off and is not the default. The xcframework version is read from `NEIRO_CORE_VERSION`, the same file `scripts/fetch-core.sh` uses.

**Symbol contract.** Mandatory symbols, already used by `ui/src/engine.rs`: `NAE_BaseStartWithLibrary`, `NAE_BaseCommand`, `NAE_BaseSubscribe`, `NAE_BaseUnsubscribe`, `NAE_BaseSnapshot`, `NAE_BaseFreeString`, `NAE_BaseShutdown`. On iOS the queue group (`PlayContext`, `PlayNext`, `PlayLater`, `SetRepeatMode`, `SetShuffleMode`, `Previous`, `RemoveQueueItem`, `ClearUserQueue`, `MoveUserQueueItem`), `NAE_BaseRemoteProviders` and `NAE_BaseListDirectory` are also required, because they are linked at build time. The plugin group is absent.

New symbols required for this design:

| Symbol | Signature | Purpose |
| --- | --- | --- |
| `NAE_BaseAbiVersion` | `() -> u32` | returns the ABI version; the host requires `2` |
| `NAE_BaseAudioCapabilities` | `() -> *mut c_char` | JSON capability object, freed with `NAE_BaseFreeString` |
| `NAE_BaseReinitializeAudio` | `(sample_rate: u32, channels: u16) -> i32` | rebuild the output unit after a route change or media-services reset |

`connect()` checks `NAE_BaseAbiVersion` first. A mismatch marks health `failed` with a message naming both versions. There is no silent degradation on iOS.

**Event contract.** New JSON events on the existing subscribe channel, `{"event": "<name>", ...}`:

| Event | Fields | Emitted when |
| --- | --- | --- |
| `audio_route_changed` | `route`: `speaker`/`headphones`/`bluetooth`/`usb`/`airplay`/`carplay`/`unknown`; `name`; `sample_rate`; `channels` | the output route changes |
| `audio_format_changed` | `sample_rate`; `channels`; `sample_format` (`f32`/`s16`/`s24`/`s32`); `resampling` | the render format changes |
| `audio_interruption` | `phase`: `began`/`ended`; `should_resume` | the system interrupts or resumes |
| `audio_capabilities` | `exclusive`; `device_selection`; `dither`; `dsd`; `system_mixer` | after open and after reinitialize |

Parsing stays in the shared `parse()`. The desktop loader parses the same events if an older core never emits them; nothing on desktop consumes them in this release, so desktop behaviour is unchanged.

**Threading contract.** Commands and `NAE_BaseReinitializeAudio` are called on the host's control thread. The render callback runs on the audio thread and must not allocate, lock a JS-visible mutex, or call back into the host. Events are delivered on the existing subscription thread.

### 4. Audio session ownership

One owner per fact:

*   **The core owns the audio I/O unit and the render callback** (RemoteIO on iOS). It reports the actual render format.
*   **The host owns `AVAudioSession`**, its category and activation, and receives interruption, route-change and media-services-reset notifications. It never touches the render callback.
*   **The host owns lock-screen metadata and remote commands**, calling the same player functions the UI calls.

Ordering is fixed:

| Moment | Order |
| --- | --- |
| first play | `setCategory(.playback)` → `setActive(true)` → `NAE_BaseReinitializeAudio(rate, channels)` → `play` |
| interruption began | host pauses playback; core stops the renderer |
| interruption ended, `should_resume` | `setActive(true)` → `NAE_BaseReinitializeAudio` → resume |
| route change | read the new route → `NAE_BaseReinitializeAudio` → core emits `audio_format_changed` → UI repaints |
| media-services reset | `setActive(false)` → rebuild the session object → `setActive(true)` → `NAE_BaseReinitializeAudio`; the engine handle and queue are not torn down |
| background | session stays active; background audio is declared in `Info.plist` |
| shutdown | `setActive(false)` after the engine stops |

`ui/src/audio_session.rs` (iOS only) implements this with `objc2-avf-audio` for `AVAudioSession`, `objc2-media-player` for `MPNowPlayingInfoCenter` and `MPRemoteCommandCenter`, and `objc2-foundation` for the timer/observer plumbing.

**Output truth.** `isStreamBitPerfect()` in `ui/src/state/resolved.ts` keeps its single-owner role and its current body; it is not modified. iOS never draws the desktop `BIT-PERFECT` badge. A new `ui/src/components/player/surface/route-panel.tsx` shows the audio route, render format, channels and system volume, with the neutral label "system mixer in path". `output-drawer.tsx` renders that panel on iOS and the existing console on desktop. `AGENTS.md` records that the iOS route panel is not a bit-perfect surface.

Route and capability state live in `ui/src/backend/engine.ts` signals, with contract types in `ui/src/backend/types.ts`. `state/resolved.ts` may read them, but no other module keeps a copy.

### 5. Local files

Security-scoped bookmarks are the model.

*   **Picker.** iOS needs `UIDocumentPickerViewController` with the folder content type. NSH's only picker, `Picker.pickFiles`, returns file bytes and has no folder or bookmark mode. `ui/src/ios/picker.rs` presents the native picker through `objc2-ui-kit`; the NSH iOS host must expose the root view controller. If it does not, the first task of Stage 0 is an upstream NSH change or a host-side presentation shim; without one, folder selection is not available and G2 is blocked.
*   **Identity.** A local source is `local:{uuid}`, where the UUID is generated when the folder is picked and persisted with the bookmark. The resolved absolute path exists only in memory. `ui/src/state/catalog.ts` owns the roots (`library:roots`), which become records `{ id, bookmark, label }`; `ui/src/state/sources.ts` derives labels and ids from them and no longer builds `local:${root}`.
*   **Lifetime.** `startAccessingSecurityScopedResource` is called when a root is resolved and held for as long as the root is active, because the core reads files from its own threads. Access is released when the root is removed or the app terminates.
*   **Scanning.** Scan on launch and on foreground. No filesystem watcher and no reconcile loop on iOS. The last scan result and timestamp are shown in Settings.
*   **Failure states.** A stale bookmark asks the user to re-pick. A missing file is shown as unavailable in the list that renders file rows (Tracks), with no silent deletion.
*   **Desktop.** The macOS path, watcher and absolute paths remain unchanged; the record/branch is selected by `cfg`-equivalent runtime checks.

### 6. Remote audio

Jellyfin only, using the existing whole-file fetch (`engine.fetchRemote` → `Base::fetch_remote`). No byte-range path and no progressive playback in this release.

*   The Rust host (`ui/src/lib.rs`) remains the sole cache owner: cache accounting, clearing and budgeted oldest-first pruning already exist. `ui/src/state/cache.ts` stays a stats reader over `engine.cacheStats()`.
*   `ui/src/state/remote.ts` keeps the catalog and player UI state; it does not manage files.
*   A per-install Jellyfin device ID is generated in the host and passed to `set_remote`; `neiro-core` must accept a `device_id`. If the core release does not, the existing `Device="desktop"` identity remains and cross-install collisions are accepted and documented.
*   Network policy is stated as two stacks, not one: the connection probe uses JavaScript `fetch` (URLSession, ATS applies); listings and downloads go through the core's own HTTP client (system trust, local-network privacy prompt). `Info.plist` sets `NSAllowsLocalNetworking` for LAN servers, no arbitrary loads, and an `NSLocalNetworkUsageDescription` naming the server feature. HTTPS is required for non-local servers.

### 7. Packaging and lifecycle

Files are committed, not generated blindly: `ui/ios/Info.plist`, `ui/ios/App.entitlements`, `ui/ios/PrivacyInfo.xcprivacy`. `scripts/ios-prepare.sh` copies them into the generated `ui/.nsh/rust/ios-app` project before the build. Stage 0 verifies the generated layout; if NSH's project already supports these keys, the script becomes a no-op.

`Info.plist` carries `UIBackgroundModes: audio`, `NSLocalNetworkUsageDescription`, the ATS local-networking key, and the scene configuration NSH needs. The privacy manifest declares the app's required-reason APIs (user defaults for settings, file timestamps if used, disk space for the cache). Entitlements are signing-dependent; the release owner supplies them.

### 8. Data and secrets

*   **Application Support** (`NSHomeDirectory()/Library/Application Support/Neiro`): `library.db`, bookmark blobs, settings. `data_root()` gains an explicit `#[cfg(target_os = "ios")]` branch; the existing unix fallback is wrong on iOS.
*   **Caches** (`Library/Caches/Neiro`): artwork and remote downloads, excluded from backup, purgeable.
*   **Keychain** (`ui/src/keychain.rs`, iOS only, through `objc2-security`): only secret values, keyed `remote:secret:{serverId}`. The non-secret server roster stays in `remote:servers` under `Storage`. There is no migration in the first release because no iOS build has ever shipped.
*   **File protection:** `NSFileProtectionCompleteUnlessOpen` on the library database and bookmarks, so background audio can read an open database while the device is locked.

### 9. Request flow

```text
TSX page
  │  play(track)
  ▼
Rust host (neiro-ui)
  ├── local path ──► Engine::play ──► neiro_base.a ──► RemoteIO
  ├── remote:// ──► fetchRemote (whole file) ──► host cache ──► Engine::play
  └── session ──► AVAudioSession / Now Playing / remote commands
```

## Risks & Mitigations

| Risk | Impact | Mitigation | Verified by |
| --- | --- | --- | --- |
| `neiro-core` does not ship an iOS xcframework or the new symbols | Release blocked at Stage 0 | Contract signed before Stage 1; Stage 0 gate is no-go without the artifact | `nm` check on both slices; `NAE_BaseAbiVersion` at connect |
| FFmpeg LGPL compliance rejected for the chosen link model | Release blocked at packaging | FFmpeg stays a dynamic framework; static fallback requires written legal sign-off | Legal review recorded in the release checklist |
| NSH exposes no root view controller for the folder picker | G2 unavailable | Stage 0 spike proves presentation; upstream NSH change is the fallback | Device test picking a folder |
| Bookmark expiry or container UUID change | Library vanishes after update | Bookmark re-resolve on launch/foreground; explicit re-pick state; never persist paths | Simulator reinstall test |
| Media-services reset loses the queue | User-visible playback loss | Reinitialize audio only; engine handle and queue are not torn down | Manual reset test |
| Jetsam kills a backgrounded app with four live stacks | Playback stops | One stack per tab with the existing page cap; unmount far tabs when memory warns; cap artwork; measure on device | Device memory measurement |
| ATS and local-network prompt confuse LAN users | Server unreachable | `NSAllowsLocalNetworking`; explicit prompt text; HTTPS required for internet servers | Manual LAN and internet tests |
| Keychain is unavailable after restore | Credentials lost | Secrets only; surface a re-enter prompt; non-secret roster stays in `Storage` | Restore test |
| Desktop regresses through shared layout helpers | G7 violated | Mobile branches only; desktop tests untouched; screenshot pass per stage | Existing test suite and screenshots |
| Two navigation implementations drift | Mobile and desktop diverge silently | Mobile uses `activateTab()`; desktop keeps `openRoot()`; no shared sidebar component | Nav tests for both targets |
| Stage 0 passes against a stub | False green CI | Stub lives outside the default build graph; CI asserts the real artifact hash and fails without it | CI log and linked binary inspection |

## Open Questions

| Question | Owner | Decision point | Fallback |
| --- | --- | --- | --- |
| Which `neiro-core` release contains the xcframework, `NAE_BaseAbiVersion`, capabilities, reinitialize and the audio events? | `neiro-core` maintainer | Stage 0 gate | No iOS release; design stays open |
| Does legal accept static `libneiro_base.a` plus dynamic `FFmpeg.framework`? | Author / legal | Before Stage 5 | Static FFmpeg with relinkable objects and written sign-off |
| Does NSH expose the root view controller to the host? | NSH upstream / author | Stage 0 spike | Upstream picker API; otherwise copy-import (rejected alternative E) becomes the fallback |
| Does `neiro-core` accept a host-supplied Jellyfin `device_id`? | `neiro-core` maintainer | Stage 4 | Keep the current identity; document collision |

## Alternatives Considered

### Alternative A: Right-edge icon dock on the inner screen

*   **Pros:** one custom control for both widths; predictable content width.
*   **Cons:** icon labels need a touch mechanism (NSH's tooltip does not open on touch); the right edge is the least reachable edge one-handed and sits near the crease.
*   **Reason for Rejection:** Rejected by author: the folded tab bar and unfolded sidebar are NSH-native controls, so the dock adds a third navigation component for no capability.

### Alternative B: List/detail or three-column inner layout

*   **Pros:** uses the inner width for browsing and detail together.
*   **Cons:** every page needs a detail-pane variant; no existing two-column structure to extend; doubles the page surface to test.
*   **Reason for Rejection:** Rejected by author: four page-layout variants and their tests cost more than the width gain in a first release.

### Alternative C: Keep the bottom tab bar when unfolded

*   **Pros:** one navigation component for both widths.
*   **Cons:** leaves the inner width unused for navigation; the tab bar's labels crowd at large widths.
*   **Reason for Rejection:** Rejected by author: the sidebar already exists as a layout helper, and the unfolded shell reuses its width logic.

### Alternative D: Now Playing as a tab or a routed page

*   **Pros:** five conventional destinations; browser-style back behaviour.
*   **Cons:** covers its own tab and leaves no tab selected; a routed page removes the globally available transport.
*   **Reason for Rejection:** Rejected by author: a sheet keeps transport global and the tab bar intact.

### Alternative E: Copy-import local files instead of security-scoped bookmarks

*   **Pros:** stable app-owned paths; no bookmark staleness; no security scope held across engine threads.
*   **Cons:** doubles storage; cannot follow changes in an external folder; deleting the original does not free the copy.
*   **Reason for Rejection:** Rejected by author: users pick their own music folders, and copying a 100 GB library onto a phone is not acceptable.

### Alternative F: Progressive (byte-range) streaming in the first release

*   **Pros:** playback starts before the track is fetched.
*   **Cons:** requires a new `neiro-core` range API and a second cache owner beside the existing host cache; server range behaviour and token refresh vary; the current whole-file path is implemented and tested.
*   **Reason for Rejection:** Rejected by author: the core API does not exist, the release would gate on an external schedule, and duplicate caches orphan favourites and playlists. Deferred to a follow-up design.

### Alternative G: Swift component for the audio session

*   **Pros:** idiomatic AVFoundation; less code for interruptions and the lock screen.
*   **Cons:** adds an Xcode target, a second language and a C ABI; NSH does not manage that target.
*   **Reason for Rejection:** Rejected by author: one Rust build and `objc2` bindings avoid a second toolchain and ABI.

### Alternative H: Host owns the audio I/O unit and render callback

*   **Pros:** the host already owns lifecycle and notifications.
*   **Cons:** the core's render format and resampling would be controlled outside the core; two owners of the audio graph; the host would need to feed decoded samples back into the core or bypass it.
*   **Reason for Rejection:** Rejected by author: the core owns the I/O unit and reports the real format; the host configures the session and sends ordered commands.

### Alternative I: Move desktop to the mobile IA in the same change

*   **Pros:** one navigation model everywhere.
*   **Cons:** a user-visible desktop rewrite rides on a platform port; every desktop surface and test changes; no rollback if the port slips; G7 cannot be true.
*   **Reason for Rejection:** Rejected by author: the desktop rewrite is a separate product decision and a separate design.

### Alternative J: Keep five tabs by shipping New and Radio in v1

*   **Pros:** literal Apple Music tab set; no renumbering later.
*   **Cons:** New has no added-at data source in the core or host; Radio is a rename of the existing remote page and duplicates Servers; both would ship empty or fabricated content.
*   **Reason for Rejection:** Rejected by author: a tab that cannot be populated is worse than a tab added later.

## Detailed Implementation Plan

The plan is staged by vertical slice, not by file concern. Each stage ends in a state that can be run and judged. Stage 0 is the contract gate; no stage may skip it. Dependencies are derived from the files each stage touches, not asserted.

### Stage 0 — Contract and build spike

Goal: prove the seam with the real artifact contract signed, without shipping a fake engine.

*   **Docs.** `docs/ios/core-contract.md` (Create): the artifact, symbol, event and ownership tables from §3–§4, signed by the `neiro-core` owner.
*   **Toolchain.** Verify `rustup target add aarch64-apple-ios aarch64-apple-ios-sim`, `nsh component add ios`, and an installed simulator. These are prerequisites, not optional steps.
*   **Files.** `ui/Cargo.toml` (Modify: target-specific deps, pinned `objc2` versions); `ui/build.rs` (Create: slice selection and link flags); `ui/src/engine.rs` (Modify: cfg split, shared parser kept); `ui/src/engine/desktop.rs` (Create: current loader moved); `ui/src/engine/ios.rs` (Create: `extern "C"` table); `scripts/fetch-core-ios.sh` (Create: fetch and verify both slices, version from `NEIRO_CORE_VERSION`; reuse rather than add a second pin); `.github/workflows/ios.yml` (Create: `cargo check` for both targets, artifact presence check, `nm` symbol check).
*   **Spike only.** `spike/ios_stub/` (Create): a stub exporting the mandatory symbols with inert behaviour, used on a developer machine to prove the build seam. It is not in the default build graph and CI never links it.
*   **Done when:** `cargo check --target aarch64-apple-ios` and `--target aarch64-apple-ios-sim` pass against the fetched xcframework; a simulator build launches and displays `NAE_BaseAbiVersion()`; CI fails when the artifact is missing and cannot pass with the stub.

### Stage 1 — Audio vertical slice

Goal: play one bundled FLAC through the real core, with session, lock screen and interruption handling.

*   **Files.** `ui/src/audio_session.rs` (Create); `ui/src/lib.rs` (Modify: session exports, ordered activation); `ui/src/engine.rs` (Modify: new `WireEvent` variants and shared parsing); `ui/src/backend/engine.ts` (Modify: route/format/capability signals, and the session commands, because it is the only module that imports `neiro/native`); `ui/src/backend/types.ts` (Modify: contract types); `ui/ios/Fixtures/tone.flac` (Create: bundled test file).
*   **Not yet:** the folder picker, any navigation change, remote audio.
*   **Done when:** the fixture plays in the simulator and on one device; lock-screen metadata and artwork appear; play/pause/next from the lock screen work; an interruption pauses and resumes; a route change updates the reported format; a media-services reset reinitializes audio without losing the queue.

### Stage 2 — Mobile shell and IA

Goal: the four-tab mobile application on both widths, with desktop unchanged.

*   **Files.** `ui/src/components/layout.tsx` (Modify: `"mobile"`, width helpers, `pageEnd`); `ui/App.tsx` (Modify: mobile branch, `Toaster` offset, startup calls gated per target); `ui/src/nav.ts` (Modify: `library` route, `MOBILE_ROOTS`, `activateTab`; desktop `openRoot` untouched); `ui/src/shells/mobile.tsx` (Create: folded and unfolded shells); `ui/src/shells/main.tsx` (no change: it already renders one live stack per opened root and reads the same `openedRoots()` signal); `ui/src/pages/library.tsx` (Create); `ui/src/pages/home.tsx` (Modify: most-played and source summary only); `ui/src/pages/search.tsx` (Modify: owns the input and keyboard handling); `ui/src/pages/settings.tsx` (Modify: mobile grouping and modal presentation); `ui/src/state/overlays.ts` (Modify: settings modal state); `ui/src/components/player/player-bar.tsx` (Modify: mini mode); `ui/src/components/player/now-playing.tsx` (Modify: sheet from mini player); `ui/src/components/player/queue-sheet.tsx` (Modify: bottom sheet, no detents); pages with literal `pb-28` (`albums.tsx`, `remote.tsx`, `tracks.tsx`, `settings.tsx`).
*   **Not touched:** `ui/src/shells/desktop.tsx`, `sidebar.tsx`, `header.tsx`, `title-bar.tsx`, `studio-minimal.tsx`. Desktop keeps its title-bar search and nine roots.
*   **Done when:** folded shows four labelled tabs and a mini player; unfolded shows the sidebar at >= 700 pt and the tab bar below 660 pt; each tab keeps its stack; Settings opens as a modal from the gear; desktop screenshots and navigation tests are unchanged.
*   **As built (2026-11-22).** Settings is a page the gear pushes onto the tab's stack, not a modal: iOS gives it a native back button that returns to what was being read, and the tab bar stays lit. The gear shows folded only; unfolded, the sidebar lists Settings itself. The library's categories and Settings are pages, never places: `open()` treats only `MOBILE_ROOTS` as roots on mobile, so they push and the tab keeps its stack. A page's content sits in flow inside `AppPage` (`min-h-0 flex-1`): an `absolute inset-0` child escapes the page box and draws under the iOS system header. `pageEnd()` replaced the literal `pb-28` in `albums.tsx`, `artists.tsx`, `eq.tsx`, `remote.tsx`, `settings.tsx` and `tracks.tsx`; `rememberScroll` passes no `contentOffset` at 0 because iOS's resting offset is not 0. Tests written: `mobile-shell.test.tsx` (the 660-700 pt band), `mobile-nav.test.tsx` and `library-categories.test.tsx` over `tests/mobile-shell-harness.tsx`, each app opened as `platform: "ios"`. The mobile settings grouping and a narrowed mobile home are not done.

### Stage 3 — Local folders

Goal: pick folders, keep them across launches, and play from them.

*   **Files.** `ui/src/ios/picker.rs` (Create: `UIDocumentPickerViewController`); `ui/src/ios/bookmarks.rs` (Create: create/resolve/stale/start-stop access); `ui/src/lib.rs` (Modify: exports); `ui/src/state/catalog.ts` (Modify: records `{id, bookmark, label}`, iOS scan-on-launch/foreground, no watcher on iOS); `ui/src/state/sources.ts` (Modify: ids from UUIDs, display paths not persisted); `ui/src/pages/tracks.tsx` (Modify: unavailable rows); `ui/src/pages/settings.tsx` (Modify: add-folder flow, scan status, stale state); `ui/src/backend/engine.ts` / `types.ts` (Modify: bridge methods).
*   **Done when:** a picked folder plays after a relaunch; a removed folder shows a re-pick state; a deleted file shows unavailable; no absolute container path is written to storage; desktop folder handling is unchanged.
*   **As built (2026-11-23), picker and bookmarks.** `ui/src/ios/picker.rs` presents `UIDocumentPickerViewController` on `public.folder` (`asCopy: false`) from the key window's top controller; a `FolderPickerDelegate` answers a oneshot channel, held weakly by the picker and kept in a `thread_local` list. A task of the engine may run off the main thread, so `pick_folder` hops through `dispatch2`'s main queue. `ui/src/ios/bookmarks.rs` creates the bookmark with `MinimalBookmark` (iOS's security-scoped one; `WithSecurityScope` is macOS-only), resolves it on launch, recreates and re-keys it when iOS reports it stale, and holds `startAccessingSecurityScopedResource` in a process-wide list until the root leaves. Base exports `pickFolderBookmark` (async), `resolveFolderBookmark` and `releaseFolderBookmark`; on the desktop `resolveFolderBookmark` returns the stored path, so the same TSX calls keep the desktop's path roots working. `catalog.ts` stores `{id, bookmark, label}` on iOS (uuid ids, paths memory-only) and paths on the desktop, accepts both shapes on read, scans on launch and when `lifecycle()` returns to `active`, and keeps unresolvable folders as `staleRoots`; Settings shows them as 需要重新选择 with a re-pick button (`repairRoot`). Verified in the simulator: the picker presents as an out-of-process Files sheet (DocumentManager), which `app.screenshot` does not capture — a full `simctl io screenshot` does. Tests: `tests/ios-import.test.tsx` (resolve on launch, stale row, re-pick replaces the bookmark with no path in storage, foreground rescan). Still open: playing from a picked folder end to end on a device, the Tracks page's unavailable rows for files a scan lost, and showing the last scan's time in Settings.

### Stage 4 — Remote Jellyfin

Goal: add a Jellyfin server, browse it, and play with the existing fetch path.

*   **Files.** `ui/src/state/remote.ts` (Modify: host-supplied device ID, remove range assumptions); `ui/src/state/cache.ts` (Modify: display host stats only); `ui/src/lib.rs` (Modify: `device_id` passed to `set_remote`, cache reconciliation on launch); `ui/src/pages/remote.tsx` (Modify: Servers shell and connection states); `ui/src/components/ui/server-dialog.tsx` (Modify: iOS fields and validation).
*   **Done when:** a Jellyfin server is added on device, its catalog browses, a track plays after the whole file downloads, the cache stays under its cap, and an interrupted download leaves no orphan temp file.

### Stage 5 — Packaging, secrets and verification

Goal: a device build that can be shipped to TestFlight.

*   **Files.** `ui/ios/Info.plist`, `ui/ios/App.entitlements`, `ui/ios/PrivacyInfo.xcprivacy` (Create); `scripts/ios-prepare.sh` (Create); `ui/nsh.json` (Modify: iOS metadata); `ui/src/keychain.rs` (Create); `ui/src/state/sources.ts` (Modify: secrets read/write through the bridge); `ui/src/lib.rs` (Modify: `data_root()` iOS branch, keychain exports); `ui/src/pages/settings.tsx` (Modify: re-enter credential state); `.github/workflows/ios.yml` (Modify: simulator and artifact checks); `AGENTS.md` (Modify: iOS invariants); `ui/HANDOVER.md` (Modify: boundary decisions and Phase 0 result).
*   **Done when:** background audio survives a device lock; the privacy manifest is present in the built app; credentials are Keychain-only; the database uses `CompleteUnlessOpen`; the full verification matrix below passes.

### Tests and verification

Test ownership follows the layer that implements the behaviour:

*   **Rust unit tests** (fakes, no device): `engine/ios.rs` symbol table, `WireEvent` new variants in `engine.rs`, bookmark create/resolve/stale in `bookmarks.rs`, session state machine in `audio_session.rs`, `device_id` plumbing.
*   **TSX tests with an injected bridge:** `ui/tests/mobile-nav.test.tsx` (Create: four roots, per-tab stacks, re-tap pops, settings modal), `ui/tests/mobile-shell.test.tsx` (Create: tab bar, sidebar threshold, `pageEnd`), `ui/tests/library-categories.test.tsx` (Create: category list and pushes), `ui/tests/search-page.test.tsx` (Create), `ui/tests/ios-import.test.tsx` (Create: stale and unavailable states over fakes), `ui/tests/audio-session.test.tsx` (Create: interruption, route change, metadata over fakes).
*   **Manual device matrix** (the only way to test real audio): background playback, lock screen, interruption, route change, media-services reset, local-network denial, bookmark re-pick, cache cap.
*   **Desktop regression:** `layout-and-formats.test.tsx`, `album-navigation.test.tsx`, `player-bar.test.tsx`, `queue-director.test.tsx`, `screenshots.test.tsx` must pass unchanged. Any edit to them is a G7 failure.
*   **Known baseline:** the four playback tests (`dither-policy`, `info-drawer-play`, `player-bar`, `share-mode`) need the repo-root `analysis_tone.wav`. It is gitignored and nothing generates it, so a fresh clone runs them without a playable file and they hang waiting for the playing state, which reads as an engine failure. Generate a 30-second 44.1 kHz stereo tone first; with it the suite passes except an occasional `lyrics-context-menu` flake. Measured on 2026-11-21, recorded in `docs/ios/parity-verification.md`.

Commands:

*   `cargo check --target aarch64-apple-ios` and `--target aarch64-apple-ios-sim`
*   `(cd ui && nsh check)`
*   `cargo test --manifest-path ui/Cargo.toml --lib`
*   `cargo test --manifest-path ui/Cargo.toml --test engine_shutdown`
*   `(cd ui && nsh test)`
*   `nsh run --target ios` on a booted iPhone 18 Pro and on the iPhone Duo unfolded profile
*   `nm -gU` on the linked binary to prove the symbol set and the absence of the stub

## Appendix: changes from the reviewed draft

*   Scope cut to four mobile tabs; New, Radio, Genres, Downloaded, the account avatar and EQ/OPRA removed from the first release.
*   Desktop navigation, sidebar, search placement and Settings route are out of scope, restoring G7.
*   Progressive streaming replaced by the existing whole-file fetch.
*   The link model is decided: static engine, dynamic FFmpeg framework.
*   Audio ownership is decided: core owns I/O, host owns the session, with a fixed command order.
*   The stage plan is re-sliced vertically; the stub cannot make CI green.
