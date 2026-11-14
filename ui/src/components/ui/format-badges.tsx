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
import { resolvedPath } from "../../state/resolved";

/** What the source's numbers are worth, as a listener reads it. */
export type Grade = "HI-RES+" | "HI-RES" | "CD" | "DSD" | "PCM";

/** The grade a source earns: DSD is its own thing; otherwise the higher of
 * rate and depth decides. Plain 16/44.1 is CD; anything above it is Hi-Res. */
export function gradeOf(rateHz: number, bits: number, dsd: boolean): Grade {
  if (dsd) return "DSD";
  if (rateHz >= 352_800 || bits >= 32) return "HI-RES+";
  if (rateHz >= 96_000 || bits >= 24) return "HI-RES";
  if (rateHz >= 44_100 && bits >= 16) return "CD";
  return "PCM";
}

const LOSSY = /\.(mp3|aac|m4a|opus|ogg|wma|mp2|ac3)$/i;
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
    if (/^(aac|mp3|mp2|opus|vorbis|ac3|eac3|wmav?)/i.test(codec)) return false;
    // `pcm_s24le` and friends: lossless by construction.
    if (/^pcm/i.test(codec)) return true;
  }
  if (LOSSY.test(path)) return false;
  return LOSSLESS.test(path);
}

/** The codec a lossy file is in, named so the reader sees why no grade plate
 * appeared. `AAC` for the .m4a family, which is what a phone writes. */
export function lossyCodec(path: string): string {
  const match = path.match(/\.([a-z0-9]+)$/i);
  const ext = match ? match[1].toLowerCase() : "";
  if (ext === "m4a" || ext === "aac") return "AAC";
  if (ext === "mp3" || ext === "mp2") return "MP3";
  if (ext === "opus") return "OPUS";
  if (ext === "ogg") return "VORBIS";
  if (ext === "wma") return "WMA";
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
      <Text class="font-mono text-[9px] font-extrabold tracking-[0.08em]" style={{ color: tone().text }}>
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
        class={`text-[9px] ${solid ? "font-bold lowercase" : "font-mono font-semibold"}`}
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
        class="font-mono text-[9px] font-bold tracking-[0.08em]"
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
  const lossless = () => isLossless(engine.player().path ?? "", engine.player().codec);
  const grade = () => {
    const p = path();
    if (!p) return null;
    const dsdHz = p.dsdRate > 0 ? p.dsdRate : p.sourceRate >= 2_822_400 ? p.sourceRate : 0;
    if (dsdHz > 0) return "DSD" as Grade;
    if (!lossless()) return null;
    return gradeOf(p.sourceRate, p.validBits || p.containerBits, false);
  };
  const bitPerfect = () => engine.renderPath()?.bitPerfect === true;
  return (
    <View class={`flex-row items-center gap-1 ${props.wrap === false ? "flex-nowrap" : "flex-wrap"} ${props.class ?? ""}`}>
      <If when={grade() !== null}>
        <GradeBadge grade={grade()!} />
      </If>
      <If when={lossless()}>
        <Plate label="LOSSLESS" tone="plain" />
      </If>
      {/* A lossy file is still named: a row of plates that says nothing
          leaves the reader wondering whether the badges failed to load. */}
      <If when={!props.hideCodec && !lossless() && grade() === null}>
        <Plate label={lossyCodec(engine.player().path ?? "")} tone="plain" />
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
