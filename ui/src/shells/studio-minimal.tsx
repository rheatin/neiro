// Studio Minimal Deck: an ultra-clean mastering console layout.
// Full-width track stage, centered large vinyl cover art, precision signal path telemetry,
// and floating master bus controls.

import { Text, View } from "nsh";
import { Button } from "nsh/base";
import { player } from "../state/player";
import { resolvedPath } from "../state/resolved";
import { engine } from "../backend/engine";
import { Cover } from "../components/ui/cover";
import { PlayerBar } from "../components/player/player-bar";
import { MainStack } from "./main";
import { Logo, titleBarArea } from "../components/ui/title-bar";
import { NavControls } from "../components/header";
import { Icon } from "../components/ui/icons";
import { focusRing } from "../components/ui/controls";
import { openModal } from "../state/overlays";
import { switchShell, DEFAULT_SHELL_ID } from "../state/plugins";

export function StudioMinimalShell() {
  const currentPath = () => player().path ?? "";
  const trackTitle = () => player().title || "未播放曲目";
  const trackArtist = () => player().artist || "Studio Console";
  const trackAlbum = () => player().album || "Direct Mastering Stream";

  const isBitPerfect = () => engine.renderPath()?.bitPerfect ?? false;
  const sampleRate = () => {
    const sr = resolvedPath()?.sampleRate ?? 0;
    return sr > 0 ? `${(sr / 1000).toFixed(1)} kHz` : "PCM";
  };
  const formatStr = () => {
    const bits = resolvedPath()?.validBits ?? 0;
    return bits > 0 ? `${bits}-bit` : "";
  };

  const barHeight = () => (titleBarArea().height > 0 ? titleBarArea().height : 48);
  const padLeft = () => titleBarArea().left + (titleBarArea().left > 0 ? 16 : 16);
  const padRight = () => titleBarArea().right + 16;

  return (
    <View class="flex-1 bg-background" testID="studio-minimal-shell">
      {/* Precision Signal Path Telemetry Header (always visible, including fullscreen) */}
      <View
        desktop={{ windowDrag: true }}
        class="shrink-0 flex-row items-center gap-2"
        style={{
          height: barHeight(),
          paddingLeft: padLeft(),
          paddingRight: padRight(),
        }}
      >
        <Logo size={18} />
        <Text class="mr-3 font-mono text-[12px] font-bold uppercase tracking-[0.3em] text-foreground">
          NEIRO
        </Text>
        <View class="flex-1 flex-row items-center gap-3 pr-2">
          <NavControls />
          <View class="flex-row items-center gap-2 rounded-(--radius) border border-border bg-secondary px-2.5 py-1">
            <View
              class={`h-2 w-2 rounded-full ${isBitPerfect() ? "bg-emerald-500" : "bg-amber-500"}`}
            />
            <Text class={`text-xs font-bold ${isBitPerfect() ? "text-emerald-500" : "text-amber-500"}`}>
              {isBitPerfect() ? "BIT-PERFECT" : "DSP ACTIVE"}
            </Text>
            <Text class="font-mono text-xs text-muted-foreground">
              {sampleRate()} {formatStr()}
            </Text>
          </View>

          <View class="flex-1" />

          {/* Quick Shell Switcher Pill */}
          <Button
            onPress={() => switchShell(DEFAULT_SHELL_ID)}
            accessibilityLabel="切回经典工作台"
            class={`flex-row items-center gap-1.5 rounded-full border border-border bg-secondary px-3 py-1 hover:bg-muted active:opacity-75 ${focusRing}`}
          >
            <Icon name="disc" size={13} color="primary" />
            <Text class="text-xs font-bold text-foreground">
              切回 Classic Deck
            </Text>
          </Button>

          {/* Open Plugin Manager */}
          <Button
            onPress={() => openModal("plugins")}
            accessibilityLabel="插件与壳层管理器"
            class={`items-center justify-center rounded-(--radius) border border-border bg-secondary p-1.5 hover:bg-muted active:opacity-75 ${focusRing}`}
          >
            <Icon name="settings" size={15} color="muted-foreground" />
          </Button>
        </View>
      </View>

      {/* Main Mastering Console Body */}
      <View class="flex-1 flex-row gap-4 px-4 pb-24 pt-2">
        {/* Left Side: Large Centered Artwork & Master Deck Card */}
        <View class="w-72 shrink-0 flex-col items-center justify-center rounded-xl border border-border bg-card p-5">
          <View
            class="overflow-hidden rounded-lg border border-border bg-secondary shadow-xl"
            style={{ width: 220, height: 220 }}
          >
            <Cover path={currentPath()} size={220} fallback="disc" />
          </View>

          <View class="mt-4 w-full flex-col items-center text-center">
            <Text class="truncate text-base font-bold text-foreground">
              {trackTitle()}
            </Text>
            <Text class="mt-1 truncate text-xs font-semibold text-primary">
              {trackArtist()}
            </Text>
            <Text class="mt-0.5 truncate text-xs text-muted-foreground">
              {trackAlbum()}
            </Text>
          </View>

          <View class="mt-5 w-full gap-1.5 rounded-lg border border-border bg-secondary p-3">
            <View class="flex-row items-center justify-between">
              <Text class="text-xs text-muted-foreground">
                Master Clock
              </Text>
              <Text class={`font-mono text-xs font-bold ${resolvedPath()?.sampleRate ? "text-emerald-500" : "text-muted-foreground"}`}>
                {resolvedPath()?.sampleRate ? `${resolvedPath()!.sampleRate} Hz` : "IDLE"}
              </Text>
            </View>
            <View class="flex-row items-center justify-between">
              <Text class="text-xs text-muted-foreground">
                Render Mode
              </Text>
              <Text class="font-mono text-xs font-bold text-foreground">
                {resolvedPath()?.backend ? resolvedPath()!.backend!.toUpperCase() : "STANDBY"}
              </Text>
            </View>
          </View>
        </View>

        {/* Right Side: Full Nav Stack & Track Browser */}
        <View class="min-w-0 flex-1 overflow-hidden rounded-xl border border-border bg-card">
          <MainStack class="min-w-0 flex-1" />
        </View>
      </View>

      {/* Floating Bottom Console Transport Bar */}
      <View style={{ position: "absolute", left: 16, right: 16, bottom: 8 }}>
        <PlayerBar />
      </View>
    </View>
  );
}
