# neiro-base — the composition root

Base is the layer above the engine and the analyzer. It is not an audio engine
and it does not own a device, a clock, a sample or a measurement.

```
UI / remote client
    ↕  commands + events (serializable)
Base (Rust)
    ├─ library: favourites, playlists, play counts (SQLite)
    ├─ tags: read/write per container family, atomic replace
    ├─ transport: play/pause/seek/stop, queue, endpoint choice
    ├─ output policy: rate mode, resampler quality, share mode
    ├─ analysis scheduler + LRU cache
    ├─ render-path fact (`RenderPath`)
    └─ plugin boundary (planned)
         ├→ NAE_Session (C++): playback thread, stream, DSP, gapless
         └→ NAE_AnalyzeFile (C++): path → format/tags/DR/BPM/peaks
```

## Why the session, not `Stream`

`Stream` is the render primitive: `NAE_Play(ms)` blocks, so every caller becomes
an audio scheduler. `Session` already owns the playback thread, the stream, the
position, the queue and the DSP. Base uses `Session`; it must not build a second
render loop around `Stream`. The `Stream` wrapper remains in `neiro-audio` for
the harnesses and low-level probes.

## Tags belong to the file, not to a cache

A tag write replaces the file: the edit lands in a sibling temp file, the audio
stream is copied through untouched, and the temp file is renamed over the
original. The samples are not decoded or re-encoded, and a check decodes the
file before and after and compares the bytes.

Which families a file can carry is asked of the file (`writable_families`), not
assumed from its extension. `writable_keys` reports the fields a family can
hold, so a UI draws a form for this file rather than a generic one; ID3v1 has a
fixed eight-field block and reports those eight rather than an empty list.

`TagPolicy::id3v2_only_when_id3v1_overflows` is read both ways. Text that fits
ID3v1 gets an ID3v1 tag and no ID3v2 block. Text that does not fit -- over 30
bytes, or outside ISO-8859-1, which is every CJK title -- drops ID3v1 and is
written to ID3v2 alone. Writing ID3v1 truncated, or failing the edit because one
family cannot hold the text, are both worse than leaving that family out.

### Over the wire

| Command | Answer | Notes |
|---|---|---|
| `read_tags { path }` | `tags_read` | `families` is what the file carries now; `writable` is what its container can be written with. Two questions: a WAV may carry an ID3v2 chunk and still have no ID3v1 to write. |
| `write_tags { path, fields, policy }` | `tags_changed` / `error { op: "tags" }` | The policy has no default. Which families to write is the caller's decision, and a preset would make it here. |
| `list_tag_keys { families }` | `tag_keys_changed` | Empty asks for every family. The answer depends on the family and on no file, so it is asked once, not per selection. |
| `read_artwork { path, index? }` | `artwork_read` | Without an index: every picture's role, mime and size, no bytes. With one: that picture's bytes, base64. |
| `set_artwork { path, family, index?, data, mime?, kind?, description? }` | `artwork_changed` | `index` replaces a picture in that family's list; absent appends. |
| `remove_artwork { path, family, index }` | `artwork_changed` | The count must fall by one; the re-read is the assertion. |

Every artwork command names a family, because pictures live in one. A file can
carry the same cover in ID3v2 and in APE, each with its own list, so an index
without a family addresses nothing — the flattened listing is for display, and
each entry carries its family so the caller can address it.

A family that is eight fixed text fields (ID3v1) or text chunks (RIFF INFO,
AIFF text) has nowhere to put a picture. That is a different question from
whether the container is writable, and it is asked separately: offering "add a
cover" for a container that cannot hold one is a promise the file cannot keep.

The two modes of `read_artwork` are separate because they run at different
frequencies. A track change asks "is there a cover, and how big" and must not
move a hundred kilobytes of JPEG through a JSON string; a view that has to draw
one asks for it by index. A listing that carried the bytes would make the cheap
question as expensive as the rare one.

`mime` comes from the tag and may be absent. A file that does not say what its
picture is has not told us, and sniffing the first bytes here would be a second
opinion the file never gave.

`tags_read` carries every field with the family it came from, because two
families in one file can disagree about the same key: an MP3 whose ID3v2 says
2019 and whose APEv2 says 2018 is a real file. Flattening them would have to
pick a winner, and that winner would be a value nobody owns.

A field on the wire carries two names, and they are not interchangeable:

