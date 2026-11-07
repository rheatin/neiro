/*
 * The header contract, driven from C.
 *
 * `base_capi_check` proves the JSON bridge from Rust, which shares the
 * serialized types with the crate: a type that failed to serialize would fail
 * to compile on both sides at once. A C host shares nothing, so this is the
 * only check that sees `include/nae_base.h` as a consumer does -- symbol names,
 * calling convention, string ownership, and the shape of the JSON that crosses.
 *
 * Every export in the header is called here, and each check measures what the
 * header promises rather than that a call returned:
 *
 *   * `NAE_BaseStartWithShare` refuses a mode it does not define, instead of
 *     picking one;
 *   * with no library, a library command reports "no library is open" rather
 *     than an empty list -- "you have no playlists" is a statement about the
 *     listener's music, not about this process;
 *   * a second subscribe is refused with -4 and a second unsubscribe reports
 *     there was nothing to drop;
 *   * `FreeString(NULL)` and `Shutdown(NULL)` are safe, and the live handle
 *     still works after them;
 *   * `tags_read` reports `families` and `writable` as separate arrays, because
 *     a WAV can be written with ID3v2 and still carry none;
 *   * a write the container cannot take comes back as an error event;
 *   * a value written is the value read back, with the family, the identity and
 *     the format's own name for the field all present.
 *
 * Build and run with `test/run-c-check.ps1`, or by hand (MinGW, from the
 * repository root):
 *
 *   cc -I base/include base/test/nae_base_c_check.c \
 *      base/target/debug/libneiro_base.dll.a -mcrtdll=ucrt -o base_c_check.exe
 *   PATH="$PWD/base/target/debug:$PATH" ./base_c_check.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "nae_base.h"

/* The callback runs on a Rust thread, so the capture has to be guarded. */
static CRITICAL_SECTION g_lock;
static char g_events[32][16384];
static int g_event_count = 0;
static int g_failures = 0;

/* A 1x1 PNG, base64. Real bytes on purpose: a made-up string would store and
 * read back fine and prove nothing about a picture. */
static const char *TINY_PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAACklEQVR4nGMAAQAABQABDQot"
    "tAAAAABJRU5ErkJggg==";

#define PUT16(p, v)                        \
    do {                                   \
        unsigned _v = (unsigned)(v);       \
        (p)[0] = (unsigned char)(_v);      \
        (p)[1] = (unsigned char)(_v >> 8); \
    } while (0)

#define PUT32(p, v)                            \
    do {                                       \
        unsigned long _v = (unsigned long)(v); \
        (p)[0] = (unsigned char)(_v);          \
        (p)[1] = (unsigned char)(_v >> 8);     \
        (p)[2] = (unsigned char)(_v >> 16);    \
        (p)[3] = (unsigned char)(_v >> 24);    \
    } while (0)

static void on_event(const char *json, void *user) {
    (void)user;
    if (json == NULL) {
        return;
    }
    EnterCriticalSection(&g_lock);
    if (g_event_count < 32) {
        /* strncpy leaves it unterminated when the source is longer, and an
         * unterminated probe buffer reads as a truncated event. */
        strncpy(g_events[g_event_count], json, sizeof(g_events[0]) - 1);
        g_events[g_event_count][sizeof(g_events[0]) - 1] = '\0';
        g_event_count++;
    }
    LeaveCriticalSection(&g_lock);
}

static void reset_events(void) {
    EnterCriticalSection(&g_lock);
    g_event_count = 0;
    LeaveCriticalSection(&g_lock);
}

/* Index of the first captured event containing `needle`, or -1 after `ms`. */
static int wait_for(const char *needle, int ms) {
    int waited = 0;
    while (waited < ms) {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_event_count; i++) {
            if (strstr(g_events[i], needle) != NULL) {
                LeaveCriticalSection(&g_lock);
                return i;
            }
        }
        LeaveCriticalSection(&g_lock);
        Sleep(10);
        waited += 10;
    }
    return -1;
}

static void dump_events(void) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_event_count; i++) {
        fprintf(stderr, "  event[%d] %s\n", i, g_events[i]);
    }
    LeaveCriticalSection(&g_lock);
}

static void check(int ok, const char *label, const char *detail) {
    printf("  [%s] %-58s %s\n", ok ? "PASS" : "FAIL", label,
           detail == NULL ? "" : detail);
    if (!ok) {
        g_failures++;
    }
}

/* JSON has no \U or \n escape, so a Windows path pasted in raw is a parse
 * error and not a path. The backend takes forward slashes on every platform. */
