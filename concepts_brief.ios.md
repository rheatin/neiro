# Neiro iOS Layout and Foldable Support

* **Author:** Char ONE
* **Date:** 2026-11-20

## Overview

Neiro is a high-fidelity music player that today ships as a desktop app: one runtime-loaded C++ core (`neiro_base`), a desktop shell with a sidebar, a floating player bar, and full-screen Now Playing, queue and output sheets. This concept adds an iOS client and designs its layout for a normal iPhone and for a book-style foldable (an outer cover screen and a large inner screen).

The author fixed these decisions before review:

- The core is ported to iOS as a statically linked `neiro_base` and wrapped with `AVAudioSession`. Local files and Jellyfin / Navidrome / WebDAV streams are both in scope.
- Feature scope is full parity with desktop except runtime plugins and external UI shells. EQ, OPRA, output policy, settings, queue and lyrics are included.
- The information architecture follows Apple Music. Folded (iPhone): a bottom tab bar with five tabs — Home, New, Radio, Library, Search — a mini player above it, and Settings behind the account avatar. New holds recently added and newly imported music; Radio holds remote servers (Jellyfin / Navidrome / WebDAV) and, later, internet radio.
- Unfolded (inner screen) and desktop follow Apple Music on iPad and Mac: a left sidebar with the same five entries plus the account, and a single content area to its right. No right-edge dock, and no two- or three-column content layout.
- Library is a category list (Playlists, Artists, Albums, Songs, Genres, Downloaded), not a segmented control; a category opens its own page with sort and grid/list controls.
- Now Playing is a full-screen sheet raised from the mini player, not a tab or a route. The page stack, mini player, queue sheet and output sheet are reused.

The review's verdict: the navigation shell itself is the least risky part of the design. The parts that decide whether this ships — what "bit-perfect" means when iOS owns the audio path, how the static core is built and linked, how files enter the sandbox, how remote audio survives suspension — are unspecified, and several are directly contradicted by the current repository.

After review the author narrowed the plan: the first release is the smallest set the foundations can honestly support, and four boundaries are frozen before any layout work — the audio-session contract, the static-link seam, the local-file model and the remote-streaming model. Full parity stays the destination, not the first build.

The author then chose Apple Music's navigation model outright, replacing the earlier single-dock plan: bottom tabs when folded, a left sidebar when unfolded and on desktop, Settings behind the account avatar, and Now Playing as a sheet raised from the mini player.

## Analysis

### A note on sections 5, 6 and 8

Those sections critique the earlier single-dock plan. The author has since adopted Apple Music's navigation: a bottom tab bar when folded, a left sidebar when unfolded and on desktop, and Now Playing as a sheet. The IA-table requirement in section 5 and the "Now Playing is a sheet" conclusion in section 6 still stand; the dock-label critique in section 8 no longer applies.

### 1. The product's central claim cannot be evaluated on iOS, and the current UI would state a false or permanently broken verdict

`isStreamBitPerfect()` (`ui/src/state/resolved.ts:176-183`) is the single authority for the green badge. It checks the engine's `bitPerfect` flag plus host DSP. On iOS the system mixer and route are in the path, hardware volume is outside the engine, and the app cannot guarantee an exclusive stream. The surrounding UI makes claims that cannot hold: the master-bus lamps require `shareMode === 1` (`ui/src/components/player/surface/master-bus.tsx:80-98`), settings default the backend label to `"WASAPI"` and share mode to `EXCL` (`ui/src/pages/settings.tsx:64-68`), and the policy model still exposes exclusive, fixed rates, dither and `dsd: "dop"` (`ui/src/state/resolved.ts:287-297`). The `AudioBackend` enum has no iOS value. The result is either an amber "DSP ACTIVE" panel forever, or a green badge while the system mixer is in the path. Both are dishonest.

