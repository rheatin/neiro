# Zoom Out Review — Neiro iOS and Foldable Support

**Target:** `/Users/char/Repos/neiro/docs/designs/neiro-ios.md` (lines 1–320)
**Standard:** zoom-out-standard.md
**Mode:** REVIEW — Stage 5 (Initial Zoom Out)
**Date:** 2026-11-20

## Bottom Line

Context sufficiency passes, and the problem inventory is unusually well verified against the codebase. The design is not, however, one design: it is an iOS platform port bundled with a desktop navigation redesign, two platform primitives that no one owns, a second cache owner, and an unresolved legal/linking model for the audio core. Four items are blockers because they either contradict the document's own goals or must be decided before the first stage can start. The fastest path forward is to cut the release to a mobile-only vertical slice and move the desktop IA work, progressive streaming, and speculative destinations to separate designs.

## Reviewer Status

| Reviewer | Status |
| --- | --- |
| `egm-zoom-out` instance A | completed |
| `egm-zoom-out` instance B | completed |

## Prioritized Findings

### 1. The first release bundles a desktop navigation redesign with the iOS port, directly contradicting G7 — Blocker

* **Affected scope:** lines 17, 29, 40–43, 57–80, 218–222, 241–262.
* **Discrepancy:** G7 (line 43) promises desktop behaviour and tests remain unchanged except behind a target seam. Stage 1 then replaces the desktop root list, rewrites the sidebar, removes the search field from the desktop title bar, restructures Settings, changes desktop stack semantics in `nav.ts`, and rewrites the player bar and context menus. These are a desktop IA redesign, not a target seam. The current desktop surfaces are stable and tested: `ROOT_ROUTES` at `ui/src/nav.ts:44`, the four-panel sidebar at `ui/src/components/library/sidebar.tsx:345-364`, title-bar search at `ui/src/components/header.tsx:20-31`, and Settings at `ui/src/pages/settings.tsx:145-155`.
* **Recommendation:** Add a mobile-only root projection and mobile shell; leave the desktop roots, sidebar, title-bar search and Settings alone. If the five-root IA is wanted, land it as a separate design.

### 2. Local import depends on a picker and data model that neither NSH nor the host owns — Blocker

* **Affected scope:** lines 38, 118–126, 142–147, 275–281.
* **Discrepancy:** G2 says the document picker selects folders, and `lib.rs` exports bookmark create/resolve. But NSH's only TS-visible picker is `Picker.pickFiles` (`ui/src/.nsh/types/nsh/index.d.ts:436-440`), which returns bytes and provides no folder selection, security-scoped URL or bookmark. The desktop folder dialog (`ui/src/lib.rs:2646-2650`) is not an iOS picker. No stage owns presenting `UIDocumentPickerViewController`. The data side is unspecified: `data_root()` has no iOS branch (`ui/src/lib.rs:523-555`), source identity is the absolute path (`ui/src/state/sources.ts:61-70`, `catalog.ts:190-208`), and the design says not to persist container paths while the active source is persisted.
* **Recommendation:** Name the capability and owner: an NSH folder-picker/bookmark API gated by a spike, or a host-side `objc2` picker bridge. Specify a stable per-bookmark UUID as source identity, with the resolved path in memory only, plus iOS Application Support and Caches path providers. If neither primitive is ready, choose copy-import or remove G2 from the first release.

### 3. The static-link and LGPL artifact contract is unresolved, and it gates Stage 0 — Blocker

* **Affected scope:** lines 96–103, 164, 230–237, 294.
* **Discrepancy:** The plan commits to `cargo:rustc-link-lib=static=neiro_base` and an `xcframework`, while line 164 admits the FFmpeg linking model is not decided. The repository builds FFmpeg shared-only (`--enable-shared --disable-static`, `scripts/build-ffmpeg-lgpl-macos.sh:24-31`) to keep the engine LGPL-clean. Static-linking that stack into an App Store binary changes the compliance story, the xcframework contents, the link flags and the packaging rule.
* **Recommendation:** Resolve the artifact contract before approval: static `neiro_base` slice plus either a shared FFmpeg framework or relinkable objects; name the `neiro-core` owner; state the App Store compliance route and the fallback.

