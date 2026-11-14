// The control surface's hardware: the chassis, the recessed module bays, the
// OLED panel, hex screws, the mechanical keycap, the fader slot and cap, the
// knurled encoder. Every piece is a View with the demo's gradients and inner
// shadows: the demo's look comes from those, not from flat fills.
//
// Colours are literals here on purpose. This is one instrument's finish, not
// a theme: the chassis is #16181C→#101114 brushed aluminium whether the app
// is in light or dark appearance, exactly as a piece of hardware is.

import { Text, View, type JSX } from "nsh";

/** The panel's own palette. */
export const HARDWARE = {
  chassisTop: "#16181C",
  chassisBottom: "#101114",
  chassisBorder: "#2B2E36",
  bayTop: "#191B1F",
  bayBottom: "#141519",
  bayBorder: "#262930",
  screenBg: "#0A0B0D",
  screenBorder: "#272A32",
  keyTop: "#25282E",
  keyBottom: "#1B1D22",
  keyBorder: "#333640",
  amber: "#F5A623",
  amberBright: "#FFB83D",
  cyan: "#00E5FF",
  magenta: "#FF2A85",
  emerald: "#10B981",
  text: "#E5E7EB",
  textDim: "#8A8F99",
  textFaint: "#5C6169",
} as const;

/** The brushed aluminium chassis: a hairline vertical grain over a vertical
 * gradient, an outer edge and two inner edges (a light top, a dark bottom). */
export function Chassis(props: { children: JSX.Element; class?: string }) {
  return (
    <View
      class={`rounded-xl ${props.class ?? ""}`}
      style={{
        backgroundImage: `repeating-linear-gradient(90deg, rgba(255,255,255,0.012) 0px, rgba(255,255,255,0.012) 1px, transparent 1px, transparent 3px), linear-gradient(180deg, ${HARDWARE.chassisTop} 0%, ${HARDWARE.chassisBottom} 100%)`,
        borderWidth: 1,
        borderColor: HARDWARE.chassisBorder,
        boxShadow: [
          { x: 0, y: 24, blur: 60, color: "#000000e6" },
          { x: 0, y: 1, blur: 0, color: "#ffffff17", inset: true },
          { x: 0, y: -1, blur: 0, color: "#000000f2", inset: true },
        ],
      }}
    >
      {props.children}
    </View>
  );
}

/** A recessed module bay: sunk into the chassis by an inner shadow. */
export function Bay(props: { children: JSX.Element; class?: string; style?: Record<string, unknown> }) {
  return (
    <View
      class={`rounded-lg ${props.class ?? ""}`}
      style={{
        backgroundImage: `linear-gradient(180deg, ${HARDWARE.bayTop} 0%, ${HARDWARE.bayBottom} 100%)`,
        borderWidth: 1,
        borderColor: HARDWARE.bayBorder,
        boxShadow: [
          { x: 0, y: 2, blur: 6, color: "#000000a6", inset: true },
          { x: 0, y: 1, blur: 0, color: "#ffffff0a" },
        ],
        ...props.style,
      }}
    >
      {props.children}
    </View>
  );
}

/** The OLED monitor: near-black, sunk deeper than a bay, with a faint amber
 * bloom from its top edge. */
export function Screen(props: { children: JSX.Element; class?: string; style?: Record<string, unknown> }) {
  return (
    <View
      class={`rounded-md ${props.class ?? ""}`}
      style={{
        backgroundImage: `radial-gradient(ellipse at 50% 0%, rgba(245,166,35,0.06) 0%, transparent 72%), linear-gradient(180deg, ${HARDWARE.screenBg}, ${HARDWARE.screenBg})`,
        borderWidth: 1,
        borderColor: HARDWARE.screenBorder,
        boxShadow: [
          { x: 0, y: 3, blur: 14, color: "#000000f5", inset: true },
          { x: 0, y: 0, blur: 0, spread: 1, color: "#000000d9", inset: true },
          { x: 0, y: 1, blur: 0, color: "#ffffff0d" },
        ],
        ...props.style,
      }}
    >
      {props.children}
    </View>
  );
}