This does not mean dropping output policy. It means the iOS output surface must be redefined before it is designed: route name, route format, channel count, system volume, an explicit engine capability report, an iOS-specific meaning for `isStreamBitPerfect()`, and honest labels for what the app does and does not control.

### 2. "Port the core and wrap it with AVAudioSession" is a host-boundary rewrite, and there is no iOS artifact to port

The current boundary is built around runtime loading: `libloading`, per-OS library names, candidate directories, optional-symbol probing and `-2` fallbacks (`ui/src/engine.rs:28-33`, `:805-931`). On iOS the broad Unix `cfg` selects `libneiro_base.so`, which is not an iOS artifact. A static link makes most of `engine.rs` dead code, and leaves a plugin manager that reports plugins the app can never load.

There is also no pipeline. `scripts/fetch-core.sh:74-78` only knows `neiro-core-${version}-macos-${arch}` and requires dylibs. `ui/nsh.json` has no iOS keys, `ui/Cargo.toml` declares `crate-type = ["lib"]` rather than a static library, and there is no Xcode project, `Info.plist`, entitlements, privacy manifest, signing step or CI job. The audio layer is not an adapter either: the macOS backend owns device enumeration and exclusive/shared ownership, while iOS gives the app a render callback and owns interruptions and route changes. The engine protocol has no interruption, route-change or media-services-reset event (`ui/src/engine.rs:1504-1662`). This is an ownership inversion.

App Store rules also remove runtime code loading, so plugins and external shells must be compiled out of the iOS target. Statically linking the shared LGPL FFmpeg build raises relink obligations that need a deliberate answer.

### 3. "Local files" has no iOS acquisition model

`addRoot()` opens a folder dialog, stores the returned path, starts a recursive watcher and reconciles every 600 seconds (`ui/src/state/catalog.ts:101-137`; `ui/src/lib.rs:2646-2657`). iOS has no arbitrary folder picker, no recursive filesystem watcher and no SMB mount. The NSH picker returns file bytes and names with a 512 MiB cap and no directory grant. A security-scoped URL must come from the document picker, be persisted as a bookmark, and be started and stopped around access; iCloud "optimize storage" can evict a file underneath a scan.

The app container path is also a UUID that changes on reinstall and restore, while favourites, playlists, roots and queue entries persist absolute paths. Either files are copied into an app-owned media directory, or the app uses security-scoped bookmarks with stale-bookmark and evicted-file states. The watcher must become scan-on-launch and scan-on-foreground with a visible last-scan result. "添加本地文件夹" cannot ship unchanged.

### 4. Remote playback is a desktop whole-file downloader, not a mobile streaming or offline feature

`playRemote` calls `engine.fetchRemote(id)` and waits for `remote_fetched` to return a local path (`ui/src/state/remote.ts:986-1000`). The engine downloads the whole track before playback. The cache index is an in-memory `Map`, so after relaunch a file already on disk is invisible and the app fetches again. There is no cap, no LRU, no atomic temp-and-rename, no resume, and the watchdogs are JS timers that do not run while suspended (`remote.ts:933-948`). A mid-download termination leaves an orphan file and a stuck row. Offline playback is not a state the UI understands.

The listing path is desktop-shaped too: 200 tracks per page written wholesale to a 5 MB key-value store (`remote.ts:557-609`), with client-side search over the full array. This needs a persisted download ledger, atomic files, a real "downloaded" state, startup reconciliation, a byte ceiling with eviction, a Wi-Fi-only switch, and either a background `URLSession` or an explicit statement that downloads stop when suspended. If progressive/range streaming is out of scope, say so in the product definition.

### 5. The five-entry IA does not map onto the app's existing routes

There are nine roots — Home, Search, Albums, Artists, Tracks, Remote, Liked, Playlists, Settings — and no `library` route (`ui/src/nav.ts:34-44`). Liked, Albums, Artists and Tracks are reachable only through `LibraryPanel`; playlists only through `PlaylistsPanel`. `PLAYLISTS` is defined but nothing navigates to it, so the playlists page is effectively unreachable even on desktop. `openRoot` resets every other root's stack to its root page (`ui/src/nav.ts:264-297`); that is deliberate desktop sidebar behaviour, not tab semantics. On iOS, Library → album → Now Playing → Library must still show the album.