### 4. Audio-session and engine ownership are split across two repos with no written contract — Blocker

* **Affected scope:** lines 15, 25, 99–100, 107–116, 149–159, 212–216.
* **Discrepancy:** The host owns `AVAudioSession`, while the engine also gains route, format, resampling, interruption and route-change events. Interruption and route change therefore have two possible producers. The document never states who owns the AudioUnit and render callback, who is authoritative for route facts, or the ordering between session activation and engine open. "Media-services reset: tear down and rebuild the engine handle" would reopen the library and lose the queue.
* **Recommendation:** One owner per fact, consistent with `AGENTS.md` section 1. The host owns session lifecycle and notifications; the engine exposes a capability struct and one "reinitialize audio" command. A media-services reset rebuilds the audio object, not the engine handle. Publish the exact C ABI contract before Stage 2, and re-examine putting the iOS output backend beside the existing CoreAudio/WASAPI backend.

### 5. "New", "Genres", "Downloaded", "Radio" and the account avatar have no data model or contradict a non-goal — Major

* **Affected scope:** lines 13, 47–53, 61, 63, 134, 249, 251–254.
* **Discrepancy:** Local tracks carry no added-at date (`ui/src/pages/user-collections.tsx:116-117`); queue history is not a persisted play log (`ui/src/backend/types.ts:214-221`). Radio duplicates the existing server list (`ui/src/state/sources.ts:47-78`, `pages/home.tsx:152-198`). `GENRES` and `DOWNLOADED` are not routes (`ui/src/nav.ts:14-30`), and Downloaded contradicts the non-goal "no user-visible downloaded state". There is no account concept; the avatar would be fake UI. Home and New would both show recently added.
* **Recommendation:** For v1 use Home, Library and Search (plus Sources if needed), Settings behind a gear, and only implemented categories: Playlists, Artists, Albums, Songs, Liked. Defer New and Radio, cut Downloaded and Genres, do not render an account avatar.

### 6. Progressive streaming is a first-release overreach, and the proposed cache duplicates an existing owner — Major

* **Affected scope:** lines 39, 128–136, 151–159, 163–166, 283–289.
* **Discrepancy:** Progressive streaming requires a byte-range API in `neiro-core`, an iOS SDK artifact, agreed server range behaviour and token-refresh semantics, and gates on another team's schedule. The existing fetch-then-play path is implemented and tested (`ui/src/state/remote.ts:784-818`, `984-1000`; `tests/all-remote-catalog.test.tsx`). The host already owns cache accounting, clearing and a budgeted oldest-first prune (`ui/src/lib.rs:598-745`, `743-817`), with UI over those buckets (`ui/src/state/cache.ts`, `pages/settings.tsx:33-42`). Favourites and playlists reference cached paths (`remote.ts:696-720`), so a second byte-capped LRU that evicts audio files would orphan references. Atomic rename also means a temp file is unreadable until complete, so the diagram's range-reader → stream-cache → engine path cannot work as drawn.
* **Recommendation:** Ship the existing whole-file fetch in v1, keep one cache owner (the core), expose stats through `cache.ts`, skip startup reconciliation, and move range streaming to a follow-up design.

### 7. `EngineHandle` is an invented interface; the host already has a concrete `Engine` — Major

* **Affected scope:** lines 99, 232–234, 268.
* **Discrepancy:** No `EngineHandle` trait exists. The actual type is a concrete `struct Engine` at `ui/src/engine.rs:61`, with about eighteen function-pointer fields (`engine.rs:35-56`) and behaviour in `impl Engine` (`engine.rs:192`). A parallel iOS handle would duplicate queue normalization and event parsing. There is no `ui/src/engine/` directory today, and the entry is `ui/App.tsx`, not `ui/src/App.tsx`.
* **Recommendation:** Only the loader differs. Keep one `Engine` and one shared symbol table; the iOS loader fills the same fields from `extern "C"` symbols and skips optional probing. Use a target-selected constructor, not a new trait.

