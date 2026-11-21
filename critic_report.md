# Goldfish Review Report

**Target:** `docs/designs/neiro-ios.md` (`1–223`)

## Reviewer Status

| Reviewer | Status |
| --- | --- |
| `egm-design` | completed |
| `egm-structural` | completed |
| `egm-style` | completed |

## Prioritized Findings (Blockers -> Major -> Nits)

### Missing Risks & Mitigations section — Severity: Blocker

* **Reviewer Lens:** `egm-structural` (Blocker); `egm-design` (Major)
* **Affected Scope:** Whole document; absent between line 159 and `## Open Questions` (161–166)
* **Discrepancy Description:** The feature-spec standard requires the specification to call out its own constraints, failure modes, security implications, scalability limits and mitigations. The audited range contains no "risk", "mitigation", "failure mode" or "scalability" content. `## Open Questions` records unresolved choices, not impact/mitigation pairs. Unaddressed risks include: iOS memory limits (jetsam) against five live tab stacks and an in-process decoder; storage budget for the stream cache, artwork and `library.db`; background suspension or app kill during playback; interruption/route-change state loss mid-track; credential and file-protection failure paths; App Store review exposure from background audio; and rollback of the desktop IA change.
* **Rationale & Recommendation:** A fresh reader cannot tell which failure is tolerated and which is release-blocking, so the standard's "all major risks addressed" bar is not met. Add a `## Risks & Mitigations` table (risk, likelihood/impact, owner, mitigation, verification) covering at minimum the items above, plus bookmark expiry (123–125), cache corruption after termination (166), Keychain migration (146), static-link/LGPL distribution (164) and simulator/device slice drift (101).

### Alternatives Rule violated — unlinked author attribution and circular rationales — Severity: Blocker

* **Reviewer Lens:** `egm-structural` (Blocker); `egm-design` (Major); `egm-style` (Major)
* **Affected Scope:** Lines 170–222 (reasons at 174, 180, 186, 192, 198, 204, 210, 216, 222); unlinked review at 179
* **Discrepancy Description:** The standard states rejection rationales "must not attribute a rejection to a maintainer without linking their issue, discussion, or PR", and must state concrete technical or scope trade-offs. All nine alternatives read "Rejected by author: …" with no linked decision record; line 179 cites "the review" without naming it; the document contains no Markdown links at all. Several reasons are circular, citing the chosen design as its own justification (A "Apple Music's left sidebar is the chosen model"; B "content stays one column"; C "the unfolded screen uses Apple Music's sidebar"; D "Apple Music raises Now Playing as a sheet"; E "folders … should stay attached to their source"; I "the IA change and the desktop sidebar change land together"). The Pros/Cons fields contain real trade-offs, but the decisive field does not invoke them.
* **Rationale & Recommendation:** The standard says the specification fails if alternatives are not proven rejected for documented reasons. Replace each "Rejected by author" with the defeating trade-off from that alternative's own Cons (cost, test surface, implementation effort, number of page variants), and link a decision record, issue or PR where the rejection is a product decision. Link the review referenced at 179.

### No contract for the iOS core artifact — Severity: Blocker

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 15, 23, 96–101, 130, 163
* **Discrepancy Description:** The architecture requires `neiro-core` to gain an iOS/static target, an iOS output backend, byte-range reads, a capability report and interruption/route-change events. Line 101 says the artifact comes from the `neiro-core` repository and this repository does not compile the C++ core; line 163 reduces all of it to "require agreement with `neiro-core`". There is no ABI version, symbol list, event schema, threading contract, ownership/lifetime rule or mismatch fallback. `AGENTS.md` already requires compatibility with `v0.2.0` and `>= v0.2.1`; the iOS pin at line 100 is not defined against either.
* **Rationale & Recommendation:** G1–G6 cannot be assessed or scheduled against an interface that does not exist. Add a Core Contract section: exact exported symbols and signatures, the pinned ABI version and how a mismatch is rejected at build time, the JSON schemas for route, render format, interruption and route-change events, thread/ownership rules for the render callback, and behavior when the pinned core is older than expected. Name the owning release.