An IA table must freeze, for each of the five tabs and Library's categories, the exact root route it activates and the pages it owns; the fate of Liked, Playlists, Remote and EQ; and what re-tapping the selected tab does. Then `ROOT_ROUTES`, `MainStack` and the reset policy change.

### 6. Now Playing as a dock tab creates two owners of "what is on screen"

Now Playing is a full-screen modal sheet mounted at the app root, outside every shell (`ui/App.tsx:139`). It is opened by a boolean, not a route (`ui/src/state/overlays.ts:41-67`), and the overlay stack is mutually exclusive. A dock tab that sets that boolean covers the dock, leaves no tab selected, and returns to whatever was behind it when dismissed. The player bar is hidden while it is open (`ui/src/shells/desktop.tsx:53-58`), so the tab representing Now Playing is not visible alongside its own trigger. The review recommends keeping Now Playing as the sheet summoned from the mini-player and giving the fifth slot to Playlists (or Queue). If Now Playing must be a dock tab, it has to become a real page and the modal sheet must go — which makes transport no longer globally available, a worse trade for a music player.

### 7. The Search tab is a dead end

`SearchField` exists only inside `TrailingControls`, which renders only inside `TitleBar` (`ui/src/components/header.tsx:20-31`). `TitleBar` draws only when `titleBarArea().height > 0`, which is zero on iOS, so the field, nav controls and settings button all disappear. `SearchPage` reads `searchQuery` but never renders an input (`ui/src/pages/search.tsx:138-140`). Tapping Search on iPhone shows an empty-state prompt with no way to type. Search must own an auto-focused field in the page body, handle the keyboard frame, keep results above the keyboard and provide a cancel path.

### 8. The stated label mechanism does not exist on touch, and the fold story is asserted rather than designed

`Tooltip` shows on pointer rest or keyboard focus; on touch it opens only when the app explicitly does. The compact sidebar uses `Hint` tooltips and never controls their `open` state (`ui/src/components/library/sidebar.tsx:94`, `:321`). On iPhone the five icons would have no visible labels at all. `onLongPress` is unused throughout `src/`.

NSH has no fold-posture or hinge API; `platform.idiom` has no "book fold" concept. A width-triggered flip will fire during a continuous fold drag, and phone landscape (852×393) also qualifies by width. The right edge is the least reachable edge one-handed, sits near the crease, and puts navigation diagonally opposite the native back affordance. "Merely narrower" at 700–850 pt also wastes the inner screen: a single column leaves track rows and lyric lines past a readable measure, while the card grid produces roughly six columns and `wideTracks()` is hard-gated to desktop (`ui/src/components/layout.tsx:63`). The intent of the decision — labels without an expanded persistent sidebar — is right. Apple Music's tab bar already labels its icons and its sidebar labels entries in place, so the touch-label problem disappears with that direction. Fold detection should still use measured shell width with hysteresis, animate after the fold settles, and define partial-fold and landscape behaviour.

### 9. `layout()` is a binary desktop gate, and page geometry is derived from a sidebar that will not exist

`layout()` returns only `"desktop"` or `"unsupported"` (`ui/src/components/layout.tsx:29-32`), and `App.tsx:134-139` sends everything else to `UnsupportedPage`. Adding a third value silently changes every `layout() === "desktop"` check and every `else` branch. `barSize()` subtracts `sidebarWidth()` (274 or 72) and shell gaps (`layout.tsx:35-39`); on a 390 pt iPhone that is negative. `pageEnd()` is fixed `pb-28`/`pb-36`. `measuredShellWidth` is fed only by `DesktopShell`.

