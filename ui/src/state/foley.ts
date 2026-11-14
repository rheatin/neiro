// The control surface's own foley: short mechanical sounds, synthesised as
// PCM16 WAV bytes and loaded through `Sound` (NSH has no WebAudio, and a
// bundled sample would be a file to ship and a licence to carry).
//
// These play on the system's own output, separate from the HiFi stream: they
// are UI feedback, not audio. One switch silences them all, and the switch is
// remembered.
//
// Each sound is a few tens of milliseconds of an oscillator through a filter
// with a fast exponential decay, which is what the hardware does: a switch is
// a click, a detent is a metal snap, a relay is two thumps.

import { Sound, Storage } from "nsh";

const FOLEY_KEY = "ui:foley";
/** 44.1 kHz is enough for a 2.4 kHz click and keeps the byte arrays small. */
const RATE = 44100;

type Voice = (samples: Float32Array, rate: number) => void;

let enabled = true;
/** Loaded sounds, by name; `null` while loading or when loading failed. */
const loaded = new Map<string, Sound | null>();
let booted = false;

// Read the remembered switch once; until it arrives the default is on.
Storage.getItem(FOLEY_KEY)
  .then((saved) => {
    if (saved !== null) enabled = saved === "1";
  })
  .catch(() => {});

export const foleyEnabled = (): boolean => enabled;

export function setFoleyEnabled(on: boolean): void {
  enabled = on;
  void Storage.setItem(FOLEY_KEY, on ? "1" : "0").catch(() => {});
}

/** A WAV file's bytes around `samples` (mono, 16-bit). */
function wav(samples: Float32Array, rate: number): Uint8Array {
  const dataBytes = samples.length * 2;
  const buffer = new ArrayBuffer(44 + dataBytes);
  const view = new DataView(buffer);
  const ascii = (offset: number, text: string) => {
    for (let i = 0; i < text.length; i++) view.setUint8(offset + i, text.charCodeAt(i));
  };
  ascii(0, "RIFF");
  view.setUint32(4, 36 + dataBytes, true);
  ascii(8, "WAVE");
  ascii(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true); // PCM
  view.setUint16(22, 1, true); // mono
  view.setUint32(24, rate, true);
  view.setUint32(28, rate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  ascii(36, "data");
  view.setUint32(40, dataBytes, true);
  for (let i = 0; i < samples.length; i++) {
    const clamped = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(44 + i * 2, Math.round(clamped * 32767), true);
  }
  return new Uint8Array(buffer);
}

/** A short burst: `voice` writes one sample per index, `ms` long. */
function render(ms: number, voice: Voice): Uint8Array {
  const count = Math.max(1, Math.round((RATE * ms) / 1000));
  const samples = new Float32Array(count);
  voice(samples, RATE);
  return wav(samples, RATE);
}

/** A resonant click: a square whose pitch falls fast through a band-pass. */
function click(ms: number, startHz: number, endHz: number, centre: number, q: number, gain: number): Uint8Array {
  return render(ms, (out, rate) => {
    let phase = 0;
    let low = 0;
    let band = 0;
    const decay = Math.exp(-1 / (0.0008 * rate));
    let envelope = gain;
    for (let i = 0; i < out.length; i++) {
      const t = i / out.length;
      const hz = startHz + (endHz - startHz) * t;
      phase += (2 * Math.PI * hz) / rate;
      const square = Math.sin(phase) >= 0 ? 1 : -1;
      // A two-pole band-pass (a state-variable filter), so the click has a
      // pitch centre instead of a broadband spit.
      const f = 2 * Math.sin((Math.PI * centre) / rate);
      low += f * band;
      const high = square - low - q * band;
      band += f * high;
      out[i] = band * envelope;
      envelope *= decay;
    }
  });
}

/** A low thump: a sine falling from 145 Hz to 64 Hz with a short body. */
function thump(ms: number, gain: number): Uint8Array {
  return render(ms, (out, rate) => {
    let phase = 0;
    for (let i = 0; i < out.length; i++) {
      const t = i / out.length;
      const hz = 145 - 81 * Math.min(1, t * 1.6);
      phase += (2 * Math.PI * hz) / rate;
      const envelope = gain * Math.exp(-t * 6);
      out[i] = Math.sin(phase) * envelope;
    }
  });
}

/** The four sounds, made once and cached by name. */
const BUILDERS: Record<string, () => Uint8Array> = {
  // A microswitch: ~2.4 kHz, 7 ms.
  key: () => click(9, 2100, 520, 1800, 2.2, 0.5),
  // The 0 dB detent: a brighter, longer snap.
  snap: () => click(12, 2400, 920, 2250, 4.0, 0.75),
  // An encoder passing one mark: quieter, shorter, a little lower.
  tick: () => click(6, 1450, 640, 1200, 1.6, 0.28),
  // A heavy relay: two thumps, the second softer.
  relay: () => {
    const first = thump(38, 0.85);
    const second = thump(30, 0.45);
    const a = new Float32Array((first.length - 44) / 2);
    const b = new Float32Array((second.length - 44) / 2);
    for (let i = 0; i < a.length; i++) a[i] = new DataView(first.buffer).getInt16(44 + i * 2, true) / 32767;
    for (let i = 0; i < b.length; i++) b[i] = new DataView(second.buffer).getInt16(44 + i * 2, true) / 32767;
    const gap = Math.round(RATE * 0.028);
    const joined = new Float32Array(a.length + gap + b.length);
    joined.set(a, 0);
    joined.set(b, a.length + gap);
    return wav(joined, RATE);
  },
};

/** Loads every sound once; `Sound.load` decodes into memory and each play is
 * cheap. A platform without sound leaves every entry `null` and the calls
 * below become no-ops. */
function boot(): void {
  if (booted) return;
  booted = true;
  for (const [name, build] of Object.entries(BUILDERS)) {
    loaded.set(name, null);
    Sound.load(build())
      .then((sound) => loaded.set(name, sound))
      .catch((error) => {
        // A platform without sound, or a refused capability: the panel keeps
        // working, silently. One line, not a toast.
        console.warn(`neiro: 音效 ${name} 没有加载:`, String(error));
        loaded.set(name, null);
      });
  }
}

function play(name: string, volume = 1): void {
  if (!enabled) return;
  boot();
  loaded.get(name)?.play({ volume });
}

/** A microswitch: the step keys, the mode keys, the reset keys. */
export const playKey = (): void => play("key", 0.9);
/** The magnetic detent at 0 dB, and a double-click reset. */
export const playSnap = (): void => play("snap", 0.85);
/** An encoder passing a mark. */
export const playTick = (): void => play("tick", 0.6);
/** The bit-perfect bus relay. */
export const playRelay = (): void => play("relay", 0.9);