| Field | Example | Purpose |
|---|---|---|
| `key` | `TrackTitle` | The identity: the `ItemKey` variant name. Stable across formats, and the only thing a write is resolved by. |
| `name` | `TIT2`, `TITLE`, `Title` | How that family spells the field. Display only. |

The identity exists because a family spells several keys the same way. ID3v2
puts the track number and the track total in one `TRCK` frame; `TIPL` holds
producer, engineer and five other roles under one name; APE calls both
`TrackNumber` and `TrackTotal` "Track"; MP4 calls `Lyrics` and `UnsyncLyrics`
both `©lyr`. Resolving a write by the display name would pick one of them and
write the wrong field — silently, since the write would still succeed.

A name no family maps, and a value that is not text (an embedded picture), are
refused before the file is opened: a write that dropped a field would report
success for an edit that did not happen.

A policy with `clear` and an empty `write` erases those families and writes
nothing — Picard's "remove ID3 tags from FLAC files". It is a real edit, and it
goes through the same copy-and-rename; the saved file is read back to confirm
each named family is gone, so a backend that left the bytes in place cannot be
reported as a success. Erasing goes through an *empty* tag rather than a
removal: the backend does not write empty tags, and dropping the tag from its
list leaves the original bytes untouched — measured, not assumed.

## Local folders

| Command | Answer | Notes |
|---|---|---|
| `scan_folder { root }` | `scan_started`, then `scan_done` | A walk compared against the index, not a list of the tree. |
| `watch_folder { root }` | `folder_watched { roots }` | Recursive. Watching one twice is not an error. |
| `unwatch_folder { root }` | `folder_unwatched { roots }` | |
| `set_reconcile_interval { seconds }` | `reconcile_changed` | 0 turns it off. |
| — | `files_changed { changes }` | Coalesced per worker tick. |

This layer used to say it did not scan, on the grounds that a host supplying
paths kept Base from growing into a media library. A folder the listener added
is a fact about the music, not about the host, and two hosts would disagree
about it — so it lives here, next to the favourites that name the same files.

### A scan is a difference, not a listing

`scan_done` carries `added`, `modified`, `removed` and `moved`. The whole tree
would make every launch a re-import, which is worse than not scanning at all.

The index is `files(path, root, size, mtime_ms)` in the same SQLite file as the
favourites. It belongs there for two reasons: it is the only way to answer "what
left?" when a directory is removed, and it is what makes the comparison cheap.
It is derived — every row can be rebuilt by walking — so a file that disappears
loses its row and keeps its star.

### A move is detected, not guessed

Within one reconciliation, a removed file and an added file that share a size
and a timestamp are the same file under a new name. Base then rewrites the path
in `tracks` and `playlist_entries`, so the star, the play count and the playlist
membership follow the file, and it carries the analysis cache entry over.

That last part is not an optimisation detail. The analysis key is
path + size + mtime + analyzer version, and a rename changes only the first:
without the carry-over, moving an album costs one decode per file and produces
the same numbers as before. `folder_probe` measures it — first analysis is a
miss, the move is detected, the second analysis is a hit.

The host gets `moved { from, to }` and moves whatever else it caches by path.

Two candidates on either side of the pairing means ambiguity, and ambiguity is
not resolved by picking one: a wrong pairing hands one file another's history,
while a missed pairing only costs what not detecting moves cost. Uniqueness of
the fingerprint is not enough either — two rips of one master can share a size
and a timestamp — so a pairing also requires the file name or the directory to be
unchanged. A rename that changes both is reported as a removal and an addition,
which is the same loss the old behaviour had and never a wrong star.

The index compares paths the way the platform does: ASCII case-folded. Two
spellings of one share are one share. Before that rule existed, a host that
reached the library as `\\server\Music` after having reached it as
`\\SERVER\Music` re-imported all 3,585 files and then held two rows for each. If the
destination is already known to the library the move is refused outright and
reported, rather than merging two files.

This is why the crate needs no persistent id column. Navidrome needs one because
its pipeline stages run across separate scans; here both halves of the pairing
are in one diff, and the pairing rule is the identity.

### A scheduled reconciliation is the only thing that sees a silent loss

A watcher reports the failures it is told about. A network share loses events
without telling anyone: no error, no gap, just a library that drifts. The only
mechanism that can see that is walking the tree again, and on the library this
was measured against it costs six seconds for 3,585 files over SMB.

