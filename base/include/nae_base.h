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
