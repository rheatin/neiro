// Set the output device's nominal sample rate and exit.
//
// A one-off, because the engine restores whatever a stream changed -- which is
// the right behaviour and also means it cannot be used to put a device back to a
// known rate after an experiment left it elsewhere.
#include <CoreAudio/CoreAudio.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rate_set <hz>\n");
        return 2;
    }
    AudioObjectPropertyAddress dev{kAudioHardwarePropertyDefaultOutputDevice,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain};
    AudioDeviceID id = 0;
    UInt32 size = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &dev, 0, nullptr, &size, &id) !=
        noErr) {
        std::printf("no default output device\n");
        return 1;
    }
    AudioObjectPropertyAddress rate{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 want = std::atof(argv[1]);
    const OSStatus st = AudioObjectSetPropertyData(id, &rate, 0, nullptr, sizeof(want), &want);
    // The HAL applies a nominal-rate change late: the set returns noErr and the
    // device still reports the old value for a while. Read until it settles, the
    // same way the engine does after a policy change.
    Float64 now = 0.0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        UInt32 sz = sizeof(now);
        AudioObjectGetPropertyData(id, &rate, 0, nullptr, &sz, &now);
        if (now == want) break;
        usleep(10000);
    }
    std::printf("set %0.f -> status %d, device now %0.f\n", want, static_cast<int>(st), now);
    return st == noErr ? 0 : 1;
}