/** A corner hex screw: a machined disc with a hex socket. */
export function HexScrew() {
  return (
    <View
      class="items-center justify-center rounded-full"
      style={{
        width: 11,
        height: 11,
        backgroundImage: "radial-gradient(circle at 35% 35%, #3E414B 0%, #1B1C20 75%)",
        boxShadow: [
          { x: 0, y: 1, blur: 1, color: "#ffffff33", inset: true },
          { x: 0, y: 1, blur: 2, color: "#000000e6" },
        ],
      }}
      accessibilityElementsHidden
    >
      <View
        style={{
          width: 5,
          height: 5,
          backgroundColor: "#0E0F11",
          // A hexagon, as a rotated square pair: the demo's clip-path has no
          // counterpart in this style system.
          transform: [{ rotate: "45deg" }],
        }}
      />
    </View>
  );
}

/** The mechanical keycap: a slab that sits 3pt proud of its bay and sinks
 * 1.5pt when pressed. `lit` is the amber engaged state; `tone` picks another
 * LED colour for the racks that need one (the EQ's cyan, OPRA's magenta). */
export function MechKey(props: {
  children: JSX.Element;
  onPress?: () => void;
  lit?: boolean;
  tone?: "amber" | "cyan" | "magenta" | "emerald";
  disabled?: boolean;
  label?: string;
  class?: string;
}) {
  const tone = () => {
    switch (props.tone ?? "amber") {
      case "cyan":
        return { fill: "rgba(0,229,255,0.22)", fill2: "rgba(0,229,255,0.09)", edge: "rgba(0,229,255,0.72)", text: "#67E8F9", glow: "rgba(0,229,255,0.20)", foot: "#06181C" };
      case "magenta":
        return { fill: "rgba(255,42,133,0.24)", fill2: "rgba(255,42,133,0.10)", edge: "rgba(255,42,133,0.75)", text: "#FF70AE", glow: "rgba(255,42,133,0.22)", foot: "#1D0711" };
      case "emerald":
        return { fill: "rgba(16,185,129,0.24)", fill2: "rgba(16,185,129,0.11)", edge: "rgba(16,185,129,0.75)", text: "#34D399", glow: "rgba(16,185,129,0.24)", foot: "#071610" };
      default:
        return { fill: "rgba(245,166,35,0.24)", fill2: "rgba(245,166,35,0.11)", edge: "rgba(245,166,35,0.72)", text: HARDWARE.amberBright, glow: "rgba(245,166,35,0.22)", foot: "#141008" };
    }
  };
  return (
    <View
      class={props.class}
      onPress={props.disabled ? undefined : props.onPress}
      accessibilityLabel={props.label}
      accessibilityState={{ disabled: props.disabled ?? false, selected: props.lit ?? false }}
      style={{
        backgroundImage: props.lit
          ? `linear-gradient(180deg, ${tone().fill} 0%, ${tone().fill2} 100%)`
          : `linear-gradient(180deg, ${HARDWARE.keyTop} 0%, ${HARDWARE.keyBottom} 100%)`,
        borderWidth: 1,
        borderColor: props.lit ? tone().edge : HARDWARE.keyBorder,
        boxShadow: props.lit
          ? [
              { x: 0, y: 2, blur: 0, color: tone().foot },
              { x: 0, y: 0, blur: 14, color: tone().glow },
              { x: 0, y: 1, blur: 0, color: "rgba(255,210,120,0.32)", inset: true },
            ]
          : [
              { x: 0, y: 3, blur: 0, color: "#0D0E10" },
              { x: 0, y: 4, blur: 8, color: "#000000a6" },
              { x: 0, y: 1, blur: 0, color: "#ffffff1f", inset: true },
            ],
        opacity: props.disabled ? 0.44 : 1,
      }}
      // The key's own press feedback: 1.5pt down and the shadow collapses.
      activeStyle={{
        transform: [{ translateY: 1.5 }],
        boxShadow: [
          { x: 0, y: 1, blur: 0, color: "#0D0E10" },
          { x: 0, y: 1, blur: 3, color: "#000000cc" },
          { x: 0, y: 1, blur: 2, color: "#00000099", inset: true },
        ],
      }}
    >
      {props.children}
    </View>
  );
}

/** The fader's recessed slot: a dark groove with a machined rail down its
 * middle and a bright 0 dB datum line at `datum` (0…1 from the top). */