### Linking model committed as static while the LGPL decision is open — Severity: Blocker

* **Reviewer Lens:** `egm-design` (Blocker); `egm-structural` (Major)
* **Affected Scope:** Lines 15, 37 (G1), 96–101, 164
* **Discrepancy Description:** G1 requires linking `neiro_base` statically and §3 states "iOS uses a static link" with an `xcframework` linked through `build.rs`. Line 164 then says static linking into an App Store binary "may require a dynamic framework or relinkable objects; the linking model is not decided". No alternative covers the dynamic-framework or relinkable-object path. The current SDK ships `libneiro_base` **and** `libnae` plus FFmpeg (`scripts/fetch-core.sh`; `ui/src/engine.rs` documents the FFmpeg chain), but the document names only `neiro_base.xcframework` and never says whether FFmpeg is inside it. A dynamic-framework answer would also reopen the engine-boundary decision in §3.
* **Rationale & Recommendation:** The standard requires constraints to be called out and mitigated. A licensing/linking decision that can invalidate the engine boundary, packaging and App Store submission cannot be left as an open question at the same time as G1 asserts the answer. Choose and document the linking model with its fallback, list the xcframework slices and transitive link set (core, `nae`, FFmpeg, libc++), and add a compliance check to G1's acceptance.

### Audio-graph ownership is contradictory — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 15, 25, 105–116, 149–159, 212–216
* **Discrepancy Description:** Line 15 says the core is "driven through an `AVAudioSession` render path"; line 25 says "the app receives a render callback from `AVAudioSession`/RemoteIO"; §9 routes `neiro_base (static .a) → RemoteIO`; §4 says a host module owns `AVAudioSession` and, on route change, "tell the engine to reopen its output". Alternative H rejects an iOS output backend inside `neiro_base` because "the session belongs in the host". Both host and core are thus described as owning the same audio graph, with no call order for category/activation, stream format, interruption begin/end, route change, media-services reset or shutdown. `AVAudioSession` is a session/configuration object, not a render path.
* **Rationale & Recommendation:** Two owners of the audio graph is the same failure mode the document criticizes for Now Playing ("two owners of what is on screen"); on iOS it produces silent streams and wrong format reporting. Pick one owner (recommended: core owns the I/O unit, host configures the session and passes ordered commands/events), define the sequencing for each lifecycle event, fix line 15's terminology, and make Alternative H's rejection match the chosen split.

### G7 ("No desktop regression") contradicts the desktop navigation change — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 13, 43 (G7), 59–65, 218–222
* **Discrepancy Description:** G7 promises desktop behaviour, tests and the existing core path "remain unchanged except where shared code is refactored behind a target seam". Section 1 replaces `openRoot`'s semantics and says this is "a deliberate break from the desktop sidebar's current behaviour and must land with the same change as the sidebar itself"; Alternative I says the IA change and the desktop sidebar change land together. The desktop navigation change is user-visible, not behind a target seam.
* **Rationale & Recommendation:** A goal the design knowingly violates cannot serve as an acceptance criterion. Narrow G7 to output/core/path behaviour, add an explicit desktop-navigation goal with its verification (naming the UI tests that will change), and state a rollback plan.

### Shell selection and the `"mobile"` layout record are underspecified — Severity: Major

* **Reviewer Lens:** `egm-design`; `egm-style`
* **Affected Scope:** Lines 11, 13, 40, 68, 80, 84–92, 172
* **Discrepancy Description:** Line 80 chooses the shell "by measured width and height with hysteresis" but gives no breakpoints, hysteresis band, settle/debounce time, measured signal, orientation or height rule, or launch-on-either-screen behavior. The IA, G4, the diagram label and the alternatives nonetheless describe shells as folded/unfolded physical states, and "both screens"/"both screen shapes" never say whether a normal iPhone and a foldable's cover screen share one shell, or that iPad follows the unfolded shell. The `"mobile"` record (84–92) omits the width-driven decisions that actually differ (`wideTracks`, `barSize`, `railSidebar`, `controlsInTitleBar`) and never says which shell component renders the unfolded form; one `"mobile"` value conflates folded, unfolded and iPad, while `App.tsx` switches on `layout() === "desktop"`.
* **Rationale & Recommendation:** Foldable support is a named deliverable whose only mechanism cannot be reviewed, tested or reproduced, and a reader will implement hinge detection the plan explicitly does not use. State thresholds, hysteresis, debounce, measured signal and the device/simulator matrix; define the shape-to-shell mapping in width classes; extend the record with the width-derived predicates or their values per form; name the shell component for each form; state explicitly that iPad uses the unfolded shell.

