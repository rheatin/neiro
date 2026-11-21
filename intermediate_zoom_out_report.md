# Goldfish Review Report — Intermediate Zoom Out (Stage 8)

**Target:** `docs/designs/neiro-ios.md` (lines 1–320, current revision)
**Standard:** `zoom-out-standard.md`
**Mode:** REVIEW — Stage 8 (Intermediate Zoom Out)
**Date:** 2026-11-20

## Bottom Line

Context sufficiency still passes: the objective (run Neiro on iOS) and the five root problems (lines 21–31) remain legible and accurate.

The revision adds a plan, tests, a verification plan and nine alternatives. The writing is better; the scope is not smaller. The four Stage 5 Blockers are unchanged in substance: one is now argued rather than fixed (desktop bundling), one is half-assigned (bookmarks), and two are now committed to in build flags and CI files while their contracts remain open (static/LGPL link, audio ownership). The new Detailed Implementation Plan is the largest artifact in the document and it converts scope into a 60-plus-file execution list with no cut line. It also introduces four new plan-level defects: a false dependency graph, cache/work placement contradicted by the architecture, event parsing duplicated across two loaders, and tests that cover the desktop rewrite rather than the iOS behaviour the release exists to deliver.

## Reviewer Status

| Reviewer | Status |
| --- | --- |
| `egm-zoom-out` | completed |

## Prioritized Findings (Blockers -> Major -> Nits)

### Carried-Forward Stage 5 Blockers — still open

The reviewer re-audited each Stage 5 Blocker against the current file rather than repeating it. Verdicts:

| Stage 5 Blocker | Verdict | Evidence in current file |
| --- | --- | --- |
| 1. Desktop navigation redesign bundled with the iOS port, contradicting G7 | **STILL OPEN (now more concrete)** | G7 at `43` is unchanged ("remain unchanged except where shared code is refactored behind a target seam"). Line `65` still calls the semantics change "a deliberate break from the desktop sidebar's current behaviour". Alternative I (`218–222`) argues for the bundle instead of resolving the contradiction. Stage 1 makes it actionable: `246` replaces the desktop sidebar, `249` removes desktop title-bar search, `241` replaces `openRoot` (the dispatcher for every root route, `ui/src/nav.ts:236-239,264-297`), `256` restructures desktop Settings, `257` changes the desktop player bar, `261–262` replace desktop hover and right-click affordances. No narrowed G7, no desktop-navigation goal, no rollback. |
| 2. Picker / bookmark owned by neither NSH nor host | **PARTIALLY RESOLVED** | The bookmark half is now assigned: `ui/src/lib.rs` exports create/resolve/lifetime/stale detection (`278`), and Stage 3 represents a source as bookmark plus display path (`279`). Still unspecified: who presents `UIDocumentPickerViewController`. Line `122` still asserts "the document picker selects folders" as available; NSH exposes only `Picker.pickFiles` (`ui/src/.nsh/types/nsh/index.d.ts:440`), which returns bytes, and no bookmark API exists in the type surface. No spike gates it. The identity conflict is unresolved: `sources.ts` builds source ids from the absolute root path (`local:${root}`), `catalog.ts` persists `library:roots` as paths, and `source:active` persists one of those ids, while lines `126` and `279` say absolute container paths are never persisted. No stable per-bookmark UUID. |
| 3. Static / LGPL artifact contract unresolved | **STILL OPEN (now committed)** | Lines `96–103` and Stage 0 (`231`) fix the static link; line `164` still says the model is undecided. The xcframework contents are unstated: no mention of FFmpeg, `libnae`, libc++, or that the current FFmpeg build is shared-only (`scripts/build-ffmpeg-lgpl-macos.sh:24-31`). No ABI version is named ("pins one ABI version", `100`), no symbol list, no App Store compliance route, no fallback if a dynamic framework is required. |
| 4. Audio-session / engine ownership split with no contract | **STILL OPEN** | Line `15` still says the core is "driven through an `AVAudioSession` render path" (the session is a configuration object, not a render path); line `25` still says "the app receives a render callback"; §4 has the host tell the engine to "reopen its output" (`111`); §9 still draws `neiro_base → RemoteIO` (`156`); media-services reset still "tear down and rebuild the engine handle" (`112`), which loses the queue. Alternative H (`212–216`) still rejects core-side ownership. Stage 2 (`264–273`) lists modules but no ordering, no authoritative owner per fact, no reinitialize-audio command. |

