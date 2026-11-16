// 04 // OUTPUT INTERFACE & MASTER GAIN — the endpoint, the share mode, and
// the big encoder.
//
// Three facts this module is careful about:
//
// - Enumerating endpoints opens each one in turn, which takes a live
//   exclusive stream's device away (the engine then reports `deviceLost`).
//   So the list is locked while a stream is open, and the lock says so.
// - Shared mode is a deliberate choice, never a fallback: it is a switch, and
//   the panel says what it costs (the mixer is in the path).
// - The master encoder is a real gain, but at unity while bit-perfect holds:
//   the engine's own applied volume is what the panel shows, so the knob and
//   the stream cannot disagree.
// - Endpoints are listed, not hidden in a dropdown: a machine has a handful
//   and which one the stream is on is the whole question this module answers.
//   The list is the only element whose height follows the data, so the knob
//   sits in its own column rather than being stretched to match it.

import { Animated, createAnimatedValue, effect, reduceMotion, signal, Text, untracked, View } from "nsh";
import { HARDWARE, Led, MechKey, Silkscreen } from "../../ui/hardware";
import { Button } from "nsh/base";
import { Popover } from "nsh/base";
import { Icon } from "../../ui/icons";
import { focusRing } from "../../ui/controls";
import { engine, reportEngineFailure } from "../../../backend/engine";
import { currentRate, formatHz, policyJson, resolvedPath, shareMode } from "../../../state/resolved";
import { playTick } from "../../../state/foley";
import { dragVolume, player, setVolume, volumeLevel } from "../../../state/player";

const listing = signal(false);

/** Whether a stream is open: the list cannot be refreshed then. */
const streamOpen = (): boolean => {
  const state = player().state;
  return state === "playing" || state === "paused" || state === "ready";
};

/** Enumerates when it is safe (no live stream). Called once per console
 * open: an endpoint list that is only filled by a manual press reads as "the
 * machine has none" until someone presses it. */
export function ensureDevices(): void {
  // Every call is a real request: an endpoint plugged in after the last
  // enumeration only appears if something asks again, and a one-shot flag here
  // meant a DAC connected after launch never showed up.
  if (streamOpen()) return;
  refreshDevices();
}

/** Enumerates once at app start, before any stream exists: the only moment it
 * is free. Later the console offers the list this produced (and a stream's own
 * resolved device), so a DAC that was connected is selectable even mid-play —
 * choosing it reopens the live stream at the current position, which is what
 * the engine's `setDevice` does. */
export function enumerateAtStartup(): void {
  if (streamOpen()) return;
  refreshDevices();
}

/** Whether a stream is open, for the panels that watch the transition. */
export const devicesLocked = (): boolean => streamOpen();

/** Refreshes the endpoint list, refusing while a stream is open. */
function refreshDevices(): void {
  if (streamOpen()) return;
  listing.set(true);
  try {
    engine.listDevices();
  } catch (error) {
    reportEngineFailure(error, "枚举输出设备失败");
  }
  setTimeout(() => listing.set(false), 1200);
}

/** A device row: a key that selects it, with its own capability facts. */
function DeviceKey(props: { name: string; detail: string; selected: boolean; onPress: () => void; disabled?: boolean }) {
  return (
    <MechKey
      label={props.name}
      lit={props.selected}
      disabled={props.disabled}
      onPress={props.onPress}
      class="w-full flex-row items-center gap-2 rounded px-2 py-1.5"
    >
      <Led lit={props.selected} size={5} />
      <View class="min-w-0 flex-1">
        <Text class="line-clamp-1 font-mono text-[10px]" style={{ color: props.selected ? HARDWARE.amberBright : HARDWARE.text }}>
          {props.name}
        </Text>
        <Text class="line-clamp-1 font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
          {props.detail}
        </Text>
      </View>
    </MechKey>
  );
}

/** The 24-mark ring around the master encoder. */
function MasterRing(props: { value: number }) {
  const MARKS = 24;
  const SIZE = 84;
  return (
    <View style={{ position: "absolute", width: SIZE, height: SIZE }} accessibilityElementsHidden>
      <Each of={Array.from({ length: MARKS }, (_, i) => i)} key="index">
        {(index) => {
          const angle = -135 + (270 * index()) / (MARKS - 1);
          const rad = (angle * Math.PI) / 180;
          const lit = () => index() / (MARKS - 1) <= props.value + 0.0001;
          const radius = SIZE / 2 - 2;
          return (
            <View
              class="absolute rounded-full"
              style={{
                width: 2,
                height: lit() ? 5 : 3,
                left: SIZE / 2 + radius * Math.sin(rad) - 1,
                top: SIZE / 2 - radius * Math.cos(rad),
                backgroundColor: lit() ? HARDWARE.amberBright : "rgba(255,255,255,0.12)",
                boxShadow: lit() ? [{ x: 0, y: 0, blur: 5, color: HARDWARE.amber }] : undefined,
              }}
            />
          );
        }}
      </Each>
    </View>
  );
}

