// The format plates: what the file is (HI-RES / CD / DSD / LOSSLESS) and
// whether the stream came out bit-perfect.
//
// One module because two surfaces ask the same question — the 输出控制台's
// source stage and the Now Playing sheet — and two answers to "is this a CD"
// would drift. The grade reads the source's own numbers; bit-perfect reads the
// engine's verdict, and is drawn only when the engine says yes: an absent
// plate is the honest way to say "not bit-perfect", a greyed one invites the
// reader to look for the reason on a page that cannot give it.
//
// LOSSLESS is a container fact, not a rate: flac/wav/aiff/alac carry the
// samples, mp3/aac/opus do not. A lossy file never gets a grade plate.

import { Text, token, View } from "nsh";
import { HARDWARE } from "./hardware";
import { engine } from "../../backend/engine";
import { isStreamBitPerfect, resolvedPath, sourceBits } from "../../state/resolved";

/** What the source's numbers are worth, as a listener reads it. */
export type Grade = "HI-RES+" | "HI-RES" | "CD" | "DSD" | "PCM";

/** The grade a source earns.
 *
 * Hi-Res is a *both* claim, which is what the Japan Audio Society's definition
 * says and what the badges are read as: 24-bit words **and** a rate above the
 * CD's 44.1 kHz. A 24-bit file at 44.1 kHz carries more headroom, not more
 * music — it is still a CD's band, and calling it Hi-Res is the kind of
 * flattery a listener cannot hear. */
export function gradeOf(rateHz: number, bits: number, dsd: boolean): Grade {
  if (dsd) return "DSD";
  if (rateHz >= 352_800 || (rateHz >= 176_400 && bits >= 24) || (rateHz >= 88_200 && bits >= 32)) return "HI-RES+";
  if (bits >= 24 && rateHz >= 88_200) return "HI-RES";
  if (rateHz >= 44_100 && bits >= 16) return "CD";
  return "PCM";
}

const LOSSY = /\.(mp3|aac|m4a|m4b|mp4|opus|ogg|wma|mp2|ac3|ec3|eac3|mka)$/i;
const LOSSLESS = /\.(flac|wav|wave|aif|aiff|alac|ape|wv|dsf|dff)$/i;

/** Whether the file carries its samples as a codec that keeps them. An
 * unknown extension is not claimed either way.
 *
 * The extension is a guess: `.m4a` carries AAC and ALAC alike. When the
 * analysis has named the codec, that answer wins — it is the one the engine
 * actually decoded with. */
export function isLossless(path: string, codec?: string): boolean {
  if (codec && codec !== "") {
    if (/^(alac|flac|ape|wavpack|wv|dsd|dff|mlp|truehd|tta)/i.test(codec)) return true;
    if (/^(aac|mp3|mp2|opus|vorbis|ac3|eac3|ec3|wmav?)/i.test(codec)) return false;
    // `pcm_s24le` and friends: lossless by construction.
    if (/^pcm/i.test(codec)) return true;
  }
  if (LOSSY.test(path)) return false;
  return LOSSLESS.test(path);
}

/** The codec the engine decoded with, as a listener reads it. The .m4a box
 * carries AAC, ALAC and E-AC-3 alike, so its extension cannot name the codec. */
export function codecDisplayName(codec: string): string {
  const c = codec.toLowerCase();
  if (c === "eac3" || c === "ec3") return "E-AC-3";
  if (c === "ac3") return "AC-3";
  return codec.toUpperCase();
}

/** The engine's codec profile is the only fact that separates E-AC-3 JOC
 * (Dolby Atmos) from plain DD+ 5.1; both report the codec `eac3`. */
export function isAtmos(codecProfile: string): boolean {
  return /atmos/i.test(codecProfile);
}

/** The codec a lossy file is in, named so the reader sees why no grade plate
 * appeared. The engine's own answer wins over the extension guess. */
export function lossyCodec(path: string, codec?: string): string {
  if (codec && codec !== "") return codecDisplayName(codec);
  const match = path.match(/\.([a-z0-9]+)$/i);
  const ext = match ? match[1].toLowerCase() : "";
  if (ext === "m4a" || ext === "aac") return "AAC";
  if (ext === "mp3" || ext === "mp2") return "MP3";
  if (ext === "opus") return "OPUS";
  if (ext === "ogg") return "VORBIS";
  if (ext === "wma") return "WMA";
  if (ext === "ec3" || ext === "eac3") return "E-AC-3";
  if (ext === "ac3") return "AC-3";
  return ext === "" ? "LOSSY" : ext.toUpperCase();
}

/** The gold sticker for Hi-Res, cyan for DSD, quiet for CD. */
export function GradeBadge(props: { grade: Grade; size?: "sm" | "md" }) {
  const tone = () => {
    switch (props.grade) {
      case "HI-RES+":
        return { fill: "linear-gradient(180deg, #FFD98A 0%, #F0A62B 55%, #C97F16 100%)", text: "#1A1206", edge: "#FFE3A6", glow: "rgba(245,166,35,0.55)" };
      case "HI-RES":
        return { fill: "linear-gradient(180deg, #F7C764 0%, #D89A2C 55%, #A96F14 100%)", text: "#1A1206", edge: "#FFDF9E", glow: "rgba(245,166,35,0.45)" };
      case "DSD":
        return { fill: "linear-gradient(180deg, rgba(0,229,255,0.34) 0%, rgba(0,229,255,0.14) 100%)", text: "#9DF3FF", edge: "rgba(0,229,255,0.8)", glow: "rgba(0,229,255,0.35)" };
      case "CD":
        return { fill: "linear-gradient(180deg, rgba(255,255,255,0.16) 0%, rgba(255,255,255,0.06) 100%)", text: HARDWARE.text, edge: "rgba(255,255,255,0.42)", glow: "transparent" };
      default:
        return { fill: "", text: HARDWARE.textDim, edge: "#2B2F35", glow: "transparent" };
    }
  };
  const small = (props.size ?? "md") === "sm";
  return (
    <View
      class="items-center justify-center rounded px-1.5"
      style={{
        height: small ? 16 : 18,
        backgroundImage: tone().fill === "" ? undefined : tone().fill,
        backgroundColor: tone().fill === "" ? "transparent" : undefined,
        borderWidth: 1,
        borderColor: tone().edge,
        boxShadow:
          tone().glow === "transparent"
            ? undefined
            : [
                { x: 0, y: 0, blur: 10, color: tone().glow },
                { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.45)", inset: true },
              ],
      }}
      accessibilityLabel={props.grade}
    >
      <Text class="font-mono text-[9px] font-bold" style={{ color: tone().text }}>
        {props.grade}
      </Text>
    </View>
  );
}

