// 插件与壳层管理器对话框 (Plugin & Shell Manager Dialog)
// 包含一键全界面壳层热插拔切换，三层架构（UI / Base / Engine）插件矩阵，以及开发者模式。

import { Scroll, signal, Text, View, token } from "nsh";
import { AppDialog } from "./overlays";
import { Icon } from "./icons";
import {
  activeShellId,
  devWatchEnabled,
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

export function PluginManagerDialog() {
  return (
    <AppDialog
      open={pluginDialogOpen()}
      onOpenChange={(next) => {
        if (next) refreshPlugins();
        setPluginDialogOpen(next);
      }}
      testID="plugin-dialog"
    >
      <PluginDialogContent />
    </AppDialog>
  );
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
    <View class="flex-col gap-4 p-6" style={{ width: 680, maxHeight: 620 }}>
      {/* Title Header */}
      <View class="flex-row items-center justify-between border-b border-border pb-2">
        <View class="flex-row items-center gap-2">
          <Icon name="settings" size={20} color="primary" />
          <Text class="text-base font-bold" style={{ color: token("foreground") }}>
            插件与界面壳层管理器
          </Text>
        </View>
        <View
          class="rounded p-1"
          onPress={() => setPluginDialogOpen(false)}
          accessibilityLabel="关闭"
        >
          <Icon name="close" size={16} color="muted" />
        </View>
      </View>

      {/* Top Section: One-Click Full UI Shell Switcher */}
      <View class="flex-col gap-2 rounded-xl border border-border p-3" style={{ backgroundColor: "#11131a" }}>
        <View class="flex-row items-center justify-between">
          <Text class="text-xs font-bold uppercase tracking-wider" style={{ color: token("primary") }}>
            整套界面壳层一键切换 (UI Shell Hot-Swap)
          </Text>
          <Text class="text-xs" style={{ color: token("muted") }}>
            无缝热插拔，音频连续播放不丢帧
          </Text>
        </View>

        <View class="mt-1 flex-row gap-3">
          {shells().map((shell) => {
            const isActive = activeShellId() === shell.id;
            return (
              <View
                class={`flex-1 cursor-pointer flex-col gap-1.5 rounded-lg border p-3 ${
                  isActive ? "border-primary" : "border-border"
                }`}
                style={{
                  backgroundColor: isActive ? "#181d2a" : "#141721",
                }}
                onPress={() => switchShell(shell.id)}
                accessibilityLabel={`切换至 ${shell.name}`}
              >
                <View class="flex-row items-center justify-between">
                  <Text class="text-sm font-semibold" style={{ color: isActive ? token("primary") : token("foreground") }}>
                    {shell.name}
                  </Text>
                  <If when={isActive}>
                    <Text class="rounded bg-primary/20 px-1.5 py-0.5 text-xs font-bold" style={{ color: token("primary") }}>
                      当前使用
                    </Text>
                  </If>
                </View>
                <Text class="truncate text-xs leading-relaxed" style={{ color: token("muted") }}>
                  {shell.description}
                </Text>
              </View>
            );
          })}
        </View>
      </View>

      {/* Tabs */}
      <View class="flex-row items-center gap-2 pt-1">
        {(
          [
            ["all", "全部插件"],
            ["ui", "UI 视图与插槽"],
            ["base", "音源与分轨扩展"],
            ["engine", "播放与 DSP 引擎"],
          ] as const
        ).map(([tabKey, label]) => {
          const isCurrent = currentTab() === tabKey;
          return (
            <View
              class={`cursor-pointer rounded-lg border px-3 py-1.5 ${
                isCurrent ? "border-foreground/30" : "border-transparent"
              }`}
              style={{
                backgroundColor: isCurrent ? "#232838" : "transparent",
              }}
              onPress={() => currentTab.set(tabKey)}
            >
              <Text class="text-xs font-medium" style={{ color: isCurrent ? token("foreground") : token("muted") }}>
                {label}
              </Text>
            </View>
          );
        })}
      </View>

      {/* Plugin Matrix List */}
      <Scroll class="flex-1 pr-1" contentClass="flex w-full flex-col gap-2" style={{ maxHeight: 260 }}>
        {filtered().map((p) => {
          return <PluginRow plugin={p} />;
        })}
      </Scroll>

      {/* Footer / Developer Mode Section */}
      <View
        class="flex-row items-center justify-between rounded-lg border border-border px-3 py-2 text-xs"
        style={{ backgroundColor: "#11131a" }}
      >
        <View class="flex-col">
          <Text class="font-mono text-xs" style={{ color: token("muted") }}>
            插件目录: ~/.config/neiro/plugins
          </Text>
          <Text class="font-mono text-xs" style={{ color: token("muted") }}>
            CLI: neiro_plugin list | enable | install | probe
          </Text>
        </View>
        <View class="flex-row items-center gap-2">
          <View
            class="cursor-pointer rounded border border-border px-2.5 py-1"
            style={{ backgroundColor: devWatchEnabled() ? "#1e293b" : "#161922" }}
            onPress={() => devWatchEnabled.set(!devWatchEnabled())}
          >
            <Text class="text-xs font-medium" style={{ color: devWatchEnabled() ? token("primary") : token("muted") }}>
              {devWatchEnabled() ? "Dev Watch 开启" : "Dev Watch 关闭"}
            </Text>
          </View>
          <View
            class="cursor-pointer rounded border border-border px-2.5 py-1"
            style={{ backgroundColor: "#1c2130" }}
            onPress={() => rescanPlugins()}
          >
            <Text class="text-xs font-semibold" style={{ color: token("foreground") }}>
              重新扫描本地插件
            </Text>
          </View>
        </View>
      </View>
    </View>
  );
}

