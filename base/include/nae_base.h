#ifndef NAE_BASE_H
#define NAE_BASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Base is the composition root above the audio session and the analyzer.
 * The Rust API is the implementation; this header is the JSON bridge a Swift 6
 * UI uses.
 *
 * Ownership: strings returned by NAE_BaseSnapshot belong to Base and must be
 * released with NAE_BaseFreeString. Callbacks run on a dedicated Rust thread;
 * a Swift client must hop to its actor before touching UI state.
 *
 * Commands and events are the serialized types in `base/src/protocol.rs`:
 *   {"command":"play","path":"...","device_id":null}
 *   {"command":"set_policy","policy":{"rate":{"mode":"fixed","rate_hz":96000},
 *    "quality":"very_high","share":"shared"}}
 *   {"command":"set_favourite","path":"...","on":true}
 *   {"command":"create_playlist","name":"Evening"}
 *   {"event":"state","state":"playing",...}
 *
 * Library commands: set_favourite, list_favourites, create_playlist,
 * rename_playlist, delete_playlist, list_playlists, add_to_playlist,
 * remove_from_playlist, list_playlist_entries, list_playlists_containing,
 * record_play, list_most_played, preanalyze, move_in_queue, cancel_analysis,
 * set_analysis_workers.
 *
 * Library events: favourites_changed, playlists_changed,
 * playlist_entries_changed, playlists_containing_changed, most_played_changed,
 * preanalysis_progress, analysis_workers_changed.
 *
 * Tag commands:
 *   {"command":"read_tags","path":"..."}
 *   {"command":"write_tags","path":"...","fields":[
 *      {"family":"id3v2","key":"TrackTitle","name":"TIT2","value":"Salt"}],
 *    "policy":{"write":["id3v2"],"clear":[],"id3v2":"utf8"}}
 *   {"command":"list_tag_keys"}                      // every family
 *   {"command":"list_tag_keys","families":["id3v2"]}  // one family
 *   {"command":"read_artwork","path":"..."}           // list, no bytes
 *   {"command":"read_artwork","path":"...","index":0} // that picture, base64
 *   {"command":"set_artwork","path":"...","family":"id3v2","data":"<base64>",
 *    "mime":"image/jpeg","kind":"CoverFront"}         // appends without index
 *   {"command":"set_artwork","path":"...","family":"id3v2","index":0,...}
 *   {"command":"remove_artwork","path":"...","family":"id3v2","index":0}
 *
 * Tag events: tags_read, tags_changed, tag_keys_changed, artwork_read,
 * artwork_changed, and error with op "tags" or "artwork".
 *
 * Remote commands (Jellyfin and Navidrome):
 *   {"command":"set_remote","server":{"kind":"jellyfin",
 *      "base_url":"http://host:8096",
 *      "auth":{"kind":"token","token":"..."}},        // or:
 *    "cache_dir":"C:/Users/me/AppData/Local/neiro/remote"}
 *   {"command":"set_remote","server":{"kind":"jellyfin",
 *      "base_url":"http://host:8096",
 *      "auth":{"kind":"password","user":"char","password":"..."}},
 *    "cache_dir":"..."}
 *   {"command":"set_remote","server":{"kind":"navidrome",
 *      "base_url":"http://host:4533","user":"joe","password":"..."},
 *    "cache_dir":"..."}
 *   {"command":"list_remote","start":0,"limit":200}
 *   {"command":"fetch_remote","id":"<item id>"}
 *   {"command":"fetch_remote_artwork","id":"<item id>"}
 *   {"command":"list_remote_playlists"}
 *   {"command":"list_remote_playlist_items","id":"<playlist id>","start":0,"limit":200}
 *
 * `"server": null` disconnects. The two kinds are different protocols, not two
 * names for one: Jellyfin takes a token header, Subsonic signs every request
 * with a fresh salt and md5(password + salt).
 *
 * Jellyfin's password form is exchanged for an access token once, when the
 * server is configured; nothing below that point holds a password, and a wrong
 * one fails at connect time rather than on the first listing.
 *
 * The Navidrome download asks for `format=raw`, which is the only way Subsonic
 * returns the file as stored. Without it the server may transcode, and every
 * bit-perfect claim about the track becomes false while the download still
 * succeeds. A refused stream comes back as a JSON error inside an HTTP 200, so
 * the content type is checked before the bytes are believed.
 *
 * Remote events: remote_changed, remote_listed, remote_fetched, remote_artwork,
 * remote_playlists, remote_playlist_items, and error with op "remote".
 *
 * `remote_artwork` without `data` means the server has no picture for that
 * track, which is a normal answer; an error means the request failed. The two
 * are different and a host should not draw them the same way.
 *
 * Covers are kept under `cache_dir/artwork/<server>/`, so a restart does not
 * download them again. The directory is bounded and keyed by server; changing
 * the server does not clear it.
 *
 * Both backends answer playlists in the same shape. Jellyfin has no playlist
 * collection of its own -- the entries are asked for with `ParentId` -- and
 * Subsonic has `getPlaylists` / `getPlaylist`; neither difference reaches the
 * host.
 *
 * Local folders:
 *   {"command":"scan_folder","root":"D:/Music"}
 *   {"command":"watch_folder","root":"D:/Music"}
 *   {"command":"unwatch_folder","root":"D:/Music"}
 *   {"command":"set_reconcile_interval","seconds":600}   // 0 turns it off
 *
 * Folder events: scan_started, scan_done, folder_watched, folder_unwatched,
 * reconcile_changed, files_changed, and error with op "folders".
 *
 * `set_reconcile_interval` is not a convenience. A watcher reports the failures
 * it is told about, and a network share loses events silently; a periodic
 * reconciliation is the only mechanism that can see a silent loss. It is cheap:
 * a 3,585-file library over SMB walks in about six seconds. It runs the same
 * scan command a host sends, through the same comparison, so there is one place
 * a diff can come from.
 *
 * `scan_done` is a DIFF against the index Base keeps, not the whole tree:
 * `added`, `modified`, `removed` and `moved`. Reporting every file on every
 * launch would make a host re-import the library each time, which is worse than
 * not scanning at all.
 *
 * A `moved` entry is a file that was renamed rather than deleted and added. It
 * is paired by size and timestamp within one reconciliation, and Base has
 * already moved the star, the play count, the playlist entries and the analysis
 * cache entry to the new path; the host only has to move whatever else it
 * caches by path. When two files
 * share a size and a timestamp the pairing is abandoned and the change is
 * reported as a removal and an addition -- a wrong pairing would hand one file
 * another's history, and a missed one only costs what not detecting moves cost.
 *
 * `files_changed` is how the host hears about changes between scans. It is
 * coalesced: one save produces several backend events and one message here.
 * Paths Base is writing itself are filtered out, so a tag edit is reported as
 * `tags_changed` and not as a file appearing. When `rescan` is set the watcher
 * could not name what changed -- a directory was removed, or the backend
 * reported lost events -- so the honest answer is to scan the root again rather
 * than invent a list.
 *
 * A remote track is downloaded and then played as a file: `remote_fetched`
 * carries the path, which goes to the same `play` command a local file uses.
 * Streaming was rejected on purpose. The engine decodes from paths -- its DSD
 * path opens a file, its analyser decodes a file, and the library keys
 * favourites and playlists on paths -- so a URL would work for plain decoding
 * and quietly fail everywhere else, starting with DSD falling back to the
 * generic decoder without saying so.
 *
 * `cache_dir` must be stable across runs. The cache name is `<item id>.<ext>`,
 * so a directory that moved would orphan every favourite and playlist entry
 * that names one of its files.
 *
 * An empty `token` disconnects. `fetch_remote` only accepts an id from the last
 * `list_remote`: a guessed id would put a file in the cache under a name the
 * caller never saw.
 *
 * Every artwork command names a family, because pictures live in one. A file
 * can carry the same cover in ID3v2 and in APE, each with its own list, so an
 * index without a family addresses nothing. A family that is eight text fields
 * (ID3v1) or text chunks (RIFF INFO, AIFF text) has nowhere to put a picture
 * and is refused before the file is opened.
 *
 * `read_artwork` answers two different questions by design. Without an index it
 * lists every embedded picture with its role, mime and byte length and carries
 * no bytes -- that is what a selection asks, on the path that runs on every
 * track change. With an index it also carries that picture's bytes as base64,
 * because the bridge is one JSON string and a picture is not text. An index
 * past the end is an error, not an empty picture: silence would look like a
 * cover that failed to decode.
 *
 * A field carries two names and they are not interchangeable. `key` is the
 * identity -- an ItemKey variant name such as TrackTitle -- and it is the only
 * thing a write is resolved by. `name` is how that family spells the field
 * (TIT2, TITLE, Title) and is for display only. A family can spell several
 * keys the same way: ID3v2 puts the track number and the track total in one
 * TRCK frame, and TIPL holds seven roles under one name, so resolving a write
 * by the display name would pick one of them and write the wrong field.
 *
 * `write_tags` has no default policy on purpose: which families to write is
 * the caller's decision. A policy with `clear` and an empty `write` erases
 * those families and writes nothing -- that is "remove ID3 tags from FLAC
 * files", and it is a real edit rather than a degenerate write.
 *
 * `tags_read` reports `families` (what the file carries now) and `writable`
 * (what its container can be written with) separately. A WAV may carry an
 * ID3v2 chunk and still have no ID3v1 to write.
 *
 * `set_policy` is a request. The engine reports the rate it actually granted in
 * the snapshot's `render_path`; `policy` is what was asked for and may differ.
 */