/** One fact about a file, as a chip. `solid` is the library's own container
 * chip (inverted, 9pt bold lower-case) — the same mark a remote track wears,
 * so a format looks the same in the list and in the console. `quiet` is the
 * outlined variant the numbers use, so a name and a measurement do not read
 * as the same kind of thing. */
export function MetaChip(props: { label: string; tone?: "solid" | "quiet" }) {
  const solid = (props.tone ?? "solid") === "solid";
  return (
    <View
      class="items-center justify-center rounded-sm px-1.5"
      style={{
        height: 18,
        backgroundColor: solid ? token("muted-foreground") : "transparent",
        borderWidth: solid ? 0 : 1,
        borderColor: "#2B3035",
      }}
      accessibilityLabel={props.label}
    >
      <Text
        class={`text-[9px] ${solid ? "font-mono font-bold" : "font-mono font-bold"}`}
        style={{ color: solid ? token("background") : HARDWARE.textDim }}
      >
        {props.label}
      </Text>
    </View>
  );
}

/** One quiet plate: LOSSLESS or BIT-PERFECT. */
function Plate(props: { label: string; tone: "amber" | "plain" }) {
  const amber = props.tone === "amber";
  return (
    <View
      class="items-center justify-center rounded px-1.5"
      style={{
        height: 18,
        borderWidth: 1,
        borderColor: amber ? "rgba(245,166,35,0.85)" : "rgba(255,255,255,0.34)",
        backgroundColor: amber ? "rgba(245,166,35,0.16)" : "rgba(255,255,255,0.07)",
        boxShadow: amber
          ? [{ x: 0, y: 0, blur: 10, color: "rgba(245,166,35,0.35)" }]
          : undefined,
      }}
      accessibilityLabel={props.label}
    >
      <Text
        class="font-mono text-[9px] font-bold"
        style={{ color: amber ? HARDWARE.amberBright : HARDWARE.text }}
      >
        {props.label}
      </Text>
    </View>
  );
}

/** The row of plates for what is playing: the source's grade, LOSSLESS when
 * the container keeps the samples, and BIT-PERFECT only when the engine says
 * the stream came out untouched.
 *
 * Reads the engine's resolved path, so it is empty before a stream opens —
 * the same rule as every other engine number in the app. */
export function NowPlayingBadges(props: {
  class?: string;
  hideCodec?: boolean;
  resampled?: boolean;
  resampleLabel?: string;
  /** Wraps by default (a narrow Now Playing column); a caller that has one
a   * line to spend turns it off, so two plates never stack. */
  wrap?: boolean;
}) {
  const path = () => resolvedPath();
  const codec = () => engine.player().codec;
  const lossless = () => isLossless(engine.player().path ?? "", codec());
  const atmos = () => isAtmos(engine.player().codecProfile);
  const grade = () => {
    const p = path();
    if (!p) return null;
    const dsdHz = p.dsdRate > 0 ? p.dsdRate : p.sourceRate >= 2_822_400 ? p.sourceRate : 0;
    if (dsdHz > 0) return "DSD" as Grade;
    if (!lossless()) return null;
    return gradeOf(p.sourceRate, sourceBits() || 16, false);
  };
  const bitPerfect = () => isStreamBitPerfect();
  return (
    <View class={`flex-row items-center gap-1 ${props.wrap === false ? "flex-nowrap" : "flex-wrap"} ${props.class ?? ""}`}>
      <If when={grade() !== null}>
        <GradeBadge grade={grade()!} />
      </If>
      {/* A Hi-Res file is lossless by definition, so the plate beside its
          grade says nothing. It stays for the CD-and-below case, where it is
          the only statement about the file's samples. */}
      <If when={lossless() && grade() === null}>
        <Plate label="LOSSLESS" tone="plain" />
      </If>
      {/* A lossy file is still named: a row of plates that says nothing
          leaves the reader wondering whether the badges failed to load.
          Atmos names itself, so its plate replaces the codec one. */}
      <If when={!props.hideCodec && !lossless() && grade() === null && !atmos()}>
        <Plate label={lossyCodec(engine.player().path ?? "", codec())} tone="plain" />
      </If>
      <If when={atmos()}>
        <Plate label="DOLBY ATMOS" tone="amber" />
      </If>
      {/* Why the two rates differ. The source column already names the
          container, so this is the one fact the middle column adds. */}
      <If when={props.resampled === true}>
        <Plate label={props.resampleLabel ?? "RESAMPLED"} tone="plain" />
      </If>
      <If when={bitPerfect()}>
        <Plate label="BIT-PERFECT" tone="amber" />
      </If>
    </View>
  );
}
