// What the engine resolved, read from `player().resolvedJson`.
//
// Base forwards the engine's own resolved signal path as JSON; this reads the
// facts the panel shows. Nothing here is derived: a field the engine did not
// report stays `null`, and the panel says "—" rather than inventing a value.
//
// The DSD facts are the reason this module exists: `dsdModeResolved`,
// `dsdTransportResolved`, `dsdRateResolved`, `dsdCarrierRate` and the three
// fallback flags are all in the JSON the engine already sends, and no view
// read them before.

import { computed } from "nsh";
import { engine } from "../backend/engine";
import type { RenderPath } from "../backend/types";

/** The engine's DSD domain. */
export type DsdMode = "pcm" | "dsd" | "pcm_to_dsd" | null;
/** How a DSD bitstream reaches the device. */
export type DsdTransport = "none" | "dop" | "native" | null;
/** The backend that carried the stream (`NAE_AudioBackend`). */
export type AudioBackend = "wasapi" | "coreaudio" | "aaudio" | "opensl" | "alsa" | "asio" | "usb" | null;

/** The resolved path, as far as the engine reports it. */
export interface ResolvedPath {
  deviceId: string;
  deviceName: string;
  /** The backend the engine actually opened the device with. */
  backend: AudioBackend;
  sourceRate: number;
  sourceChannels: number;
  sampleRate: number;
  channels: number;
  validBits: number;
  containerBits: number;
  isFloat: boolean;
  resampling: boolean;
  dithered: boolean;
  bufferLatencyMs: number;
  totalLatencyMs: number;
  /** DSD, when the source or the policy is one. */
  dsdMode: DsdMode;
  dsdTransport: DsdTransport;
  /** The DSD rate the engine settled on (2822400 / 5644800 / 11289600). */
  dsdRate: number;
  /** The PCM carrier rate for DoP (DSD rate / 16); 0 for native. */
  dsdCarrierRate: number;
  dsdPcmFallback: boolean;
  dsdDownrated: boolean;
  dsdExtracted: boolean;
  /** The decode path string, when the host asked for it. */
  decodePath: string;
  /** The share mode the stream runs in, as `NAE_SHARE_MODE_*`: 0 unknown,
   * 1 exclusive, 2 shared. */
  shareMode: number;
  /** The dither algorithm (`NAE_DITHER_*`) and the depth it lands on. */
  ditherMode: number;
  ditherBits: number;
  /** The engine's own explanation lines for why the path is not
   * bit-perfect. */
  diagnostics: string[];
  /** `eqActive` and friends: what the engine actually applied. */
  eqActive: boolean;
  crossfeedActive: boolean;
  convolverActive: boolean;
  crossfadeActive: boolean;
}

// `NAE_DsdMode`: 0 PCM, 1 DSD, 2 PCM_TO_DSD (`-1` is AUTO). The list is
// indexed by the value, so index 0 must be "pcm" — an earlier version started
// with `null` and read every DSD stream one step off (a DoP carrier showed as
// "PCM 176.4 kHz" while the source was 2822.4 kHz).
const DSD_MODES: (DsdMode)[] = ["pcm", "dsd", "pcm_to_dsd"];
// `NAE_DsdTransport`: 0 NONE, 1 DOP, 2 NATIVE.
const DSD_TRANSPORTS: (DsdTransport)[] = ["none", "dop", "native"];
// `NAE_AudioBackend`: 0 UNKNOWN, 1 WASAPI, 2 CoreAudio, 3 AAudio, 4 OpenSL,
// 5 ALSA, 6 ASIO, 7 USB. The share-mode keys cannot pick this — the engine's
// policy has only exclusive/shared — so the panel reports what was chosen.
const BACKENDS: (AudioBackend)[] = [null, "wasapi", "coreaudio", "aaudio", "opensl", "alsa", "asio", "usb"];

function num(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}
function str(value: unknown): string {
  return typeof value === "string" ? value : "";
}
function bool(value: unknown): boolean {
  return value === true;
}
/** A policy enum, indexed by its own value. `-1` is `NAE_AUTO` — the engine
 * chose, which for DSD means "follow the source" — and reads as `null` rather
 * than as the first entry. */
function enumOf<T>(list: T[], value: unknown): T | null {
  const index = num(value);
  return Number.isInteger(index) && index >= 0 && index < list.length ? list[index] : null;
}

/** The engine's diagnostics array: objects with a `message`. Anything else
 * in it is skipped rather than read as a message it is not. */
function diagnosticLines(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  const lines: string[] = [];
  for (const row of value) {
    if (!row || typeof row !== "object" || !("message" in row)) continue;
    const message = row.message;
    if (typeof message === "string" && message !== "") lines.push(message);
  }
  return lines;
}

