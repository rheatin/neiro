// Native DSD sink: the engine reads the bitstream and the host owns the device
// through one callback. The check is device-free and compares the bytes the
// callback received with the bytes in the file, bit for bit.

#include "nae.h"
#include "dff_reader.h"
#include "dsd_downrate.h"
#include "dsf_reader.h"
#include "utf8_args.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-50s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

struct Capture {
    std::vector<uint8_t> bytes;
    int32_t calls = 0;
    int32_t failOnCall = -1;
    int32_t failCode = 0;
    int32_t channelsSeen = 0;
};

int32_t writeBytes(void* context, const uint8_t* interleaved, int32_t bytesPerChannel,
                   int32_t channels) {
    auto* capture = static_cast<Capture*>(context);
    ++capture->calls;
    capture->channelsSeen = channels;
    if (capture->failOnCall >= 0 && capture->calls >= capture->failOnCall) {
        return capture->failCode;
    }
    capture->bytes.insert(capture->bytes.end(), interleaved,
                          interleaved + static_cast<size_t>(bytesPerChannel) * channels);
    return 0;
}

// Interleaved source bytes from the container, row-major per channel byte.
bool readSource(const std::string& path, bool* isMsbFirst,
                std::vector<uint8_t>* interleaved, uint32_t* rate) {
    if (path.size() > 3 && path.substr(path.size() - 3) == "dsf") {
        dsf::File file;
        if (!file.open(path.c_str())) return false;
        *isMsbFirst = false;
        *rate = file.fmt.dsdRate;
        const size_t blockSize = file.fmt.blockSize;
        const size_t ch = file.fmt.channels;
        std::vector<uint8_t> raw;
        interleaved->clear();
        for (uint64_t block = 0; block < file.totalBlocks(); ++block) {
            if (!file.readBlockRaw(block, &raw)) return false;
            if (raw.size() < blockSize * ch) return false;
            const size_t base = interleaved->size();
            interleaved->resize(base + blockSize * ch);
            for (size_t i = 0; i < blockSize; ++i) {
                for (size_t c = 0; c < ch; ++c) {
                    (*interleaved)[base + i * ch + c] = raw[c * blockSize + i];
                }
            }
        }
        return true;
    }
    dff::File file;
    if (!file.open(path.c_str())) return false;
    *isMsbFirst = true;
    *rate = file.fmt.sampleRate;
    interleaved->assign(file.fmt.dataBytes, 0);
    return interleaved->empty() ||
           file.readData(0, interleaved->data(), interleaved->size());
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: native_dsd_sink_check <file.dsf|dff>\n");
        return 1;
    }
    const std::string path = args[1];

    bool isMsbFirst = false;
    std::vector<uint8_t> source;
    uint32_t sourceRate = 0;
    const bool sourceRead = readSource(path, &isMsbFirst, &source, &sourceRate);
    check(sourceRead, "source is readable",
          std::to_string(source.size()) + " bytes at " + std::to_string(sourceRate));

    NAE_Handle* handle = nullptr;
    Capture capture;
    int32_t rate = 0, channels = 0, bitOrder = 0;
    double durationMs = 0.0;
    check(NAE_OpenNativeDsdSink(path.c_str(), 0, writeBytes, &capture, &handle, &rate,
                                &channels, &bitOrder, &durationMs) == NAE_OK &&
              handle != nullptr,
          "sink opens at the source rate");
    if (!handle) {
        std::printf("open failed: %s\n", NAE_GetLastError());
        return 1;
    }
    check(rate == static_cast<int32_t>(sourceRate), "reported rate is the file's",
          std::to_string(rate));
    check(channels == 2, "reported channels", std::to_string(channels));
    check(bitOrder == (isMsbFirst ? 1 : 0), "reported bit order matches the container",
          std::to_string(bitOrder));
    const double expectedMs =
        static_cast<double>(source.size() / 2) * 8.0 / sourceRate * 1000.0;
    check(std::fabs(durationMs - expectedMs) < 1.0, "duration matches the data",
          std::to_string(durationMs) + " / " + std::to_string(expectedMs));

    // Pause must stop the flow; resume must restart it.
    NAE_Pause(handle);
    check(NAE_PlayNativeDsd(handle, 50) == NAE_OK && capture.bytes.empty(),
          "paused sink writes nothing");
    NAE_Resume(handle);

    // Play in slices until the file ends.
    for (int i = 0; i < 400 && capture.bytes.size() < source.size(); ++i) {
        const int32_t rc = NAE_PlayNativeDsd(handle, 20);
        if (rc != NAE_OK) {
            check(false, "play returned an error", std::to_string(rc));
            break;
        }
    }
    check(capture.bytes.size() == source.size(), "the sink received every byte",
          std::to_string(capture.bytes.size()) + " / " + std::to_string(source.size()));
    check(capture.bytes == source, "the sink received the file bit for bit");
    check(capture.calls > 1 && capture.channelsSeen == 2, "writes are interleaved stereo",
          std::to_string(capture.calls) + " calls");
    check(std::fabs(NAE_Position(handle) - expectedMs / 1000.0) < 0.01,
          "position is in seconds of DSD",
          std::to_string(NAE_Position(handle)));
    NAE_Close(handle);

    // Downrate through the sink matches the same downrate computed directly.
    if (sourceRate > 2822400u) {
        const uint32_t target = sourceRate / 2;
        Capture down;
        handle = nullptr;
        check(NAE_OpenNativeDsdSink(path.c_str(), static_cast<int32_t>(target), writeBytes,
                                    &down, &handle, &rate, &channels, &bitOrder,
                                    &durationMs) == NAE_OK &&
                  handle != nullptr,
              "sink opens at half the source rate");
        if (handle) {
            check(rate == static_cast<int32_t>(target), "downrated rate is reported",
                  std::to_string(rate));
            for (int i = 0; i < 400 && down.bytes.size() < source.size() / 2; ++i) {
                if (NAE_PlayNativeDsd(handle, 20) != NAE_OK) break;
            }
            neiro::dsd::Downrate expected;
            std::string err;
            const bool configured =
                expected.configure(sourceRate, target, 2, isMsbFirst, &err);
            check(configured, "reference downrate configures", err);
            std::vector<uint8_t> reference;
            if (configured) expected.feed(source.data(), source.size(), &reference);
            check(down.bytes == reference, "the sink applies the downrate exactly",
                  std::to_string(down.bytes.size()) + " / " +
                      std::to_string(reference.size()));
            NAE_Close(handle);
        }
    }

    // The handle must describe itself: a sink has no WASAPI format, and the
    // zeroed struct with NAE_OK was indistinguishable from "not open".
    {
        NAE_Handle* probe = nullptr;
        Capture c2;
        int32_t pr = 0, pc = 0, pb = 0;
        double pd = 0.0;
        if (NAE_OpenNativeDsdSink(path.c_str(), 0, writeBytes, &c2, &probe, &pr, &pc, &pb,
                                  &pd) == NAE_OK &&
            probe) {
            NAE_SignalPath sp{};
            sp.structSize = sizeof(sp);
            // Query first, then build the detail: the arguments of a single
            // call are evaluated in unspecified order, so reading `sp` in the
            // detail expression of the same `check(...)` call printed zeros
            // while the assertion itself passed.
            const int32_t spRc = NAE_QuerySignalPath(probe, &sp);
            check(spRc == NAE_OK && sp.sampleRate == static_cast<int32_t>(sourceRate) &&
                      sp.channels == 2 && sp.validBits == 1 && sp.bitPerfect == 1,
                  "the sink handle reports its stream",
                  std::to_string(sp.sampleRate) + " Hz / " + std::to_string(sp.channels) +
                      " ch / " + std::to_string(sp.validBits) + " bit");
            NAE_Close(probe);
        } else {
            check(false, "sink opens for the signal-path probe");
        }
    }

    // The host's error code must reach the caller unchanged.
    {
        Capture failing;
        failing.failOnCall = 2;
        failing.failCode = 77;
        handle = nullptr;
        if (NAE_OpenNativeDsdSink(path.c_str(), 0, writeBytes, &failing, &handle, &rate,
                                  &channels, &bitOrder, &durationMs) == NAE_OK &&
            handle) {
            check(NAE_PlayNativeDsd(handle, 200) == 77,
                  "the host's write error is returned as-is");
            NAE_Close(handle);
        } else {
            check(false, "failing-sink fixture opens");
        }
    }

    // A missing callback is rejected before anything is opened.
    {
        NAE_Handle* none = nullptr;
        check(NAE_OpenNativeDsdSink(path.c_str(), 0, nullptr, nullptr, &none, &rate,
                                    &channels, &bitOrder, &durationMs) ==
                  NAE_ERR_INVALID_ARGUMENT &&
                  none == nullptr,
              "a missing write callback is rejected");
    }

    std::printf("\n%s\n", failures == 0 ? "All native DSD sink checks passed."
                                        : "Some native DSD sink checks FAILED.");
    return failures == 0 ? 0 : 1;
}
