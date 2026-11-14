// 硬件感的旋钮：一圈刻度点 + 可转的指针 + 下面一个看不见的竖直滑条。
//
// 外观照 demo 的做法：径向渐变的金属帽、内高光、外投影、琥珀色的刻度。
// 位置用三角函数算进 left/top，不用 transform 旋转（旋转原点在各后端不
// 一致，真机上曾挤成一团）。
//
// 行为由垫在下面的 `Slider` 承担：拖拽、键盘、屏幕阅读器的值广播都走它。

import { token, View } from "nsh";
import { Slider } from "nsh/base";
import { focusRing } from "./controls";

/** How many marks the ring has, over 270°. */
const TICKS = 11;
/** Where the ring starts and ends, degrees from 12 o'clock. */
const FROM = -135;
const TO = 135;

/** A point on the knob's ring: `radius` from the centre, `degrees` from 12
 * o'clock, in points inside a `size` box. */
function onRing(size: number, radius: number, degrees: number): { left: number; top: number } {
  const rad = (degrees * Math.PI) / 180;
  return {
    left: size / 2 + radius * Math.sin(rad),
    top: size / 2 - radius * Math.cos(rad),
  };
}

export function Knob(props: {
  /** What screen readers say it is. */
  label: string;
  /** 0…1: where the pointer is and how much of the ring is lit. */
  value: number;
  /** Points across (default 44). */
  size?: number;
  disabled?: boolean;
  /** While dragging: local only, cheap. */
  onChange: (value: number) => void;
  /** On release: the value to send to the engine. */
  onChangeEnd: (value: number) => void;
}) {
  const size = () => props.size ?? 44;
  const value = () => Math.min(1, Math.max(0, props.value));
  const pointerAngle = () => FROM + (TO - FROM) * value();
  const tickAngle = (index: number) => FROM + ((TO - FROM) * index) / (TICKS - 1);
  const lit = (index: number) => index / (TICKS - 1) <= value() + 0.0001;
  const cap = () => size() - 16;
  const tickAt = (index: number) => onRing(size(), size() / 2 - 1.5, tickAngle(index));
  const pointerAt = () => onRing(size(), size() / 2 - 8.5, pointerAngle());
  const accent = String(token("primary"));
  // The unlit marks have to read on the panel's near-black: the border token
  // is too dark there (the demo's #3a3f43 is the level that works).
  const dim = "rgba(255,255,255,0.16)";

  return (
    <View class="items-center justify-center" style={{ width: size(), height: size() }}>
      {/* The ring of marks: one per step, lit up to where the knob is. */}
      <Each of={Array.from({ length: TICKS }, (_, i) => i)} key="index">
        {(index) => (
          <View
            class="absolute rounded-full"
            style={{
              width: 2,
              height: lit(index()) ? 4 : 3,
              left: tickAt(index()).left - 1,
              top: tickAt(index()).top,
              backgroundColor: lit(index()) ? accent : dim,
            }}
            accessibilityElementsHidden
          />
        )}
      </Each>
      {/* The cap: a machined disc — a radial gradient, an inner top
          highlight, and a soft drop under it. */}
      <View
        style={{
          width: cap(),
          height: cap(),
          borderRadius: cap() / 2,
          backgroundImage: `radial-gradient(circle at 34% 24%, #4a5157 0%, #2a3034 46%, #121516 100%)`,
          borderWidth: 1,
          borderColor: "rgba(255,255,255,0.09)",
          boxShadow: [
            { x: 0, y: 1, blur: 2, color: "#000000a6" },
            { x: 0, y: -1, blur: 0, color: "#ffffff14", inset: true },
            { x: 0, y: 1, blur: 0, color: "#000000b3", inset: true },
          ],
        }}
        accessibilityElementsHidden
      />
      {/* The pointer: a short radial mark from the cap's edge. */}
      <View
        style={{
          position: "absolute",
          width: 2,
          height: 7,
          borderRadius: 1,
          left: pointerAt().left - 1,
          top: pointerAt().top - 1,
          backgroundColor: props.disabled ? String(token("muted-foreground")) : accent,
          boxShadow: props.disabled ? undefined : [{ x: 0, y: 0, blur: 4, color: accent }],
        }}
        accessibilityElementsHidden
      />
      {/* The behaviour, invisible on top: a vertical slider over the knob's
          own box (0…1000 keeps the drag fine-grained; the caller maps it). */}
      <View class="absolute inset-0">
        <Slider
          value={Math.round(value() * 1000)}
          min={0}
          max={1000}
          step={0}
          axis="vertical"
          disabled={props.disabled ?? false}
          accessibilityLabel={props.label}
          class={`h-full w-full ${focusRing}`}
          trackClass="h-full w-full justify-center"
          indicatorClass="h-full w-px bg-transparent"
          rangeClass="bg-transparent"
          thumbClass="size-0 opacity-0"
          onChange={({ value: v }) => typeof v === "number" && props.onChange(v / 1000)}
          onChangeEnd={({ value: v }) => typeof v === "number" && props.onChangeEnd(v / 1000)}
        />
      </View>
    </View>
  );
}