function PluginRow(props: { plugin: PluginInfo }) {
  const p = () => props.plugin;
  const isEnabled = () => p().enabled;

  const tierBadge = () => {
    switch (p().tier) {
      case "ui":
        return { label: "UI Tier [热插拔]", bg: "#14221a", color: "#22c55e" };
      case "base":
        return { label: "Base Tier [即时生效]", bg: "#1a1f2e", color: "#60a5fa" };
      case "engine":
        return { label: "Engine Tier [破坏 Bit-Perfect / 需重开]", bg: "#2a1e16", color: "#f97316" };
    }
  };

  return (
    <View
      class="flex-row items-center justify-between rounded-lg border border-border p-3"
      style={{ backgroundColor: "#141720" }}
    >
      <View class="min-w-0 flex-1 flex-col gap-1 pr-3">
        <View class="flex-row items-center gap-2">
          <Text class="text-sm font-semibold" style={{ color: token("foreground") }}>
            {p().name}
          </Text>
          <Text class="rounded px-1.5 py-0.5 text-xs font-mono" style={{ backgroundColor: tierBadge().bg, color: tierBadge().color }}>
            {tierBadge().label}
          </Text>
          <Text class="text-xs font-mono" style={{ color: token("muted") }}>
            v{p().version}
          </Text>
        </View>

        <Text class="truncate text-xs" style={{ color: token("muted") }}>
          {p().description || p().category}
        </Text>

        <If when={p().permissions && p().permissions!.length > 0}>
          <View class="mt-0.5 flex-row flex-wrap gap-1">
            {p().permissions!.map((perm) => (
              <Text
                class="rounded bg-black/40 px-1 text-xs font-mono"
                style={{ color: "#94a3b8" }}
              >
                {perm}
              </Text>
            ))}
          </View>
        </If>
      </View>

      {/* Switch Toggle */}
      <View
        class="cursor-pointer rounded-full px-3 py-1"
        style={{
          backgroundColor: isEnabled() ? "#15803d" : "#334155",
        }}
        onPress={() => togglePlugin(p().id, !isEnabled())}
        accessibilityLabel={`启用或禁用 ${p().name}`}
      >
        <Text class="text-xs font-bold" style={{ color: "#ffffff" }}>
          {isEnabled() ? "已启用" : "已禁用"}
        </Text>
      </View>
    </View>
  );
}