The bottom edge already has three claimants and no shared owner: the player bar is anchored at `bottom: 4` with no safe-area inset (`ui/src/shells/desktop.tsx:54-58`), the Toaster sits at 96, pages reserve `pb-28`, and the tab bar would add a fourth. `AppSheet` pads side panels by `titleBarArea().height`, which is zero on iOS. Now Playing's top row falls back to 16 pt, under a Dynamic Island.

The fix is structural: one published chrome/inset record owned by the shell (content width, safe-area insets, navigation form, chrome height), read by `pageEnd`, the bar, the Toaster, sheets and Now Playing. Platform differences should be values the shell publishes, not branches scattered through shared components.

### 10. Background audio, lock-screen metadata and interruption handling are a missing subsystem

Nothing configures an audio session, declares `UIBackgroundModes: audio`, publishes `MPNowPlayingInfoCenter` or handles `MPRemoteCommandCenter`. NSH's only audio-session surface is a per-video prop. The app never reads `lifecycle()`, the engine protocol has no interruption or route-change event, and transport is driven only by engine events. A call, Siri, unplugged headphones or backgrounding leaves the UI and engine disagreeing, and the lock screen shows nothing. This is v1, not polish: `.playback` category, activation on first play, metadata and artwork updates, remote commands wired to the same functions the UI calls, interruption begin/end with position preservation, route-change reconfiguration and media-services-reset recovery. An owner must be picked (Rust `objc2` bridge or Swift component) before transport UI is designed.

### 11. The network layer fails on first use, and credentials are stored in plaintext

The product assumes HTTP LAN servers. iOS blocks cleartext HTTP by default and local-network access requires a permission prompt whose denial is silent; there is no ATS story, no `NSLocalNetworkUsageDescription`, and `ui/nsh.json` declares no capabilities. The probe uses NSH `fetch` while listings and streams use Rust `ureq`/rustls (`ui/src/state/remote.ts:91-189`), so a server can probe successfully and then fail to stream.

Passwords and API tokens are serialized into `Storage` (`remote.ts:82-88`; `ui/src/state/sources.ts:44`, `:116-130`). `Storage` is a 5 MB key-value store, not a secret store, and NSH exposes no Keychain API. On iOS this is a shipping security defect. It needs a Keychain bridge with `AfterFirstUnlockThisDeviceOnly`, a one-time migration that deletes the old blobs, and a test that no secret reaches `Storage`.

### 12. Full parity is a destination, not a first release

The parity list is largely hardware panels and pointer idioms: a 668 pt output console with 8–9 px mono type (`ui/src/components/player/output-drawer.tsx:23-29`), device enumeration and exclusive mode, a convolver IR picker through a file dialog, right-click source removal (`ui/src/components/library/sidebar.tsx:309-317`), hover-only card play buttons (`ui/src/components/library/cards.tsx:116-130`), keyboard shortcuts mounted unconditionally (`ui/App.tsx:63-82`), the sidebar-collapse setting, and the plugin manager and shell switcher, which App Store rules forbid anyway. Each needs a touch redesign or must be hidden, and each hides a decision. If all of it must ship first, the schedule is the engine port plus a rewrite of every panel; the navigation shell should not be the first thing designed.

### Second-tier findings

