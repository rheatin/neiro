// Exclusive-mode format negotiation.
//
// # The contract this file exists to satisfy
//
// The backend is the authority on format. The pipeline *requests*; the backend
// negotiates and returns what it actually got; the decoder is then configured
// from the return value and never from the request. A pipeline that configured
// itself from what it asked for would be a pipeline that believes it is playing
// 44.1 kHz on a device it actually opened at 192 kHz.
//
// So everything here is a pure function over an injected `isSupported` predicate.
// There is no COM in this file, which is not a purity exercise: it is what makes
// the ordering in `rateOrder` and `depthOrder` testable. Those two functions are
// the entire behavioural content of the negotiation, and on a machine with one
// sound card they can only be observed by finding out what the card happens to
// accept. `negotiation_check.cpp` drives them through a fake endpoint and
// asserts the order, including the cases a real card will not produce.
//
// # What "resampled" means here, and why it is not what you would guess
//
// §3.3: a candidate counts as a conversion unless the source and the output agree
// on rate, channels, normalised depth, and sample format -- *or* differ only in
// container while carrying the same integer payload. So:
//
//   24-bit source onto a device that only does Int24In32   -> not a conversion
//   24-bit source onto 16-bit, or onto 32-bit               -> a conversion
//   any rate change                                          -> a conversion
//
// The Int24 case is the one worth arguing for. Int24 (3 packed bytes) and
// Int24In32 (4 bytes, 24 valid) hold the *same integer samples*. The device
// receives identical values in a different container. Reporting a conversion
// there would make the reason code say the engine altered the signal when it did
// not touch it, and would put a listener in front of a control that cannot change
// the outcome.

#pragma once

#include <windows.h>

// WAVEFORMATEXTENSIBLE, WAVE_FORMAT_EXTENSIBLE, KSDATAFORMAT_SUBTYPE_*, and the
// SPEAKER_* masks all live in ksmedia.h.
//
// Not mmregapi.h, which is where they are on the real SDK: MinGW-w64 does not
// ship that header at all, and the build here is MinGW. ksmedia.h is what
// mmregapi.h includes on MSVC, so naming it directly compiles under both and
// needs no conditional.
#include <ksmedia.h>
#include <audioclient.h>

#include "wasapi_compat.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace neiro {

/** The container the samples are written in. See `containerBits` for each. */
enum class SampleFormat : int32_t {
    Int16 = 0,     // 16 in 16
    Int24 = 1,     // 24 in 3 packed bytes
    Int24In32 = 2, // 24 valid in a 4-byte container
    Int32 = 3,     // 32 in 32
    Float32 = 4,   // 32-bit IEEE float
};

/** A format as the pipeline and the backend both talk about it. */
struct AudioFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    /** Valid bits, not container bits. 24 for both Int24 and Int24In32. */
    uint16_t bitDepth = 0;
    SampleFormat sampleFormat = SampleFormat::Int16;
};

inline const char* sampleFormatName(SampleFormat f) {
    switch (f) {
        case SampleFormat::Int16: return "Int16";
        case SampleFormat::Int24: return "Int24";
        case SampleFormat::Int24In32: return "Int24In32";
        case SampleFormat::Int32: return "Int32";
        case SampleFormat::Float32: return "Float32";
    }
    return "?";
}

/** Bytes each sample of one channel occupies in the wire format. */
inline uint16_t containerBytes(SampleFormat f) {
    switch (f) {
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int24: return 3;
        case SampleFormat::Int24In32: return 4;
        case SampleFormat::Int32: return 4;
        case SampleFormat::Float32: return 4;
    }
    return 0;
}

/** The bit depth the engine normalises everything to before comparing. */
inline uint16_t normaliseDepth(uint16_t bitDepth) {
    if (bitDepth <= 16) return 16;
    if (bitDepth <= 24) return 24;
    return 32;
}