static void to_json_path(char *path) {
    for (char *p = path; *p != '\0'; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

static int send_json(NAE_Base *base, const char *json) {
    return NAE_BaseCommand(base, json) == 0;
}

/* A 44-byte-header WAV with real samples: an empty data chunk is a file the
 * backend refuses to parse, and a read that fails for that reason says nothing
 * about tags. */
static int write_wav(const char *path) {
    const unsigned frames = 44100;
    unsigned char header[44];
    unsigned char *body = (unsigned char *)malloc(frames * 2);
    if (body == NULL) {
        return 0;
    }

    memcpy(header + 0, "RIFF", 4);
    PUT32(header + 4, 36 + frames * 2);
    memcpy(header + 8, "WAVEfmt ", 8);
    PUT32(header + 16, 16);
    PUT16(header + 20, 1);
    PUT16(header + 22, 1);
    PUT32(header + 24, 44100);
    PUT32(header + 28, 44100 * 2);
    PUT16(header + 32, 2);
    PUT16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    PUT32(header + 40, frames * 2);

    for (unsigned i = 0; i < frames; i++) {
        short v = (short)(8000.0 * ((i % 100) / 100.0));
        body[i * 2 + 0] = (unsigned char)(v & 0xff);
        body[i * 2 + 1] = (unsigned char)((v >> 8) & 0xff);
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(body);
        return 0;
    }
    fwrite(header, 1, 44, f);
    fwrite(body, 1, frames * 2, f);
    fclose(f);
    free(body);
    return 1;
}

/* ---- one export group per function -------------------------------------- */

static void check_start_variants(void) {
    printf("\nstart\n");

    NAE_Base *b = NAE_BaseStartWithShare(0);
    check(b != NULL, "share 0 starts", NULL);
    NAE_BaseShutdown(b);

    b = NAE_BaseStartWithShare(1);
    check(b != NULL, "share 1 starts", NULL);
    NAE_BaseShutdown(b);

    /* The header defines 0 and 1. Anything else is the caller's mistake and
     * must not be turned into a default: choosing shared here would silently
     * take the endpoint away from another process. */
    check(NAE_BaseStartWithShare(2) == NULL,
          "an undefined share mode is refused, not defaulted", NULL);
    check(NAE_BaseStartWithLibrary(NULL) == NULL,
          "a NULL library path is refused", NULL);
    check(NAE_BaseStartWithLibrary("") == NULL,
          "a library that cannot be opened is refused", NULL);

    b = NAE_BaseStart();
    check(b != NULL, "the plain start returns a handle", NULL);
    NAE_BaseShutdown(b);
    check(1, "shutting the plain handle down does not crash", NULL);
}

static void check_command_codes(void) {
    printf("\ncommand and ownership\n");

    NAE_Base *b = NAE_BaseStart();
    if (b == NULL) {
        check(0, "Base starts for the command checks", NULL);
        return;
    }

    check(NAE_BaseCommand(NULL, "{}") == -1, "a NULL handle is -1", NULL);
    check(NAE_BaseCommand(b, NULL) == -1, "a NULL command is -1", NULL);
    check(NAE_BaseCommand(b, "not json") == -2, "malformed JSON is -2", NULL);
    check(NAE_BaseCommand(b, "{\"command\":\"nonsense\"}") == -2,
          "an unknown command is -2", NULL);

    /* These two take a NULL pointer by contract. The check is not that they
     * return: it is that the live handle still works afterwards. */
    NAE_BaseFreeString(NULL);
    NAE_BaseShutdown(NULL);
    char *json = NAE_BaseSnapshot(b);
    check(json != NULL, "FreeString(NULL) and Shutdown(NULL) leave the handle usable",
          NULL);
    NAE_BaseFreeString(json);

    NAE_BaseShutdown(b);
}

static void check_no_library(void) {
    printf("\nno library\n");

    NAE_Base *b = NAE_BaseStart();
    if (b == NULL) {
        check(0, "Base starts without a library", NULL);
        return;
    }
    check(NAE_BaseSubscribe(b, on_event, NULL) == 0, "subscribe succeeds", NULL);
    reset_events();

    check(send_json(b, "{\"command\":\"list_favourites\"}"),
          "a library command is accepted", NULL);
    int err = wait_for("\"op\":\"favourites\"", 5000);
    check(err >= 0, "it comes back as an error, not as an empty list", NULL);
    if (err >= 0) {
        check(strstr(g_events[err], "no library is open") != NULL,
              "and the error says why", NULL);
    }

    NAE_BaseShutdown(b);
}

static void check_subscription(void) {
    printf("\nsubscription\n");

    NAE_Base *b = NAE_BaseStart();
    if (b == NULL) {
        check(0, "Base starts for the subscription checks", NULL);
        return;
    }
    check(NAE_BaseSubscribe(b, on_event, NULL) == 0, "the first subscribe is 0", NULL);
    check(NAE_BaseSubscribe(b, on_event, NULL) == -4,
          "a second subscribe is -4, not a silent second stream", NULL);
    check(NAE_BaseUnsubscribe(b) == 0, "unsubscribe is 0", NULL);
    check(NAE_BaseUnsubscribe(b) == -4,
          "a second unsubscribe reports nothing to drop", NULL);

    /* A host that closes a window unsubscribes and keeps Base. Asking again
     * must work after that, which is the whole reason the ticket exists. */
    check(NAE_BaseSubscribe(b, on_event, NULL) == 0, "resubscribing works", NULL);
    NAE_BaseShutdown(b);
}

static void check_tag_flow(const char *wav) {
    printf("\ntags\n");

    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    char db[MAX_PATH];
    snprintf(db, MAX_PATH, "%snae-c-check.db", dir);
    to_json_path(db);
    DeleteFileA(db);

    NAE_Base *base = NAE_BaseStartWithLibrary(db);
    if (base == NULL) {
        check(0, "Base starts with a library", NULL);
        return;
    }
    check(NAE_BaseSubscribe(base, on_event, NULL) == 0, "subscribe succeeds", NULL);

    char *snapshot = NAE_BaseSnapshot(base);
    check(snapshot != NULL && strstr(snapshot, "\"state\"") != NULL,
          "NAE_BaseSnapshot returns JSON the caller owns", NULL);
    NAE_BaseFreeString(snapshot);

    reset_events();
    check(send_json(base, "{\"command\":\"list_tag_keys\"}"),
          "list_tag_keys is accepted", NULL);
    int keys = wait_for("\"tag_keys_changed\"", 5000);
    check(keys >= 0, "tag_keys_changed arrives", NULL);
    if (keys >= 0) {
        /* TIT2 is ID3v2's own name for the title. Matching it proves the lists
         * came from the format's mapping rather than from a constant here. */
        check(strstr(g_events[keys], "TIT2") != NULL,
              "ID3v2's own field name is in the answer", NULL);
        check(strstr(g_events[keys], "\"family\"") != NULL,
              "each list names its family", NULL);
    }

    reset_events();
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "{\"command\":\"read_tags\",\"path\":\"%s\"}", wav);
    check(send_json(base, cmd), "read_tags is accepted", NULL);
    int read1 = wait_for("\"tags_read\"", 5000);
    check(read1 >= 0, "tags_read arrives", NULL);
    if (read1 >= 0) {
        check(strstr(g_events[read1], "\"families\":[]") != NULL,
              "an untagged file carries no families", NULL);
        check(strstr(g_events[read1], "riff_info") != NULL,
              "writable names the container's family", NULL);
        check(strstr(g_events[read1], "id3v2") != NULL,
              "writable names a family the file does not carry", NULL);
    }

    reset_events();
    char refused[1024];
    snprintf(refused, sizeof(refused),
             "{\"command\":\"write_tags\",\"path\":\"%s\",\"fields\":["
             "{\"family\":\"vorbis_comments\",\"key\":\"TrackTitle\","
             "\"name\":\"TITLE\",\"value\":\"x\"}],"
             "\"policy\":{\"write\":[\"vorbis_comments\"]}}",
             wav);
    check(send_json(base, refused), "an impossible write is accepted as a command",
          NULL);
    check(wait_for("\"op\":\"tags\"", 5000) >= 0,
          "the refusal arrives as an error event", NULL);

    reset_events();
    char write[1024];
    snprintf(write, sizeof(write),
             "{\"command\":\"write_tags\",\"path\":\"%s\",\"fields\":["
             "{\"family\":\"riff_info\",\"key\":\"TrackTitle\","
             "\"name\":\"INAM\",\"value\":\"Probe Title\"}],"
             "\"policy\":{\"write\":[\"riff_info\"]}}",
             wav);
    check(send_json(base, write), "write_tags is accepted", NULL);
    check(wait_for("\"tags_changed\"", 10000) >= 0, "tags_changed arrives", NULL);

    reset_events();
    check(send_json(base, cmd), "reading again is accepted", NULL);
    int read2 = wait_for("\"Probe Title\"", 5000);
    check(read2 >= 0, "the value written is the value read back", NULL);
    if (read2 >= 0) {
        const char *event = g_events[read2];
        check(strstr(event, "\"family\":\"riff_info\"") != NULL,
              "the field names its family", NULL);
        check(strstr(event, "\"key\":\"TrackTitle\"") != NULL,
              "the wire carries the identity, not the display name", NULL);
        check(strstr(event, "\"name\":\"INAM\"") != NULL,
              "and the format's own name for it", NULL);
    }

    /* ---- artwork ---------------------------------------------------------
     *
     * The listing and the bytes are separate questions: a view asks whether
     * there is a cover far more often than it asks to draw one, and a cover is
     * a hundred kilobytes. This fixture has none, which is a measurement and
     * not a failure: an empty list means the file carries no picture.
     */
    reset_events();
    char art[1024];
    snprintf(art, sizeof(art), "{\"command\":\"read_artwork\",\"path\":\"%s\"}", wav);
    check(send_json(base, art), "read_artwork is accepted", NULL);
    int listed = wait_for("\"artwork_read\"", 5000);
    check(listed >= 0, "artwork_read arrives", NULL);
    if (listed >= 0) {
        check(strstr(g_events[listed], "\"pictures\":[]") != NULL,
              "a file with no picture reports an empty list, not an error", NULL);
        check(strstr(g_events[listed], "\"data\"") == NULL,
              "a listing carries no bytes", NULL);
    }

    /* An index the file does not have is an error, not an empty picture:
     * silence would look like a cover that failed to decode. */
    reset_events();
    snprintf(art, sizeof(art),
             "{\"command\":\"read_artwork\",\"path\":\"%s\",\"index\":3}", wav);
    check(send_json(base, art), "read_artwork with an index is accepted", NULL);
    int missing = wait_for("\"op\":\"artwork\"", 5000);
    check(missing >= 0, "a missing index is an error", NULL);

    /* ---- writing artwork -------------------------------------------------
     *
     * The bytes cross as base64 in both directions, so a host that read a cover
     * can write it back. The fixture is a WAV, whose ID3v2 chunk is the family
     * that can hold a picture.
     */
    reset_events();
    snprintf(art, sizeof(art),
             "{\"command\":\"set_artwork\",\"path\":\"%s\","
             "\"family\":\"id3v2\",\"data\":\"%s\","
             "\"mime\":\"image/png\",\"kind\":\"CoverFront\"}",
             wav, TINY_PNG_B64);
    check(send_json(base, art), "set_artwork is accepted", NULL);
    int added = wait_for("\"artwork_changed\"", 10000);
    check(added >= 0, "artwork_changed arrives", NULL);
    if (added >= 0) {
        check(strstr(g_events[added], "\"kind\":\"CoverFront\"") != NULL,
              "the picture is listed with the role it was given", NULL);
        check(strstr(g_events[added], "\"family\":\"id3v2\"") != NULL,
              "and with the family that holds it", NULL);
    }

    /* Reading it back is what proves the bytes survived the round trip: the
     * command returning 0 says only that it was accepted. */
    reset_events();
    snprintf(art, sizeof(art),
             "{\"command\":\"read_artwork\",\"path\":\"%s\",\"index\":0}", wav);
    check(send_json(base, art), "the picture can be read back", NULL);
    int round = wait_for("/9j/", 5000);
    if (round < 0) {
        round = wait_for("iVBOR", 5000);
    }
    check(round >= 0, "the bytes come back base64", NULL);

    /* Removing it, and the listing says so afterwards. */
    reset_events();
    snprintf(art, sizeof(art),
             "{\"command\":\"remove_artwork\",\"path\":\"%s\","
             "\"family\":\"id3v2\",\"index\":0}",
             wav);
    check(send_json(base, art), "remove_artwork is accepted", NULL);
    int removed = wait_for("\"artwork_changed\"", 10000);
    check(removed >= 0, "removing answers with the new listing", NULL);
    if (removed >= 0) {
        check(strstr(g_events[removed], "\"pictures\":[]") != NULL,
              "and the listing is empty again", NULL);
    }

    /* A family with nowhere to put a picture is refused, not silently ignored. */
    reset_events();
    snprintf(art, sizeof(art),
             "{\"command\":\"set_artwork\",\"path\":\"%s\","
             "\"family\":\"riff_info\",\"data\":\"%s\"}",
             wav, TINY_PNG_B64);
    check(send_json(base, art), "an impossible family is accepted as a command", NULL);
    check(wait_for("\"op\":\"artwork\"", 5000) >= 0,
          "and comes back as an error", NULL);

    NAE_BaseShutdown(base);
    DeleteFileA(db);
}

int main(void) {
    InitializeCriticalSection(&g_lock);

    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) {
        fprintf(stderr, "no temp directory\n");
        return 2;
    }
    char wav[MAX_PATH];
    snprintf(wav, MAX_PATH, "%snae-c-check.wav", dir);
    to_json_path(wav);
    DeleteFileA(wav);
    if (!write_wav(wav)) {
        fprintf(stderr, "could not write the fixture\n");
        return 2;
    }

    check_start_variants();
    check_command_codes();
    check_no_library();
    check_subscription();
    check_tag_flow(wav);

    if (g_failures > 0) {
        dump_events();
    }

    DeleteFileA(wav);
    DeleteCriticalSection(&g_lock);

    if (g_failures > 0) {
        printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nBASE C HEADER CHECK PASSED\n");
    return 0;
}