- **Data root and backup.** iOS takes the generic Unix branch of `data_root()` and lands in `$HOME/.local/share/Neiro`, with `"."` (the read-only app bundle) as fallback (`ui/src/lib.rs:523-549`). Add an iOS branch: Application Support for `library.db`, Caches for media and artwork, excluded from backup, with a file-protection class background audio can read while locked.
- **Touch idioms.** Source removal, card actions, remote-row actions and Now Playing cover actions are `onContextMenu` only; cards hide play behind `group-hover`. Track rows have a `⋯` fallback (`ui/src/components/library/tracks.tsx:145-158`); most others do not.
- **Hidden native stacks.** `MainStack` keeps one live `NavStack` per visited root (`ui/src/shells/main.tsx:65-85`). On iOS each is a real `UINavigationController`; memory cost and jetsam are real. Cap live roots or swap route state in one stack.
- **Sheets are desktop-shaped.** Queue is a 360 pt right panel, output a 668 pt drawer; iPhone is 393 pt wide. NSH has native detents; use them and write a per-surface presentation table.
- **Engine lifecycle.** `Base::new` synchronously opens the library and starts the engine on the UI thread (`ui/src/lib.rs:1492-1507`). Open it off the main thread with an explicit opening state; make shutdown idempotent; add mobile lifecycle hooks.
- **Observability and device testing.** No os_log, panic hook, root error boundary or rejected-command counters. Verification is desktop-only. Add iOS headless tests with safe-area, storage, network and lifecycle fakes, plus device tests for background playback, lock screen, interruption, route change, local-network denial, Keychain migration and file import.
- **Thermal and battery.** 60 Hz FFT plus shader plus EQ, convolution, SoXR and DSD-to-PCM is a thermal budget on a phone. Default the visualiser off or 30 Hz on battery, and react to thermal state before audio glitches.
- **Per-device data.** Decide whether iOS data syncs with desktop. Today favourites, playlists, roots, EQ/OPRA profiles and policy are per-device and keyed by absolute paths. Say "per-device, with import/export" or define sync and stable IDs; define the `library.db` schema-version contract with `neiro-core`.
- **Small leaks.** Duplicate titles once `NavPage` draws a native header; EQ's own back button; Jellyfin claims hardcode `Device="desktop", DeviceId="neiro-base"` (`ui/src/state/remote.ts:111`, `:128`), so two installs collide; `UnsupportedPage` says Neiro is a desktop app; generated hosts pin NSH 0.5 while `Cargo.toml` pins 0.6.1.

### Where the reviewers disagreed

The zoom-out lens argued for not porting the core at all and using NSH's `AudioPlayer` as the iOS audio path. That conflicts with the author's decision and cannot deliver EQ, OPRA, DSD policy, or the library and queue semantics: it is useful only as a throwaway harness to validate the shell, tab bar/sidebar, fold transition and touch IA while the real port is in progress, never to validate a fidelity claim. The design lens's suggestion to remove exclusive, dither and DSD-transport controls is not re-litigating scope: those features remain in the product, but their iOS meaning must be defined, and removing the controls is the honest consequence if the platform cannot express them.

### What holds up

There is no two-column main layout to remove; `MainStack` is already a single stack and `SplitView` is unused. `NavStack`/`NavPage` are viable because NSH supplies a native `UINavigationController` and native header on iOS. List virtualization already exists for albums, artists and tracks. The native bottom `TabBar` covers the folded screen, and the existing sidebar pattern covers the unfolded screen and desktop; no custom dock is needed. The problem is not the navigation shape; it is everything under it.

## Goals vs. Non-Goals

**In scope (eventual):** an iOS build of the app; Apple Music-style navigation (bottom tabs folded, left sidebar unfolded and on desktop); single-column content; Library category pages; reuse of the page stack, mini player, Now Playing sheet, queue sheet and output sheet; local files; Jellyfin / Navidrome / WebDAV; EQ, OPRA, output policy with iOS-specific meaning; background audio, lock screen and interruptions; Settings behind the account avatar.

**First build (narrowed after review):** the navigation shell (tab bar folded, sidebar unfolded and on desktop) with one chrome/inset model; Home; the Library category list plus one category page; Playlists; Search with its own field; Settings (behind the account avatar) trimmed to the first-build pages; the mini player; the Now Playing sheet; the queue sheet; lyrics; one local-file import path; one remote protocol with progressive streaming; EQ/OPRA; audio-session, lock-screen and interruption integration. The output console, DSD, convolver, exclusive mode, device enumeration, plugin manager and filesystem watcher follow once the route model and the port exist.

