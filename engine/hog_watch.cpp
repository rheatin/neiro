// Report who holds the output device, then exit.
//
// A separate executable on purpose. Reading `kAudioDevicePropertyHogMode` from
// inside the process that holds it is not the question a host cares about -- and
// a forked child cannot make the read at all. A program that has never touched
// the audio hardware is the closest thing to "some other application".
#include <CoreAudio/CoreAudio.h>

#include <cstdio>

int main() {
    AudioObjectPropertyAddress dev{kAudioHardwarePropertyDefaultOutputDevice,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain};
    AudioDeviceID id = 0;
    UInt32 size = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &dev, 0, nullptr, &size, &id) !=
        noErr) {
        std::printf("E%d\n", -2);
        return 1;
    }
    AudioObjectPropertyAddress hog{kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain};
    pid_t owner = -1;
    UInt32 sz = sizeof(owner);
    const OSStatus st = AudioObjectGetPropertyData(id, &hog, 0, nullptr, &sz, &owner);
    if (st != noErr) {
        std::printf("E%d\n", static_cast<int>(st));
        return 1;
    }
    std::printf("%d\n", static_cast<int>(owner));
    return 0;
}