typedef struct NAE_Base NAE_Base;

typedef void (*NAE_BaseEventCallback)(const char *json, void *user);

/*
 * Start the workers with exclusive output and no library.
 *
 * Library commands then report "no library is open" rather than an empty
 * library: "you have no playlists" is a statement about the listener's music,
 * not about this process.
 *
 * NULL when the workers could not start.
 */
NAE_Base *NAE_BaseStart(void);

/*
 * Start with a library file for favourites, playlists and play counts.
 *
 * The file and its schema are created when they do not exist, and parent
 * directories with them. NULL when the library could not be opened or the
 * workers could not start.
 *
 * An empty path is refused. SQLite reads "" as a private temporary database,
 * so accepting it would hand back a library that vanishes on exit and lose
 * every playlist the listener made -- for a host that passed the wrong
 * variable, silently. NAE_BaseStart is how to ask for no library.
 */
NAE_Base *NAE_BaseStartWithLibrary(const char *path);

/*
 * Start with an explicit share mode: 0 = exclusive, 1 = shared. Shared is for
 * platforms where another process owns the endpoint; it is a choice, never an
 * automatic fallback.
 */
NAE_Base *NAE_BaseStartWithShare(int32_t share);

/*
 * Send one serialized Command. Returns 0 on success, -1 for a bad pointer,
 * -2 for malformed JSON, -3 when the workers are gone, -4 when already
 * subscribed (NAE_BaseSubscribe only).
 */
int32_t NAE_BaseCommand(NAE_Base *base, const char *json);

/* Subscribe once. The callback and `user` must stay valid until shutdown. */
int32_t NAE_BaseSubscribe(NAE_Base *base, NAE_BaseEventCallback callback, void *user);

/*
 * Drop the subscription and let its thread exit. The callback is not invoked
 * again once this returns. Returns 0 on success, -1 for a bad pointer, -4 when
 * there is no subscription to drop.
 *
 * Without this a host that closes a window has to shut Base down entirely.
 */
int32_t NAE_BaseUnsubscribe(NAE_Base *base);

/* Current snapshot as JSON; NULL on failure. Free with NAE_BaseFreeString. */
char *NAE_BaseSnapshot(NAE_Base *base);

void NAE_BaseFreeString(char *text);

/* Stop playback, join the workers and release Base. Safe on NULL; call once. */
void NAE_BaseShutdown(NAE_Base *base);

#ifdef __cplusplus
}
#endif

#endif /* NAE_BASE_H */
