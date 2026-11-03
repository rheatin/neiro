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
 *   {"event":"state","state":"playing",...}
 */
typedef struct NAE_Base NAE_Base;

typedef void (*NAE_BaseEventCallback)(const char *json, void *user);

/* Start the workers with exclusive output. NULL when they could not start. */
NAE_Base *NAE_BaseStart(void);

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

/* Current snapshot as JSON; NULL on failure. Free with NAE_BaseFreeString. */
char *NAE_BaseSnapshot(NAE_Base *base);

void NAE_BaseFreeString(char *text);

/* Stop playback, join the workers and release Base. Safe on NULL; call once. */
void NAE_BaseShutdown(NAE_Base *base);

#ifdef __cplusplus
}
#endif

#endif /* NAE_BASE_H */