### 1. The plan is a ticket-level change list, and it is the document's largest section — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `224–311`, especially `230–237` and `241–262`
* **Discrepancy:** The standard (§3) asks high-level sections for architecture, interfaces and data flow. This section instead names exact `cargo:rustc-link-search` / `rustc-link-lib` flags (`231`), the two exported stub symbols (`237`), twenty-one file edits in Stage 1, and per-component layout dimensions. It also leaves product decisions inside tasks: "Adapt to the five roots or explicitly mark it desktop-only" (`247`), "keep dither only if the engine path can apply it" (`273`), "Create or generate" Info.plist (`294`). A plan that is both this detailed and still contains undecided forks is not a plan an implementer can follow without redoing the design.
* **Rationale & Recommendation:** Keep stage goals, owners and acceptance criteria only; move the file list to a checklist outside the design. Replace the per-file enumeration with the four contracts requested in Stage 5 #11 — engine seam, session lifecycle state machine, chrome/inset ownership, cache ownership — and resolve the forks named above before the plan is binding.

### 2. Stage 0's verifiable result is a green build against a stub, and the artifact resolver is committed before the artifact exists — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `228–237`, `289`, `296`
* **Discrepancy:** `ui/build.rs` is declared "the only place that knows where the core artifact lives" (`231`), and `spike/neiro_base_stub/` is a committed library that exports the two symbols the health path uses (`237`). Nothing distinguishes a stub-linked build from a real-core build: the CI job "fails if the xcframework fetch fails" (`236`) can pass by linking the stub, and the deletion of the stub has no task or gate. If the LGPL decision at `164` forces a dynamic framework, `build.rs`, `fetch-core-ios.sh`, `ios.yml` and Stage 5's slice verification (`296`) all change. The same plan that says "this repository does not compile the C++ core" (`101`) commits a C stub that pretends to be the core.
* **Rationale & Recommendation:** Do not commit the stub to the default build path, or gate CI on artifact provenance. Prefer letting the platform toolchain consume the `.xcframework` (the format exists to select device or simulator slices) rather than teaching a `build.rs` to choose a slice; that removes a file, a flag set and a slice-selection failure mode.

### 3. The stage dependency graph is false and incomplete — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `226`; conflicts at `251` vs `281`, `253` vs `288`, `256` vs `273`, `287`
* **Discrepancy:** Line `226` says "Stage 2 depends on Stage 0; Stages 3 and 4 depend on Stage 2." In the task lists, Stage 3 modifies `pages/library.tsx`, which Stage 1 creates (`281` vs `251`); Stage 4 modifies `pages/radio.tsx`, which Stage 1 creates (`288` vs `253`); Stage 2 modifies `pages/settings.tsx`, which Stage 1 modifies (`273` vs `256`). So Stages 2, 3 and 4 all depend on Stage 1, not (only) on 0 and 2. Conversely, Stage 3 (local files) has no real dependency on the audio session in Stage 2. Line `287` says "the exact direction is fixed with `neiro-core` in Stage 0", but Stage 0 contains no `neiro-core` task (`228–237`), and the graph routes Stage 4 through Stage 2. The one scheduling claim in the plan is wrong in both directions.
* **Rationale & Recommendation:** Re-derive the graph from actual file dependencies, or better, restage by vertical slice: (i) iOS build plus one local file playing through the real core; (ii) shell and IA; (iii) streaming; (iv) packaging. This reordering also fixes the delivery inversion: as planned, the desktop regression lands in Stage 1 and the first playable iOS build lands after Stage 3.