/** The resolved path, or `null` before the engine has opened a stream. */
export const resolvedPath = computed<ResolvedPath | null>(() => {
  const text = engine.player().resolvedJson;
  if (!text) return null;
  let raw: Record<string, unknown>;
  try {
    raw = JSON.parse(text) as Record<string, unknown>;
  } catch {
    return null;
  }
  if (raw.resolved !== true) return null;
  return {
    deviceId: str(raw.deviceId),
    deviceName: str(raw.deviceName),
    backend: enumOf(BACKENDS, raw.backend),
    sourceRate: num(raw.sourceRate),
    sourceChannels: num(raw.sourceChannels),
    sampleRate: num(raw.sampleRate),
    channels: num(raw.channels),
    validBits: num(raw.validBits),
    containerBits: num(raw.containerBits),
    isFloat: bool(raw.isFloat),
    resampling: bool(raw.resampling),
    dithered: bool(raw.ditherActive),
    bufferLatencyMs: num(raw.bufferLatencyMs),
    totalLatencyMs: num(raw.totalLatencyMs),
    dsdMode: enumOf(DSD_MODES, raw.dsdModeResolved),
    dsdTransport: enumOf(DSD_TRANSPORTS, raw.dsdTransportResolved),
    dsdRate: num(raw.dsdRateResolved),
    dsdCarrierRate: num(raw.dsdCarrierRate),
    dsdPcmFallback: bool(raw.dsdPcmFallback),
    dsdDownrated: bool(raw.dsdDownrated),
    dsdExtracted: bool(raw.dsdExtracted),
    decodePath: str(raw.decodePath),
    shareMode: num(raw.shareMode),
    ditherMode: num(raw.ditherMode),
    ditherBits: num(raw.ditherBits),
    diagnostics: diagnosticLines(raw.diagnostics),
    eqActive: bool(raw.eqActive),
    crossfeedActive: bool(raw.crossfeedActive),
    convolverActive: bool(raw.convolverActive),
    crossfadeActive: bool(raw.crossfadeActive),
  };
});

/** The render path Base reports (the same stream, its own fields). */
export const renderPath = (): RenderPath | null => engine.renderPath();

/** How a DSD bitstream reaches the device (`null` when not DSD). */
export const dsdTransport = (): DsdTransport => resolvedPath()?.dsdTransport ?? null;

/** The share mode the stream runs in (`null` before a stream). */
export const shareMode = (): number | null => resolvedPath()?.shareMode ?? null;

/** The engine's last explanation for the path, or `""`. */
export const lastDiagnostic = (): string => {
  const lines = resolvedPath()?.diagnostics ?? [];
  return lines.length > 0 ? lines[lines.length - 1] : "";
};

/** Whether the engine fell back to PCM for a DSD source. */
export const dsdPcmFallback = (): boolean => resolvedPath()?.dsdPcmFallback ?? false;

/** Whether the *file* is a DSD bitstream.
 *
 * The engine reports a DSD file's bit rate as `sourceRate` (2 822 400 for
 * DSD64 and each step doubling it), whether it carries the bits as DSD or
 * decimates them to PCM. The two questions are different: "is the source DSD"
 * decides the label, "how is it carried" decides the transport line. Reading
 * only the mode said "PCM 2822.4 kHz" for a DSD64 file being extracted —
 * true about the path, wrong about the file.
 */
export const isDsdFile = (): boolean => {
  const path = resolvedPath();
  if (!path) return false;
  const rate = path.sourceRate;
  // DSD bit rates are 64 × 44.1 kHz and its multiples of two.
  return rate >= 2_822_400 && rate % 44_100 === 0 && rate % 2_822_400 === 0;
};

/** Whether the stream is DSD at all (source is DSD and the path keeps it). */
export const isDsdSource = (): boolean => {
  const path = resolvedPath();
  if (!path) return false;
  return isDsdFile() || path.dsdMode === "dsd" || path.dsdMode === "pcm_to_dsd";
};

/** The DSD rate in MHz, as a hardware panel prints it: `5.6448 MHz`. */
export function dsdRateLabel(hz: number): string {
  if (hz <= 0) return "—";
  return `${(hz / 1_000_000).toFixed(4)} MHz`;
}

/** The DSD family name the rate belongs to: DSD64 = 2.8224 MHz, and each
 * step doubles it. */
export function dsdFamily(hz: number): string {
  if (hz <= 0) return "DSD";
  const step = Math.round(hz / 2_822_400);
  const multiplier = Math.max(1, step) * 64;
  return `DSD${multiplier}`;
}