`set_reconcile_interval` runs the same `scan_folder` a host sends, through the
same comparison, so there is one place a diff can come from. It is off until the
host asks for it, because the interval is a policy decision and this layer does
not make those.

A round that could not stat part of the tree does not report removals. A file
that could not be read looks exactly like a file that was deleted, and reporting
it as gone is the one answer the source did not give.

### The watcher never implies it knows

`files_changed` carries `rescan` when the watcher cannot name what changed: a
directory was removed (its contents were never enumerated), or the backend
reported lost events — a full buffer, a handle that stopped working. Silence
would be indistinguishable from "nothing changed", which is the one thing a
watcher must not imply when it does not know.

### A write from this process is not an import

`tag.rs` replaces a file by staging a copy beside it and renaming over it, which
the watcher sees as a file appearing. The control thread therefore marks the path
it is about to write, and the watcher consults the same set; the mark survives
two seconds past the write because the backend delivers the rename late. Jellyfin
needs a 45-second heuristic for the same problem because its writers are other
processes; here the writer is in this one, so the window is exact.

The rule that came out of the same measurement: a leading dot is skipped
everywhere — walking, watching, listing. That removes the tag writer's staged
copy and macOS's `._track.flac` resource forks, which have audio extensions and
are not audio at all.

### What is deliberately not here

- **No scheduled full scan.** It guards against network mounts dropping events,
  which is a server's problem: the engine only plays local paths, and local
  events are pushed by the kernel. The real gap — changes while the process was
  closed — is what the launch reconciliation covers.
- **No percentage progress.** `scan_started` and `scan_done` only. A library
  big enough that the silence hurts is big enough to deserve moving the walk off
  the worker, and a progress bar would hide that.
- **No second thread for watching.** notify drains the kernel buffer on its own
  thread, so a slow scan only grows the in-process queue; it does not lose
  events. Splitting would buy latency during a scan and cost an interleaving
  problem between two event sources.
- **Roots are not in SQLite.** SQLite holds derived data that can be rebuilt; a
  root is a user setting, and it belongs with the other settings where the host
  already keeps the output device. Base only has to make `watch_folder`
  idempotent, which it is.

`cargo run --bin folder_probe` builds a tree, scans it twice (the second diff
must be empty), renames a file (one move, not a removal and an addition), watches
it, creates and deletes files, removes a directory, writes a tag and asserts the
write is *not* reported as a file change, and unwatches.

## Remote libraries (Jellyfin and Navidrome)

Two protocols, one shape. `set_remote` takes a tagged server, `list_remote`
returns tracks either way, `fetch_remote` downloads and hands back a local path.

| | Jellyfin | Navidrome (Subsonic) |
|---|---|---|
| Auth | token or password sign-in, both ending in a token header | per-request salt, `t = md5(password + salt)` |
| Listing | `GET /Items?IncludeItemTypes=Audio` | `GET /rest/search3.view?query=&songCount=` |
| Download | `GET /Items/{id}/Download` | `GET /rest/stream.view?id=&format=raw` |
| Extension | `MediaSources[0].Container` | `song.suffix` |
| Cover | `GET /Items/{id}/Images/Primary?maxHeight=600` | `GET /rest/getCoverArt.view?id=&size=600` |
| Playlists | `GET /Items?IncludeItemTypes=Playlist` | `GET /rest/getPlaylists.view` |
| Entries | `GET /Items?ParentId={id}` | `GET /rest/getPlaylist.view?id=` |

Covers are cached in two layers. Memory answers the second look within one run;
a directory under `cache_dir/artwork/<server>/` answers the *first* look after a
restart, which is the one that costs the listener bandwidth and makes a first
browse feel slow.

The directory is keyed by server because a cover id is only meaningful on the
server that issued it — two servers can both have `cover-1` and mean different
pictures. Configuring a server drops the memory layer and keeps the directory,
so a host that re-sends `set_remote` on every launch keeps its cache; that is
the common case, and wiping on configure would make the persistence useless.

Each file is named `<cover id>.<mime with / replaced by _>`, so the type
survives with the bytes without a sidecar or a second index. The directory is
bounded at 512 files, oldest by modification time first.

A cover that the server does not have is `remote_artwork` with no `data`, not an
error: a track without a picture is a normal track. Jellyfin answers 404 for it,
Subsonic answers a JSON error inside a 200 — the same trap the stream endpoint
has, handled the same way.