### 4. The plan assigns the stream cache to the wrong module and duplicates the existing cache owner — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `286` vs `130–134`; existing owner at `ui/src/lib.rs:682-745,781-842`
* **Discrepancy:** Section 6 (`130–134`) says "The host keeps: a bounded stream cache". Stage 4 puts "a byte cap, LRU eviction, temp-file writes and atomic rename" in `ui/src/state/cache.ts` (`286`). That file is a stats reader: `CacheKind`, `cacheStats`, `refreshCacheStats` (`ui/src/state/cache.ts:11-73`). The real owner of filesystem cache accounting, clearing and budgeted oldest-first pruning is the Rust host (`lib.rs:682-745`, `781-842`). The plan thus creates a second cache owner in the layer that cannot touch the filesystem, and it does not mention the existing budget/prune machinery at all.
* **Rationale & Recommendation:** Put the stream cache beside the existing prune/temp-write code in `lib.rs` and keep `state/cache.ts` a reader, exactly as today. If progressive streaming is deferred (Stage 5 #6), this finding disappears with it.

### 5. Event parsing is duplicated across two loaders, and capability state gets a second home — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `268`, `269`, `232–234`
* **Discrepancy:** Line `268` says `engine/ios.rs` adds event parsing "for route descriptor, render format, resampling, interruption and route change. The desktop loader parses the same events when the core emits them." That is two implementations of one wire format in two modules, which is the failure mode `AGENTS.md` section 1 forbids ("Every piece of state has exactly ONE authoritative owner"). Separately, line `269` stores the capability report in `state/engine.ts`, which today is only the engine health gate, while output policy and bit-perfect ownership live in `state/resolved.ts` (`AGENTS.md` §1).
* **Rationale & Recommendation:** One parser in a shared `engine.rs`; the loaders fill the symbol table only. One state module, consumed by `resolved.ts`. Leave `isStreamBitPerfect()` untouched — its existing body already returns false unless the core reports bit-perfect and host DSP is off (`ui/src/state/resolved.ts:176-183`), so line `270`'s "first release never sets that flag" branch is dead code.

### 6. The plan's tests cover the desktop UI rewrite, not the iOS behaviour the release exists to deliver — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `301–311`, `317–319`
* **Discrepancy:** Of seven new tests, three (`303–305`) test the new IA, which is the bundled desktop change; `search-page` tests a page (`306`); `library-categories` repeats the IA. The two tests that could verify G6 — `audio-session` (`308`) and `ios-import` (`307`) — are headless tests "with fakes", which cannot exercise backgrounding, lock screen, interruption or route change; those are deferred to the manual device matrix (`319`). Four new or rewritten pages (`home`, `new`, `radio`, `settings`) have no direct test. EQ/OPRA, lyrics and category sort/grid are in the first release (`17`) and have no test or verification step. The verification baseline still references unnamed pre-existing failures (`317`), so a green run cannot be distinguished from a regressed one without discovering them from CI.
* **Rationale & Recommendation:** State per goal whether acceptance is automated or manual, and cut the goals whose acceptance is neither (EQ, lyrics, sort/grid). Name the four failing tests. If interruption and lock screen can only be manual, say so in G6 rather than implying test coverage.

### 7. A middle stage is externally gated, and the plan has already rejected the only fallback — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `39`, `164–166`, `200–204`, `285–289`
* **Discrepancy:** Line `289` now states plainly that `neiro-core` "does not implement it; the design depends on the core team's schedule." Alternative F (`200–204`) rejects whole-file fetch, which is the implemented, tested path and the only fallback if the range API slips. The result is a release whose middle stage can stall indefinitely with no stated plan B, while Stage 1's desktop change has already shipped.
* **Rationale & Recommendation:** Either commit Stage 4 to the existing fetch path for v1 and move range streaming to a follow-up design, or state the fallback and the date after which the fallback triggers.

### 8. The Verification Plan assumes an environment the plan never provisions — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `236`, `315–318`
* **Discrepancy:** `nsh run --target ios` with a booted simulator is a routine step in CI (`236`) and verification (`318`). The repository's own iOS work package records that the NSH iOS runtime is not installed and the simulator list is empty (`docs/ios/phase0-s1.md:20`), and it lists toolchain/runtime/simulator setup as its first task. Stage 0 of the design omits that step.
* **Rationale & Recommendation:** Add the runtime and simulator provisioning to Stage 0 as an explicit, checked prerequisite, or mark the simulator verification as pending on it.

### 9. Shared-component touch affordances remove desktop interactions without a target gate — Severity: Major

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `248`, `261–262`
* **Discrepancy:** Stage 1 tells the implementer to "Remove right-click-only removal in favour of a long-press menu" (`248`) and to "Replace hover-only play buttons and `onContextMenu`-only actions" (`261–262`) in `cards.tsx`, `tracks.tsx`, `remote-cards.tsx`, `selection-bar.tsx` and `context-menu.tsx`. Unlike other tasks that say "behind a desktop check" (`242`, `249`, `250`), these are not gated. Read literally they remove desktop hover and right-click behaviour across every library list — a second desktop regression beyond the IA change, and one that G7 does not cover.
* **Rationale & Recommendation:** Say "add a long-press affordance on iOS; keep hover and context menu on desktop", or move these to a separate touch-affordance design.

### 10. A staged plan that revisits the same files in three to five stages cannot have a verifiable result per stage — Severity: Nit

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `256`/`273`/`280` (`pages/settings.tsx`), `267`/`278`/`287`/`295` (`lib.rs`), `235`/`296` (`fetch-core-ios.sh`), `236`/`297` (`ios.yml`)
* **Discrepancy:** Four stages edit `lib.rs`, three edit `settings.tsx`; two artifacts are "Create" in Stage 0 and "Modify" in Stage 5. The stage boundary is drawn by concern, not by a shippable state, so the stated "each stage has a verifiable result" (`226`) holds for Stages 0 and maybe 2, and not for the others. (Stage 5 finding 16 already noted the Create/Modify pair; the plan has since added two more multi-stage files.)
* **Rationale & Recommendation:** Either stage by slice, or state per stage that it is not independently shippable and why.

### 11. Competing owners and dead ends introduced by the plan — Severity: Nit

* **Reviewer Lens:** `egm-zoom-out`
* **Affected Scope:** `251–253`, `279`, `84–92`
* **Discrepancy:**
  * (i) `pages/radio.tsx` is created while `pages/remote.tsx` is kept "as the catalog body" (`253`), so two page components own one screen and future edits must know which is which.
  * (ii) `sources.ts` gains a bookmark identity (`279`) while `catalog.ts` still persists `library:roots` as absolute paths and `source:active` persists a path-derived id — two source identities.
  * (iii) The chrome record (`84–92`) is adopted in Stage 1 for `layout.tsx` (`243`), but the plan never lists the literal `pb-28` pages that actually hard-code the bottom inset, so the migration stops halfway and leaves both the record and the literals authoritative.
* **Rationale & Recommendation:** Rename rather than wrap (`radio.tsx` replaces `remote.tsx`); make the bookmark UUID the source id and fold paths into a display-only field; list the literal-inset pages in the same layout task or drop the record in favour of extending `pageEnd()`.

## Stage 7 Findings That Are Really Scope Findings

Three Stage 7 items are misclassified as document-writing defects and should be treated as scope defects:

* **Missing Risks & Mitigations.** The absent risks (LGPL distribution, jetsam against five live stacks, an external core schedule, a second cache, bookmark expiry, no rollback for the desktop IA) are not missing prose — they are the open scope decisions themselves. A table can be written today, but three of its rows would read "mitigation: decide Blocker 1/3/4". The section cannot be completed until the scope is cut.
* **Alternatives attribution rule.** The circular rationales (A, B, C, D, I all reduce to "we chose Apple Music" or "we land it together") are a symptom of a product premise with no decision record. The substantive issue is that Alternative I is the only scope-reduction option and it is dismissed in one sentence; that belongs to Blocker 1, not to a style pass. The missing link is real, but linking a record for a decision no one has made is not a fix.
* **IA-table inconsistency.** Three divergent inventories (`13`, `61`, `251`) cannot be reconciled by writing a table, because the document keeps adding destinations with no data model or owner (New, Genres, Downloaded, Liked, the account avatar). Cut the destinations first; the table then has one answer.

Partially misclassified: **Open Questions lacks resolution mechanics.** The real issue is that all four questions are external commitments (core range API, LGPL, server range behaviour, cache-after-kill). Owners and dates help, but the scope decision is whether a first release may depend on four unagreed external answers at all.

Correctly classified as scope already: first-release inventory inconsistency, "New" with no data source, tab-stack memory pressure, and the desktop IA contradiction. The remaining Stage 7 Majors and Nits are genuine prose/terminology items.

## What Holds Up

* The five-problem statement (`21–31`) is accurate against the tree: nine roots and no `library` route (`ui/src/nav.ts:44`); `openRoot` resets other stacks (`nav.ts:279-289`); `libloading`-only loader (`ui/src/engine.rs:23,278-279`); no `data_root()` iOS branch (`ui/src/lib.rs:523-549`); no `ui/build.rs`, no `ui/src/engine/`, no iOS workflow (only `macos.yml`, `windows.yml`); NSH picker is file-bytes only.
* Security-scoped bookmarks with scan on launch and foreground is the right replacement for the watcher plus the 600-second reconcile loop (`ui/src/state/catalog.ts:63-86`), and it is the best simplification in the document.
* Static linking on iOS is correct for the platform, and splitting the loader by target is the right direction even if the plan invents a trait to do it.
* Alternatives E, F, G and H are a genuine trade-off record; F in particular makes the "reuse the existing fetch path" argument visible, even though it is rejected.
* Per-tab stack preservation reuses `nav.ts`'s existing per-root machinery (`MainStack` already keeps one live stack per root, `ui/src/shells/main.tsx:58-88`); only the reset loop is the delta.
* G5 and the non-goals remain the strongest part: the "system mixer in path" neutral state and the refusal to draw the bit-perfect badge are exactly the truthful-output stance the standard wants. Leaving `isStreamBitPerfect()` as sole owner is right, even though the proposed iOS branch is dead.
* The Verification Plan's desktop-regression line (`320`) is the correct acceptance axis, and it is the clearest admission that G7 as written is not true.

## Could Not Verify

* Whether the NSH iOS runtime can be installed and a simulator created in this environment. The only source is the untracked work package `docs/ios/phase0-s1.md:20`, which says it is not installed and the list is empty; the design's Stage 0 does not address it.
* Whether `objc2-avf-audio` (`230`) is the correct crate for `AVAudioSession`; AVAudioSession/AVAudioEngine bindings are usually in an AVFoundation crate. This needs a registry check that a read-only reviewer cannot perform.
* The identity of the "four engine-dependent failures that predate this work" (`317`); enumerating them requires running the suite.
* Any LGPL/App Store outcome; that is a legal determination, not a code fact.
* Whether `nsh` 0.6.1 generates `Info.plist` and entitlements from `ui/nsh.json`; the design leaves it open at `294`, and the current `ui/nsh.json` has no iOS keys.
* Whether all supported Jellyfin servers honour byte ranges (open question at `165`).

## Note on the Plan's Core Seam

The Stage 5 #7 recommendation — one `Engine`, one symbol table, a target-selected constructor — was not adopted; the plan still says "re-export `EngineHandle`" (`232`) and "an `EngineHandle` implementation" (`234`) for a type that does not exist. Because the plan now also packages `spike/neiro_base_stub/` and `build.rs`, this is no longer only a naming problem: it is the concrete shape of the seam that Stage 0 will build and Stage 2 will then extend with duplicated event parsing (finding 5).
