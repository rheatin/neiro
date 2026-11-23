// Spike stub headers. `xcodebuild -create-xcframework` needs headers for a
// `-library` slice; the Rust host declares the symbols itself, so this file
// only exists to satisfy the packaging step.

#ifndef NAE_STUB_H
#define NAE_STUB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *NAE_BaseStartWithLibrary(const char *library);
int NAE_BaseCommand(void *base, const char *json);
int NAE_BaseSubscribe(void *base, void (*cb)(const char *, void *), void *user);
int NAE_BaseUnsubscribe(void *base);
char *NAE_BaseSnapshot(void *base);
void NAE_BaseFreeString(char *text);
void NAE_BaseShutdown(void *base);

int NAE_BasePlayContext(void *base, const char *items, size_t start);
int NAE_BasePlayNext(void *base, const char *items);
int NAE_BasePlayLater(void *base, const char *items);
int NAE_BaseSetRepeatMode(void *base, int mode);
int NAE_BaseSetShuffleMode(void *base, int enabled);
int NAE_BasePrevious(void *base);
int NAE_BaseRemoveQueueItem(void *base, const char *id);
int NAE_BaseClearUserQueue(void *base);
int NAE_BaseMoveUserQueueItem(void *base, size_t from, size_t to);

char *NAE_BaseRemoteProviders(void);
char *NAE_BaseListDirectory(void *base, const char *path);

unsigned int NAE_BaseAbiVersion(void);
char *NAE_BaseAudioCapabilities(void);
int NAE_BaseReinitializeAudio(unsigned int sample_rate, unsigned short channels);

#ifdef __cplusplus
}
#endif

#endif