**Out of scope:** runtime plugins and external UI shells (App Store rules); native DSD output (DoP only if the route preserves the carrier); exclusive mode, device enumeration and convolver folder picking as desktop presents them; two- or three-column content layouts; a right-edge icon dock; a Now Playing tab; desktop data sync unless separately decided; an explicit offline download library in the first release.

## Alternatives Weighed

- **Right-edge icon dock on the inner screen** — rejected by the author after choosing Apple Music: labels need a touch mechanism Apple's tab bar and sidebar do not need, the right edge is the least reachable one-handed, and Apple Music itself uses a left sidebar on iPad and Mac.
- **List/detail or three-column inner layout** — rejected by the author: navigation lives in the sidebar, content stays one column.
- **Bottom tab bar when unfolded** — rejected: Apple Music moves to a sidebar on iPad and Mac, and the extra width is better spent on content.
- **Now Playing as a tab** — rejected by the author: Apple Music raises it as a sheet from the mini player, which keeps transport global and leaves the five tab labels intact.
- **Five-entry dock (Home / Library / Search / Now Playing / Settings)** — superseded by the author's Apple Music direction: tabs folded, sidebar unfolded, Settings behind the account, Now Playing as a sheet.
- **NSH `AudioPlayer` as the iOS engine** — rejected as an end state: no EQ, OPRA, DSD policy or library/queue semantics. Acceptable only as a throwaway UI harness.
- **Full parity as the first release** — rejected by the author after review: with the boundaries undefined, the first build becomes the engine port plus a rewrite of every desktop panel, and the schedule is set before the route model exists.

## Key Decisions & Staging for Next Steps

Freeze these before any UI work. Items 1–6 are the boundaries and come first; 7–12 follow once the port has a shape:

1. The iOS audio-session contract: category/mode, activation timing, the render callback, route descriptor, and the interruption / route-change / media-reset events added to the engine protocol.
2. The static-link seam: which exports are hard-linked, whether optional symbols and `-2` fallbacks exist on iOS, and which parts of `engine.rs` are desktop-only.
3. The iOS build artifact and pipeline: `.xcframework` or static library, device and simulator slices, Xcode project, `Info.plist`, entitlements, privacy manifest, signing, CI.
4. The iOS meaning of output policy and the exact wording of the strongest truthful claim about the audio path.
5. The local-file model: copy-import into the container, or security-scoped bookmarks, with stale and evicted states.
6. The remote-streaming model: progressive range reads, a bounded stream cache with atomic writes, startup reconciliation, and no user-visible offline library in the first release.
7. The IA table: the five tabs (Home / New / Radio / Library / Search) and Library's categories, the root and pages each owns, where Liked / Playlists / Remote / EQ live, and per-tab stack preservation.
8. Now Playing: the sheet raised from the mini player. It is not a tab and not a route.
9. The chrome/inset record and the fold-detection rule (measured width, hysteresis, partial fold, landscape).
10. Credentials in Keychain; ATS, local-network permission and privacy manifest; one network trust authority.
11. The iOS test matrix and the diagnostics payload.
12. Desktop navigation: Apple Music's Mac sidebar with the same five entries, replacing the current Library / Playlists / Sources panels.

A defensible first build on a phone: the navigation shell (tab bar folded, sidebar unfolded and on desktop), one chrome model, Home, the Library category list with one category page, Playlists, Search with its own field, Settings behind the account avatar, the mini player, the Now Playing sheet, the queue sheet, lyrics, imported local files of one kind, one remote protocol with progressive streaming, EQ/OPRA, and session / lock-screen integration. The output console, DSD, convolver, exclusive mode, plugin manager and local watcher come after the route model and the port exist.

Phase 0 spikes, before layout estimates: play one local FLAC through RemoteIO with a statically linked core; write the session contract; build the `.xcframework`; prove the range-read path for remote streaming.

The navigation geometry should be the last thing tuned, not the first thing designed.
