// Studio Minimal Deck: an ultra-clean mastering console layout.
// Full-width track stage, centered large vinyl cover art, precision signal path telemetry,
// and floating master bus controls.

import { token, Text, View } from "nsh";
import { player } from "../state/player";
import { resolvedPath } from "../state/resolved";
import { engine } from "../backend/engine";
import { Cover } from "../components/ui/cover";
import { PlayerBar } from "../components/player/player-bar";
import { MainStack } from "./main";
import { TitleBar } from "../components/ui/title-bar";
import { NavControls } from "../components/header";
import { Icon } from "../components/ui/icons";
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

  return (
    <View class="flex-1" style={{ backgroundColor: "#0c0d10" }}>
      {/* Precision Signal Path Telemetry Header */}
      <TitleBar brand>
        <View class="flex-1 flex-row items-center gap-3 pr-2">
          <NavControls />
          <View class="flex-row items-center gap-2 rounded px-2 py-0.5" style={{ backgroundColor: "#16181f" }}>
            <View
              class="h-1.5 w-1.5 rounded-full"
              style={{ backgroundColor: isBitPerfect() ? "#22c55e" : "#eab308" }}
            />
            <Text class="text-xs font-semibold" style={{ color: isBitPerfect() ? "#22c55e" : "#eab308" }}>
              {isBitPerfect() ? "BIT-PERFECT" : "DSP ACTIVE"}
            </Text>
            <Text class="text-xs font-mono" style={{ color: token("muted") }}>
              {sampleRate()} {formatStr()}
            </Text>
          </View>

          <View class="flex-1" />

          {/* Quick Shell Switcher Pill */}
          <View
            class="flex-row items-center gap-1.5 rounded-full px-2.5 py-1"
            style={{ backgroundColor: "#1e212b" }}
            onPress={() => switchShell(DEFAULT_SHELL_ID)}
            accessibilityLabel="切换回工业机架主视图"
          >
            <Icon name="disc" size={13} color="primary" />
            <Text class="text-xs font-medium" style={{ color: token("foreground") }}>
              切回 Industrial Rack
            </Text>
          </View>

          {/* Open Plugin Manager */}
          <View
            class="rounded p-1"
            style={{ backgroundColor: "#16181f" }}
            onPress={() => openModal("plugins")}
            accessibilityLabel="插件与壳层管理器"
          >
            <Icon name="settings" size={15} color="muted" />
          </View>
        </View>
      </TitleBar>

      {/* Main Mastering Console Body */}
      <View class="flex-1 flex-row gap-4 px-4 pb-20 pt-2">
        {/* Left Side: Large Centered Artwork & Master Deck Card */}
        <View
          class="w-72 shrink-0 flex-col items-center justify-center rounded-xl border border-border p-5"
          style={{ backgroundColor: "#12141a" }}
        >
          <View
            class="overflow-hidden rounded-lg shadow-2xl"
            style={{ width: 220, height: 220, backgroundColor: "#0a0a0c" }}
          >
            <Cover path={currentPath()} size={220} fallback="disc" />
          </View>

          <View class="mt-4 w-full flex-col items-center text-center">
            <Text class="truncate text-base font-bold" style={{ color: "#ffffff" }}>
              {trackTitle()}
            </Text>
            <Text class="mt-1 truncate text-xs" style={{ color: token("primary") }}>
              {trackArtist()}
            </Text>
            <Text class="mt-0.5 truncate text-xs" style={{ color: token("muted") }}>
              {trackAlbum()}
            </Text>
          </View>

          <View class="mt-5 w-full rounded-md p-2.5" style={{ backgroundColor: "#181a22" }}>
            <View class="flex-row items-center justify-between">
              <Text class="text-xs" style={{ color: token("muted") }}>
                Master Clock
              </Text>
              <Text class="text-xs font-mono font-medium" style={{ color: resolvedPath()?.sampleRate ? "#22c55e" : token("muted") }}>
                {resolvedPath()?.sampleRate ? `${resolvedPath()!.sampleRate} Hz` : "IDLE"}
              </Text>
            </View>
            <View class="mt-1.5 flex-row items-center justify-between">
              <Text class="text-xs" style={{ color: token("muted") }}>
                Render Mode
              </Text>
              <Text class="text-xs font-mono font-medium" style={{ color: token("foreground") }}>
                {resolvedPath()?.backend ? resolvedPath()!.backend!.toUpperCase() : "STANDBY"}
              </Text>
            </View>
          </View>
        </View>

        {/* Right Side: Full Nav Stack & Track Browser */}
        <View class="min-w-0 flex-1 overflow-hidden rounded-xl border border-border" style={{ backgroundColor: "#12141a" }}>
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
