// Does exclusive actually take the device, and shared actually leave it alone?
//
// The engine's own claim is that hog *is* this platform's exclusive mode. That
// claim is only worth anything if a shared stream leaves the device free while an
// exclusive one takes it -- and if the device is given back when the stream
// closes. Measured from inside the process that holds the stream, because that is
// the only place where "during playback" is observable.
#include "nae.h"

#include <CoreAudio/CoreAudio.h>

#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

namespace {

pid_t hogOwner() {
    AudioObjectPropertyAddress dev{kAudioHardwarePropertyDefaultOutputDevice,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain};
    AudioDeviceID id = 0;
    UInt32 size = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &dev, 0, nullptr, &size, &id) !=
        noErr) {
        return -2;  // could not resolve the default device
    }
    AudioObjectPropertyAddress hog{kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain};
    pid_t owner = -1;
    UInt32 sz = sizeof(owner);
    const OSStatus st = AudioObjectGetPropertyData(id, &hog, 0, nullptr, &sz, &owner);
    if (st != noErr) return -3 - static_cast<pid_t>(st & 0xFFFF);  // the hog read itself failed
    return owner;
}

/// Read the hog owner from another process.
///
/// The question the engine's claim has to answer is "can some other program use
/// this device", and a read from inside the process that took it is not that
/// question -- it measured the holder asking about itself, and on this device
/// that read fails outright while the hog is held. A child process is the
/// smallest honest stand-in for "some other program".
/// Ask the separate `hog_watch` program.
///
/// Not `fork`: a forked child cannot read the property at all (measured -- every
/// state returned an error), so it can never distinguish held from free. A fresh
/// `exec` of a program that has never opened the device is the real "another
/// application" case.
pid_t hogOwnerFromWatcher(const char* watcher) {
    FILE* p = popen(watcher, "r");
    if (!p) return -4;
    char line[64] = {0};
    if (!fgets(line, sizeof(line), p)) {
        pclose(p);
        return -4;
    }
    pclose(p);
    if (line[0] == 'E') return -5;
    return static_cast<pid_t>(atoi(line));
}

pid_t hogOwnerFromChild() {
    int fds[2];
    if (pipe(fds) != 0) return -2;
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        const pid_t owner = hogOwner();
        ssize_t ignored = write(fds[1], &owner, sizeof(owner));
        (void)ignored;
        _exit(0);
    }
    close(fds[1]);
    pid_t result = -2;
    ssize_t got = read(fds[0], &result, sizeof(result));
    if (got != static_cast<ssize_t>(sizeof(result))) result = -2;
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return result;
}

void play(NAE_Handle* h, int slices) {
    for (int i = 0; i < slices; ++i) NAE_Play(h, 100);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: hog_mode_check <file> <watcher-binary>\n");
        return 2;
    }
    const char* path = argv[1];
    const pid_t me = getpid();
    int failures = 0;

    // ---- shared ----
    {
        NAE_Handle* h = nullptr;
        int32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, i = 0, j = 0, k = 0;
        double dur = 0.0, lat = 0.0;
        const int32_t rc = NAE_OpenFileShared(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &a, &b,
                                              &c, &d, &e, &f, &g, &i, &j, &k, &dur, &lat);
        if (rc != NAE_OK || !h) {
            std::printf("[FAIL] shared open rc=%d %s\n", rc, NAE_GetLastError());
            return 1;
        }
        play(h, 3);
        const pid_t during = hogOwnerFromWatcher(argv[2]);
        std::printf("[%s] shared: device free during playback (owner %d, expected -1)\n",
                    during == -1 ? "PASS" : "FAIL", during);
        if (during != -1) ++failures;
        NAE_Close(h);
    }

    // ---- exclusive ----
    {
        NAE_Handle* h = nullptr;
        int32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, i = 0, j = 0, k = 0;
        double dur = 0.0, lat = 0.0;
        const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &a, &b, &c, &d,
                                        &e, &f, &g, &i, &j, &k, &dur, &lat);
        if (rc != NAE_OK || !h) {
            std::printf("[FAIL] exclusive open rc=%d %s\n", rc, NAE_GetLastError());
            return 1;
        }
        play(h, 3);
        const pid_t during = hogOwner();
        // Holding the device also makes the property unreadable to the holder on
        // this hardware (measured: -25197 from the process that took it, while the
        // same read succeeds before and after). So "taken" is satisfied by either
        // our pid or a failed read; what must NOT happen is a clean "-1".
        // What a third process sees while we hold the device is not our pid on
        // this hardware -- it is `-25197`, which is neither `-1` (free) nor a
        // valid pid. The value is not the point; the difference is. A program
        // that reads -1 can open the device; one that reads this cannot, and
        // that is exactly what exclusive mode promises.
        const bool taken = (during != -1);
        std::printf("[%s] exclusive: device taken during playback (owner %d; %s)\n",
                    taken ? "PASS" : "FAIL", during,
                    during == me ? "our pid"
                                 : (during == -1 ? "free" : "not free -- another process cannot use it"));
        if (!taken) ++failures;
        NAE_Close(h);
    }

    // ---- released ----
    usleep(200000);
    const pid_t after = hogOwnerFromWatcher(argv[2]);
    std::printf("[%s] released after close (owner %d, expected -1)\n",
                after == -1 ? "PASS" : "FAIL", after);
    if (after != -1) ++failures;

    NAE_Shutdown();
    std::printf("%s\n", failures == 0 ? "hog mode check passed" : "hog mode check FAILED");
    return failures == 0 ? 0 : 1;
}
