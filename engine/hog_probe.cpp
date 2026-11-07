// Does hog mode actually succeed, and who holds it?
//
// The two engine experiments that concluded "hog silences the stream" never
// recorded whether `AudioObjectSetPropertyData` returned noErr or what the owner
// was afterwards, so that conclusion was unsupported. This asks the device
// directly, in the order the hi-fi recipe uses: hog first, before any IOProc or
// AudioUnit exists.
#include <CoreAudio/CoreAudio.h>

#include <cstdio>
#include <unistd.h>

namespace {

AudioDeviceID defaultOutput() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioDeviceID id = 0;
    UInt32 size = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &id) !=
        noErr) {
        return 0;
    }
    return id;
}

pid_t hogOwner(AudioDeviceID id, OSStatus* status) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyHogMode,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    pid_t value = -1;
    UInt32 size = sizeof(value);
    *status = AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &value);
    return value;
}

OSStatus setHog(AudioDeviceID id, pid_t owner) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyHogMode,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    pid_t value = owner;
    return AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof(value), &value);
}

}  // namespace

int main() {
    const AudioDeviceID id = defaultOutput();
    if (!id) {
        std::printf("no default output device\n");
        return 1;
    }
    OSStatus getStatus = noErr;
    const pid_t before = hogOwner(id, &getStatus);
    std::printf("device %u, hog owner before: %d (get status %d)\n", id, before, getStatus);

    const OSStatus setStatus = setHog(id, getpid());
    std::printf("setHog(pid %d) -> OSStatus %d (%s)\n", getpid(), setStatus,
                setStatus == noErr ? "noErr" : "FAILED");

    const pid_t after = hogOwner(id, &getStatus);
    std::printf("hog owner after: %d (get status %d)  %s\n", after, getStatus,
                after == getpid() ? "= us" : (after < 0 ? "= nobody" : "= someone else"));

    const bool took = (setStatus == noErr && after == getpid());
    std::printf("hog %s\n", took ? "TAKEN" : "NOT TAKEN");

    if (after == getpid()) {
        const OSStatus rel = setHog(id, -1);
        std::printf("released -> %d\n", rel);
    }
    return took ? 0 : 1;
}
