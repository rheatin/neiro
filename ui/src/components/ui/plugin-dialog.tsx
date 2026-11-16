// 插件与壳层管理器对话框 (Plugin & Shell Manager Dialog)
// 包含一键全界面壳层热插拔切换，三层架构（UI / Base / Engine）插件矩阵，以及开发者模式。

import { effect, Scroll, signal, Text, View } from "nsh";
import { Button } from "nsh/base";
import { AppDialog } from "./overlays";
import { Icon } from "./icons";
import { focusRing } from "./controls";
import {
  activeShellId,
  devWatchEnabled,
  isClassicShell,
  isStudioMinimalShell,
  plugins,
  refreshPlugins,
  rescanPlugins,
  switchShell,
  togglePlugin,
} from "../../state/plugins";
import { pluginDialogOpen, setPluginDialogOpen } from "../../state/overlays";
import type { PluginInfo, PluginTier } from "../../backend/types";

type ActiveTab = "all" | PluginTier;
const currentTab = signal<ActiveTab>("all");

const TABS: { key: ActiveTab; label: string }[] = [
  { key: "all", label: "全部插件" },
  { key: "ui", label: "UI 视图与插槽" },
  { key: "base", label: "音源与分轨扩展" },
  { key: "engine", label: "播放与 DSP 引擎" },
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
      maxWidthClass="max-w-2xl"
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

function PluginDialogContent() {
  const allPlugins = () => plugins();
  const shells = () => allPlugins().filter((p) => p.category === "ui.shell");
  const filtered = () => {
    const tab = currentTab();
    if (tab === "all") return allPlugins();
    return allPlugins().filter((p) => p.tier === tab);
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
            整套界面壳层一键切换 (UI Shell Hot-Swap)
          </Text>
          <Text class="text-xs text-muted-foreground">
            无缝热插拔 · 音频连续播放不丢帧
          </Text>
        </View>

        <View class="flex-row gap-3">
          <Each of={shells()}>
            {(shell) => {
              const active = () => isShellSelected(shell().id);
              return (
                <Button
                  selected={active()}
                  onPress={() => switchShell(shell().id)}
                  accessibilityLabel={`切换至 ${shell().name}`}
                  class={`flex-1 flex-col items-start gap-1.5 rounded-lg border p-3 text-left transition duration-150 ${
                    active()
                      ? "border-primary bg-card"
                      : "border-border bg-card/60 hover:bg-card"
                  } ${focusRing}`}
                >
                  <View class="w-full flex-row items-center justify-between gap-2">
                    <Text
                      class={`text-sm font-bold ${
                        active() ? "text-primary" : "text-foreground"
                      }`}
                    >
                      {shell().name}
                    </Text>
                    <If when={active()}>
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

      {/* Plugin Matrix List */}
      <Scroll
        class="w-full pr-1"
        contentClass="w-full gap-2"
        style={{ height: 270 }}
      >
        <Each of={filtered()}>
          {(plugin) => <PluginRow plugin={plugin()} />}
        </Each>
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

function PluginRow(props: { plugin: PluginInfo }) {
  const p = () => props.plugin;
  const isEnabled = () => p().enabled;

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

  return (
    <View
      class="flex-row items-center justify-between rounded-lg border border-border bg-secondary/50 px-3.5 py-2.5"
      accessibilityLabel={`插件 ${p().name}`}
    >
      <View class="min-w-0 flex-1 gap-1 pr-3">
        <View class="flex-row flex-wrap items-center gap-2">
          <Text class="text-sm font-bold text-foreground">
            {p().name}
          </Text>
          <View class={`rounded px-1.5 py-0.5 ${tierClass()}`}>
            <Text class={`font-mono text-[10px] font-bold ${tierClass()}`}>
              {tierLabel()}
            </Text>
          </View>
          <Text class="font-mono text-xs text-muted-foreground">
            v{p().version}
          </Text>
        </View>

        <Text class="line-clamp-1 text-xs text-muted-foreground">
          {p().description || p().category}
        </Text>

        <If when={(p().permissions?.length ?? 0) > 0}>
          <View class="mt-0.5 flex-row flex-wrap gap-1">
            <Each of={p().permissions ?? []}>
              {(perm) => (
                <View class="rounded border border-border bg-card px-1.5 py-0.5">
                  <Text class="font-mono text-[10px] text-muted-foreground">
                    {perm()}
                  </Text>
                </View>
              )}
            </Each>
          </View>
        </If>
      </View>

      {/* Switch Toggle */}
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
    </View>
  );
}