### Information architecture is not frozen and its inventories disagree — Severity: Major

* **Reviewer Lens:** `egm-design`; `egm-structural`; `egm-style`
* **Affected Scope:** Lines 13, 33, 59–63
* **Discrepancy Description:** Line 33 promises to "freeze an IA table", but the document contains no table. The IA is restated inconsistently: line 13 lists Library as "Playlists, Artists, Albums, Songs, Genres, Downloaded" (six entries, name "Songs", no Liked); line 61 pushes `ALBUMS`, `ARTISTS`, `TRACKS`, `LIKED`, `PLAYLISTS`, `GENRES`, `DOWNLOADED` (seven entries, route name `TRACKS`); `GENRES` and `DOWNLOADED` appear without being introduced as new routes (`ui/src/nav.ts` defines neither). `LIKED` becomes a Library page but is absent from the category list, so under the new IA nothing navigates to it — the same defect the document criticizes for `PLAYLISTS` at line 29. Line 59's "five roots become five" framing also hides that `RADIO` is a rename and `LIBRARY` an addition.
* **Rationale & Recommendation:** The IA is the document's central decision and must have one authority. Add a canonical table (current route, current state, destination tab/stack, new vs renamed, parent category) and make the Overview, §1 and G4 reference it rather than restating it. Decide whether Liked remains a destination, and reconcile "Songs" with `TRACKS`.

### First-release scope inventory is inconsistent — Severity: Major

* **Reviewer Lens:** `egm-structural`; `egm-style`
* **Affected Scope:** Lines 13, 17, 33, 35–43, 130, 146
* **Discrepancy Description:** Line 17's first-release list covers "the navigation shell, Home, Library, Playlists, Search, a trimmed Settings" — omitting New and Radio, the two tabs the IA commits to, while inserting Playlists, which line 13 defines as a Library page. The v1 content of New is never stated. Line 33 adds "audio-session, lock-screen, interruption and Keychain work" as first-release, but goals G1–G7 contain no credentials/Keychain goal; they also contain no EQ/OPRA goal despite line 17 making it first-release. Radio's source set is ambiguous: line 13 says Jellyfin, Navidrome and WebDAV, line 146 provisions credentials for all three, but G3 and §6 describe only Jellyfin as the first source.
* **Rationale & Recommendation:** A reader cannot determine what actually ships or how it is accepted. List v1 contents per tab, add goals (or explicit non-goals) for credentials and EQ, and state the first-release remote source set in G3 and §6, aligning the Keychain migration scope.

### "New" has no data source — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 13, 40, 163
* **Discrepancy Description:** "New holds recently added and newly imported music," but the host's browsing index (`TrackRow` in `ui/src/engine.rs`) carries no added-at timestamp and `ui/src/pages/user-collections.tsx` records the same limitation. The `neiro-core` additions at line 163 list only byte-range, route descriptor, render format, interruption and route change, and no host-side first-seen ledger is specified.
* **Rationale & Recommendation:** A first-release destination cannot be populated. Add `added_at` or a host import ledger to the core contract, and define what New shows on first launch with an existing library and for remote tracks.

### EQ/OPRA and Settings route ownership is undefined — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 17, 52, 59–63
* **Discrepancy Description:** EQ/OPRA ships in the first release, but its current entry points are the output drawer (whose console is deferred) and Settings; the document moves Settings to "a root-level modal stack, outside the five tabs" without naming that stack or its routes. `open()` pushes non-root routes onto the active tab's stack (`ui/src/nav.ts`), so `open({name:"eq"})` from modal Settings would land in whichever tab is underneath. Settings' existing links to `REMOTE` and `TRACKS` have the same problem.
* **Rationale & Recommendation:** This is a concrete route-ownership bug the design introduces. Define the modal stack's route set and the mechanism for cross-stack navigation (Settings → tab, Settings → EQ), and state where EQ is reached on iOS.

