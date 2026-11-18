// 插件与壳层管理器对话框 (Plugin & Shell Manager Dialog)
// 包含一键全界面壳层热插拔切换，五大分类 Grid 矩阵，以及开发者模式。

import { effect, Scroll, signal, Text, View } from "nsh";
import { Button } from "nsh/base";
import { AppDialog } from "./overlays";
import { Icon, type IconName } from "./icons";
import { focusRing } from "./controls";
import {
  activeShellId,
  devWatchEnabled,
  isClassicShell,
  isPluginEnabled,
  isStudioMinimalShell,
  isVisualizerPluginEnabled,
  plugins,
  refreshPlugins,
  rescanPlugins,
  switchShell,
  togglePlugin,
} from "../../state/plugins";
import { servers } from "../../state/sources";
import { pluginDialogOpen, setPluginDialogOpen } from "../../state/overlays";
import type { PluginInfo } from "../../backend/types";

export type PluginTabCategory = "all" | "ui" | "sources" | "lyrics" | "engine";
const currentTab = signal<PluginTabCategory>("all");

export const TABS: { key: PluginTabCategory; label: string }[] = [
  { key: "all", label: "全部插件" },
  { key: "ui", label: "界面与光效" },
  { key: "sources", label: "音源与分轨" },
  { key: "lyrics", label: "在线歌词源" },
  { key: "engine", label: "音频引擎核心" },
];

export function PluginManagerDialog() {
  effect(() => {
    if (pluginDialogOpen()) {
      refreshPlugins();
    }
  });

  return (
    <AppDialog
      open={pluginDialogOpen()}
      onOpenChange={(next) => {
        if (next) refreshPlugins();
        setPluginDialogOpen(next);
      }}
      maxWidthClass="max-w-4xl"
      testID="plugin-dialog"
    >
      <PluginDialogContent />
    </AppDialog>
  );
}

function isShellSelected(shellId: string): boolean {
  const current = activeShellId();
  if (current === shellId) return true;
  if (
    (shellId === "builtin.shell.industrial-rack" || shellId === "builtin.ui.shell.classic") &&
    isClassicShell(current)
  ) {
    return true;
  }
  if (
    (shellId === "builtin.shell.studio-minimal" || shellId === "builtin.ui.shell.studio-minimal") &&
    isStudioMinimalShell(current)
  ) {
    return true;
  }
  return false;
}

function pluginIcon(p: PluginInfo): IconName {
  if (p.category === "ui.shell") return "layout";
  if (p.category === "ui.visualizer") return "wave";
  if (p.category === "source.connector") return "cloud";
  if (p.category === "lyrics.provider") return "fileText";
  if (p.id.includes("cue")) return "fileText";
  if (p.id.includes("sacd")) return "disc";
  if (p.id.includes("dsd")) return "cpu";
  if (p.id.includes("soxr") || p.category === "playback.dsp") return "sliders";
  return "layers";
}

function pluginLiveStatus(p: PluginInfo): string {
  if (p.category === "ui.visualizer") {
    return isVisualizerPluginEnabled() ? "60Hz FFT 实时频谱活跃" : "频谱已旁路停用";
  }
  if (p.category === "source.connector") {
    if (p.id === "jellyfin") {
      const count = servers().filter((s) => s.kind === "jellyfin" || s.kind === "jellyfin_token").length;
      return `${count} 台服务器挂载`;
    }
    if (p.id === "navidrome") {
      const count = servers().filter((s) => s.kind === "navidrome").length;
      return `${count} 台服务器挂载`;
    }
    if (p.id === "webdav") {
      const count = servers().filter((s) => s.kind === "webdav").length;
      return `${count} 处挂载`;
    }
  }
  if (p.category === "lyrics.provider") {
    return "在线检索池已连通 · 双向同步";
  }
  if (p.locked) {
    return "系统常驻核心 · 零内存泄漏";
  }
  if (p.category === "ui.shell") {
    return isShellSelected(p.id) ? "当前活动壳层" : "待命热插拔";
  }
  return "即时生效";
}