### 8. "Foldable support" is speculative framing for a width breakpoint — Major

* **Affected scope:** lines 1, 11–13, 40, 80, 170–174.
* **Discrepancy:** No current iOS device exposes a cover/inner screen pair; if the intended hardware is an Android foldable, the non-goal excludes Android. What remains is responsive width, already served by `measuredShellWidth` / `railSidebar` (`ui/src/components/layout.tsx:45-60`) and NSH's native `TabBar`. The hinge discussion, height term and fold-drag hysteresis are unproven complexity.
* **Recommendation:** State one responsive contract: system tab bar below a width breakpoint, sidebar above it. Test phone and tablet widths. Drop "foldable" as a deliverable and delete the right-edge dock alternative until a device or API exists.

### 9. The "chrome record" duplicates NSH signals and existing layout helpers, and its adoption is not assigned — Major

* **Affected scope:** lines 84–92, 239–262.
* **Discrepancy:** NSH already publishes `safeArea`, `safeAreaInsets`, `keyboardFrame`, `windowSize` and `platform` (`ui/src/.nsh/types/nsh/index.d.ts:593-609`, `1179-1187`); the app already uses `safeArea()` (`components/ui/feedback.tsx:37`). `pageEnd()` already owns the bottom inset (`components/layout.tsx:67`). The real defect is literal `pb-28` classes in many pages (`pages/albums.tsx:232,246,279,293`, `pages/remote.tsx:197,452,506,562`, `pages/tracks.tsx:229`, `pages/settings.tsx:147`) and the Toaster offset (`App.tsx:148`). None appear in the Stage 1 task list.
* **Recommendation:** Extend `pageEnd()` (and `barSize()` if needed) to account for the mini player plus tab bar, derive safe areas from NSH, and replace every literal `pb-28`, including `App.tsx`. Do not republish safe-area values.

### 10. Several existing components are reinvented, and some Stage 1 tasks are already implemented — Major

* **Affected scope:** lines 244, 247, 249, 251, 255, 257, 259–262.
* **Discrepancy:** `MainStack` already keeps one live `NavStack` per root and hides inactive ones (`shells/main.tsx:58-88`; `nav.ts:70-73,122-126`); only the reset loop in `openRoot` (`nav.ts:278-296`) needs to change, and re-tapping the active root already pops it (`nav.ts:267-277`). `SearchBox` already exists (`components/header.tsx:34-77`). `AppSheet` already supports `side="bottom"` and swipe-to-close (`components/ui/overlays.tsx:54-84`). `PlayerBar` already has compact and stacked arrangements (`player/player-bar.tsx:43-100`). `StudioMinimalShell` already renders `MainStack` and `NavControls`.
* **Recommendation:** Name the reused components; extract `SearchBox` if the page needs it; use `AppSheet` bottom without detents; reuse the compact `PlayerBar`; mark Studio Minimal desktop-only; change only the reset loop in `openRoot`.

### 11. The "Detailed Implementation Plan" is a file-level task list, not architecture — Major

* **Affected scope:** lines 224–320, especially 230–237 and 241–243.
* **Discrepancy:** Instructions such as exact `cargo:rustc-link-lib` flags, a temporary stub exporting symbols, and twenty-plus named file edits are ticket-level detail. The standard asks for architectural contracts and interfaces.
* **Recommendation:** Keep stage goals and acceptance criteria; move per-file lists to an implementation checklist. Replace them with four contract sections: engine seam and ABI symbols, session lifecycle state machine, chrome/inset contract, and import/cache ownership.

### 12. Output capability and route state are placed outside the single owner, and the iOS bit-perfect branch is dead code — Major

* **Affected scope:** lines 116, 269–270.
* **Discrepancy:** Stage 2 stores capability and route state in `state/engine.ts`, which is only the health gate today (`state/engine.ts:8-26`). Render-path and resolved output state belong to `state/resolved.ts` (`resolved.ts:168-183`), named as the single owner in `AGENTS.md` section 1. The proposed iOS branch of `isStreamBitPerfect()` can never become true because the first release never sets the flag; the existing predicate already returns false unless the engine reports bit-perfect and no host DSP is active.
* **Recommendation:** Put route and capability state in `resolved.ts` or a module consumed only by it. Leave `isStreamBitPerfect()` untouched. Implement the iOS display as a separate `RoutePanel`, and state in `AGENTS.md` that the iOS route panel is not a bit-perfect surface.