### Tab stacks and background audio ignore iOS memory pressure — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 40, 65, 90–92
* **Discrepancy Description:** G4 and §1 keep one live navigation stack per tab. The shell already keeps every visited root mounted and laid out (`shells/main.tsx`, `visibility: hidden`) with `MAX_PAGES = 4` per stack (`ui/src/nav.ts`), and search filters fully loaded album/artist lists in JS (`pages/search.tsx`). Five such tabs plus artwork caches and an in-process decoder on a jetsam-limited device is not budgeted anywhere, and background audio makes a kill user-visible.
* **Rationale & Recommendation:** Add an iOS memory policy (unmount far tabs, cap per-tab stack depth, evict artwork aggressively, bound search inputs) and a measurement in the verification plan.

### "One network trust path" cannot deliver the described ATS policy — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 136, 140
* **Discrepancy Description:** Probing is JavaScript `fetch` (`ui/src/state/remote.ts`), while listings and streams go through the C core (`engine.listRemote` / `engine.fetchRemote`). ATS is enforced by URLSession, not by a C socket/FFmpeg client, so the ATS decision governs only the probe. The iOS local-network privacy prompt is enforced below the framework and will fire for the core's sockets at a time and in a context the design does not control. TLS validation and token handling parity between the two stacks is unstated.
* **Rationale & Recommendation:** State which stack performs each request; either give the core a host-provided transport or document two policies explicitly, including where the local-network prompt appears and what the UI says.

### Folder picker and security-scoped bookmarks have no specified mechanism — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 27, 38, 118–126
* **Discrepancy Description:** The document treats "the document picker selects folders; the app stores bookmark data" as existing capability. NSH's only picker API selects files and reads bytes into memory, with no folder mode or bookmark API; the host's `pick_folder` uses `OpenDialog::folders()` (`ui/src/lib.rs`) and the document never says whether that works on iOS or yields a URL suitable for `NSURL.bookmarkData`.
* **Rationale & Recommendation:** G2 and the entire local-import model rest on an unspecified capability. Name the host module and APIs (`UIDocumentPickerViewController` with a folder content type, `startAccessingSecurityScopedResource`, bookmark creation/resolution), and state cancellation, re-pick and failure behavior.

### Startup side effects assume desktop output semantics — Severity: Major

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 41, 49–50, 116
* **Discrepancy Description:** On engine-ready, `App.tsx` calls `enumerateAtStartup()` → `engine.listDevices()` and `restorePolicyAtStartup()` → `engine.setPolicy` with `share`, `rate`, `dither` and `dsd_mode`. Device enumeration and exclusive mode are non-goals (49), and G5 promises truthful reporting, but the document never says what these calls do on iOS: whether the core accepts `share: "exclusive"`, whether `rate: "device"` is meaningful under the system mixer, or whether the device list is empty, an error, or omitted.
* **Rationale & Recommendation:** An unaddressed startup path will either fail loudly or silently report desktop-only semantics. State the iOS policy mapping (force shared, no device selection, no DSD) and guard `enumerateAtStartup`, `restorePolicyAtStartup` and plugin refresh per target.

### Open Questions lacks resolution mechanics and contradicts settled plan text — Severity: Major

* **Reviewer Lens:** `egm-structural`; `egm-style`
* **Affected Scope:** Lines 116, 130, 133, 161–166
* **Discrepancy Description:** None of the four questions has an owner, decision date, decision criteria or link, so the section is indistinguishable from an unowned risk register. It also reopens decisions the plan states as settled: line 163 says byte-range reads and the event set "require agreement with `neiro-core`", while line 116 says the engine "gains" a capability report and events and line 130 says `neiro-core` "gains" a byte-range path; line 166 asks whether partial cache data survives a kill, while line 133 states startup reconciliation "deletes orphaned temp files".
* **Rationale & Recommendation:** A reader cannot tell which text is binding. For each question, name the owner, decision point and fallback if unresolved; soften the plan's indicative statements to "subject to agreement with `neiro-core`"; and delete either the §6 decision or the corresponding open question.