function PluginDialogContent() {
  const allPlugins = () => plugins();
  const shells = () => allPlugins().filter((p) => p.category === "ui.shell");
  const filtered = () => {
    const tab = currentTab();
    if (tab === "all") return allPlugins();
    if (tab === "ui") {
      return allPlugins().filter(
        (p) => p.category === "ui.shell" || p.category === "ui.visualizer" || p.category === "ui.slot"
      );
    }
    if (tab === "sources") {
      return allPlugins().filter(
        (p) =>
          p.category === "source.connector" ||
          p.category === "source.storage" ||
          p.category === "source.expander" ||
          p.category === "source.enricher"
      );
    }
    if (tab === "lyrics") {
      return allPlugins().filter((p) => p.category === "lyrics.provider");
    }
    if (tab === "engine") {
      return allPlugins().filter(
        (p) =>
          p.category === "playback.dsp" ||
          p.category === "playback.decoder" ||
          p.category === "playback.output"
      );
    }
    return allPlugins();
  };

  return (
    <View class="w-full gap-4 p-6" testID="plugin-dialog-content">
      {/* Title Header */}
      <View class="flex-row items-center justify-between border-b border-border pb-3">
        <View class="flex-row items-center gap-2.5">
          <Icon name="settings" size={18} color="primary" />
          <Text role="header" class="text-lg font-bold text-foreground">
            插件与界面壳层管理器
          </Text>
          <View class="rounded-full bg-secondary px-2 py-0.5">
            <Text class="font-mono text-xs font-bold text-muted-foreground">
              {allPlugins().length} 项已注册
            </Text>
          </View>
        </View>
        <Button
          accessibilityLabel="关闭"
          onPress={() => setPluginDialogOpen(false)}
          class={`items-center justify-center rounded-(--radius) p-1.5 hover:bg-muted active:opacity-60 ${focusRing}`}
        >
          <Icon name="close" size={16} color="muted-foreground" />
        </Button>
      </View>

      {/* Top Section: One-Click Full UI Shell Switcher */}
      <View class="gap-2.5 rounded-xl border border-border bg-secondary/70 p-3.5">
        <View class="flex-row items-center justify-between">
          <Text class="text-xs font-bold uppercase tracking-wider text-primary">
            整套界面壳层一键切换 (UI SHELL HOT-SWAP)
          </Text>
          <Text class="text-xs text-muted-foreground">
            无缝热插拔 · 音频连续播放不丢帧
          </Text>
        </View>

        <View class="flex-row gap-3">
          <Each of={shells()}>
            {(shell) => {
              const active = () => isShellSelected(shell().id);
              const enabled = () => isPluginEnabled(shell().id);
              return (
                <Button
                  selected={active()}
                  disabled={!enabled()}
                  onPress={() => {
                    if (enabled()) switchShell(shell().id);
                  }}
                  accessibilityLabel={`切换至 ${shell().name}`}
                  class={`flex-1 flex-col items-start gap-1.5 rounded-lg border p-3 text-left transition duration-150 ${
                    !enabled()
                      ? "opacity-45 border-border bg-card/30"
                      : active()
                      ? "border-primary bg-card"
                      : "border-border bg-card/60 hover:bg-card"
                  } ${focusRing}`}
                >
                  <View class="w-full flex-row items-center justify-between gap-2">
                    <Text
                      class={`text-sm font-bold ${
                        !enabled()
                          ? "text-muted-foreground"
                          : active()
                          ? "text-primary"
                          : "text-foreground"
                      }`}
                    >
                      {shell().name}
                    </Text>
                    <If when={!enabled()}>
                      <View class="rounded bg-muted px-1.5 py-0.5">
                        <Text class="text-[11px] font-bold text-muted-foreground">
                          已禁用
                        </Text>
                      </View>
                    </If>
                    <If when={enabled() && active()}>
                      <View class="rounded bg-primary/15 px-1.5 py-0.5">
                        <Text class="text-[11px] font-bold text-primary">
                          当前使用
                        </Text>
                      </View>
                    </If>
                  </View>
                  <Text class="line-clamp-2 text-xs leading-relaxed text-muted-foreground">
                    {shell().description}
                  </Text>
                </Button>
              );
            }}
          </Each>
        </View>
      </View>

      {/* Category Filter Tabs */}
      <View class="flex-row items-center gap-1.5">
        <Each of={TABS}>
          {(tab) => {
            const isCurrent = () => currentTab() === tab().key;
            return (
              <Button
                selected={isCurrent()}
                onPress={() => currentTab.set(tab().key)}
                class={`rounded-(--radius) border px-3 py-1.5 transition duration-150 ${
                  isCurrent()
                    ? "border-foreground/25 bg-secondary text-foreground"
                    : "border-transparent text-muted-foreground hover:bg-secondary/60 hover:text-foreground"
                } ${focusRing}`}
              >
                <Text
                  class={`text-xs font-bold ${
                    isCurrent() ? "text-foreground" : "text-muted-foreground"
                  }`}
                >
                  {tab().label}
                </Text>
              </Button>
            );
          }}
        </Each>
      </View>

      {/* Plugin Grid Matrix List */}
      <Scroll
        class="w-full pr-1"
        contentClass="w-full"
        style={{ height: 420 }}
      >
        <View class="grid" style={{ display: "grid", gridTemplateColumns: "repeat(2, minmax(0, 1fr))", gap: 10 }}>
          <Each of={filtered()}>
            {(plugin) => <PluginCard plugin={plugin()} />}
          </Each>
        </View>
      </Scroll>

      {/* Footer / Developer Mode Section */}
      <View class="flex-row items-center justify-between rounded-lg border border-border bg-secondary/70 px-3.5 py-2.5">
        <View class="gap-0.5">
          <Text class="font-mono text-[11px] text-muted-foreground">
            插件目录: ~/.config/neiro/plugins
          </Text>
          <Text class="font-mono text-[11px] text-muted-foreground">
            CLI: neiro_plugin list | enable | disable | install | probe
          </Text>
        </View>
        <View class="flex-row items-center gap-2">
          <Button
            selected={devWatchEnabled()}
            onPress={() => devWatchEnabled.set(!devWatchEnabled())}
            class={`rounded-(--radius) border border-border px-2.5 py-1.5 ${
              devWatchEnabled() ? "bg-primary/15" : "bg-card"
            } hover:bg-muted ${focusRing}`}
          >
            <Text
              class={`text-xs font-bold ${
                devWatchEnabled() ? "text-primary" : "text-muted-foreground"
              }`}
            >
              {devWatchEnabled() ? "Dev Watch 开启" : "Dev Watch 关闭"}
            </Text>
          </Button>
          <Button
            onPress={() => rescanPlugins()}
            class={`rounded-(--radius) border border-border bg-card px-3 py-1.5 hover:bg-muted active:opacity-75 ${focusRing}`}
          >
            <Text class="text-xs font-bold text-foreground">
              重新扫描本地插件
            </Text>
          </Button>
        </View>
      </View>
    </View>
  );
}