// The rates a device is asked about, ascending.
//
// Twelve, and the list is fixed rather than discovered. A device with a rate
// outside it -- 32 kHz, 8 kHz, 384 kHz -- is not asked, which is a deliberate
// narrowing: these are the rates music is actually delivered at, and asking a
// driver about a thousand combinations is how a negotiation turns into a stall.
// The source's own rate is still tried first even when it is not on this list,
// because a file at 32 kHz on a device that opens at 32 kHz should not be
// resampled just to reach an entry in somebody's table.
inline constexpr uint32_t kSupportedRates[] = {
    44100, 48000, 88200, 96000, 176400, 192000, 352800,
    384000, 705600, 768000, 1411200, 1536000,
};
inline constexpr size_t kSupportedRateCount = sizeof(kSupportedRates) / sizeof(kSupportedRates[0]);

/** Which of the two standard families a rate belongs to, or 0 for neither. */
inline uint32_t rateFamily(uint32_t rate) {
    if (rate != 0 && rate % 44100 == 0) return 44100;
    if (rate != 0 && rate % 48000 == 0) return 48000;
    return 0;
}

/**
 * The order to try rates in, per §3.2.
 *
 *   1. the source's own rate
 *   2. same family, higher than the source, ascending
 *   3. same family, lower than the source, descending
 *   4. everything else, by distance from the source, ties going higher
 *
 * The point of 2 and 3 is that a rate the device can reach *by doubling* is a
 * better answer than a nearby unrelated one. A 44.1 kHz file on an endpoint that
 * refuses 44.1 but opens at 88.2 should land at 88.2, not at 48: 88.2 is an
 * exact multiple, 48 is a conversion either way, and the ratio an ear hears as
 * the quality of a resample is the one that stays a whole number. This ordering
 * is also why the engine does not "default to the device's rate" -- that is the
 * step-4 behaviour wearing step-1's clothes, and it converts a 44.1 kHz CD to
 * 192 kHz on a device that would have played 88.2 without complaint.
 *
 * Step 4's tie-break matters and is easy to lose: 44.1 kHz sits 441 Hz from
 * 44100 and 390 Hz from 48000, so those are not the case; but 88.2 kHz is 0 from
 * 88200 and 176.4 kHz from 88200... the real tie is a source at 72 kHz, which is
 * equidistant from nothing here, and a source at 96000/2. Without the tie-break
 * the order would depend on the table's order, which is not a property anyone
 * chose.
 */
inline std::vector<uint32_t> rateOrder(uint32_t sourceRate) {
    std::vector<uint32_t> out;
    out.reserve(kSupportedRateCount + 1);

    const uint32_t family = rateFamily(sourceRate);

    // 1. The source itself, even when it is not a table entry.
    if (sourceRate != 0) out.push_back(sourceRate);

    // 2. Same family, higher, ascending.
    if (family != 0) {
        for (size_t i = 0; i < kSupportedRateCount; ++i) {
            const uint32_t r = kSupportedRates[i];
            if (r > sourceRate && rateFamily(r) == family) out.push_back(r);
        }
    }

    // 3. Same family, lower, descending.
    if (family != 0) {
        for (size_t i = kSupportedRateCount; i-- > 0;) {
            const uint32_t r = kSupportedRates[i];
            if (r < sourceRate && rateFamily(r) == family) out.push_back(r);
        }
    }

    // 4. The rest, nearest first, ties going higher.
    std::vector<std::pair<uint32_t, uint32_t>> rest; // (distance, rate)
    for (size_t i = 0; i < kSupportedRateCount; ++i) {
        const uint32_t r = kSupportedRates[i];
        if (std::find(out.begin(), out.end(), r) != out.end()) continue;
        const uint32_t distance = r > sourceRate ? r - sourceRate : sourceRate - r;
        rest.emplace_back(distance, r);
    }
    std::sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second > b.second; // ties: the higher rate wins
    });
    for (const auto& entry : rest) out.push_back(entry.second);

    return out;
}

/** The order to try bit depths in, per §3.2. Never wider than three. */
inline std::vector<uint16_t> depthOrder(uint16_t sourceBitDepth) {
    switch (normaliseDepth(sourceBitDepth)) {
        case 16: return {16, 24, 32};
        case 24: return {24, 32, 16};
        default: return {32, 24, 16};
    }
}