### "root" denotes four unrelated concepts — Severity: Major

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 29, 50, 59, 63, 65, 123
* **Discrepancy Description:** "Root" is used for a navigation root route (29, 59), the destination of a stack pop (65), a modal presentation level (63), and a local music folder (123; "roots" at 50). The file sense collides with the navigation sense in the same document.
* **Rationale & Recommendation:** An implementer cannot tell which sense applies at line 123, and the sync non-goal at 50 is ambiguous about what is not synced. Use "navigation root" and "stack root" for navigation, and "music folder" or "source root" for file sources.

### "core", "engine" and "EngineHandle" are used interchangeably — Severity: Major

* **Reviewer Lens:** `egm-style`; `egm-design`
* **Affected Scope:** Lines 11, 15, 17, 25, 33, 99–101, 111–116, 130
* **Discrepancy Description:** `neiro_base` is called "a C++ audio core", "the audio core", "the core", "the engine" and "the core library". "Engine" also names the host-side wrapper, while line 116 "The engine gains a capability report" and line 130 "`neiro-core` gains a byte-range read path" leave unclear which side gains what. Line 15 "The host keeps one C ABI" is ambiguous (version or surface), and line 100 pins "one ABI version" without naming it. `EngineHandle` does not exist: `ui/src/engine.rs` exposes a concrete `Engine` struct and the host holds `Arc<Mutex<Option<Engine>>>` (`ui/src/lib.rs`), so "implements the same internal `EngineHandle` interface" conceals an unscoped refactor.
* **Rationale & Recommendation:** The engine boundary is a central decision; naming drift makes ownership of the new events and range path unreadable. Fix three terms once (`neiro_base` = core library, engine = the C ABI instance, `EngineHandle` = host wrapper), name the pinned ABI version, and name the real seam (`Engine::open`, `Exports::resolve`) plus what must change.

### "import" names the model the document rejects — Severity: Major

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 17, 33, 38, 120, 194
* **Discrepancy Description:** The local-file path is described as "one local-file import path" and "an explicit import model", but §5 and Alternative E settle on bookmarks to external folders and explicitly reject copying files ("Copy-import local files"). "Import" conventionally means copying into app storage.
* **Rationale & Recommendation:** The term contradicts the chosen architecture and points a reader at the rejected Alternative E. Use "folder attachment" or "bookmark registration", or define "import" once as registering a picked folder without copying.

### ASCII shell diagram is malformed — Severity: Nit

* **Reviewer Lens:** `egm-structural`; `egm-style`
* **Affected Scope:** Lines 67–78 (defect at line 76)
* **Discrepancy Description:** The folded pane is 29 columns wide (lines 69 and 77), but line 76 places 38 characters in its cell (the tab-label string is 29 characters, then gutter and sidebar), so the folded pane's right border is missing and the two panels merge.
* **Rationale & Recommendation:** The diagram is the primary shell reference. Use the 29-character label string with single spaces and pad to the pane interior.

### No hyperlinks for external artifacts and references — Severity: Nit

* **Reviewer Lens:** `egm-structural`
* **Affected Scope:** Whole document; notably lines 17, 101, 130, 135–136
* **Discrepancy Description:** The document references the `neiro-core` repository, NSH, Jellyfin, OPRA and per-server device IDs without a single link; no Markdown link exists anywhere in the file.
* **Rationale & Recommendation:** A fresh reader or AI session cannot check claims such as Jellyfin range behavior. Link `neiro-core`, the NSH iOS host documentation, and the Jellyfin API sections relied on.

### DSD non-goal carries an undefined exception — Severity: Nit