/** The master gain encoder: a big knurled knob with its ring, a readout in
 * percent and dB, and a lock when the stream is bit-perfect (unity is a
 * condition of the claim, not a preference). */
function MasterGain(props: { locked: boolean }) {
  const value = () => volumeLevel() ?? 0;
  const angle = () => -135 + 270 * value();
  const db = () => {
    const v = value();
    return v <= 0.0001 ? "-∞" : `${(20 * Math.log10(v)).toFixed(1)}`;
  };
  const handlers = {
    onMouseMove: (e: { pagePosition: { x: number; y: number }; pressedButton: string | null }) => {
      if (props.locked || e.pressedButton === null) return;
      // A 200pt throw covers the whole range; the panel's own box carries the
      // pointer past the knob's edges.
      dragVolume(Math.min(1, Math.max(0, value() + (lastY - e.pagePosition.y) / 200)));
      lastY = e.pagePosition.y;
    },
    onPressDown: (e: { pagePosition: { x: number; y: number } }) => {
      lastY = e.pagePosition.y;
    },
    onPressUp: () => {
      if (props.locked) return;
      setVolume(value());
    },
  };
  let lastY = 0;
  const glow = createAnimatedValue(0.5);
  effect(() => {
    if (props.locked || reduceMotion()) {
      glow.set(props.locked ? 1 : 0.5);
      return;
    }
    const run = Animated.loop(
      Animated.sequence([
        Animated.timing(glow, { toValue: 0.85, duration: 1400, easing: "easeInOut" }),
        Animated.timing(glow, { toValue: 0.45, duration: 1400, easing: "easeInOut" }),
      ]),
    );
    void run.start();
    return () => run.stop();
  });
  return (
    <View class="items-center gap-1.5">
      <View class="items-center justify-center" style={{ width: 92, height: 92 }} {...handlers}>
        <MasterRing value={value()} />
        <View
          style={{
            width: 66,
            height: 66,
            borderRadius: 33,
            backgroundImage:
              "radial-gradient(circle at 32% 26%, rgba(255,255,255,0.18) 0%, rgba(255,255,255,0.03) 46%, transparent 74%), conic-gradient(from 0deg, #2A2D33, #1C1E22, #303339, #191B1F, #2A2D33)",
            boxShadow: props.locked
              ? [
                  { x: 0, y: 10, blur: 24, color: "#000000f2" },
                  { x: 0, y: 0, blur: 0, spread: 2, color: HARDWARE.amberBright },
                  { x: 0, y: 0, blur: 22, color: "rgba(245,166,35,0.4)" },
                ]
              : [
                  { x: 0, y: 8, blur: 18, color: "#000000e0" },
                  { x: 0, y: 1, blur: 1.5, color: "#ffffff3d", inset: true },
                  { x: 0, y: -2, blur: 5, color: "#000000e6", inset: true },
                  { x: 0, y: 0, blur: 0, spread: 1.5, color: "#0E1012" },
                ],
            opacity: props.locked ? glow : 1,
          }}
        >
          {/* The pointer: a radial bar from the cap's edge. */}
          <View style={{ position: "absolute", width: 66, height: 66, transform: [{ rotate: `${angle()}deg` }] }}>
            <View
              style={{
                position: "absolute",
                left: 32,
                top: 6,
                width: 2,
                height: 13,
                borderRadius: 1,
                backgroundColor: HARDWARE.amberBright,
                boxShadow: [{ x: 0, y: 0, blur: 6, color: HARDWARE.amber }],
              }}
            />
          </View>
        </View>
      </View>
      <Text class="font-mono text-[13px] font-bold tabular-nums" style={{ color: HARDWARE.amberBright }}>
        {Math.round(value() * 100)}%
      </Text>
      <Silkscreen tone="faint">{`${db()} dB · MASTER`}</Silkscreen>
    </View>
  );
}