/**
 * The container variants for one depth, per §3.2.
 *
 * 24 gets two, packed first. Compact is tried first because it moves half the
 * bytes for the identical sample values, and a device that accepts both is
 * telling us it can do either.
 *
 * 32 gets two in an order that depends on the source: a float source prefers
 * float, and an integer source prefers integer. This is the one place the
 * candidate order carries information about the *source* rather than about the
 * device -- everything else is ordered by how likely the device is to accept it.
 */
inline std::vector<SampleFormat> variants(uint16_t depth, SampleFormat sourceFormat) {
    switch (normaliseDepth(depth)) {
        case 16: return {SampleFormat::Int16};
        case 24: return {SampleFormat::Int24, SampleFormat::Int24In32};
        default:
            return sourceFormat == SampleFormat::Float32
                ? std::vector<SampleFormat>{SampleFormat::Float32, SampleFormat::Int32}
                : std::vector<SampleFormat>{SampleFormat::Int32, SampleFormat::Float32};
    }
}

/** The default speaker mask for a channel count, per §3.2. */
inline DWORD channelMask(uint16_t channels) {
    switch (channels) {
        case 1: return SPEAKER_FRONT_CENTER;
        case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                    SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 6: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                    SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                    SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        case 8: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                    SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                    SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                    SPEAKER_TOP_FRONT_LEFT | SPEAKER_TOP_FRONT_RIGHT;
        default: return 0;
    }
}

/**
 * Build the WAVEFORMATEXTENSIBLE for a candidate.
 *
 * `wBitsPerSample` is the *container* width and `wValidBitsPerSample` is the valid
 * width, which for Int24In32 is where the two differ. Getting them the wrong way
 * round produces a format a driver will accept and a stream that is wrong, so they
 * are set from separate places rather than from one number.
 */