* **Reviewer Lens:** `egm-structural`
* **Affected Scope:** Line 48
* **Discrepancy Description:** "Native DSD output. DoP only if the route passes the carrier untouched; otherwise DSD is not offered on iOS." A non-goal that admits an exception on an undefined condition splits scope without a matching goal or open question; "carrier untouched" has no detection criterion.
* **Rationale & Recommendation:** State the route-descriptor check that would permit DoP and make it a conditional goal, or remove the exception and keep DSD fully out of the first release.

### Undefined internal terms and acronyms — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 23, 37, 43, 84, 92, 100, 116, 122, 132, 136, 140
* **Discrepancy Description:** `layout()` "gains a third value" without naming the existing values or saying whether `"unsupported"` is replaced. Used once without definition: "the Toaster", "the `-2` fallback", "single-owner role", "untouched route", "route model", "target seam"; "relative paths for display" never says relative to what. Unexpanded acronyms: OPRA, DoP, LRU, ATS.
* **Rationale & Recommendation:** Define or expand on first use, name the `layout()` values explicitly, and state the relative-path basis and refresh rule.

### Chrome record fields overlap without a stated authority — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 84–92
* **Discrepancy Description:** The record holds content insets, safe-area insets, mini-player height and a title-bar boolean. Three fields can describe the same bottom offset; none is marked authoritative or derived, and a boolean sits among dimensions.
* **Rationale & Recommendation:** State which fields are inputs and which are derived, and remove duplicates.

### Resource and protection requirements are unquantified — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 132, 140, 147
* **Discrepancy Description:** The stream cache gives no byte cap, eviction trigger, or relation to the 600-second reconcile loop. "Shutdown becomes idempotent and does not leak on timeout" states no mechanism or test. "File protection that background audio can read while the device is locked" has no verb, protection class, directory scope, or unreadable-file behavior.
* **Rationale & Recommendation:** State the cap and where it is configured, the timeout behavior (handle marked failed, output closed, reference dropped) with a test, and the protection class and scope (or mark each TBD in Open Questions).

### iOS meaning of `isStreamBitPerfect()` has no consumer — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Line 116 versus lines 25, 41
* **Discrepancy Description:** Line 116 defines an iOS return condition, then says the iOS surface does not draw the badge. Within the audited range nothing consumes the iOS value or says whether it can ever be true.
* **Rationale & Recommendation:** State which surface reads it in v1, or that the iOS branch is unused in the first release.

### Claims about the existing tree are cited inconsistently — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 23, 25, 135, 180, 203, 208
* **Discrepancy Description:** Line 23 cites `ui/src/engine.rs` and `scripts/fetch-core.sh`; line 135 asserts the hardcoded `Device="desktop"` literal with no file or symbol; line 180 appeals to "the review" without naming it; line 25 predicts either "a permanent failure or a green badge" without the branch producing each. Line 208 "shorter and better documented in Swift" is unverifiable and line 203 gives no basis for "can take seconds before the first sample".
* **Rationale & Recommendation:** Cite path/symbol for each claim about existing code, state which condition yields which outcome, and replace subjective comparisons with countable costs or measurements.

### Wordiness, abstraction and repetition — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 13, 17, 21, 29, 33, 41, 43, 116, 191
* **Discrepancy Description:** Line 33 is a roughly 70-word sentence with six semicolon clauses that do not map to the five numbered problems; line 13 stacks two "with" phrases. Abstractions requiring interpretation: "the strongest truthful claim", "Security-scoped bookmarks are the model", "not merely missing UI", "deliberately smaller than desktop parity". The desktop no-change rule appears three times (41, 43, 116), and the Now Playing description appears at 29 and 191.
* **Rationale & Recommendation:** Split line 33 into a list keyed to the five problems, replace abstractions with concrete statements, keep the desktop rule in G7 only, and reference it elsewhere.

### Component naming and hyphenation drift — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 11, 17, 41, 49, 89, 92, 111, 116
* **Discrepancy Description:** "Output console" versus "desktop mastering console"; "mini player" versus "mini-player height" versus "player bar"/"floating player bar", where line 92 then says both are the same component; "route format", "render format" and "format" are used as if interchangeable although they measure different things.
* **Rationale & Recommendation:** Use one name and one hyphenation per component, and keep route format distinct from render format.

