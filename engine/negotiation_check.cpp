// Format negotiation, driven through a fake endpoint.
//
// # Why a fake, when there is a real device
//
// `rateOrder` and `depthOrder` are the entire behavioural content of the
// negotiation. Everything else is "call the device and take the first yes". On a
// machine with one sound card, the only way to observe those two functions is to
// find out what the card happens to accept -- which means the cases that matter
// most are the ones the card cannot produce. There is no endpoint anywhere that
// accepts 24-bit and nothing else, so the rule that Int24 and Int24In32 are the
// same samples would go untested on real hardware forever.
//
// A fake endpoint is not a mock of the code under test. It is a mock of the
// *device*, and the code under test is a pure function over an injected
// predicate. That is the same reason `output_mode.h` is header-only and
// `mode_check` links it directly rather than through the DLL.
//
// # What "no conversion" has to mean here
//
// Three of these tests exist only to pin §3.3, and they are the reason the
// integer passthrough path is worth building:
//
//   24-bit source, device offers only Int24In32   -> NOT a conversion
//   24-bit source, device offers both              -> compact Int24 wins
//   24-bit source, device offers only 16 or 32     -> IS a conversion
//
// A format comparison that forgets the Int24 case would report the first as a
// conversion, which puts a "your depth is wrong" message in front of a listener
// whose samples arrived intact in a different container.
//
// Usage: negotiation_check.exe

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wasapi_compat.hpp"
#include "exclusive_negotiation.h"

using namespace neiro;

