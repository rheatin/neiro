// The struct-size contract: a caller's declared size is honoured, not overwritten.
//
// `nae.h` says the field exists so "a host built against a newer header than the
// DLL it loaded can tell which fields exist instead of reading past the end of
// what was actually written". That only works if the engine reads the declaration
// *before* it writes anything.
//
// Five exports did the opposite: they `memset` the whole struct first, which
// zeroed the declaration, and then had a branch `if (structSize == 0)` that was
// therefore always true. The caller's size was replaced with this build's
// `sizeof` every time, whatever the caller allocated.
//
// The check is the one that would have caught it: declare a struct smaller than
// this build's, and assert the engine still reports the declared size rather than
// its own. A host built against an old header passes a small number; a DLL that
// overwrites it is telling that host the struct is bigger than the memory it has.

#include "nae.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

/// The smallest legal declaration: enough for the size field itself.
constexpr uint32_t kMinimal = sizeof(uint32_t);

}  // namespace

int main() {
    // A null handle makes every one of these return NAE_OK with a zeroed
    // struct, so the check needs no device and no file -- and the size
    // negotiation is the same code either way.

    // ---- NAE_QueryTags -----------------------------------------------------
    {
        // Declare only the first field, then look for writes past it.
        //
        // The whole struct is allocated so the engine has room; the sentinel
        // beyond the declaration is what detects a write that ignored it. The
        // first version of this fix reported the declared size while still
        // memsetting and writing the full struct, and only a sentinel sees that.
        std::vector<unsigned char> storage(sizeof(NAE_Tags) + 64, 0xAB);
        NAE_Tags* tags = reinterpret_cast<NAE_Tags*>(storage.data());
        tags->structSize = kMinimal;
        const int32_t rc = NAE_QueryTags(nullptr, tags);
        check(rc == NAE_OK, "NAE_QueryTags accepts a null handle");
        check(tags->structSize == kMinimal,
              "NAE_QueryTags reports the declared size",
              std::to_string(tags->structSize) + " vs " + std::to_string(kMinimal));
        // Everything past the declared four bytes has to be untouched.
        bool clean = true;
        for (size_t i = kMinimal; i < storage.size(); ++i) {
            if (storage[i] != 0xAB) {
                clean = false;
                break;
            }
        }
        check(clean, "NAE_QueryTags writes nothing past the declaration", "");
    }

    // ---- NAE_QueryTrackInfo ------------------------------------------------
    {
        std::vector<unsigned char> storage(sizeof(NAE_TrackInfo) + 64, 0xAB);
        NAE_TrackInfo* info = reinterpret_cast<NAE_TrackInfo*>(storage.data());
        info->structSize = kMinimal;
        const int32_t rc = NAE_QueryTrackInfo(nullptr, info);
        check(rc == NAE_OK, "NAE_QueryTrackInfo accepts a null handle");
        check(info->structSize == kMinimal,
              "NAE_QueryTrackInfo honours the declared size",
              std::to_string(info->structSize) + " vs " + std::to_string(kMinimal));
    }

    // ---- NAE_QuerySpectrum -------------------------------------------------
    {
        std::vector<unsigned char> storage(sizeof(NAE_SpectrumInfo) + 64, 0xAB);
        NAE_SpectrumInfo* info = reinterpret_cast<NAE_SpectrumInfo*>(storage.data());
        info->struct_size = kMinimal;
        float bins[8] = {0};
        const int32_t rc = NAE_QuerySpectrum(nullptr, info, bins, 8);
        check(rc == NAE_OK, "NAE_QuerySpectrum accepts a null handle");
        check(info->struct_size == kMinimal,
              "NAE_QuerySpectrum reports the declared size",
              std::to_string(info->struct_size) + " vs " + std::to_string(kMinimal));
        bool clean = true;
        for (size_t i = kMinimal; i < storage.size(); ++i) {
            if (storage[i] != 0xAB) { clean = false; break; }
        }
        check(clean, "NAE_QuerySpectrum writes nothing past the declaration", "");

        // A null buffer with a non-zero capacity is a caller that thinks it
        // passed one; writing through it is a null dereference.
        check(NAE_QuerySpectrum(nullptr, info, nullptr, 8) == NAE_ERR_INVALID_ARGUMENT,
              "a null buffer with a capacity is refused", "");
        // Zero capacity with a null buffer is the documented shape query.
        check(NAE_QuerySpectrum(nullptr, info, nullptr, 0) == NAE_OK,
              "a null buffer with zero capacity is allowed", "");
    }

    // ---- NAE_QueryEnvelope -------------------------------------------------
    {
        std::vector<unsigned char> storage(sizeof(NAE_EnvelopeInfo) + 64, 0xAB);
        NAE_EnvelopeInfo* info = reinterpret_cast<NAE_EnvelopeInfo*>(storage.data());
        info->struct_size = kMinimal;
        float frames[8] = {0};
        const int32_t rc = NAE_QueryEnvelope(nullptr, info, frames, 8);
        check(rc == NAE_OK, "NAE_QueryEnvelope accepts a null handle");
        check(info->struct_size == kMinimal,
              "NAE_QueryEnvelope honours the declared size",
              std::to_string(info->struct_size) + " vs " + std::to_string(kMinimal));
    }

    // ---- NAE_ComputePeaks --------------------------------------------------
    {
        std::vector<unsigned char> storage(sizeof(NAE_PeaksInfo) + 64, 0xAB);
        NAE_PeaksInfo* info = reinterpret_cast<NAE_PeaksInfo*>(storage.data());
        info->struct_size = kMinimal;
        float peaks[4] = {0};
        const int32_t rc = NAE_ComputePeaks("no-such-file.wav", 2, info, peaks, 4);
        check(rc == NAE_ERR_CANNOT_OPEN, "NAE_ComputePeaks reports a missing file",
              std::to_string(rc));
        check(info->struct_size == kMinimal,
              "NAE_ComputePeaks honours the declared size",
              std::to_string(info->struct_size) + " vs " + std::to_string(kMinimal));
    }

    // ---- the counterpart: a zero declaration means "this build's size" ------
    //
    // The rule is not "always keep what was given". A caller that declares
    // nothing gets the current struct, which is what every harness here does.
    {
        NAE_Tags tags{};
        tags.structSize = 0;
        check(NAE_QueryTags(nullptr, &tags) == NAE_OK, "a zero declaration is accepted");
        check(tags.structSize == sizeof(NAE_Tags),
              "a zero declaration becomes this build's size",
              std::to_string(tags.structSize) + " vs " + std::to_string(sizeof(NAE_Tags)));
    }

    std::printf("\n%s\n", failures == 0 ? "All struct-size checks passed."
                                        : "Some struct-size checks FAILED.");
    return failures == 0 ? 0 : 1;
}
