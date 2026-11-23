// Spike stub for the iOS core artifact.
//
// Not part of the default build graph: `ui/build.rs` links the real
// `neiro_base.xcframework` from `ui/vendor/`. This stub exists only to prove
// the cfg split, the link flags and the slice selection on a developer
// machine, and is never linked by CI. It implements the mandatory symbols
// inertly so the host can open, subscribe, snapshot and shut down.

#include <stdlib.h>
#include <string.h>

#define STUB_ABI_VERSION 2u

void *NAE_BaseStartWithLibrary(const char *library) {
    return (void *)0x1;
}

int NAE_BaseCommand(void *base, const char *json) {
    (void)base;
    (void)json;
    return 0;
}

int NAE_BaseSubscribe(void *base, void (*cb)(const char *, void *), void *user) {
    (void)base;
    (void)cb;
    (void)user;
    return 0;
}

int NAE_BaseUnsubscribe(void *base) {
    (void)base;
    return 0;
}

char *NAE_BaseSnapshot(void *base) {
    (void)base;
    char *text = malloc(64);
    if (text == NULL) return NULL;
    strcpy(text, "{\"state\":\"stub\"}");
    return text;
}

void NAE_BaseFreeString(char *text) {
    free(text);
}

void NAE_BaseShutdown(void *base) {
    (void)base;
}

int NAE_BasePlayContext(void *base, const char *items, size_t start) {
    (void)base;
    (void)items;
    (void)start;
    return 0;
}

int NAE_BasePlayNext(void *base, const char *items) {
    (void)base;
    (void)items;
    return 0;
}

int NAE_BasePlayLater(void *base, const char *items) {
    (void)base;
    (void)items;
    return 0;
}

int NAE_BaseSetRepeatMode(void *base, int mode) {
    (void)base;
    (void)mode;
    return 0;
}

int NAE_BaseSetShuffleMode(void *base, int enabled) {
    (void)base;
    (void)enabled;
    return 0;
}

int NAE_BasePrevious(void *base) {
    (void)base;
    return 0;
}

int NAE_BaseRemoveQueueItem(void *base, const char *id) {
    (void)base;
    (void)id;
    return 0;
}

int NAE_BaseClearUserQueue(void *base) {
    (void)base;
    return 0;
}

int NAE_BaseMoveUserQueueItem(void *base, size_t from, size_t to) {
    (void)base;
    (void)from;
    (void)to;
    return 0;
}

char *NAE_BaseRemoteProviders(void) {
    char *text = malloc(4);
    if (text == NULL) return NULL;
    strcpy(text, "[]");
    return text;
}

char *NAE_BaseListDirectory(void *base, const char *path) {
    (void)base;
    (void)path;
    char *text = malloc(32);
    if (text == NULL) return NULL;
    strcpy(text, "{\"folders\":[],\"tracks\":[]}");
    return text;
}

unsigned int NAE_BaseAbiVersion(void) {
    return STUB_ABI_VERSION;
}

char *NAE_BaseAudioCapabilities(void) {
    char *text = malloc(128);
    if (text == NULL) return NULL;
    strcpy(
        text,
        "{\"exclusive\":false,\"device_selection\":false,\"dither\":false,\"dsd\":false,\"system_mixer\":true}"
    );
    return text;
}

int NAE_BaseReinitializeAudio(unsigned int sample_rate, unsigned short channels) {
    (void)sample_rate;
    (void)channels;
    return 0;
}