inline WAVEFORMATEXTENSIBLE buildCandidate(const AudioFormat& f) {
    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = f.channels;
    wfx.Format.nSamplesPerSec = f.sampleRate;
    wfx.Format.wBitsPerSample = static_cast<WORD>(containerBytes(f.sampleFormat) * 8);
    // On WAVEFORMATEXTENSIBLE itself, not on its nested WAVEFORMATEX. The valid
    // width is a property of the *extension*, which is the whole reason the
    // extensible struct exists; there is nowhere for it to live in a plain
    // WAVEFORMATEX.
    wasapi::validBits(wfx) = static_cast<WORD>(f.bitDepth);
    wfx.Format.nBlockAlign =
        static_cast<WORD>(containerBytes(f.sampleFormat) * f.channels);
    wfx.Format.nAvgBytesPerSec =
        wfx.Format.nSamplesPerSec * static_cast<DWORD>(wfx.Format.nBlockAlign);
    wfx.dwChannelMask = channelMask(f.channels);
    if (f.sampleFormat == SampleFormat::Float32) {
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else {
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    return wfx;
}

/**
 * A candidate in the form it should actually be handed to the device.
 *
 * # Why this is not just a WAVEFORMATEXTENSIBLE
 *
 * The spec's §3.2 says to build a WAVEFORMATEXTENSIBLE for every candidate, and for
 * a candidate that carries a valid-bit count or more than two channels there is no
 * alternative. For everything else it is the *wrong* form, and measurably so.
 *
 * Measured on the endpoint this build runs against, an HDMI receiver:
 *
 *     44100 Hz  2ch 16-bit  plain WAVEFORMATEX   S_OK            opened
 *     44100 Hz  2ch 16-bit  WAVEFORMATEXTENSIBLE 0x80070057      E_INVALIDARG
 *     192000 Hz 2ch 16-bit  plain WAVEFORMATEX   S_OK            opened
 *     192000 Hz 2ch 16-bit  WAVEFORMATEXTENSIBLE 0x80070057      E_INVALIDARG
 *
 * The device refuses the extensible form at every rate, so a negotiation that only
 * ever builds it finds nothing and reports that the endpoint supports no format at
 * all -- which reads as a hardware limit and is a bug in the question.
 *
 * The reason is not the driver being fussy. WAVEFORMATEXTENSIBLE exists to carry
 * what a plain WAVEFORMATEX cannot: a valid-bit count narrower than the container,
 * a speaker mask, a subformat GUID. For two channels of integer PCM at full
 * container width it carries nothing the plain form does not, and a superset
 * structure presented where a subset was expected is a structure the driver has no
 * obligation to understand.
 *
 * So the form is chosen by whether the format actually needs it.
 */
struct WireFormat {
    /** Always populated. What to pass when the plain form is correct. */
    WAVEFORMATEX plain{};
    /** Alias of `plain`, so a caller holding a WAVEFORMATEXTENSIBLE-shaped argument
        can be handed the same object without a second branch at every site. */
    WAVEFORMATEXTENSIBLE built{};
    /** Populated only when `extensible` is true. */
    WAVEFORMATEXTENSIBLE extended{};
    bool extensible = false;

    /** The pointer to hand to IsFormatSupported and to Initialize. */
    const WAVEFORMATEX* forOpen() const {
        return extensible ? &extended.Format : &plain;
    }
};

/**
 * Whether a format needs the extensible structure at all.
 *
 * It needs it when the valid-bit count is narrower than the container -- 24 bits in
 * a 32-bit container, which is the case the whole Int24In32 distinction is about --
 * or when there are more than two channels, where the speaker mask carries
 * information the channel count alone does not. It does not need it for two
 * channels of integer PCM or float at full container width.
 */
inline bool needsExtensible(const AudioFormat& f) {
    // More than two channels: the speaker mask carries information the channel
    // count alone does not, and a 5.1 stream described as "6 channels" is not a
    // description of where the channels go.
    if (f.channels > 2) return true;

    // Packed 24-bit, always.
    //
    // Not a preference and not a "the driver might be fussy" case. A plain
    // WAVEFORMATEX has exactly one lever for width, `wBitsPerSample`, and setting
    // it to 24 says "24 bits per sample in a 24-bit sample" -- which is not a
    // format Windows defines. There is no such thing as 24-bit-in-3-bytes in the
    // plain form; the only way to say it is wValidBitsPerSample, which lives in the
    // extension.
    //
    // Measured here: the negotiation offered this candidate, the device accepted the
    // query, and Initialize answered E_INVALIDARG -- `exclusive mode refused
    // (0x80070057): 192000 Hz, 24-bit 2ch`. A candidate the search was willing to
    // pick and the device would not open is worse than one never offered, because
    // it turns a format question into a playback failure.
    if (f.sampleFormat == SampleFormat::Int24) return true;

    // 24 valid bits in a 32-bit container: the case the extension was made for.
    const uint16_t containerBits =
        static_cast<uint16_t>(containerBytes(f.sampleFormat) * 8);
    return f.bitDepth < containerBits;
}

/** Build the candidate in whichever form the device will understand. */
inline WireFormat buildWireFormat(const AudioFormat& f) {
    WireFormat wire;

    // The plain form first, from the same fields, so the two cannot disagree about
    // block align or average byte rate.
    wire.plain.wFormatTag = (f.sampleFormat == SampleFormat::Float32)
                                ? WAVE_FORMAT_IEEE_FLOAT
                                : WAVE_FORMAT_PCM;
    wire.plain.nChannels = f.channels;
    wire.plain.nSamplesPerSec = f.sampleRate;
    wire.plain.wBitsPerSample = static_cast<WORD>(containerBytes(f.sampleFormat) * 8);
    wire.plain.nBlockAlign =
        static_cast<WORD>(containerBytes(f.sampleFormat) * f.channels);
    wire.plain.nAvgBytesPerSec = wire.plain.nSamplesPerSec *
                                 static_cast<DWORD>(wire.plain.nBlockAlign);

    // `extended` is filled unconditionally, not only when the format needs it.
    //
    // A caller that wants the extensible object for any other reason -- a
    // diagnostic comparing the two forms, for instance -- should not have to ask
    // whether this particular candidate happened to require it. The form that gets
    // *opened* is `forOpen()`; this is only the data.
    wire.extended = buildCandidate(f);
    std::memcpy(&wire.built, &wire.extended, sizeof(WAVEFORMATEXTENSIBLE));
    wire.extensible = needsExtensible(f);
    return wire;
}


/**
 * Whether two formats carry the same samples, per §3.3.
 *
 * The Int24/Int24In32 equivalence is the whole content of this function and the
 * reason it exists separately from a field-by-field comparison.
 */
inline bool sameSourceFormat(const AudioFormat& a, const AudioFormat& b) {
    if (a.sampleRate != b.sampleRate) return false;
    if (a.channels != b.channels) return false;
    if (normaliseDepth(a.bitDepth) != normaliseDepth(b.bitDepth)) return false;

    if (a.sampleFormat == b.sampleFormat) return true;

    // Same integer payload, different container.
    const bool aInt = a.sampleFormat != SampleFormat::Float32;
    const bool bInt = b.sampleFormat != SampleFormat::Float32;
    if (!aInt || !bInt) return false;
    // Only 24-bit integers have two containers. Int16 and Int32 have one each, so
    // reaching here with different names already means 24-bit.
    return normaliseDepth(a.bitDepth) == 24 && normaliseDepth(b.bitDepth) == 24;
}

/** One call to the device, kept so the whole search can be printed. */
struct Attempt {
    AudioFormat format;
    uint32_t hresult = 0;
    bool supported = false;
};

/** What the negotiation concluded, and why. */
struct NegotiationResult {
    bool ok = false;
    /** The format the device accepted. Meaningless when `ok` is false. */
    AudioFormat format;
    /** Per §3.3. False when the accepted format carries the source unchanged. */
    bool resampled = true;

    /** Every candidate tried, in order, with its HRESULT. */
    std::vector<Attempt> attempts;

    /** §3.4. Names the dimension that was rejected rather than "failed". */
    std::string reason;
    /** The first HRESULT the device returned, hex. 0 when it returned S_OK. */
    uint32_t firstFailure = 0;

    // Which dimensions were ever offered a candidate at all, and which were
    // offered and refused. These are what turn "it failed" into "the depth was
    // refused": a dimension that was never tried cannot have been refused, and
    // reporting it as refused sends the listener to a control that was never
    // consulted.
    bool sawSourceRate = false;
    bool rejectedSourceRate = false;
    bool rejectedChannels = false;
    bool sawDepth = false;
    bool rejectedDepth = false;
    bool sawSampleFormat = false;
    bool rejectedSampleFormat = false;
};

/**
 * Try candidates in §3.2's order and take the first the device accepts.
 *
 * `preferredFirstRate` moves one rate to the front of the order. It is how Neiro's
 * three output modes survive the move to source-first ordering without being
 * thrown away:
 *
 *   Native -> 0, which leaves the order exactly as the spec states it
 *   Device -> the endpoint's own rate
 *   Fixed  -> the rate the listener chose
 *
 * A mode here is a *preference about which candidate to try first*, not a
 * constraint. That distinction is the whole reason the three modes can coexist
 * with a source-first search: a listener who picks "Fixed 96 kHz" and then plays a
 * file the endpoint cannot open at 96 kHz still hears the file, at the next
 * candidate the endpoint will take, with the conversion reported. A mode that were
 * a constraint would be a mode that could refuse to play music, and a music player
 * that refuses music is not a player with a preference.
 *
 * `isSupported` is called as `isSupported(WireFormat) -> uint32_t hresult`
 * and must return S_OK (zero) for an accepted candidate. It is a template so the
 * device path passes a lambda over `IAudioClient::IsFormatSupported` and the tests
 * pass a table lookup, and neither pays for a virtual call or a std::function.
 *
 * Channels are never negotiated. The candidate loop does not vary them: a source
 * is played with the channels it has, and if the device will not open at that
 * count the open fails and says so. Upmixing here would be a decision about the
 * music made by the layer that opens devices, and §3.1 agrees.
 */
template <typename IsSupported>
NegotiationResult negotiateExclusive(const AudioFormat& source,
                                     uint32_t preferredFirstRate,
                                     IsSupported&& isSupported) {
    NegotiationResult result;
    result.sawSourceRate = false;
    result.sawDepth = false;
    result.sawSampleFormat = false;

    // The channel count is fixed by the source, so "was it refused" is a single
    // fact discovered at the first attempt, and it is reported for every failure
    // because if the count is wrong no candidate can ever succeed.
    bool channelsRefused = false;
    bool anyAttempted = false;

    std::vector<uint32_t> rates = rateOrder(source.sampleRate);
    if (preferredFirstRate != 0) {
        const auto at = std::find(rates.begin(), rates.end(), preferredFirstRate);
        if (at != rates.end()) rates.erase(at);
        rates.insert(rates.begin(), preferredFirstRate);
    }

    for (const uint32_t rate : rates) {
        if (rate == source.sampleRate) result.sawSourceRate = true;
        for (const uint16_t depth : depthOrder(source.bitDepth)) {
            if (depth == normaliseDepth(source.bitDepth)) result.sawDepth = true;
            for (const SampleFormat variant : variants(depth, source.sampleFormat)) {
                AudioFormat candidate;
                candidate.sampleRate = rate;
                // The source's own depth is carried as the *valid* bits; a
                // candidate at a different depth is a different valid width.
                candidate.bitDepth = depth;
                candidate.channels = source.channels;
                candidate.sampleFormat = variant;
                if (variant == source.sampleFormat) result.sawSampleFormat = true;

                // The form is the candidate's own business, decided by
                // `needsExtensible`. Handing every candidate to the device as a
                // WAVEFORMATEXTENSIBLE is what the spec says and it is wrong on any
                // driver that refuses a superfluous extension -- measured, on this
                // machine's endpoint, at every rate.
                const WireFormat wire = buildWireFormat(candidate);
                const uint32_t hr = static_cast<uint32_t>(isSupported(wire));

                Attempt attempt;
                attempt.format = candidate;
                attempt.hresult = hr;
                attempt.supported = (hr == 0);
                result.attempts.push_back(attempt);
                anyAttempted = true;

                if (hr != 0 && result.firstFailure == 0) result.firstFailure = hr;

                if (hr == 0) {
                    result.ok = true;
                    result.format = candidate;
                    result.resampled = !sameSourceFormat(source, candidate);
                    return result;
                }

                if (rate == source.sampleRate) result.rejectedSourceRate = true;
                if (depth == normaliseDepth(source.bitDepth)) result.rejectedDepth = true;
                if (variant == source.sampleFormat) result.rejectedSampleFormat = true;
                channelsRefused = true;
            }
        }
    }

    // §3.4: name the dimension. Ordered so the most specific fact wins, because
    // the listener's next action differs for each.
    result.rejectedChannels = channelsRefused;
    char buf[256];
    if (channelsRefused && source.channels != 2) {
        std::snprintf(buf, sizeof(buf),
                      "exclusive: the device refused %u channel(s) at every rate, depth "
                      "and container tried; the channel count is not negotiated",
                      static_cast<unsigned>(source.channels));
        result.reason = buf;
    } else if (result.sawSourceRate && result.rejectedSourceRate) {
        std::snprintf(buf, sizeof(buf),
                      "exclusive: the device refused the source rate %u Hz; the nearest "
                      "rate it did accept was not a format it would open at this depth",
                      source.sampleRate);
        result.reason = buf;
    } else if (result.sawDepth && result.rejectedDepth) {
        std::snprintf(buf, sizeof(buf),
                      "exclusive: the device refused %u-bit at the source rate; the "
                      "depths that remain change the sample values",
                      static_cast<unsigned>(normaliseDepth(source.bitDepth)));
        result.reason = buf;
    } else if (result.sawSampleFormat && result.rejectedSampleFormat) {
        std::snprintf(buf, sizeof(buf),
                      "exclusive: the device refused the source's own sample format at "
                      "the source rate and depth");
        result.reason = buf;
    } else if (anyAttempted) {
        std::snprintf(buf, sizeof(buf),
                      "exclusive: %zu candidate formats were all refused; first failure "
                      "0x%08lX", result.attempts.size(),
                      static_cast<unsigned long>(result.firstFailure));
        result.reason = buf;
    } else {
        result.reason = "exclusive format open/init failure";
    }
    return result;
}

} // namespace neiro