Jellyfin takes either a key minted in the dashboard or a username and password.
The password is exchanged for an access token through
`/Users/AuthenticateByName` when the server is configured, and replaced in
place: nothing below that point holds a password, and a wrong one fails at
connect time instead of on the first listing.

A download is as big as the track, and the HTTP client caps a response body at
10 MiB by default — a perfectly ordinary 40 MB FLAC came back as "the response
body is larger than request limit" against a real server. The cap is one gibibyte
here: far above any single track, still a bound.

`format=raw` on the Subsonic stream is not decoration. Without it the server may
transcode to something the client advertises, the download still succeeds, and
every bit-perfect claim about that track is false. `maxBitRate=0` goes with it
for the same reason.

A Subsonic server reports a refused stream as a JSON error document inside an
**HTTP 200**. Reading that as audio writes the error into the cache under a
`.flac` name and calls it a download, so the content type is checked before the
bytes are believed.

The MD5 is pinned against the protocol's own example (`md5("sesame" + "c19b2d")
= 26719a1196d2a940705a59634eb18eab`) rather than against this crate's output: a hash that
disagreed with every other client would still be a stable string here.


A remote track is downloaded into a local cache and then played as a file.
Streaming was rejected on purpose: the engine decodes from paths, its DSD path
opens a file, its analyser decodes a file, and the library keys favourites and
playlists on paths. A URL would decode fine and quietly fail everywhere else —
DSD would fall back to the generic decoder, the analysis cache would lose its
key (a URL has no size and no mtime), and the render path would still claim
bit-perfect about a stream nobody measured.

| Command | Answer | Notes |
|---|---|---|
| `set_remote { base_url, token, cache_dir }` | `remote_changed` | An empty token disconnects. The event never carries the token. |
| `list_remote { start, limit }` | `remote_listed` | `limit` defaults to 200: a page, not the library. |
| `fetch_remote { id }` | `remote_fetched { track, path }` | `path` goes to `play`. |

The cache name is derived from the item id and the container, so it is the same
on every run — a path that moved would orphan the favourite and the playlist
entry that name it. A download lands as `<id>.<ext>.part` and is renamed only
when the transfer finished and the length agrees with what the server announced;
a partial file carrying the final name would be played as if it were whole.

`fetch_remote` resolves its id against the last listing. A guessed id would put
a file in the cache under a name the caller never saw.

One worker, not a pool: a download saturates the link and a listing is one
request, so a second concurrent transfer would only halve the first.

The client speaks three endpoints — `/Items`, `/Items/{id}/Download` — and the
token goes in both `Authorization: MediaBrowser Token="..."` and
`X-Emby-Token`, because Jellyfin has accepted one or the other across versions
and the server decides which is good.

`cargo run --bin jellyfin_probe` starts a stub server on loopback and exercises
all of it, including the refusals. What it cannot prove is that Jellyfin's real
answers match the stub; that needs a server.

## The render path is an engine fact

`NAE_SessionState` is the single source of truth for what is actually in the
path. Base copies it into `RenderPath`:

| Field | Meaning |
|---|---|
| `resampling` / `dithered` | what the converter and requantiser actually did |
| `bit_perfect` / `reason` | the engine's honest claim, including EQ/crossfeed/convolver/crossfade/ReplayGain/volume |
| `eq_enabled` | the applied cascade, not the requested toggle |
| `crossfeed_enabled` | applied only when the stream is stereo |
| `convolver_enabled` | the IR actually loaded |
| `crossfade_enabled` | active crossfade; zero duration is gapless |
| `replay_gain_enabled` / `rg_applied_gain` | policy and resolved linear gain |
| `volume` | current linear target |

The UI must display this. It must not display "EQ on" because the user set EQ;
a flat EQ, a failed convolver, a resampler with equal rates and a zero-duration
crossfade are all cases where the requested stage and the active stage differ.

## Commands and events

Both are `serde` types in `src/protocol.rs`; the in-process UI calls
`BaseHandle`, and a future out-of-process UI can carry the same values over a
socket.

Commands: `play`, `enqueue`, `pause`, `resume`, `stop`, `seek`, `next`,
`analyze`, `set_volume`, `set_eq`, `set_crossfeed`, `set_replay_gain`,
`set_convolver`, `set_crossfade`, `set_policy`, `list_devices`, `set_device`,
`remove_from_queue`, `clear_queue`, `shutdown`, and the library commands
`set_favourite`, `list_favourites`, `create_playlist`, `rename_playlist`,
`delete_playlist`, `list_playlists`, `add_to_playlist`, `remove_from_playlist`,
`list_playlist_entries`, `list_playlists_containing`, `record_play`,
`list_most_played`, `preanalyze`.

`play` replaces the queue by design: it is the "start this track" command.
Choosing an endpoint is `set_device`, which deliberately does not touch the
queue -- a device menu that discarded what the listener queued would be the
wrong shape.

`snapshot().track_serial` increments inside one session each time a preloaded
track takes over. A UI needs it to tell a gapless or manual seam from a new
stream: a seam keeps the session and the counter moves, while a close and reopen
starts over at 0 -- and every other field looks the same.

`settings` in the snapshot is the applied configuration read back from the
session (`EqParams`, `CrossfeedParams`, `ConvolverParams`, `CrossfadeParams`,
`ReplayGainParams`, volume). `render_path` is what is actually active on the
stream. They are deliberately separate: a configured stage may be bypassed on
this source, and the UI must not merge the two.

Events: `state`, `track_changed`, `position`, `analysis_ready`,
`render_path_changed`, `settings_changed`, `policy_changed`,
`queue_changed`, `error`, `shutdown`.

`snapshot()` is the state a late subscriber paints from. Events are changes; the
snapshot is the whole. `Snapshot.resolved` is the engine's actual resolved path
and diagnostics JSON for the current track; it is refreshed on open and on a
gapless track change, not derived by Base.

## Library

Favourites, playlists and play counts live here rather than in a host. A host
that kept its own copy would eventually disagree with the queue -- a UI showing
"20 in this playlist" while the engine plays from a three-entry queue is two
answers to one question, which is what AGENTS.md rule 1 is about.

What this does not do: it does not scan directories, watch them for changes or
own a file. A path is a fact the host supplies; the library remembers what the
listener said about it.

Storage is SQLite, compiled into the crate (`bundled`). A playlist is a
many-to-many relation with an order, and "which playlists contain this track" is
asked on every selection -- that is a join, not a scan. A JSON file would rewrite
the whole library on every favourite toggle, which is fine at a hundred tracks
and not at ten thousand.

| Operation | Notes |
|---|---|
| `set_favourite` / `is_favourite` / `favourites` | returns whether it changed, so a UI can skip an event nobody needs |
| `record_play` / `play_count` / `most_played` | the clock is the host's; Base does not own one |
| `create_playlist` / `rename_playlist` / `delete_playlist` / `playlists` | names are unique case-insensitively, enforced by the schema's index rather than by a check in front of it |
| `add_to_playlist` / `remove_from_playlist` / `playlist_entries` | adding a track twice is a no-op; a removal closes the position gap |
| `move_in_queue` | a move is a remove plus an insert; the index follows the playing entry |
| `playlists_containing` | the reverse query a UI asks on selection |
| `record_play` / `list_most_played` | the clock is the host's; a play republishes the list the UI last asked for |

Every one of these is on the wire as a command, and answers as an event
(`favourites_changed`, `playlists_changed`, `playlist_entries_changed`,
`playlists_containing_changed`) rather than as a return value: the C ABI is
asynchronous, so a host that expected a synchronous answer would block a thread
waiting for something that already arrived.

A library is opened with `Base::start_with_policy_and_library` in Rust or
`NAE_BaseStartWithLibrary` in C. Without one, every library command reports
"no library is open" instead of returning an empty list -- "you have no playlists"
is a statement about the listener's music, not about this process.

Positions inside a playlist stay dense: the UI addresses entries by index, and a
hole would make `entry[3]` and `position 3` disagree. Deleting a playlist takes
its entries with it through the schema's cascade.

### The browsing index

The library also holds what a list needs to draw itself: title, artist, album,
album artist, disc and track number, year, genre, duration, sample rate, bit
depth and channel count. These are a cache of the file's own tags, read with one
open, and never written back to the file.

Why it exists: an album view that read tags per row would open and parse every
file on every launch. Ten thousand tracks is ten thousand opens before the first
row appears. The index turns that into one query.

| Command | Event | Notes |
|---|---|---|
| `list_tracks { offset, limit }` | `tracks_listed` | album/artist/disc/track order, paged |
| `list_albums` | `albums_listed` | one row per album with its credited artist and count |
| `list_artists` | `artists_listed` | track and album counts per artist |
| `search { query, limit }` | `search_results` | substring over title, artist, album, album artist |
| `index_tags { paths }` | `tags_indexed` | read these files now; a scan does it automatically |

Who fills it: the scan, after each walk, for the files it added or modified; and
a tag write, which drops its row and asks for it again so the next list shows the
new title. The worker is one thread, because this is a header read and two of
them would make a spinning disk seek between them.

Search is `LIKE`, not FTS5: the bundled SQLite may or may not carry the FTS5
module, and a search that silently returns nothing when it does not is worse than
one table scan. A host that searches as the listener types should debounce.

What a query is matched against is a `search_text` column folded in Rust, not the
raw fields: SQLite's `LIKE` folds ASCII only, so `bela` would never find `Béla`.
`%` and `_` in the query are escaped, not treated as wildcards.

A page is capped at 500 rows. The answer is one JSON event on the control
thread's channel, and ten thousand rows is megabytes of it; a host that wants
everything pages through. When Base was started without a library location the
four browse queries report an error rather than an empty list, because a host
cannot tell a misconfigured start from a library with nothing in it.

## Output policy

`PlaybackPolicy` is the one description of how a stream is opened:

| Field | Values | Default |
|---|---|---|
| `rate` | `native`, `device`, `fixed { rate_hz }` | `device` |
| `quality` | `fast`, `high`, `very_high` | `high` |
| `share` | `exclusive`, `shared` | `exclusive` |

These are open-time settings in the engine, so `set_policy` on a live session
reopens it at the current position, and while paused it reopens it paused. A
policy change that only updated a field would leave the old stream resampling
while the UI reported "native", which is exactly the disagreement the render
path exists to prevent.

`set_policy` is a request, not a guarantee. The endpoint decides: `fixed` may
be refused, and the engine reports the rate it granted plus a coercion reason in
`render_path`. `policy` in the snapshot is what was asked for; `render_path` is
what happened. Both are published because they can differ.

`Shared` is never chosen automatically. It exists for an environment where
another process holds the endpoint, and `Base::start_with_share` / the
`NEIRO_BASE_SHARE` probe input are the only ways to ask for it.

## Analysis

`play` and `enqueue` request analysis for the current and next track. The
workers decode on their own threads, write an LRU cache keyed by path + size +
mtime + `analyzerVersion`, and emit `analysis_ready`. The UI never decodes a
file and never writes the cache.

More than one worker, because the engine's analyzer is per-call state:
`computePeaksFromFile` builds its own decoder, its error text is `thread_local`,
and nothing in that path touches an FFmpeg global initialiser. A single worker
made a skip wait behind the prefetch of the track it was skipping to. The count
is `min(cores, 4)` — each decode holds a decoder and its buffers, so this is not
one thread per request — and `NEIRO_BASE_ANALYSIS_WORKERS` overrides it.

Requests for a path already being decoded are dropped, not queued: the same path
is requested twice in normal use, once as the current track and once as the next
track's prefetch, and one decode answers both through the single
`analysis_ready` event.

A track change cancels what it supersedes, at both ends. The engine's decoder
polls a flag the caller owns (`NAE_AnalyzeFileCancelable`), so a decode that is
already running stops between blocks; a request still sitting in the channel is
dropped when a worker picks it up, because the wanted set moved on without it.
A cancelled decode publishes nothing and caches nothing -- its result is a
prefix, not a measurement. That is also how shutdown avoids waiting for a file
nobody will read.

`bpm_valid == false` means the onset envelope was too short or the
autocorrelation peak too weak. The UI must show nothing for BPM, not a plausible
number.

## Plugin boundary

No loader is implemented yet. The intended split is deliberate:

- **Analysis / metadata / source plugins** are offline and may live in Base or
  in a sandboxed worker; they enrich facts for a path.
- **DSP plugins** execute inside the engine/session audio path, not in Base.
  Base only registers, configures and enables them. A plugin on the render path
  obeys the real-time rules: no allocation, no locks, no blocking.
- **UI plugins** run in the UI, not in Base.

A plugin system before the command/event contract is stable would freeze the
internal implementation. The contract above is the prerequisite.

## Run the probe

```powershell
cd engine
cmake --build build
cd base
$env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
cargo run --bin base_probe
cargo run --bin base_capi_check
cargo run --bin library_check
cargo run --bin base_tag_probe -- <file>
cargo run --bin tag_check -- <file>
```

The header has one check that does not go through Rust at all:

```powershell
powershell -ExecutionPolicy Bypass -File test/run-c-check.ps1
```

It compiles `test/nae_base_c_check.c` against `include/nae_base.h` with the
MinGW `cc` and calls every export the header declares, from a C host. That is
the only check that sees the header as a consumer does: the Rust probes share
the serialized types with the crate, so a type that failed to serialize would
fail to compile on both sides at once and never reach the wire. The script
copies `nae.dll` beside the executable, because the crate links the engine and
Windows resolves that import before `main` runs.

What it pinned down that the Rust checks could not:

| Behaviour | Why it matters |
|---|---|
| `NAE_BaseStartWithLibrary("")` must return NULL | SQLite reads an empty filename as a private temporary database. Accepting it handed back a library that vanished on exit, so a host with an uninitialised settings variable would lose the listener's playlists and never see an error. |
| `NAE_BaseStartWithShare(2)` must return NULL | A mode the header does not define is the caller's mistake. Defaulting it would take the endpoint away from another process. |
| a second `NAE_BaseSubscribe` is `-4` | Otherwise a host that opens two windows gets one callback thread that reports the same event twice. |
| `NAE_BaseFreeString(NULL)` and `NAE_BaseShutdown(NULL)` are safe | Both take a NULL by contract; the check asserts the live handle still works afterwards, which is the consequence that matters. |

`base_tag_probe` drives the tag commands through the protocol and needs one
audio file it may modify (it works on a copy). `tag_check` decodes the file
before and after the write and compares the samples, so it needs `ffmpeg` on
PATH.

The library layer pulls in `rusqlite` with the `bundled` feature, so cc-rs
compiles SQLite from source. cc-rs looks for `gcc` by name and this machine's
MinGW ships `cc`/`c++` only, so `base/.cargo/config.toml` names them -- that file
holds absolute paths and is therefore **not committed**. A machine with `gcc`
on PATH needs nothing; otherwise set `CC` and `CXX` in the environment. The CRT
matters: `nae.dll` is built with `-mcrtdll=ucrt`, and a bundled SQLite on a
different runtime would be two allocators in one process.

`NEIRO_BASE_DEVICE=<endpoint id>` selects an endpoint; `NEIRO_BASE_SHARE=shared`
chooses shared output for that probe. Both are explicit test inputs, not
production fallbacks.

It generates two 12 s, 120 BPM click WAVs, plays A, enqueues B, checks the
render path, waits for A's analysis (BPM 120.07, confidence 0.833), verifies B
is a cache hit, exercises volume/EQ/crossfeed/crossfade/ReplayGain, then pauses,
resumes, seeks, skips to B and stops. It needs a real output endpoint.

## C ABI for Swift

`include/nae_base.h` exposes the serialized protocol. The Rust side is in
`src/capi.rs`; `base_capi_check` exercises start, snapshot, JSON command,
callback, string ownership and shutdown.

A Swift 6 client should own one actor and never touch UI state from the callback
thread:

```swift
actor BaseClient {
    private let raw: OpaquePointer

    init?() {
        guard let raw = NAE_BaseStart() else { return nil }
        self.raw = raw
    }

    func send(_ json: String) {
        json.withCString { _ = NAE_BaseCommand(raw, $0) }
    }

    func snapshot() -> Snapshot? {
        guard let p = NAE_BaseSnapshot(raw) else { return nil }
        defer { NAE_BaseFreeString(p) }
        return try? JSONDecoder().decode(Snapshot.self, from: Data(bytes: p, count: strlen(p)))
    }
}
```

The C callback is a plain function pointer with a `void *user`; pass an
`Unmanaged<BaseClient>` and dispatch to the actor. Event JSON is the same
`Event` type the Rust UI would consume.

## iOS and Swift 6

Base is intended to stay in Rust on iOS too: it cross-compiles to a static
library (`libneiro_base.a`), which is allowed on the App Store, and the same
Rust core can serve the Windows and iOS builds. The Swift 6 UI talks to it
through the C ABI above; the UI owns `AVAudioSession`, interruptions, route
changes, background audio and Now Playing, because those are platform lifecycle,
not playback policy.

The engine is the part that is not yet cross-platform. `nae.dll` is a Windows
dynamic library and the device layer is WASAPI; an iOS build needs a static
CoreAudio/RemoteIO backend behind the same C ABI. Base's session wrapper does
not change for that port, because it only sees the C ABI.

The plugin model must be static on iOS. App Store rules do not allow downloading
or `dlopen`-ing executable code. Analysis plugins are compiled Rust crates and
DSP plugins are compiled into the engine; the desktop build can offer a dynamic
loader, but the iOS build must share the same static registry. Do not design a
runtime plugin ABI that only works on desktop.

## Limits

- Manual `next` crosses the same seam gapless uses, so the stream, the
  negotiated format and the resampler phase are kept. It falls back to closing
  and reopening when the engine refuses the seam -- an empty queue, or a next
  track whose rate or channel count differs from the current one, which is
  exactly the condition gapless requires.
- The cache is a bounded LRU that starts at 256 entries and grows to hold a
  pre-analysis batch, never shrinking. A scan of 5000 tracks against a 256-entry
  cache would evict its own early results as it produced them.
- Pre-analysis takes an explicit path list: Base does not scan directories, so a
  host that wants to analyze a library supplies the paths (from its own index, or
  from favourites and playlists). Progress arrives as `preanalysis_progress`
  with `done`/`total`, and `complete` on the last event.
- Cancellation is between decoded blocks, so a decode stops within one packet
  rather than instantly. It is on the wire: `analyze` takes an optional
  `cancel_id`, and `cancel_analysis` stops the decode that carries it. A
  cancelled decode publishes nothing.
- The analysis pool can be resized at runtime with `set_analysis_workers`.
  Growing starts threads immediately; shrinking moves the target and each
  worker past it exits when it next becomes idle -- a worker never exits while
  holding a request, because the request is already off the channel and would
  be lost. A slot is tracked in a shared set so a shrink followed by a grow
  within the worker's poll window does not start two threads on one index.
- A worker that drops a request because the wanted set moved releases its
  in-flight slot through `analysis::release`. That is not covered by a check:
  the window between the two `keep` checks is a filesystem stat and a hash
  lookup, and five timed attempts to land in it all missed -- a probabilistic
  test that never fired is a test that cannot fail, so there is none.
- The most-played list is refreshed after a play only if one is being watched,
  and at the limit of the last `list_most_played`. A host that never asked for
  one gets no event, which is the point: an event nobody subscribed to is work
  the UI cannot use.
- No plugin loader. The out-of-process transport exists:
  `neiro_base::transport` serves one client at a time over a named pipe, one
  JSON object per line, commands in and events out on the same connection.
- The transport is Windows-only and single-client by design: the control thread
  owns the session, and two clients issuing commands would be two owners of one
  queue.
- The transport's reads are bounded to what the pipe already holds, so a client
  that sends bytes without a newline cannot stall the server; a line past 4 KiB
  is dropped and reported once rather than buffered; more than 256 queued
  commands are dropped with an error rather than growing without limit; and
  `Transport::stop` returns while a client is connected, because the serve loop
  checks the flag instead of waiting for a disconnect.
- `set_policy` reopens the stream, which costs one device close and open. There
  is no crossfade or click suppression across that boundary; a UI should
  debounce a slider rather than send every intermediate value.
- `set_device` on a live stream reopens on the new endpoint at the current
  position. The reopen publishes `Ready` at the resumed position before it
  starts the new stream, which is what makes the resume assertable: the check
  compares against the position it left, within 0.10 s, and fails with
  `1.539s -> 0.000s` when the seek is removed.
- Queue editing is move, remove and clear. Removing an entry
  rebuilds the engine's queue from the current index forward, so the engine seams
  to what the UI shows. The entry that is playing cannot be removed -- `Stop` first.
- The queue rebuild is checked against the engine's own count, not Base's copy:
  `engine_queue_count` in the snapshot is what the session reports holding, and
  it is compared with the edited queue. Dropping the rebuild reports
  `engine Some(3), expected 1`.
- The C ABI has one subscription at a time per `NAE_Base`; `NAE_BaseUnsubscribe`
  drops it and lets the same handle subscribe again, which is what a host with
  several windows needs. Two simultaneous subscribers still require two handles.
- iOS is not built or verified here. `nae.dll` is Windows-only; an iOS build
  needs the engine's CoreAudio backend and a static link first.
