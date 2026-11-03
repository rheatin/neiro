// Output policy, device capabilities and the resolved signal path.
//
// This header is the contract between the engine (which resolves and executes)
// and the host (Base/UI, which requests and displays). It is deliberately pure
// data plus one resolver: no device is opened, no sample is touched, and no
// value is inferred in the host.
//
// Rules:
//   * Every policy field has an Auto value. Auto is resolved by the engine and
//     the result carries a reason. Auto never silently changes device, share
//     mode, DSD fallback, channel count, dither or DSP state.
//   * A requested policy and a resolved path are different objects. The UI shows
//     the resolved path; the requested policy is for the controls.
//   * Unknown capability is not false. The `probed` flags on NAE_DeviceCaps say
//     which groups were actually measured.
//   * A path that cannot be satisfied returns a failure status and leaves the
//     resolved fields zero; it does not invent a playable fallback.
//
// Status: proposed contract for review. The declarations below are not
// exported by nae.dll yet; the resolver and caps query are the next
// implementation step. The file is intentionally free of C++ types so Base and
// Swift can mirror it literally.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define NAE_POLICY_NOEXCEPT noexcept
extern "C" {
#else
#define NAE_POLICY_NOEXCEPT
#endif

#define NAE_OUTPUT_POLICY_VERSION 1
#define NAE_DEVICE_CAPS_VERSION 1
#define NAE_RESOLVED_SIGNAL_PATH_VERSION 1

#define NAE_AUTO (-1)

// ---- reasons already defined by nae.h ---------------------------------------
// NAE_REASON_* 0..13 are the existing bit-perfect reasons. New stages must get
// new numbers; never renumber an existing code.
#define NAE_REASON_BALANCE 14
#define NAE_REASON_POLARITY 15
#define NAE_REASON_CHANNEL_DELAY 16
#define NAE_REASON_CLIP_GUARD 17
#define NAE_REASON_ROUTING 18
// Reserved for a DSD transport that cannot claim bit-exactness. Exact DoP or
// native DSD with no DSP is bit-perfect and uses NAE_REASON_BIT_PERFECT.
#define NAE_REASON_DSD_TRANSPORT 19
#define NAE_REASON_PCM_TO_DSD 20
#define NAE_REASON_LOUDNESS 21
#define NAE_REASON_PHASE 22
// The source carries more precision than the engine's float32 path keeps. A
// 32-bit integer sample has 32 significant bits; the internal path has 24, so
// converting it in and back out is lossy even when nothing else is applied. The
// flag used to ignore source width entirely and call such a stream bit-perfect.
#define NAE_REASON_SOURCE_PRECISION 23

// ---- backends ---------------------------------------------------------------

typedef enum NAE_AudioBackend {
    NAE_BACKEND_UNKNOWN = 0,
    NAE_BACKEND_WASAPI = 1,
    NAE_BACKEND_COREAUDIO = 2,
    NAE_BACKEND_AAUDIO = 3,
    NAE_BACKEND_OPENSL = 4,
    NAE_BACKEND_ALSA = 5,
    NAE_BACKEND_ASIO = 6,
    NAE_BACKEND_USB = 7
} NAE_AudioBackend;

typedef enum NAE_ShareModeKind {
    NAE_SHARE_MODE_UNKNOWN = 0,
    NAE_SHARE_MODE_EXCLUSIVE = 1,
    NAE_SHARE_MODE_SHARED = 2
} NAE_ShareModeKind;

typedef enum NAE_SampleFormat {
    NAE_SAMPLE_FORMAT_UNKNOWN = 0,
    NAE_SAMPLE_FORMAT_U8 = 1,
    NAE_SAMPLE_FORMAT_S16 = 2,
    NAE_SAMPLE_FORMAT_S24 = 3,
    NAE_SAMPLE_FORMAT_S24_IN_32 = 4,
    NAE_SAMPLE_FORMAT_S32 = 5,
    NAE_SAMPLE_FORMAT_F32 = 6,
    NAE_SAMPLE_FORMAT_F64 = 7,
    NAE_SAMPLE_FORMAT_DSD_U8 = 8,
    NAE_SAMPLE_FORMAT_DSD_U32 = 9
} NAE_SampleFormat;

// ---- requested output policy ------------------------------------------------

typedef enum NAE_RatePolicy {
    NAE_RATE_AUTO = NAE_AUTO,
    NAE_RATE_NATIVE = 0,
    NAE_RATE_DEVICE = 1,
    NAE_RATE_FIXED = 2
} NAE_RatePolicy;

typedef enum NAE_ResampleAlgorithm {
    NAE_RESAMPLE_ALGORITHM_AUTO = NAE_AUTO,
    NAE_RESAMPLE_ALGORITHM_SOXR = 0,
    // Preview-only algorithms are listed so the host can request them; the
    // resolver must report them as non-bit-perfect and never choose them in Auto.
    NAE_RESAMPLE_ALGORITHM_LINEAR = 1
} NAE_ResampleAlgorithm;

typedef enum NAE_ResampleQuality {
    NAE_RESAMPLE_QUALITY_AUTO = NAE_AUTO,
    NAE_RESAMPLE_QUALITY_FAST = 0,
    NAE_RESAMPLE_QUALITY_HIGH = 1,
    NAE_RESAMPLE_QUALITY_VERY_HIGH = 2
} NAE_ResampleQuality;

typedef enum NAE_DitherMode {
    NAE_DITHER_AUTO = NAE_AUTO,
    NAE_DITHER_OFF = 0,
    NAE_DITHER_TPDF = 1,
    NAE_DITHER_RECTANGULAR = 2,
    NAE_DITHER_NOISE_SHAPED = 3
} NAE_DitherMode;

// What happens to the source domain. This is separate from the transport:
// DoP and native DSD are ways to carry a DSD bitstream, not ways to create one.
typedef enum NAE_DsdMode {
    NAE_DSD_MODE_AUTO = NAE_AUTO,
    // PCM output. A DSD source is decoded/decimated to PCM.
    NAE_DSD_MODE_PCM = 0,
    // Keep the DSD source as DSD; choose a transport below.
    NAE_DSD_MODE_DSD = 1,
    // PCM source -> DSD bitstream via a modulator. Never chosen by Auto for a
    // PCM source unless explicitly requested.
    NAE_DSD_MODE_PCM_TO_DSD = 2
} NAE_DsdMode;

// How a DSD bitstream reaches the device. DoP is a PCM carrier (DSD rate / 16,
// 24-bit words with markers); native is a backend/DAC-specific DSD path.
typedef enum NAE_DsdTransport {
    NAE_DSD_TRANSPORT_AUTO = NAE_AUTO,
    NAE_DSD_TRANSPORT_NONE = 0,
    NAE_DSD_TRANSPORT_DOP = 1,
    NAE_DSD_TRANSPORT_NATIVE = 2
} NAE_DsdTransport;

typedef enum NAE_DsdRatePolicy {
    NAE_DSD_RATE_AUTO = NAE_AUTO,
    NAE_DSD_RATE_FOLLOW_SOURCE = 0,
    NAE_DSD_RATE_DSD64 = 1,
    NAE_DSD_RATE_DSD128 = 2,
    NAE_DSD_RATE_DSD256 = 3,
    // No carrier/native rate is available: decode DSD to PCM instead. Whether
    // this is allowed is an explicit flag below.
    NAE_DSD_RATE_PCM_FALLBACK = 4,
    // Lower the DSD bitstream rate (e.g. DSD128 -> DSD64).
    NAE_DSD_RATE_DOWNRATE = 5,
    // DSD source decimated to PCM at a chosen rate.
    NAE_DSD_RATE_EXTRACT = 6
} NAE_DsdRatePolicy;

typedef enum NAE_BufferMode {
    NAE_BUFFER_AUTO = NAE_AUTO,
    NAE_BUFFER_LOW_LATENCY = 0,
    NAE_BUFFER_STABLE = 1,
    NAE_BUFFER_FIXED = 2
} NAE_BufferMode;

typedef enum NAE_RoutingMode {
    NAE_ROUTING_AUTO = NAE_AUTO,
    NAE_ROUTING_PASS_THROUGH = 0,
    NAE_ROUTING_DOWNMIX = 1,
    NAE_ROUTING_UPMIX = 2,
    NAE_ROUTING_MATRIX = 3
} NAE_RoutingMode;

typedef enum NAE_UpmixPreset {
    NAE_UPMIX_NONE = 0,
    // Mono -> stereo: L = R = M.
    NAE_UPMIX_MONO_TO_STEREO = 1,
    // Mono -> center of a 5.1/7.1 target; other channels silent.
    NAE_UPMIX_MONO_TO_CENTER = 2,
    // Mono -> every non-LFE channel of the target, equal-power; LFE silent.
    NAE_UPMIX_MONO_TO_ALL = 3,
    // Stereo -> 3.0 / 4.0 / 5.0.
    NAE_UPMIX_STEREO_TO_3_0 = 4,
    NAE_UPMIX_STEREO_TO_4_0 = 5,
    NAE_UPMIX_STEREO_TO_5_0 = 6,
    // Stereo -> 5.1: L/R direct, C from the sum, surrounds from L/R, LFE silent.
    NAE_UPMIX_STEREO_TO_5_1 = 7,
    // Same, plus an 80 Hz low-passed LFE derived from the sum.
    NAE_UPMIX_STEREO_TO_5_1_BASS = 8,
    // Stereo -> 6.1 / 7.1: 5.1 mapping plus back/side copies.
    NAE_UPMIX_STEREO_TO_6_1 = 9,
    NAE_UPMIX_STEREO_TO_7_1 = 10,
    // 5.1 -> 6.1 / 7.1: copy surround channels into the extra pair.
    NAE_UPMIX_5_1_TO_6_1 = 11,
    NAE_UPMIX_5_1_TO_7_1 = 12,
    // Immersive top layers, house standard: top channels are 0.5 * L/R.
    NAE_UPMIX_STEREO_TO_5_1_2 = 13,
    NAE_UPMIX_STEREO_TO_5_1_4 = 14,
    NAE_UPMIX_STEREO_TO_7_1_2 = 15,
    NAE_UPMIX_STEREO_TO_7_1_4 = 16,
    NAE_UPMIX_5_1_TO_5_1_2 = 17,
    NAE_UPMIX_5_1_TO_5_1_4 = 18,
    NAE_UPMIX_7_1_TO_7_1_2 = 19,
    NAE_UPMIX_7_1_TO_7_1_4 = 20
} NAE_UpmixPreset;

typedef enum NAE_DownmixPreset {
    NAE_DOWNMIX_STANDARD = 0,
    NAE_DOWNMIX_ITU_R_775 = 1,
    NAE_DOWNMIX_LORO = 2,
    NAE_DOWNMIX_LTRT = 3,
    NAE_DOWNMIX_5_1_TO_3_0 = 4,
    NAE_DOWNMIX_5_1_TO_2_1 = 5,
    NAE_DOWNMIX_5_1_TO_STEREO = 6,
    NAE_DOWNMIX_7_1_TO_3_0 = 7,
    NAE_DOWNMIX_7_1_TO_5_1 = 8,
    NAE_DOWNMIX_7_1_TO_STEREO = 9,
    // Fold the top layer back into L/R at 0.5; the base layout is unchanged.
    NAE_DOWNMIX_5_1_2_TO_5_1 = 10,
    NAE_DOWNMIX_5_1_4_TO_5_1 = 11,
    NAE_DOWNMIX_7_1_2_TO_7_1 = 12,
    NAE_DOWNMIX_7_1_4_TO_7_1 = 13
} NAE_DownmixPreset;

typedef enum NAE_ChannelMode {
    NAE_CHANNEL_MODE_AUTO = NAE_AUTO,
    NAE_CHANNEL_MODE_NORMAL = 0,
    NAE_CHANNEL_MODE_MONO = 1,
    NAE_CHANNEL_MODE_SWAP = 2,
    NAE_CHANNEL_MODE_MID_SIDE = 3
} NAE_ChannelMode;

typedef enum NAE_ClipGuardMode {
    NAE_CLIP_GUARD_AUTO = NAE_AUTO,
    NAE_CLIP_GUARD_OFF = 0,
    NAE_CLIP_GUARD_LIMITER = 1
} NAE_ClipGuardMode;

typedef enum NAE_BassManagementMode {
    NAE_BASS_AUTO = NAE_AUTO,
    NAE_BASS_OFF = 0,
    NAE_BASS_ON = 1
} NAE_BassManagementMode;

typedef enum NAE_LfeMode {
    NAE_LFE_AUTO = NAE_AUTO,
    NAE_LFE_SILENT = 0,
    NAE_LFE_LOW_PASS_SUM = 1,
    NAE_LFE_MATRIX = 2
} NAE_LfeMode;

typedef enum NAE_RoutingNormalize {
    NAE_ROUTING_NORMALIZE_NONE = 0,
    NAE_ROUTING_NORMALIZE_EQUAL_POWER = 1,
    NAE_ROUTING_NORMALIZE_PEAK = 2
} NAE_RoutingNormalize;

typedef enum NAE_PhaseMode {
    NAE_PHASE_NONE = 0,
    // Frequency-warped parallel all-pass Hilbert (Harris/Berdahl/Abel style):
    // flat magnitude, 90-degree quadrature over the audio band, low CPU.
    // phaseOrder selects the order; the resolver reports the actual latency.
    NAE_PHASE_IIR_ALLPASS_90 = 1,
    // Optional linear-phase FIR Hilbert. Constant group delay; phaseTaps is odd.
    NAE_PHASE_FIR_HILBERT = 2
} NAE_PhaseMode;

#define NAE_MAX_CHANNELS 32
#define NAE_MAX_DEVICE_RATES 32
#define NAE_MAX_DEVICE_FORMATS 16
#define NAE_MAX_CHANNEL_LAYOUTS 16
#define NAE_MAX_DOP_CARRIERS 8
#define NAE_MAX_NATIVE_DSD_RATES 8
#define NAE_MAX_DEVICE_ID 512
#define NAE_MAX_DEVICE_NAME 256
#define NAE_MAX_FAILURE_TEXT 256
#define NAE_MAX_MATRIX_CHANNELS 32
#define NAE_MAX_DIAGNOSTICS 32
#define NAE_MAX_DIAGNOSTIC_MESSAGE 192

// WAVE channel bits. Named here so a routing target is not a bare magic mask.
#define NAE_CHANNEL_FL 0x00000001u
#define NAE_CHANNEL_FR 0x00000002u
#define NAE_CHANNEL_FC 0x00000004u
#define NAE_CHANNEL_LFE 0x00000008u
#define NAE_CHANNEL_BL 0x00000010u
#define NAE_CHANNEL_BR 0x00000020u
#define NAE_CHANNEL_FLC 0x00000040u
#define NAE_CHANNEL_FRC 0x00000080u
#define NAE_CHANNEL_BC 0x00000100u
#define NAE_CHANNEL_SL 0x00000200u
#define NAE_CHANNEL_SR 0x00000400u
#define NAE_CHANNEL_TC 0x00000800u
#define NAE_CHANNEL_TFL 0x00001000u
#define NAE_CHANNEL_TFR 0x00002000u
#define NAE_CHANNEL_TBL 0x00004000u
#define NAE_CHANNEL_TBR 0x00008000u

#define NAE_LAYOUT_MONO (NAE_CHANNEL_FC)
#define NAE_LAYOUT_STEREO (NAE_CHANNEL_FL | NAE_CHANNEL_FR)
#define NAE_LAYOUT_2_1 (NAE_LAYOUT_STEREO | NAE_CHANNEL_LFE)
#define NAE_LAYOUT_3_0 (NAE_LAYOUT_STEREO | NAE_CHANNEL_FC)
#define NAE_LAYOUT_4_0 (NAE_LAYOUT_STEREO | NAE_CHANNEL_BL | NAE_CHANNEL_BR)
#define NAE_LAYOUT_5_0 (NAE_LAYOUT_3_0 | NAE_CHANNEL_BL | NAE_CHANNEL_BR)
#define NAE_LAYOUT_5_1 (NAE_LAYOUT_5_0 | NAE_CHANNEL_LFE)
#define NAE_LAYOUT_6_1 (NAE_LAYOUT_5_1 | NAE_CHANNEL_BC)
#define NAE_LAYOUT_7_1 (NAE_LAYOUT_5_1 | NAE_CHANNEL_SL | NAE_CHANNEL_SR)
#define NAE_LAYOUT_5_1_2 (NAE_LAYOUT_5_1 | NAE_CHANNEL_TFL | NAE_CHANNEL_TFR)
#define NAE_LAYOUT_5_1_4 (NAE_LAYOUT_5_1 | NAE_CHANNEL_TFL | NAE_CHANNEL_TFR | NAE_CHANNEL_TBL | NAE_CHANNEL_TBR)
#define NAE_LAYOUT_7_1_2 (NAE_LAYOUT_7_1 | NAE_CHANNEL_TFL | NAE_CHANNEL_TFR)
#define NAE_LAYOUT_7_1_4 (NAE_LAYOUT_7_1 | NAE_CHANNEL_TFL | NAE_CHANNEL_TFR | NAE_CHANNEL_TBL | NAE_CHANNEL_TBR)

typedef struct NAE_OutputPolicy {
    uint32_t structSize;
    uint32_t version;

    // Native = source rate if the device accepts it; Device = device rate;
    // Fixed = fixedRate. Auto resolves by the rules in OUTPUT-POLICY.md.
    int32_t ratePolicy;
    uint32_t fixedRate;  // only when ratePolicy == NAE_RATE_FIXED

    int32_t resampleAlgorithm;  // NAE_ResampleAlgorithm
    int32_t resampleQuality;    // NAE_ResampleQuality

    int32_t ditherMode;  // NAE_DitherMode

    int32_t dsdMode;         // NAE_DsdMode: PCM / DSD / PCM_TO_DSD
    int32_t dsdTransport;    // NAE_DsdTransport: DoP / native / none
    int32_t dsdRatePolicy;   // NAE_DsdRatePolicy
    uint32_t targetDsdRate;  // 0 = auto; otherwise 2822400/5644800/11289600

    // Explicit permission to degrade. Auto may use these only when set. The
    // resolved path still reports which one happened.
    uint32_t allowPcmFallback;
    uint32_t allowDownrate;
    uint32_t allowExtractRate;

    int32_t bufferMode;       // NAE_BufferMode
    uint32_t bufferMs;        // 0 = auto; used when bufferMode == FIXED
    uint32_t periodFrames;    // 0 = auto; used when bufferMode == FIXED

    int32_t routingMode;          // NAE_RoutingMode
    uint32_t targetChannelMask;   // 0 = device/source auto
    int32_t upmixPreset;          // NAE_UpmixPreset
    int32_t downmixPreset;        // NAE_DownmixPreset
    uint32_t matrixId;            // 0 = none; required for MATRIX
    int32_t bassManagement;       // NAE_BassManagementMode
    int32_t lfeMode;              // NAE_LfeMode
    double lfeCrossoverHz;        // 0 = auto; e.g. 80
    double lfeGainDb;
    double centerGainDb;
    double surroundGainDb;

    int32_t channelMode;          // NAE_ChannelMode
    double balance;               // -1.0 left .. +1.0 right; 0 centre
    uint32_t polarityMask;        // bit N inverts channel N
    uint32_t delayFrames[NAE_MAX_CHANNELS];
    double trimDb[NAE_MAX_CHANNELS];

    int32_t clipGuardMode;        // NAE_ClipGuardMode
    double clipCeilingDbTp;       // e.g. -1.0 dBTP; ignored when OFF
    double clipLookaheadMs;       // e.g. 5 ms
    double clipReleaseMs;         // e.g. 100 ms
} NAE_OutputPolicy;

// ---- measured device capabilities ------------------------------------------

// `probed` flags. A zero bit means "not measured", not "unsupported".
#define NAE_CAPS_PROBED_NATIVE 0x0001u
#define NAE_CAPS_PROBED_RATES 0x0002u
#define NAE_CAPS_PROBED_FORMATS 0x0004u
#define NAE_CAPS_PROBED_LAYOUTS 0x0008u
#define NAE_CAPS_PROBED_BUFFER 0x0010u
#define NAE_CAPS_PROBED_DOP 0x0020u
#define NAE_CAPS_PROBED_NATIVE_DSD 0x0040u
#define NAE_CAPS_PROBED_SHARE 0x0080u

typedef struct NAE_DeviceFormat {
    int32_t format;        // NAE_SampleFormat
    uint32_t validBits;    // bits that carry audio
    uint32_t containerBits;  // storage width
} NAE_DeviceFormat;

typedef struct NAE_DeviceCaps {
    uint32_t structSize;
    uint32_t version;
    uint32_t capsGeneration;  // increments when the device's capabilities change

    char id[NAE_MAX_DEVICE_ID];
    char name[NAE_MAX_DEVICE_NAME];
    int32_t backend;    // NAE_AudioBackend
    int32_t shareMode;  // NAE_ShareModeKind this query describes
    int32_t isDefault;
    int32_t hotplug;
    uint32_t probed;    // NAE_CAPS_PROBED_* mask

    // Native format. 0/UNKNOWN when not probed.
    int32_t nativeFormat;
    uint32_t nativeValidBits;
    uint32_t nativeContainerBits;
    uint32_t nativeRate;
    uint32_t nativeChannels;
    uint32_t nativeChannelMask;
    uint32_t maxChannels;

    uint32_t rateCount;
    uint32_t rates[NAE_MAX_DEVICE_RATES];  // ascending, exclusive/openable rates

    uint32_t formatCount;
    NAE_DeviceFormat formats[NAE_MAX_DEVICE_FORMATS];

    uint32_t layoutCount;
    uint32_t layouts[NAE_MAX_CHANNEL_LAYOUTS];  // WAVE channel masks

    uint32_t minBufferFrames;
    uint32_t maxBufferFrames;
    uint32_t defaultBufferFrames;
    uint32_t minPeriodFrames;
    uint32_t maxPeriodFrames;
    int32_t eventDriven;
    double reportedLatencyMs;

    int32_t dopSupported;
    uint32_t dopCarrierCount;
    uint32_t dopCarriers[NAE_MAX_DOP_CARRIERS];  // 176400, 352800, 705600

    int32_t nativeDsdSupported;
    uint32_t nativeDsdRateCount;
    uint32_t nativeDsdRates[NAE_MAX_NATIVE_DSD_RATES];  // bitstream rates

    uint32_t supportedShareModes;  // bitmask of NAE_SHARE_MODE_*
    int32_t exclusiveSupported;
    int32_t sharedSupported;
} NAE_DeviceCaps;

// ---- source description -----------------------------------------------------

typedef struct NAE_SourceInfo {
    uint32_t structSize;
    uint32_t sampleRate;      // PCM source rate; 0 for a DSD source
    uint32_t channels;
    uint32_t channelMask;
    int32_t sampleFormat;     // NAE_SampleFormat
    uint32_t validBits;
    uint32_t containerBits;
    int32_t isDsd;
    uint32_t dsdRate;         // bitstream rate for DSD; 0 for PCM
} NAE_SourceInfo;

// ---- resolved path ----------------------------------------------------------

typedef enum NAE_ResolveStatus {
    NAE_RESOLVE_OK = 0,
    NAE_RESOLVE_NO_DEVICE = 1,
    NAE_RESOLVE_UNSUPPORTED_RATE = 2,
    NAE_RESOLVE_UNSUPPORTED_FORMAT = 3,
    NAE_RESOLVE_UNSUPPORTED_CHANNELS = 4,
    NAE_RESOLVE_DSD_UNAVAILABLE = 5,
    NAE_RESOLVE_BUFFER_UNAVAILABLE = 6,
    NAE_RESOLVE_ROUTING_UNAVAILABLE = 7,
    NAE_RESOLVE_DEVICE_LOST = 8,
    NAE_RESOLVE_INVALID_POLICY = 9,
    NAE_RESOLVE_UNPROBED = 10
} NAE_ResolveStatus;

typedef enum NAE_ResolveReason {
    NAE_RESOLVE_REASON_NONE = 0,
    NAE_RESOLVE_REASON_SOURCE_RATE_SUPPORTED = 1,
    NAE_RESOLVE_REASON_DEVICE_NATIVE_RATE = 2,
    NAE_RESOLVE_REASON_NEAREST_SUPPORTED_RATE = 3,
    NAE_RESOLVE_REASON_FIXED_RATE = 4,
    NAE_RESOLVE_REASON_RESAMPLE_REQUIRED = 5,
    NAE_RESOLVE_REASON_NO_RESAMPLE = 6,
    NAE_RESOLVE_REASON_DITHER_REQUIRED = 7,
    NAE_RESOLVE_REASON_NO_DITHER = 8,
    NAE_RESOLVE_REASON_DSD_NATIVE = 9,
    NAE_RESOLVE_REASON_DSD_DOP = 10,
    NAE_RESOLVE_REASON_DSD_PCM_FALLBACK = 11,
    NAE_RESOLVE_REASON_DSD_DOWNRATED = 12,
    NAE_RESOLVE_REASON_DSD_EXTRACTED = 13,
    NAE_RESOLVE_REASON_BUFFER_BACKEND_DEFAULT = 14,
    NAE_RESOLVE_REASON_BUFFER_LOW_LATENCY = 15,
    NAE_RESOLVE_REASON_BUFFER_FIXED = 16,
    NAE_RESOLVE_REASON_ROUTING_PASS_THROUGH = 17,
    NAE_RESOLVE_REASON_ROUTING_DOWNMIX = 18,
    NAE_RESOLVE_REASON_ROUTING_UPMIX = 19,
    NAE_RESOLVE_REASON_ROUTING_MATRIX = 20,
    NAE_RESOLVE_REASON_CHANNEL_AUTO = 21,
    NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_ON = 22,
    NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_OFF = 23,
    NAE_RESOLVE_REASON_REPLAY_GAIN = 24,
    NAE_RESOLVE_REASON_BASS_MANAGEMENT = 25,
    NAE_RESOLVE_REASON_PHASE_SHIFT = 26,
    NAE_RESOLVE_REASON_FAILURE = 27
} NAE_ResolveReason;

typedef enum NAE_AutoField {
    NAE_AUTO_FIELD_RATE = 0,
    NAE_AUTO_FIELD_RESAMPLER = 1,
    NAE_AUTO_FIELD_DITHER = 2,
    NAE_AUTO_FIELD_DSD_MODE = 3,
    NAE_AUTO_FIELD_DSD_RATE = 4,
    NAE_AUTO_FIELD_BUFFER = 5,
    NAE_AUTO_FIELD_ROUTING = 6,
    NAE_AUTO_FIELD_CHANNEL = 7,
    NAE_AUTO_FIELD_CLIP_GUARD = 8,
    NAE_AUTO_FIELD_DSD_TRANSPORT = 9,
    NAE_AUTO_FIELD_BIT_PERFECT = 10
} NAE_AutoField;

typedef struct NAE_AutoDecision {
    uint32_t field;     // NAE_AutoField
    int32_t choice;     // enum value chosen
    int32_t reason;     // NAE_ResolveReason
    uint32_t value;     // rate/frames/bits when numeric; 0 otherwise
} NAE_AutoDecision;

// Stage bits for `activeStages` and `bypassedStages`.
#define NAE_STAGE_RESAMPLE 0x00000001u
#define NAE_STAGE_DITHER 0x00000002u
#define NAE_STAGE_EQ 0x00000004u
#define NAE_STAGE_CROSSFEED 0x00000008u
#define NAE_STAGE_CONVOLVER 0x00000010u
#define NAE_STAGE_REPLAYGAIN 0x00000020u
#define NAE_STAGE_VOLUME 0x00000040u
#define NAE_STAGE_BALANCE 0x00000080u
#define NAE_STAGE_POLARITY 0x00000100u
#define NAE_STAGE_DELAY 0x00000200u
#define NAE_STAGE_CLIP_GUARD 0x00000400u
#define NAE_STAGE_ROUTING 0x00000800u
#define NAE_STAGE_DSD_TRANSPORT 0x00001000u
#define NAE_STAGE_DSD_MODULATOR 0x00002000u
#define NAE_STAGE_CROSSFADE 0x00004000u
#define NAE_STAGE_BASS_MANAGEMENT 0x00008000u
#define NAE_STAGE_PHASE_SHIFT 0x00010000u

typedef enum NAE_DiagnosticSeverity {
    NAE_DIAG_INFO = 0,
    NAE_DIAG_AUTO = 1,
    NAE_DIAG_FALLBACK = 2,
    NAE_DIAG_WARNING = 3,
    NAE_DIAG_ERROR = 4
} NAE_DiagnosticSeverity;

typedef enum NAE_DiagnosticCode {
    NAE_DIAG_AUTO_CHOICE = 0,
    NAE_DIAG_RATE_COERCED = 1,
    NAE_DIAG_DSD_PCM_FALLBACK = 2,
    NAE_DIAG_DSD_DOWNRATE = 3,
    NAE_DIAG_DSD_EXTRACT = 4,
    NAE_DIAG_DSP_BYPASSED = 5,
    NAE_DIAG_ROUTING_DOWNMIX = 6,
    NAE_DIAG_ROUTING_UPMIX = 7,
    NAE_DIAG_PHASE_SHIFT = 8,
    NAE_DIAG_BASS_MANAGEMENT = 9,
    NAE_DIAG_CLIP_GUARD = 10,
    NAE_DIAG_RESAMPLER = 11,
    NAE_DIAG_DITHER = 12,
    NAE_DIAG_UNPROBED = 13,
    NAE_DIAG_FAILURE = 14,
    NAE_DIAG_BIT_PERFECT = 15
} NAE_DiagnosticCode;

/// One user-visible explanation. The host can show `message` directly, or key
/// localization off `code`/`field` and ignore the English text.
typedef struct NAE_Diagnostic {
    uint32_t structSize;
    int32_t severity;   // NAE_DiagnosticSeverity
    int32_t code;       // NAE_DiagnosticCode
    uint32_t field;     // NAE_AutoField
    int32_t requested;  // enum request, or -1
    int32_t resolved;   // enum result, or -1
    uint32_t value;     // rate/frames/bits, 0 when not numeric
    char message[NAE_MAX_DIAGNOSTIC_MESSAGE];
} NAE_Diagnostic;

typedef struct NAE_ResolvedSignalPath {
    uint32_t structSize;
    uint32_t version;
    int32_t resolved;       // 1 only when status == OK
    int32_t status;         // NAE_ResolveStatus
    uint32_t capsGeneration;

    char deviceId[NAE_MAX_DEVICE_ID];
    char deviceName[NAE_MAX_DEVICE_NAME];
    int32_t backend;
    int32_t shareMode;

    // Source.
    uint32_t sourceRate;
    uint32_t sourceChannels;
    uint32_t sourceChannelMask;
    int32_t sourceFormat;
    uint32_t sourceValidBits;
    uint32_t sourceContainerBits;
    int32_t sourceIsDsd;
    uint32_t sourceDsdRate;

    // Actual output format.
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t channelMask;
    int32_t sampleFormat;
    uint32_t validBits;
    uint32_t containerBits;
    int32_t isFloat;

    // Rate decision.
    int32_t ratePolicy;
    uint32_t requestedRate;
    int32_t rateReason;       // NAE_ResolveReason
    int32_t coerced;          // requested rate was not available

    // Resampler.
    int32_t resamplerAlgorithm;
    int32_t resamplerQuality;
    int32_t resampling;
    double resampleRatio;
    int32_t resampleReason;

    // Dither.
    int32_t ditherMode;
    int32_t ditherActive;
    uint32_t ditherBits;
    int32_t ditherReason;

    // DSD.
    int32_t dsdModeRequested;
    int32_t dsdModeResolved;
    int32_t dsdTransportRequested;
    int32_t dsdTransportResolved;
    int32_t dsdRatePolicy;
    uint32_t dsdRateResolved;
    uint32_t dsdCarrierRate;
    int32_t dsdPcmFallback;
    int32_t dsdDownrated;
    int32_t dsdExtracted;
    int32_t dsdReason;

    // Buffer.
    int32_t bufferMode;
    uint32_t bufferFrames;
    uint32_t periodFrames;
    int32_t eventDriven;
    double bufferLatencyMs;
    double driverLatencyMs;
    double totalLatencyMs;
    int32_t bufferReason;

    // Routing.
    int32_t routingMode;
    int32_t upmixPreset;
    int32_t downmixPreset;
    uint32_t matrixId;
    int32_t bassManagement;
    int32_t lfeMode;
    double lfeCrossoverHz;
    double lfeGainDb;
    int32_t phaseShiftActive;
    uint32_t phaseFilterTaps;
    uint32_t phaseFilterOrder;
    uint32_t phaseLatencyFrames;
    int32_t channelConversion;
    int32_t routingReason;

    // Channel trim/phase.
    int32_t channelMode;
    double balance;
    uint32_t polarityMask;
    uint32_t delayFrames[NAE_MAX_CHANNELS];
    double trimDb[NAE_MAX_CHANNELS];
    int32_t channelReason;

    // Clip guard.
    int32_t clipGuardMode;
    int32_t clipGuardActive;
    double clipCeilingDbTp;
    double clipLookaheadMs;
    int32_t clipReason;

    // Session DSP state. The pure output resolver fills the output stages;
    // the session fills these from its applied settings.
    int32_t eqActive;
    int32_t crossfeedActive;
    int32_t convolverActive;
    int32_t crossfadeActive;
    double crossfadeDurationMs;
    int32_t gapless;
    double volume;

    // ReplayGain (from the session's policy, not a duplicate calculation).
    int32_t replayGainMode;
    double replayGainAppliedDb;
    int32_t replayGainReason;

    uint32_t activeStages;
    uint32_t bypassedStages;

    // 1 also for exact DoP/native DSD with no DSP/fallback/downrate/extract.
    // PCM_TO_DSD is always 0.
    int32_t bitPerfect;
    int32_t perfectReason;  // NAE_REASON_*

    uint32_t autoDecisionCount;
    NAE_AutoDecision autoDecisions[16];

    uint32_t diagnosticCount;
    NAE_Diagnostic diagnostics[NAE_MAX_DIAGNOSTICS];

    // Empty on success. Fixed storage so the host does not own a second string.
    char failure[NAE_MAX_FAILURE_TEXT];
} NAE_ResolvedSignalPath;

// ---- arbitrary routing matrix ------------------------------------------------

// The complete route. Presets are conveniences; MATRIX is the canonical way to
// express any source-to-target mapping. `gain[target][source]`; rows may be
// normalised by the resolver. A registered matrix is copied by the engine; the
// host may release its struct immediately.
typedef struct NAE_ComplexGain {
    float real;
    float quadrature;
} NAE_ComplexGain;

typedef struct NAE_RoutingMatrix {
    uint32_t structSize;
    uint32_t version;
    uint32_t sourceChannels;
    uint32_t targetChannels;
    uint32_t sourceMask;
    uint32_t targetMask;
    // gain[target][source]. `real` is the normal tap; `quadrature` multiplies
    // the 90-degree Hilbert version of the source. LTRT needs quadrature.
    NAE_ComplexGain gain[NAE_MAX_MATRIX_CHANNELS][NAE_MAX_MATRIX_CHANNELS];
    int32_t normalize;           // NAE_RoutingNormalize
    int32_t phaseMode;           // NAE_PhaseMode
    uint32_t phaseOrder;         // IIR sections per path; 0 = default 6
    uint32_t phaseTaps;          // FIR only; 0 = default 255; odd, >= 63
    int32_t alignPhaseLatency;   // delay direct paths by group delay

    // Bass management, carried with the matrix so the processor and the policy
    // cannot disagree about the LFE. 0/auto values keep the matrix row as-is.
    int32_t bassManagement;      // NAE_BassManagementMode
    int32_t lfeMode;             // NAE_LfeMode
    double lfeCrossoverHz;       // 0 = default 80
    double lfeGainDb;            // applied after the LFE row
    double sampleRate;           // output rate; 0 = bass management disabled
} NAE_RoutingMatrix;

// ---- functions --------------------------------------------------------------

/** Fill an output policy with the defaults (everything Auto). */
void NAE_OutputPolicyDefaults(NAE_OutputPolicy* out) NAE_POLICY_NOEXCEPT;

/**
 * Register a custom routing matrix. The engine copies it and returns an opaque
 * id for NAE_OutputPolicy.matrixId. Release when no policy uses it.
 */
int32_t NAE_RegisterRoutingMatrix(const NAE_RoutingMatrix* matrix,
                                  uint32_t* outId) NAE_POLICY_NOEXCEPT;
void NAE_ReleaseRoutingMatrix(uint32_t matrixId) NAE_POLICY_NOEXCEPT;

/** Read a registered matrix. Returns NAE_ERR_NOT_OPEN for an unknown id. */
int32_t NAE_GetRoutingMatrix(uint32_t matrixId,
                             NAE_RoutingMatrix* out) NAE_POLICY_NOEXCEPT;

/**
 * Build the standard matrix for a preset. The returned matrix is a normal
 * copyable value: the host may edit any gain, then register it and set
 * routingMode = MATRIX. Presets are defaults, not an unchangeable path.
 */
int32_t NAE_BuildRoutingMatrix(int32_t routingMode, int32_t preset,
                               uint32_t sourceMask, uint32_t targetMask,
                               NAE_RoutingMatrix* out) NAE_POLICY_NOEXCEPT;

/**
 * Query one device in one share mode. `out` must have structSize set.
 * Returns NAE_OK, or NAE_ERR_*; the `probed` mask says which groups were
 * actually measured, so an unprobed group is not reported as unsupported.
 */
int32_t NAE_QueryDeviceCaps(const char* deviceId, int32_t shareMode,
                            NAE_DeviceCaps* out) NAE_POLICY_NOEXCEPT;

/**
 * Pure resolution: no device is opened, no sample is touched. The engine later
 * uses the same function before opening the stream.
 *
 * On success `out->resolved = 1`, `out->status = NAE_RESOLVE_OK`, and every
 * Auto field has a concrete choice and reason. On failure `resolved = 0` and
 * `failure` names the unmet constraint; no fallback is invented.
 */
int32_t NAE_ResolveSignalPath(const NAE_DeviceCaps* caps,
                              const NAE_SourceInfo* source,
                              const NAE_OutputPolicy* policy,
                              NAE_ResolvedSignalPath* out) NAE_POLICY_NOEXCEPT;

#ifdef __cplusplus
}
#endif
