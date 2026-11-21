# Comprehension Test — Neiro iOS and Foldable Support

**Target:** `/Users/char/Repos/neiro/docs/designs/neiro-ios.md`
**FeatureRange:** lines 1–223
**DetailedRange:** lines 224–320
**Mode:** REVIEW — Stage 6 (Comprehension Test)
**Date:** 2026-11-20

## What this proposes

The document adds an iOS client to Neiro without changing desktop behaviour. The existing C++ core is shipped as a statically linked `.xcframework` (device and simulator slices); the Rust host binds one pinned C symbol set directly instead of `libloading`, and owns an iOS-only module built on `objc2` that manages `AVAudioSession`, `MPNowPlayingInfoCenter` and `MPRemoteCommandCenter` (interruptions, route changes, media-services reset, lock-screen metadata, remote commands). Navigation copies Apple Music: five roots — Home, New, Radio, Library, Search — shown as a labelled bottom tab bar when folded and as the same five-entry left sidebar when unfolded and on desktop, with one navigation stack preserved per tab, Settings in a root-level modal behind an account avatar, and Now Playing as a sheet raised from a mini player. Local music enters by picking folders and retaining security-scoped bookmarks (resolved and scanned on launch/foreground, no watcher); remote music (Jellyfin first) is streamed by byte range through a bounded LRU stream cache; the iOS output surface reports the real route, format, channels and system volume and deliberately does not claim bit-perfect playback. The work is staged 0–5 — build/engine seam, IA and shells, audio session and output, local files, remote streaming, packaging/security — with named tests for each and a verification plan.

## How the existing system works, per this document

Neiro is a desktop host: a TSX front end calls a Rust host (`neiro-ui`), which loads the C++ core (`neiro_base`) at run time with `libloading`; on iOS `layout()` reports `"unsupported"` and there is no iOS artifact, Xcode project, `Info.plist`, entitlements or CI job. Navigation has nine roots with an `openRoot` that resets every other root's stack, no `library` route, several pages (Liked, Albums, Artists, Tracks) reachable only through the desktop sidebar's Library panel, and Now Playing as a root-level modal sheet opened by a boolean. Local music arrives through a folder picker, a recursive filesystem watcher and a 600-second reconcile loop; remote tracks are downloaded whole before playback, with an in-memory-only cache index and JavaScript-timer watchdogs that stop when the app is suspended. Remote credentials sit in a general key-value `Storage`, the macOS backend owns device enumeration and exclusive shared/exclusive ownership, and `isStreamBitPerfect()` checks only an engine flag plus host DSP.

## Findings

The reviewer produced a paraphrase for every section and raised no Blocker. Four decisions could not be reconstructed: (a) the folded/unfolded switch threshold and settle rule, (b) whether Navidrome/WebDAV are supported on iOS in release 1, (c) how a user reaches the Liked page, and (d) the contents of the promised "IA table".

1. **NSH is never defined — Major.** Affected scope: lines 37, 67–80, 101, 140, 173, 230–237, 293, 315–318. NSH is variously a simulator host, a UI toolkit, a build/run tool, a metadata format, a project generator and a source of platform limitations. A fresh reader cannot tell what it is or verify G1.

2. **"Route" means two unrelated things — Major.** Affected scope: lines 15, 25, 29, 41, 111, 116, 158, 191, 259–260, 271. It means both an audio output route and a navigation destination. Define "audio route" once and use "page"/"destination" for navigation.

3. **The foldable switch rule is unspecified — Major.** Affected scope: line 80, G4 (line 40), Stage 1 shells. "Measured width and height with hysteresis" gives no breakpoint, margin, settle interval, or rule for when width and height disagree, so shell selection cannot be implemented from the text.

4. **The Library category list is inconsistent and partly unreachable — Major.** Affected scope: lines 13, 29, 59–61, 241, 251, 305. `LIKED` is pushed into Library's stack but omitted from both category lists. "Songs" is not mapped to `TRACKS`. "Sort and grid/list controls" are promised but no stage implements them. The "nine roots" count is only reconstructable by inference.

5. **Radio sources are contradictory — Major.** Affected scope: lines 13, 33, 39, 135–136, 146, 285–288. The Overview lists Jellyfin, Navidrome and WebDAV; G3 and the first-release scope say one protocol with Jellyfin first. The document never states whether the other two ship, appear as unsupported, or are deferred.

6. **OPRA is undefined — Major.** Affected scope: lines 17, 50. Named as first-release scope and as a sync non-goal, with no expansion or link. A reader cannot tell whether it is an internal feature, an external database or a file format.

7. **Three first-release capabilities have no work item — Major.** Affected scope: lines 13, 17; Stages 0–5 (lines 228–299). EQ/OPRA, lyrics and category-page sort/grid-list controls are declared in scope but have no stage item, test or verification step.

8. **The promised IA table does not exist — Major.** Affected scope: line 33. The ASCII diagram shows screen composition, not which roots exist in which shell, which pages push into which stack, and which are modal. The IA cannot be checked against the plan.

9. **ATS is an undecided decision inside a first-release goal — Major.** Affected scope: lines 136, 140, 294. It is named as a policy and a Stage 5 deliverable, but is absent from Open Questions, has no criteria, and the acronym is never expanded.

10. **FFmpeg appears without context — Major.** Affected scope: Open Questions, line 164. The document never states that the core decodes through FFmpeg, that it is LGPL, or how it is linked today, so the App Store consequence and the challenge to G1's static link are unintelligible.

11. **The `-2` fallback is unexplained — Nit.** Affected scope: lines 100, 233. A bare token for an existing mechanism excluded from iOS.

12. **"Phase 0" is a second numbering scheme — Nit.** Affected scope: line 299. It presumably means Stage 0, but that is an inference.

13. **Four pre-existing test failures are unnamed — Nit.** Affected scope: line 317. The verification gate cannot be applied without discovering them from CI.

14. **Existing identifiers are used as if their contracts are known — Nit.** Affected scope: lines 23, 92, 243. `layout()`, `pageEnd`, `pb-28`, `Toaster`, `sidebarWidth()`, `wideTracks()` are not described.

15. **The account avatar has no defined identity or entry point — Nit.** Affected scope: lines 13, 40, 63, 248–249. Nothing says whether an account exists today, what it represents, or whether desktop Settings also moves behind it.

16. **The stage dependency graph is incomplete — Nit.** Affected scope: line 226. It omits Stage 3/4's dependency on pages created in Stage 1 and says nothing about Stage 5.

17. **DoP is unexpanded and "untouched carrier" is undefined — Nit.** Affected scope: line 48.

18. **Security-scoped bookmark mechanisms are unexpanded — Nit.** Affected scope: lines 122–126. The two platform mechanisms are named without links, and the lifetime requirement is only parenthetically justified.

19. **"The review" is an unsupplied artifact — Nit.** Affected scope: line 179. Alternative B's rejection partly rests on it.

20. **"File-protection class" is referred to but never chosen — Nit.** Affected scope: line 294 vs line 147.

21. **"Output console" and "desktop mastering console" may be the same thing — Nit.** Affected scope: lines 17 and 49. The reader cannot tell whether they refer to `output-drawer.tsx`, the 668 pt console, or separate components.

The reviewer notes that its reconstruction is plausibly wrong only on item 5 (Radio sources) and item 4 (Liked); both need explicit statement.