/** What the panel calls the file: the engine's own title for the track when
 * it has one, else the file's last path component. A remote cache file's
 * name is a hash, so its title (from the server) is the only readable name —
 * the caller passes the playing track's title in. */
export function displayName(path: string | null, title: string): string {
  if (title !== "") return title;
  if (!path) return "—";
  const name = path.replace(/\\/g, "/").split("/").pop() ?? "";
  return isRemoteCachePath(path) ? "远程缓存" : name;
}

/** A rate policy for `engine.setPolicy`. */
export type RateKind = "native" | "device" | "fixed";

/** The dither algorithm the engine applies (`NAE_DITHER_*`). */
export type DitherKind = "auto" | "off" | "tpdf" | "noise_shaped";

/** The output policy JSON, built from the *current* stream's other half.
 *
 * The engine takes one policy object at a time, so a control that changes only
 * the rate must carry the share mode it did not change — and vice versa. This
 * is the one place that composes it, which is why changing the resampler can no
 * longer flip an exclusive stream to shared behind the user's back (it did:
 * the setter hardcoded `"shared"`).
 */
export function policyJson(
  rate: RateKind,
  fixedRate: number,
  share: "exclusive" | "shared",
  dither: DitherKind = "auto",
): string {
  const ratePart =
    rate === "fixed" ? `{ "mode": "fixed", "rate_hz": ${Math.round(fixedRate)} }` : `{ "mode": "${rate}" }`;
  return `{ "rate": ${ratePart}, "quality": "very_high", "share": "${share}", "dither": "${dither}" }`;
}

/** The dither algorithm the stream is actually applying, as the engine
 * resolved it. `auto` when no stream or the engine chose. */
export function currentDither(): DitherKind {
  const path = resolvedPath();
  if (!path) return "auto";
  if (!path.dithered) return "off";
  // The resolved JSON's `ditherMode` is `NAE_DITHER_*`.
  const mode = path.ditherMode;
  if (mode === 1) return "tpdf";
  if (mode === 3) return "noise_shaped";
  if (mode === 2) return "tpdf";
  return "auto";
}

/** The share mode the stream is actually in, as policy JSON wants it. The
 * engine reports `NAE_SHARE_MODE_*`; unknown reads as shared, the app's own
 * default. */
export function currentShare(): "exclusive" | "shared" {
  return resolvedPath()?.shareMode === 1 ? "exclusive" : "shared";
}

/** The rate policy the stream is actually in, as policy JSON wants it. */
export function currentRate(): { kind: RateKind; fixedRate: number } {
  const path = resolvedPath();
  if (!path) return { kind: "device", fixedRate: 0 };
  if (!path.resampling) return { kind: "native", fixedRate: 0 };
  return { kind: "device", fixedRate: 0 };
}

/** Whether the file is a remote download (a hash in the cache): the engine
 * has no tags for it, so the panel shows what the remote listing called it. */
export const isRemoteCachePath = (path: string): boolean => {
  const name = path.replace(/\\/g, "/").split("/").pop() ?? "";
  return /^[0-9a-f]{16,}\.[a-z0-9]{2,5}$/i.test(name);
};

/** The format the source is, as the panel's first line reads it. */
export function sourceFormatLabel(): string {
  const path = resolvedPath();
  if (!path) return "—";
  const dsdHz = path.dsdRate > 0 ? path.dsdRate : isDsdFile() ? path.sourceRate : 0;
  if (dsdHz > 0) {
    return `${dsdFamily(dsdHz)} ${dsdRateLabel(dsdHz)} · ${path.sourceChannels || path.channels} ch / 1-bit`;
  }
  const rate = path.sourceRate || path.sampleRate;
  return `${formatHz(rate)} · ${path.channels || path.sourceChannels} ch`;
}

/** The codec line: what the decoder produced, in the engine's terms. */
export function codecLabel(): string {
  const path = resolvedPath();
  if (!path) return "—";
  if (isDsdSource()) {
    if (path.dsdPcmFallback || path.dsdExtracted) return "DSD → PCM 抽取";
    return path.dsdTransport === "dop" ? "DSD / DoP" : "DSD / Native";
  }
  const depth = path.validBits || path.containerBits;
  return depth > 0 ? `${path.isFloat ? "FLOAT" : "PCM"} · ${depth}/${path.containerBits}-BIT` : "PCM";
}

export function formatHz(hz: number): string {
  if (hz <= 0) return "—";
  if (hz >= 1000) {
    const k = hz / 1000;
    return `${Number.isInteger(k) ? k : k.toFixed(1)} kHz`;
  }
  return `${hz} Hz`;
}
