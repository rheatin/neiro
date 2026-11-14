// Formatting facts the pages and the drawer show (rates in their natural
// units, a volume as a percentage). Not part of the models: the drawer's
// vocabulary, kept beside it.

/** `44100` reads "44.1 kHz", `96000` "96 kHz". */
export function formatRate(hz: number): string {
  if (!Number.isFinite(hz) || hz <= 0) return "—";
  if (hz >= 1000) {
    const khz = hz / 1000;
    const rounded = Number.isInteger(khz) ? String(khz) : khz.toFixed(1);
    return `${rounded.replace(/\.0$/, "")} kHz`;
  }
  return `${Math.round(hz)} Hz`;
}