function PluginCard(props: { plugin: PluginInfo }) {
  const p = () => props.plugin;
  const isEnabled = () => Boolean(p().enabled);

  const tierLabel = () => {
    switch (p().tier) {
      case "ui":
        return "UI Tier · 热插拔";
      case "base":
        return "Base Tier · 即时生效";
      case "engine":
        return p().breaksBitPerfect
          ? "Engine Tier · 解除 Bit-Perfect"
          : "Engine Tier · 解码核心";
    }
  };

  const tierClass = () => {
    switch (p().tier) {
      case "ui":
        return "bg-emerald-500/15 text-emerald-500";
      case "base":
        return "bg-sky-500/15 text-sky-400";
      case "engine":
        return "bg-amber-500/15 text-amber-500";
    }
  };

  const isLocked = () => Boolean(p().locked);

  return (
    <View
      class={`flex-col justify-between rounded-xl border p-3.5 transition ${
        isEnabled()
          ? "border-border bg-secondary/50 hover:border-foreground/20 hover:bg-secondary/70"
          : "opacity-55 border-border/40 bg-card/30"
      }`}
      accessibilityLabel={`插件 ${p().name}`}
    >
      <View class="flex-row items-start gap-3">
        {/* 36x36 Icon Block */}
        <View class="h-9 w-9 shrink-0 items-center justify-center rounded-lg border border-border/80 bg-card/80 shadow-2xs">
          <Icon name={pluginIcon(p())} size={18} color={isEnabled() ? "primary" : "muted-foreground"} />
        </View>

        {/* Content Info */}
        <View class="min-w-0 flex-1 gap-1">
          <View class="flex-row flex-wrap items-center gap-1.5">
            <Text class="line-clamp-1 text-sm font-bold text-foreground">
              {p().name}
            </Text>
            <View class={`rounded px-1.5 py-0.5 ${tierClass()}`}>
              <Text class={`font-mono text-[10px] font-bold ${tierClass()}`}>
                {tierLabel()}
              </Text>
            </View>
            <Text class="font-mono text-[11px] text-muted-foreground">
              v{p().version}
            </Text>
          </View>

          <Text class="line-clamp-2 text-xs leading-relaxed text-muted-foreground">
            {p().description || p().category}
          </Text>

          {/* Live Status Line */}
          <View class="mt-1 flex-row items-center gap-1.5">
            <View class={`h-1.5 w-1.5 rounded-full ${isEnabled() ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
            <Text class="font-mono text-[10px] font-medium text-muted-foreground">
              {pluginLiveStatus(p())}
            </Text>
          </View>

          <If when={(p().permissions?.length ?? 0) > 0}>
            <View class="mt-1 flex-row flex-wrap gap-1">
              <Each of={p().permissions ?? []}>
                {(perm) => (
                  <View class="rounded border border-border bg-card px-1.5 py-0.5">
                    <Text class="font-mono text-[9px] text-muted-foreground">
                      {perm()}
                    </Text>
                  </View>
                )}
              </Each>
            </View>
          </If>
        </View>
      </View>

      {/* Switch Toggle or Locked Core Badge */}
      <View class="mt-3 flex-row items-center justify-end border-t border-border/50 pt-2">
        <If
          when={isLocked()}
          else={
            <Button
              selected={isEnabled()}
              onPress={() => togglePlugin(p().id, !isEnabled())}
              accessibilityLabel={`启用或禁用 ${p().name}`}
              class={`shrink-0 rounded-full px-3 py-1 transition duration-150 ${
                isEnabled()
                  ? "bg-emerald-600 hover:bg-emerald-500"
                  : "border border-border bg-card hover:bg-muted"
              } ${focusRing}`}
            >
              <Text
                class={`text-xs font-bold ${
                  isEnabled() ? "text-white" : "text-muted-foreground"
                }`}
              >
                {isEnabled() ? "已启用" : "已禁用"}
              </Text>
            </Button>
          }
        >
          <View class="rounded border border-primary/30 bg-primary/10 px-2.5 py-1">
            <Text class="font-mono text-[10px] font-bold text-primary">
              CORE · 常驻核心
            </Text>
          </View>
        </If>
      </View>
    </View>
  );
}