### 13. The Keychain migration has no source data, and "settings in Application Support" invents a second store — Major

* **Affected scope:** lines 142–147, 295.
* **Discrepancy:** There has never been an iOS build, so there are no old iOS `Storage` blobs to migrate. Non-secret settings already live in NSH `Storage` (`state/resolved.ts:299-313`, `state/eq.ts:126`, `state/library-prefs.ts:103-133`). Listing "settings" under Application Support creates a competing store.
* **Recommendation:** Keep non-secret settings in `Storage`; move only credentials to Keychain and read/write them directly. Drop the one-time deletion migration from the first release.

### 14. EQ/OPRA is in the first release but has no route on iOS — Major

* **Affected scope:** lines 17, 260, 271–273.
* **Discrepancy:** EQ is reachable only from the output console (`player/output-drawer.tsx:86-90`). On iOS the console is replaced by a small route panel, so EQ/OPRA has no entry point. Stage 2 also asks to modify console stages that are composed only by `output-drawer.tsx`; if iOS shows a route panel, they need no iOS logic.
* **Recommendation:** Choose one: add "Equalizer" under Settings > Playback & Audio, or drop EQ/OPRA from the first release. Build a small `RoutePanel` and leave the console stages desktop-only.

### 15. Queue-sheet "detents" do not exist in the platform component — Nit

* **Affected scope:** line 259.
* **Discrepancy:** `Sheet` supports an edge, backdrop, swipe-to-close and classes, but no detents (`ui/src/.nsh/types/nsh/base.d.ts:4621-4655`). `closeOnSwipe` is the only sizing and dismissal affordance.
* **Recommendation:** Use `side="bottom"` with `closeOnSwipe` and a max-height class. Delete "detents".

### 16. Plan hygiene and baseline corrections — Nit

* **Affected scope:** lines 23, 230–237, 242, 289, 294, 296, 301–320.
* **Discrepancy:**
  * `scripts/fetch-core-ios.sh` is Create in Stage 0 (line 235) and Modify in Stage 5 (line 296).
  * `objc2-avf-audio` (line 230) needs verification; AVAudioSession bindings are generally in `objc2-av-foundation`.
  * `ui/src/App.tsx` (line 242) does not exist; the entry is `ui/App.tsx`.
  * Stage 0 duplicates the untracked work package `docs/ios/phase0-s1.md`; cite or supersede it.
  * The verification plan cites four engine-dependent failures without naming them.
  * No minimum iOS version is stated, yet the design depends on specific `AVAudioSession`, `MPRemoteCommandCenter` and TabBar behaviour.
  * The problem statement says there is no `Info.plist`, while Stage 5 says "if NSH generates these files"; state which is true.
  * The overview lists Jellyfin, Navidrome and WebDAV while G3 says one remote protocol; state which are supported and tested.
  * The verification list omits existing tests: `cache-management.test.tsx`, `all-remote-catalog.test.tsx`, `remote-owner-switch.test.tsx`, `output-drawer.test.tsx`, `eq-page.test.tsx`, `info-drawer-play.test.tsx`.
* **Recommendation:** Resolve these in one current-state and baseline pass before approval.

## What Holds Up

The problem statement and its code references are accurate. Security-scoped bookmarks with scan on launch and foreground is the right simplification over the watcher plus reconcile loop. Static linking is the correct choice for iOS compared with runtime loading, and using `objc2` instead of a Swift target keeps one build and one ABI. Per-tab stack preservation reuses `nav.ts`'s existing per-root machinery. G5's truthful-output goal and the explicit non-goals are the strongest parts of the document. The central objection is scope: the release carries a desktop redesign, an unowned picker primitive, an external core dependency, and a second cache owner into a platform port whose root problem is much smaller.