/** The whole 04 module. */
export function OutputInterface() {
  // Enumerate when the console mounts (nothing is holding the endpoints then),
  // and again whenever playback stops while it is open: that is the moment a
  // DAC plugged in during playback becomes visible.
  effect(() => {
    const playing = streamOpen();
    if (playing) return;
    untracked(() => ensureDevices());
  });
  const devices = () => engine.devices();
  const resolvedId = () => resolvedPath()?.deviceId ?? "";
  const locked = () => streamOpen();
  const bitPerfect = () => engine.renderPath()?.bitPerfect ?? false;
  /** The backend the engine opened the device with (`NAE_AudioBackend`). */
  const backend = () => resolvedPath()?.backend ?? null;
  /** Running on ASIO right now, as the engine reports it. */
  const onAsio = () => backend() === "asio";
  /** The first ASIO driver this machine registered, if any. */
  const asioDevice = () => devices().find((device) => device.id.startsWith("asio:"));
  const menuOpen = signal(false);
  /** The row the trigger names: the resolved device when it is in the list,
   * otherwise the engine's default. */
  const selectedRow = () => {
    const id = resolvedId();
    const hit = deviceRows().find((row) => row.id === id);
    return hit ?? deviceRows()[0];
  };
  const selectedName = () => selectedRow()?.name ?? "引擎默认";
  const selectedDetail = () => selectedRow()?.detail ?? "上次使用的端点";
  /** One entry per place the stream can go: "the engine's own default" first,
   * then every enumerated endpoint. */
  const deviceRows = () => {
    const id = resolvedId();
    const rows = [
      {
        key: "default",
        id: "" as string | null,
        name: "引擎默认",
        detail: "上次使用的端点",
        selected: id === "",
      },
      ...devices().map((device) => ({
        key: device.id,
        id: device.id as string | null,
        name: device.name || device.id,
        // An ASIO device reports no format until it is opened: the registry
        // knows it exists, not what it negotiates. Saying "0 Hz · 0 ch" would
        // read as a broken endpoint.
        detail:
          device.sampleRate > 0
            ? `${formatHz(device.sampleRate)} · ${device.bitsPerSample} bit · ${device.channels} ch${device.exclusive ? " · EXCL" : device.shared ? " · SHRD" : ""}`
            : device.id.startsWith("asio:")
              ? "ASIO 驱动 · 打开时协商格式"
              : "格式未探测",
        selected: id === device.id,
      })),
    ];
    return rows;
  };
  return (
    <View class="gap-2.5">
      <View class="flex-row items-center justify-between">
        <Silkscreen tone="amber">EXCLUSIVE / SHARED</Silkscreen>
        <View class="flex-row items-center gap-1.5">
          <If when={shareMode() === 1}>
            <Button
              accessibilityLabel="释放独占设备 (挂起 DAC，保留播放进度)"
              onPress={() => {
                try {
                  engine.releaseDevice();
                } catch (error) {
                  reportEngineFailure(error, "释放设备失败");
                }
              }}
              class={`rounded border border-amber-500/40 bg-amber-500/10 px-2 py-0.5 active:scale-95 ${focusRing}`}
            >
              <Text class="font-mono text-[9px] font-bold text-amber-500">释放设备 / RELEASE</Text>
            </Button>
          </If>
          <Led lit={locked()} tone={locked() ? "amber" : "emerald"} size={5} />
          <Silkscreen tone="faint">{locked() ? "ENUM LOCK: ON" : "ENUM LOCK: OFF"}</Silkscreen>
        </View>
      </View>
      <View class="flex-row items-start gap-3">
        <View class="min-w-0 flex-1 gap-2.5">
          {/* Three cells of equal weight: the two share modes the policy has,
              and ASIO — a driver family rather than a share mode, so it is
              chosen by pointing the stream at the ASIO endpoint. The cell lights
              only when the engine says the stream runs on ASIO. */}
          <View class="flex-row gap-1">
            <MechKey
              label="独占直通"
              lit={shareMode() === 1}
              onPress={() => {
                // Only the share mode changes; the rate stays what the user chose.
                const rate = currentRate();
                try {
                  engine.setPolicy(policyJson(rate.kind, rate.fixedRate, "exclusive"));
                } catch (error) {
                  reportEngineFailure(error, "切独占失败");
                }
              }}
              class="h-11 flex-1 items-center justify-center rounded"
            >
              <Text class="font-mono text-[9px] font-bold tracking-wider" style={{ color: shareMode() === 1 ? HARDWARE.amberBright : HARDWARE.textDim }}>
                EXCLUSIVE
              </Text>
            </MechKey>
            <MechKey
              label="系统共享混音"
              lit={shareMode() === 2}
              onPress={() => {
                const rate = currentRate();
                try {
                  engine.setPolicy(policyJson(rate.kind, rate.fixedRate, "shared"));
                } catch (error) {
                  reportEngineFailure(error, "切共享失败");
                }
              }}
              class="h-11 flex-1 items-center justify-center rounded"
            >
              <Text class="font-mono text-[9px] font-bold tracking-wider" style={{ color: shareMode() === 2 ? HARDWARE.amberBright : HARDWARE.textDim }}>
                SHARED
              </Text>
            </MechKey>
            <MechKey
              label={asioDevice() ? "切到 ASIO 驱动" : "ASIO：这台机器没有注册的 ASIO 驱动"}
              lit={onAsio()}
              disabled={asioDevice() === undefined}
              onPress={() => {
                // ASIO is a driver family, not a share mode: the way to it is
                // the endpoint. Pick the ASIO device and the engine opens the
                // stream through it.
                const device = asioDevice();
                if (!device) return;
                try {
                  engine.setDevice(device.id);
                } catch (error) {
                  reportEngineFailure(error, "切 ASIO 失败");
                }
              }}
              class="h-11 flex-1 items-center justify-center rounded"
            >
              <Text
                class="font-mono text-[9px] font-bold tracking-wider"
                style={{ color: onAsio() ? HARDWARE.amberBright : HARDWARE.textDim }}
              >
                ASIO
              </Text>
            </MechKey>
          </View>

          <View class="flex-row items-center gap-1.5">
            <Silkscreen tone="faint">ENDPOINT</Silkscreen>
            <View class="flex-1" />
            <MechKey
              label={locked() ? "刷新设备（需先停止播放）" : "刷新设备"}
              disabled={locked()}
              onPress={refreshDevices}
              class="h-5 items-center justify-center rounded px-1.5"
            >
              <Text class="font-mono text-[8px]" style={{ color: locked() ? HARDWARE.textFaint : HARDWARE.textDim }}>
                ENUM
              </Text>
            </MechKey>
          </View>

          {/* The endpoint as one row that opens the list. The list is what
              changes with the machine, so it belongs behind a press rather
              than growing the module with every DAC plugged in. */}
          <Popover
            open={menuOpen()}
            onOpenChange={(e) => menuOpen.set(e.open)}
            placement="bottom"
            align="start"
            offset={4}
            popupClass=""
            trigger={
              <MechKey
                label="选择端点"
                class="w-full flex-row items-center gap-2 rounded px-2 py-2"
              >
                <Led lit={resolvedId() !== ""} size={5} />
                <View class="min-w-0 flex-1">
                  <Text class="line-clamp-1 text-[11px] font-medium" style={{ color: HARDWARE.text }}>
                    {selectedName()}
                  </Text>
                  <Text class="line-clamp-1 font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
                    {selectedDetail()}
                  </Text>
                </View>
                <Icon name="chevronDown" size={12} color="muted-foreground" />
              </MechKey>
            }
          >
            <View class="w-72 gap-0.5 rounded-(--radius-card) border border-border bg-popover p-1 shadow-lg">
              <Each of={deviceRows()} key={(row) => row.key}>
                {(row) => (
                  <Button
                    accessibilityLabel={`端点 ${row().name}`}
                    onPress={() => {
                      menuOpen.set(false);
                      try {
                        engine.setDevice(row().id);
                      } catch (error) {
                        reportEngineFailure(error, "切换端点失败");
                      }
                    }}
                    class={`flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 transition duration-100 ease-out hover:bg-muted ${focusRing}`}
                  >
                    <Led lit={row().selected} size={5} />
                    <View class="min-w-0 flex-1">
                      <Text class="line-clamp-1 text-[11px] font-medium" style={{ color: row().selected ? HARDWARE.amberBright : HARDWARE.text }}>
                        {row().name}
                      </Text>
                      <Text class="line-clamp-1 font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
                        {row().detail}
                      </Text>
                    </View>
                    <If when={row().selected}>
                      <Text class="font-mono text-[8px]" style={{ color: HARDWARE.amberBright }}>
                        IN USE
                      </Text>
                    </If>
                  </Button>
                )}
              </Each>
              <If when={deviceRows().length === 0}>
                <Text class="px-2 py-1.5 text-[9px]" style={{ color: HARDWARE.textFaint }}>
                  {locked() ? "播放中不枚举（会抢走独占流）；列表来自上次枚举" : "没有枚举到端点：按 ENUM 重新枚举"}
                </Text>
              </If>
            </View>
          </Popover>
        </View>
        {/* The master gain, at the endpoint card's height. */}
        <View
          class="items-center justify-center gap-0.5 rounded-lg px-3 py-2"
          style={{ width: 132, backgroundColor: "rgba(0,0,0,0.25)" }}
        >
          <MasterGain locked={bitPerfect()} />
          <If when={bitPerfect()}>
            <Silkscreen tone="amber">UNITY</Silkscreen>
          </If>
        </View>
      </View>
    </View>
  );
}