namespace {

int g_fail = 0;
int g_pass = 0;

void check(const char* label, bool ok, const std::string& detail) {
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

std::string joinRates(const std::vector<uint32_t>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += " ";
        out += std::to_string(v[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The fake endpoint
// ---------------------------------------------------------------------------

/** One format the pretend device will open at. */
struct Accepts {
    uint32_t rate;
    uint16_t channels;
    uint16_t containerBits;
    uint16_t validBits;
    bool isFloat;
};

/** The HRESULT a real driver returns when it will not open a format. */
constexpr uint32_t kFormatNotSupported = 0x80070057; // E_INVALIDARG

/**
 * A device that accepts exactly the listed formats.
 *
 * Keyed on the wire fields rather than on the engine's `AudioFormat`, because
 * that is the level at which the device makes its decision -- and because a fake
 * keyed on the engine's own type would agree with the engine by construction,
 * which is the one thing a negotiation test must not do.
 */
struct FakeEndpoint {
    std::vector<Accepts> accepts;
    /** The first refusal returned, so a test can assert it is reported. */
    uint32_t firstRefusal = kFormatNotSupported;
    int calls = 0;

    // Takes the WireFormat rather than the extensible struct, because that is what
    // the negotiation hands it -- and because a fake keyed on the form would be
    // agreeing with the code under test about the one thing being tested.
    uint32_t operator()(const WireFormat& wire) {
        ++calls;
        const WAVEFORMATEX& f = *wire.forOpen();
        // A plain candidate has no separate valid-bit field, so the two are the
        // same number and a fake keyed on them must not silently accept an
        // extensible candidate as its plain equivalent.
        const uint16_t valid = wire.extensible ? wasapi::validBits(wire.extended)
                                               : f.wBitsPerSample;
        const bool isFloat = f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        for (const Accepts& a : accepts) {
            if (a.rate != f.nSamplesPerSec) continue;
            if (a.channels != f.nChannels) continue;
            if (a.containerBits != f.wBitsPerSample) continue;
            if (a.validBits != valid) continue;
            if (a.isFloat != isFloat) continue;
            return 0; // S_OK
        }
        return firstRefusal;
    }

    /** Whether the last candidate handed to this fake used the extensible form. */
    bool lastWasExtensible = false;
};

AudioFormat source(uint32_t rate, uint16_t depth, SampleFormat fmt, uint16_t channels = 2) {
    AudioFormat f;
    f.sampleRate = rate;
    f.channels = channels;
    f.bitDepth = depth;
    f.sampleFormat = fmt;
    return f;
}

/** A 24-bit-int source, which is the case §3.3 is entirely about. */
AudioFormat src24(uint32_t rate, uint16_t channels = 2) {
    return source(rate, 24, SampleFormat::Int24In32, channels);
}

std::string describe(const NegotiationResult& r) {
    if (!r.ok) return fmt("failed: %s", r.reason.c_str());
    return fmt("%u Hz / %u ch / %u-bit / %s%s", r.format.sampleRate,
               r.format.channels, r.format.bitDepth,
               sampleFormatName(r.format.sampleFormat),
               r.resampled ? "  (conversion)" : "  (no conversion)");
}

// ---------------------------------------------------------------------------

void test_rateOrder() {
    std::printf("\n  rate order\n");

    // 44.1 kHz source. The source's own rate, then its own family upward as exact
    // multiples, then everything else nearest-first. The 48-family rates come
    // *after* 1411.2 kHz, which is the part that matters: 88.2 is a whole-number
    // relationship to 44.1 and 48 is not, so 88.2 is the better conversion even
    // though 48 is closer in hertz.
    const auto o44 = rateOrder(44100);
    check("44.1k source tries its own multiples before the 48 family",
          joinRates(o44) == "44100 88200 176400 352800 705600 1411200 "
                            "48000 96000 192000 384000 768000 1536000",
          joinRates(o44));

    const auto o48 = rateOrder(48000);
    check("48k source mirrors it",
          joinRates(o48) == "48000 96000 192000 384000 768000 1536000 "
                            "44100 88200 176400 352800 705600 1411200",
          joinRates(o48));

    // 96 kHz is a 48-family rate with one family member below it, and then the
    // rest of the table nearest-first. Note that 88.2 -- a 44.1-family rate --
    // comes before 44.1 itself, because it is 7.8 kHz from 96 kHz and 44.1 is
    // 51.9 kHz away. Step 4 ranks by distance and does not care which family a
    // rate belongs to; only steps 2 and 3 are family-scoped.
    const auto o96 = rateOrder(96000);
    check("96k source tries its own family downward, then nearest first",
          joinRates(o96) == "96000 192000 384000 768000 1536000 "
                            "48000 88200 44100 176400 352800 705600 1411200",
          joinRates(o96));

    // A rate that is not on the table is still tried first. A 32 kHz file on a
    // device that opens at 32 kHz must not be resampled to reach an entry in
    // somebody's list.
    const auto o32 = rateOrder(32000);
    check("an off-table source rate is still tried first",
          !o32.empty() && o32[0] == 32000, joinRates(o32));

    // Every rate appears exactly once: a duplicate would re-ask a question the
    // device already refused.
    std::vector<uint32_t> seen;
    for (uint32_t r : o44) {
        for (uint32_t s : seen) {
            if (s == r) {
                check("rate order has no duplicates", false,
                      std::to_string(r) + " appears twice");
                return;
            }
        }
        seen.push_back(r);
    }
    check("rate order has no duplicates", seen.size() == o44.size(),
          fmt("%zu entries", seen.size()));
}

void test_depthOrder() {
    std::printf("\n  depth order\n");
    // Source depth first, then away from it. Widening before narrowing matters:
    // a device that cannot do 24 will very often do 32, and 32 is a lossless
    // container for 24 valid bits.
    const auto d16 = depthOrder(16);
    const auto d24 = depthOrder(24);
    const auto d32 = depthOrder(32);
    check("16-bit tries wider before narrower", d16.size() == 3 && d16[0] == 16 && d16[1] == 24 && d16[2] == 32,
          fmt("%zu %u %u %u", d16.size(), d16[0], d16[1], d16[2]));
    check("24-bit tries 24, 32, 16", d24[0] == 24 && d24[1] == 32 && d24[2] == 16,
          fmt("%u %u %u", d24[0], d24[1], d24[2]));
    check("32-bit tries narrower only", d32[0] == 32 && d32[1] == 24 && d32[2] == 16,
          fmt("%u %u %u", d32[0], d32[1], d32[2]));

    // 20-bit is not a depth anything accepts; it normalises to 24 and must be
    // tried as 24.
    const auto d20 = depthOrder(20);
    check("20-bit normalises to 24 and is tried first", d20[0] == 24,
          fmt("%u %u %u", d20[0], d20[1], d20[2]));
}

void test_int24_is_not_a_conversion() {
    std::printf("\n  24-bit in two containers\n");

    // The device offers Int24In32 and nothing else at the source rate. Same
    // integer samples, different container. This is the case §3.3 exists for.
    FakeEndpoint onlyIn32;
    onlyIn32.accepts = {{48000, 2, 32, 24, false}};
    const auto r1 = negotiateExclusive(src24(48000), 0, onlyIn32);
    check("24-bit -> Int24In32 is not a conversion",
          r1.ok && !r1.resampled && r1.format.sampleFormat == SampleFormat::Int24In32,
          describe(r1));

    // Both offered: compact first, because it moves half the bytes for the same
    // values.
    FakeEndpoint both;
    both.accepts = {{48000, 2, 24, 24, false}, {48000, 2, 32, 24, false}};
    const auto r2 = negotiateExclusive(src24(48000), 0, both);
    check("24-bit prefers the compact Int24 container",
          r2.ok && r2.format.sampleFormat == SampleFormat::Int24,
          describe(r2));

    // Down to 16 or up to 32: the sample values change, so it is a conversion.
    FakeEndpoint only16;
    only16.accepts = {{48000, 2, 16, 16, false}};
    const auto r3 = negotiateExclusive(src24(48000), 0, only16);
    check("24-bit -> 16-bit is a conversion", r3.ok && r3.resampled, describe(r3));

    FakeEndpoint only32;
    only32.accepts = {{48000, 2, 32, 32, false}};
    const auto r4 = negotiateExclusive(src24(48000), 0, only32);
    check("24-bit -> 32 valid bits is a conversion", r4.ok && r4.resampled, describe(r4));

    // The wire format itself, because getting these fields the wrong way round
    // produces a format a driver accepts and a stream that is wrong. `r1` is the
    // Int24In32 case and `r2` the compact one -- asserted against the result that
    // actually carries each container.
    //
    // `nBlockAlign` is bytes per *frame*, so stereo doubles the per-sample figure:
    // 4 bytes/sample in a 4-byte container is 8 per frame, and 3 is 6. Asserting
    // the per-sample number here is a mistake worth recording, because the
    // resulting failure looks like a packing bug in the engine and is actually a
    // wrong expectation in the test.
    const auto wfx = buildCandidate(r1.format);
    check("stereo Int24In32: 32 container bits, 24 valid, 8 bytes per frame",
          wfx.Format.wBitsPerSample == 32 && wasapi::validBits(wfx) == 24 &&
              wfx.Format.nBlockAlign == 8,
          fmt("bits=%u valid=%u align=%u", wfx.Format.wBitsPerSample,
              wasapi::validBits(wfx), wfx.Format.nBlockAlign));

    const auto wfx24 = buildCandidate(r2.format);
    check("stereo Int24: 24 container bits, 24 valid, 6 bytes per frame",
          wfx24.Format.wBitsPerSample == 24 && wasapi::validBits(wfx24) == 24 &&
              wfx24.Format.nBlockAlign == 6,
          fmt("bits=%u valid=%u align=%u", wfx24.Format.wBitsPerSample,
              wasapi::validBits(wfx24), wfx24.Format.nBlockAlign));

    // Mono is where the two numbers come apart cleanly: same per-sample widths,
    // block align equal to them.
    AudioFormat mono = r1.format;
    mono.channels = 1;
    const auto wfxMono = buildCandidate(mono);
    check("mono Int24In32 has a 4-byte frame",
          wfxMono.Format.nBlockAlign == 4, fmt("align=%u", wfxMono.Format.nBlockAlign));

    // Float never shares a payload with an integer format, so Int24 -> Float32
    // must not inherit the equivalence above.
    check("24-bit int and float are never the same payload",
          !sameSourceFormat(src24(48000), source(48000, 24, SampleFormat::Float32)),
          "correctly distinct");
}

void test_rate_choice() {
    std::printf("\n  which rate is chosen\n");

    // The test that separates §3.2 from "nearest". 88.2 kHz is exactly twice
    // 44.1; 48 kHz is closer in hertz by 3.9 kHz. A nearest-rate rule picks 48 and
    // the source-first rule picks 88.2, and the whole reason the ordering exists
    // is that it should be 88.2.
    FakeEndpoint familyFirst;
    familyFirst.accepts = {
        {48000, 2, 32, 24, false},
        {88200, 2, 32, 24, false},
        {96000, 2, 32, 24, false},
    };
    const auto r1 = negotiateExclusive(src24(44100), 0, familyFirst);
    check("44.1k on {48, 88.2, 96} picks 88.2, not the nearer 48",
          r1.ok && r1.format.sampleRate == 88200, describe(r1));

    // With no same-family option, it falls to the nearest cross-family rate.
    FakeEndpoint noFamily;
    noFamily.accepts = {
        {48000, 2, 32, 24, false},
        {96000, 2, 32, 24, false},
        {192000, 2, 32, 24, false},
    };
    const auto r2 = negotiateExclusive(src24(44100), 0, noFamily);
    check("44.1k on {48, 96, 192} picks 48", r2.ok && r2.format.sampleRate == 48000,
          describe(r2));

    // Source rate accepted: nothing is converted at all.
    FakeEndpoint native;
    native.accepts = {{44100, 2, 32, 24, false}, {48000, 2, 32, 24, false}};
    const auto r3 = negotiateExclusive(src24(44100), 0, native);
    check("44.1k accepted as-is is not a conversion", r3.ok && !r3.resampled &&
          r3.format.sampleRate == 44100, describe(r3));

    // A DSD-shaped source rate with the device stopping at 192 kHz. Under §3.2 the
    // same family *downward* is tried before crossing families, so 176.4 kHz --
    // exactly half of 352.8 -- is tried before 192 kHz. Whether it is chosen
    // depends on the device, which is why this asserts the attempt order rather
    // than the outcome.
    FakeEndpoint dsdish;
    dsdish.accepts = {{176400, 2, 32, 24, false}, {192000, 2, 32, 24, false}};
    const auto r4 = negotiateExclusive(src24(352800), 0, dsdish);
    const bool tried176Before192 = [&] {
        for (const Attempt& a : r4.attempts) {
            if (a.format.sampleRate == 176400) return true;
        }
        return false;
    }();
    check("352.8k tries 176.4 (half of itself) before 192",
          tried176Before192, describe(r4));
}

void test_source_format_preference() {
    std::printf("\n  32-bit container order\n");

    // A float source prefers float, an integer source prefers integer. This is
    // the one place the candidate order carries information about the source
    // rather than about the device.
    FakeEndpoint both;
    both.accepts = {
        {48000, 2, 32, 32, false},
        {48000, 2, 32, 32, true},
    };
    const auto rf = negotiateExclusive(source(48000, 32, SampleFormat::Float32), 0, both);
    check("a float source prefers Float32", rf.ok &&
          rf.format.sampleFormat == SampleFormat::Float32, describe(rf));

    const auto ri = negotiateExclusive(source(48000, 32, SampleFormat::Int32), 0, both);
    check("an integer source prefers Int32", ri.ok &&
          ri.format.sampleFormat == SampleFormat::Int32, describe(ri));

    // Float source, integer-only device: a conversion, and it must say so.
    FakeEndpoint intOnly;
    intOnly.accepts = {{48000, 2, 32, 32, false}};
    const auto rc = negotiateExclusive(source(48000, 32, SampleFormat::Float32), 0, intOnly);
    check("float source onto an integer device is a conversion", rc.ok && rc.resampled,
          describe(rc));
}

void test_channels_are_not_negotiated() {
    std::printf("\n  channel count\n");

    // §3.1: the channel count is never negotiated. A 6-channel source on a device
    // that only opens at 2 must fail and say that, not quietly become stereo --
    // downmixing is a decision about the music and it does not belong in the layer
    // that opens devices.
    FakeEndpoint stereoOnly;
    stereoOnly.accepts = {{48000, 2, 32, 24, false}};
    const auto r = negotiateExclusive(src24(48000, 6), 0, stereoOnly);
    check("6ch on a 2ch-only device fails rather than downmixing", !r.ok, describe(r));
    check("the failure names the channel count",
          r.reason.find("channel") != std::string::npos, r.reason);

    // 5.1 offered at 6 channels must be accepted, with the right speaker mask.
    FakeEndpoint fiveOne;
    fiveOne.accepts = {{48000, 6, 32, 24, false}};
    const auto r2 = negotiateExclusive(src24(48000, 6), 0, fiveOne);
    check("6ch is opened at 6 channels", r2.ok && r2.format.channels == 6, describe(r2));
    if (r2.ok) {
        const DWORD mask = buildCandidate(r2.format).dwChannelMask;
        check("6ch uses the 5.1 speaker mask",
              mask == (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                       SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT),
              fmt("mask=0x%08lX", static_cast<unsigned long>(mask)));
    }
}

void test_failure_is_diagnosable() {
    std::printf("\n  why it failed\n");

    // Nothing works. The reason must name a dimension and carry the first
    // HRESULT, because "exclusive format open/init failure" sends a listener
    // nowhere and a support request nowhere either.
    FakeEndpoint nothing;
    nothing.firstRefusal = 0x88880004;
    const auto r = negotiateExclusive(src24(44100), 0, nothing);
    check("an empty device fails", !r.ok, describe(r));
    check("the reason names the source rate",
          r.reason.find("44100") != std::string::npos, r.reason);
    check("the first HRESULT is reported", r.firstFailure == 0x88880004,
          fmt("0x%08lX", static_cast<unsigned long>(r.firstFailure)));
    check("every attempt is retained for the capability table", !r.attempts.empty(),
          fmt("%zu attempts", r.attempts.size()));

    // The rate was offered and refused, which is a different fact from the rate
    // never being offered -- and only one of them is worth telling a listener.
    check("the source rate is recorded as tried", r.sawSourceRate, "yes");
    check("the source rate is recorded as refused", r.rejectedSourceRate, "yes");

    // A device that accepts a rate but not at any depth the source wants: the
    // depth is the dimension that failed.
    FakeEndpoint shallow;
    shallow.accepts = {{44100, 2, 16, 16, false}};
    const auto r2 = negotiateExclusive(src24(44100), 0, shallow);
    check("a device with only 16-bit still opens, reporting a conversion",
          r2.ok && r2.resampled && r2.format.bitDepth == 16, describe(r2));
}

void test_mode_is_a_preference_not_a_constraint() {
    std::printf("\n  output modes as a first choice\n");

    // Device mode: the endpoint's own rate goes first, ahead of the source rate.
    // This is the mode that used to be the only behaviour, and it is now one
    // candidate rather than the answer.
    FakeEndpoint wide;
    wide.accepts = {
        {44100, 2, 32, 24, false},
        {192000, 2, 32, 24, false},
    };
    const auto device = negotiateExclusive(src24(44100), 192000, wide);
    check("Device mode tries the endpoint's own rate first",
          device.ok && device.format.sampleRate == 192000, describe(device));

    // Native mode: no preference, so the source's own rate leads.
    const auto nativeMode = negotiateExclusive(src24(44100), 0, wide);
    check("Native mode leaves the source rate first",
          nativeMode.ok && nativeMode.format.sampleRate == 44100, describe(nativeMode));

    // The load-bearing case. A mode is a preference, so a Fixed rate the endpoint
    // will not open must fall through to something that plays -- and say it
    // converted. A mode treated as a constraint would refuse the file, and a
    // player that refuses music is not a player with a preference.
    FakeEndpoint noFixed;
    noFixed.accepts = {{88200, 2, 32, 24, false}};
    const auto fixed = negotiateExclusive(src24(44100), 96000, noFixed);
    check("an unavailable Fixed rate falls through instead of refusing",
          fixed.ok && fixed.format.sampleRate == 88200 && fixed.resampled,
          describe(fixed));

    const auto formOf = [](uint32_t rate, uint16_t depth, SampleFormat sf, uint16_t ch) {
        return fmt("%u Hz %u ch %u-bit %s%s", rate, ch, depth, sampleFormatName(sf),
                   needsExtensible(source(rate, depth, sf, ch)) ? " extensible" : " plain");
    };
    check("2ch 16-bit int is a plain format",
          !needsExtensible(source(48000, 16, SampleFormat::Int16, 2)),
          formOf(48000, 16, SampleFormat::Int16, 2));
    check("2ch 32-bit int is a plain format",
          !needsExtensible(source(48000, 32, SampleFormat::Int32, 2)),
          formOf(48000, 32, SampleFormat::Int32, 2));
    check("2ch float is a plain format",
          !needsExtensible(source(48000, 32, SampleFormat::Float32, 2)),
          formOf(48000, 32, SampleFormat::Float32, 2));
    // 24-in-32 is the case the extension exists for: the valid width is narrower
    // than the container and no plain struct can say so.
    check("2ch 24-in-32 is extensible",
          needsExtensible(source(48000, 24, SampleFormat::Int24In32, 2)),
          formOf(48000, 24, SampleFormat::Int24In32, 2));
    check("6ch is extensible, for the speaker mask",
          needsExtensible(source(48000, 16, SampleFormat::Int16, 6)),
          formOf(48000, 16, SampleFormat::Int16, 6));

    // And the plain form carries the same numbers as the extensible one would, so
    // the two cannot drift.
    const AudioFormat plain16 = source(48000, 16, SampleFormat::Int16, 2);
    const WireFormat w16 = buildWireFormat(plain16);
    check("the plain form is tagged PCM and sized from the format",
          !w16.extensible && w16.plain.wFormatTag == WAVE_FORMAT_PCM &&
              w16.plain.nBlockAlign == 4 && w16.plain.nAvgBytesPerSec == 48000 * 4,
          fmt("tag=0x%04X align=%u avg=%lu", w16.plain.wFormatTag, w16.plain.nBlockAlign,
              static_cast<unsigned long>(w16.plain.nAvgBytesPerSec)));

    // A device that only accepts the plain form must be reachable at all, which is
    // the whole point: with every candidate built as WAVEFORMATEXTENSIBLE, this
    // endpoint supported nothing.
    FakeEndpoint plainOnly;
    plainOnly.accepts = {{192000, 2, 16, 16, false}};
    const auto viaPlain = negotiateExclusive(source(192000, 16, SampleFormat::Int16, 2), 0,
                                             plainOnly);
    check("a device that only takes plain formats is usable",
          viaPlain.ok && viaPlain.format.sampleRate == 192000, describe(viaPlain));
}

void test_preferred_rate_is_moved_not_duplicated() {
    // The only way to see the difference between moving a candidate and adding one
    // is a device that refuses everything,
    // because then the whole table is walked and the result is a count of the
    // order rather than of where the search happened to stop.
    //
    // Counted in distinct rates, not in attempts. Each rate is tried against five
    // depth-and-container combinations -- 16/Int16, 24/Int24, 24/Int24In32,
    // 32/Int32, 32/Float32 -- so an extra rate is five extra attempts, and an
    // assertion written in attempts silently encodes that fan-out as if it were
    // the thing under test.
    const auto distinctRates = [](const NegotiationResult& r) {
        std::vector<uint32_t> seen;
        for (const Attempt& a : r.attempts) {
            if (std::find(seen.begin(), seen.end(), a.format.sampleRate) == seen.end()) {
                seen.push_back(a.format.sampleRate);
            }
        }
        return seen;
    };

    FakeEndpoint refusesAll;
    const auto plain = negotiateExclusive(src24(44100), 0, refusesAll);
    const auto movedIn = negotiateExclusive(src24(44100), 192000, refusesAll);
    const auto added = negotiateExclusive(src24(44100), 32000, refusesAll);
    check("a preferred rate from the table is moved, not duplicated",
          distinctRates(movedIn).size() == distinctRates(plain).size(),
          fmt("%zu vs %zu rates", distinctRates(movedIn).size(),
              distinctRates(plain).size()));
    check("a preferred rate outside the table is one extra candidate",
          distinctRates(added).size() == distinctRates(plain).size() + 1,
          fmt("%zu vs %zu rates", distinctRates(added).size(),
              distinctRates(plain).size()));
    check("and it is asked first",
          !added.attempts.empty() && added.attempts.front().format.sampleRate == 32000,
          std::to_string(added.attempts.empty()
                             ? 0
                             : added.attempts.front().format.sampleRate));
}

} // namespace

int main() {
    std::printf("exclusive format negotiation\n");

    test_rateOrder();
    test_depthOrder();
    test_int24_is_not_a_conversion();
    test_rate_choice();
    test_source_format_preference();
    test_channels_are_not_negotiated();
    test_failure_is_diagnosable();
    test_mode_is_a_preference_not_a_constraint();
    test_preferred_rate_is_moved_not_duplicated();

    std::printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