export function FaderSlot(props: { height: number; datum?: number; lit?: boolean }) {
  const datum = () => props.datum ?? 0.5;
  return (
    <View
      class="items-center justify-center"
      style={{
        width: 14,
        height: props.height,
        borderRadius: 3,
        backgroundColor: "#0A0B0D",
        borderWidth: 1,
        borderColor: "#22242A",
        boxShadow: [
          { x: 0, y: 2, blur: 5, color: "#000000f2", inset: true },
          { x: 0, y: 1, blur: 0, color: "#ffffff0f" },
        ],
      }}
      accessibilityElementsHidden
    >
      <View style={{ position: "absolute", width: 3, height: props.height - 6, borderRadius: 2, backgroundColor: "#1A1C20" }} />
      {/* The datum: the one line a fader is read against. */}
      <View
        style={{
          position: "absolute",
          left: 0,
          right: 0,
          height: 1,
          top: props.height * datum(),
          backgroundColor: props.lit ? HARDWARE.amberBright : "rgba(245,166,35,0.55)",
          boxShadow: props.lit ? [{ x: 0, y: 0, blur: 4, color: "rgba(245,166,35,0.8)" }] : undefined,
        }}
      />
    </View>
  );
}

/** The fader cap: a machined slab with a groove across it. Its `position` is
 * 0…1 from the top of the slot. */
export function FaderCap(props: { position: number; dragging?: boolean; detented?: boolean; width?: number }) {
  const width = () => props.width ?? 26;
  const hot = () => props.dragging || props.detented;
  return (
    <View
      class="items-center justify-center"
      style={{
        position: "absolute",
        width: width(),
        height: 18,
        borderRadius: 3,
        // The groove: the demo's gradient turns at 50% to read as a machined
        // channel cut across the cap.
        backgroundImage:
          "linear-gradient(180deg, #3D4049 0%, #2B2D34 18%, #1F2126 46%, #121316 50%, #1F2126 54%, #2B2D34 82%, #191A1F 100%)",
        borderWidth: 1,
        borderColor: hot() ? (props.detented ? HARDWARE.amberBright : HARDWARE.amber) : "#484C57",
        boxShadow: hot()
          ? [
              { x: 0, y: 8, blur: 16, color: "#000000e6" },
              { x: 0, y: 0, blur: 14, color: "rgba(245,166,35,0.28)" },
              { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.38)", inset: true },
            ]
          : [
              { x: 0, y: 6, blur: 12, color: "#000000d9" },
              { x: 0, y: 2, blur: 3, color: "#000000f2" },
              { x: 0, y: 1, blur: 0, color: "#ffffff47", inset: true },
              { x: 0, y: -1, blur: 0, color: "#000000d9", inset: true },
            ],
      }}
      accessibilityElementsHidden
    >
      <View style={{ width: width() - 8, height: 1, backgroundColor: "rgba(0,0,0,0.85)" }} />
    </View>
  );
}

/** The knurled encoder cap: a conic gradient reads as knurling, with a
 * pointer notch and a lit ring when engaged. */
export function EncoderCap(props: { size: number; angle: number; engaged?: boolean; dragging?: boolean }) {
  const hot = () => props.engaged || props.dragging;
  return (
    <View
      class="items-center justify-center"
      style={{
        width: props.size,
        height: props.size,
        borderRadius: props.size / 2,
        backgroundImage:
          "radial-gradient(circle at 32% 28%, rgba(255,255,255,0.16) 0%, rgba(255,255,255,0.02) 45%, transparent 72%), conic-gradient(from 0deg, #25272D, #1B1C20, #2A2C33, #18191D, #25272D)",
        boxShadow: hot()
          ? [
              { x: 0, y: 10, blur: 22, color: "#000000eb" },
              { x: 0, y: 1, blur: 1.5, color: "#ffffff4d", inset: true },
              { x: 0, y: 0, blur: 0, spread: 1.5, color: HARDWARE.amber },
              { x: 0, y: 0, blur: 18, color: "rgba(245,166,35,0.26)" },
            ]
          : [
              { x: 0, y: 8, blur: 18, color: "#000000e0" },
              { x: 0, y: 2, blur: 4, color: "#000000f2" },
              { x: 0, y: 1, blur: 1.5, color: "#ffffff38", inset: true },
              { x: 0, y: -2, blur: 4, color: "#000000e6", inset: true },
              { x: 0, y: 0, blur: 0, spread: 1.5, color: "#101113" },
            ],
      }}
      accessibilityElementsHidden
    >
      {/* The pointer notch, turned by `angle` from 12 o'clock. */}
      <View style={{ position: "absolute", width: props.size, height: props.size, transform: [{ rotate: `${props.angle}deg` }] }}>
        <View
          style={{
            position: "absolute",
            left: props.size / 2 - 1,
            top: 4,
            width: 2,
            height: Math.max(6, props.size * 0.18),
            borderRadius: 1,
            backgroundColor: hot() ? HARDWARE.amberBright : "#6B7078",
          }}
        />
      </View>
    </View>
  );
}