### Cargo `cfg` shorthand is inaccurate — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Line 98
* **Discrepancy Description:** Cargo manifests have no `cfg` placement; the mechanism is a target-specific dependency table. "Moves `libloading` under `cfg(not(target_os = "ios"))`" describes Rust source attributes, not a manifest change.
* **Rationale & Recommendation:** Say "move `libloading` to `[target.'cfg(not(target_os = "ios"))'.dependencies]`".

### Mixed spelling convention — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Lines 29, 50, 65, 165, 190
* **Discrepancy Description:** British "-our" forms ("behaviour", "favourites") appear alongside American "-ization" ("Synchronization").
* **Rationale & Recommendation:** Pick one convention; with "-our" dominant, use "Synchronisation" or standardize the other way.

### "App Store rules" is an unattributed justification — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Line 47
* **Discrepancy Description:** "Runtime plugins and external UI shells on iOS (App Store rules; the plugin manager is compiled out)" names no guideline, and one parenthetical covers two items while explaining only one.
* **Rationale & Recommendation:** Name the rule and split the two items.

### Compiled-out shell switcher has no stated reason — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Line 103
* **Discrepancy Description:** "The plugin manager and shell switcher are compiled out on iOS rather than reporting unavailable plugins." The clause explains only the plugin manager; why the shell switcher is excluded, and what it is, is unstated.
* **Rationale & Recommendation:** State the reason or remove the shell switcher from the sentence.

### Section title adds a redundant qualifier — Severity: Nit

* **Reviewer Lens:** `egm-style`
* **Affected Scope:** Line 55
* **Discrepancy Description:** "Technical Plan (Architecture)" presents architecture as an optional qualifier of a plan and does not distinguish the section.
* **Rationale & Recommendation:** Use "Technical Plan" or "Architecture".

### "Downloaded" contradicts the no-download non-goal — Severity: Nit

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 13, 51, 61
* **Discrepancy Description:** The IA freezes a `Downloaded` Library category while the first release explicitly has no user-visible download state and the cache is "not a download manager".
* **Rationale & Recommendation:** Mark it as a later-release category or state what it lists.

### "Account avatar" implies an account that does not exist — Severity: Nit

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 13, 40
* **Discrepancy Description:** Neiro is local-only (no sync, no sign-in); there is no profile, yet Settings is said to live behind "the account avatar".
* **Rationale & Recommendation:** Call it a Settings control, or define what the avatar shows.

### Keychain migration has no source — Severity: Nit

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Line 146
* **Discrepancy Description:** A first iOS release has no prior `Storage` blobs and desktop has no Keychain, so "a one-time migration that deletes the old `Storage` blobs" only makes sense if an earlier iOS build shipped.
* **Rationale & Recommendation:** Scope the migration explicitly to a beta build or drop it.

### `openRoot` description is stale — Severity: Nit

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 29, 65
* **Discrepancy Description:** `openRoot` already keeps every root's stack alive and already pops the active root's detail pages when it is re-chosen (`ui/src/nav.ts`; commit `b89f2d3`). The actual change is not popping *other* stacks' detail pages, not adopting tab semantics from scratch.
* **Rationale & Recommendation:** Describe the current behavior accurately and name the delta, so a reader does not rebuild what exists.

### "No iOS target" is imprecise — Severity: Nit

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Line 23
* **Discrepancy Description:** Generated `ui/.nsh/rust/ios-host` and `ios-app` Cargo targets exist, and §7 relies on them. What is missing is an iOS-capable build of this crate that can link a core artifact.
* **Rationale & Recommendation:** State the actual gap, not the absence of any target.

### No minimum iOS version or verification matrix — Severity: Nit

* **Reviewer Lens:** `egm-design`
* **Affected Scope:** Lines 37, 140
* **Discrepancy Description:** G1 asserts build and launch, but the deployment target (needed for the privacy manifest, document picker and NSH iOS feature set), the simulator/device matrix, and how each goal is verified (interruption, route change, background kill, fold) are unstated in this range.
* **Rationale & Recommendation:** Add a short verification section with the deployment target and the test matrix, even if the implementation plan details it later.