/** An LED: a small disc that glows in `tone` when lit, dark grey when not. */
export function Led(props: { lit: boolean; tone?: "amber" | "cyan" | "magenta" | "emerald" | "red"; size?: number; pulse?: number }) {
  const size = () => props.size ?? 7;
  const colour = () => {
    switch (props.tone) {
      case "cyan":
        return HARDWARE.cyan;
      case "magenta":
        return HARDWARE.magenta;
      case "emerald":
        return HARDWARE.emerald;
      case "red":
        return "#F1676C";
      default:
        return HARDWARE.amber;
    }
  };
  return (
    <View
      class="rounded-full"
      style={{
        width: size(),
        height: size(),
        backgroundColor: props.lit ? colour() : "#2A2E31",
        opacity: props.pulse !== undefined ? props.pulse : props.lit ? 1 : 0.85,
        borderWidth: 1,
        borderColor: props.lit ? colour() : "#0F1113",
        boxShadow: props.lit
          ? [
              { x: 0, y: 0, blur: 6, color: colour() },
              { x: 0, y: 0, blur: 14, color: colour() },
            ]
          : [{ x: 0, y: 1, blur: 2, color: "#000000d9", inset: true }],
      }}
      accessibilityElementsHidden
    />
  );
}

/** The silkscreen label: a tiny uppercase caption, the way a panel is
 * printed. */
export function Silkscreen(props: { children: string; tone?: "dim" | "faint" | "amber" | "text"; class?: string }) {
  const colour = () => {
    switch (props.tone) {
      case "amber":
        return HARDWARE.amber;
      case "text":
        return HARDWARE.text;
      case "faint":
        return HARDWARE.textFaint;
      default:
        return HARDWARE.textDim;
    }
  };
  return (
    <Text
      class={`text-[9px] font-bold uppercase tracking-[0.14em] ${props.class ?? ""}`}
      style={{ color: colour() }}
    >
      {props.children}
    </Text>
  );
}

/** The module's numbered title bar: `01 // SOURCE IN`. */
export function ModuleHeader(props: { index: string; title: string; right?: JSX.Element }) {
  return (
    <View class="flex-row items-center gap-2 px-1 pt-0.5 pb-2">
      <View
        class="items-center justify-center rounded"
        style={{ width: 22, height: 16, backgroundColor: "rgba(245,166,35,0.14)", borderWidth: 1, borderColor: "rgba(245,166,35,0.5)" }}
      >
        <Text class="font-mono text-[9px] font-bold" style={{ color: HARDWARE.amberBright }}>
          {props.index}
        </Text>
      </View>
      <Text class="text-[11px] font-bold uppercase tracking-[0.18em]" style={{ color: HARDWARE.text }}>
        {props.title}
      </Text>
      <View class="flex-1" />
      {props.right}
    </View>
  );
}

/** The signal bus between two bays: a vertical amber line with a connector
 * dot in the middle, brighter while audio flows. */
export function BusConduit(props: { flowing?: boolean; height?: number }) {
  return (
    <View class="items-center justify-center" style={{ height: props.height ?? 16 }} accessibilityElementsHidden>
      <View
        style={{
          width: 2,
          height: "100%",
          backgroundImage: props.flowing
            ? "linear-gradient(180deg, rgba(245,166,35,0.25), rgba(245,166,35,0.85), rgba(245,166,35,0.25))"
            : "linear-gradient(180deg, rgba(245,166,35,0.10), rgba(245,166,35,0.35), rgba(245,166,35,0.10))",
        }}
      />
      <View
        style={{
          position: "absolute",
          width: 7,
          height: 7,
          borderRadius: 4,
          backgroundColor: "#0C0E10",
          borderWidth: 1,
          borderColor: props.flowing ? "rgba(245,166,35,0.9)" : "rgba(245,166,35,0.4)",
          boxShadow: props.flowing ? [{ x: 0, y: 0, blur: 6, color: "rgba(245,166,35,0.7)" }] : undefined,
        }}
      />
    </View>
  );
}

