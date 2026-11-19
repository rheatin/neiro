// 设置页面：音频与输出策略、曲库与来源管理、外观与交互体验的一致化设置中心。

import { effect, signal, untracked, Scroll, Text, View } from "nsh";
import { Button } from "nsh/base";
import { AppPage } from "../components/page";
import { rememberScroll } from "../state/page-scroll";
import { ActionButton, focusRing, SectionTitle } from "../components/ui/controls";
import { Icon } from "../components/ui/icons";
import { enter } from "../components/ui/motion";
import { engine } from "../backend/engine";
import { gaplessEnabled, setGaplessEnabled } from "../state/player";
import { setOutputOpen, setPluginDialogOpen, setServerDialogOpen, sidebarCollapsed, toggleSidebar } from "../state/overlays";
import { open, switchTo, REMOTE, TRACKS } from "../nav";
import { addRoot, removeRoot, roots, setHomeRoot } from "../state/catalog";
import { servers, selectSource } from "../state/sources";
import { cachedRemoteTracks, reconnectServer, removeServerById } from "../state/remote";
import { EFFECT_NAMES, TOOLBAR_EFFECTS } from "../components/ui/toolbar-fx";
import { lightEffect, setLight } from "../state/visual";
import { isStreamBitPerfect, resolvedPath } from "../state/resolved";
import { activeShellId, isRemoteKindEnabled, plugins } from "../state/plugins";
import { autoLyricsEnabled, setAutoLyricsEnabled } from "../state/lyrics";
import {
  cacheBusy,
  cacheStats,
  clearCache,
  formatCacheSize,
  refreshCacheStats,
  type CacheKind,
} from "../state/cache";
import { AppDialog } from "../components/ui/overlays";
import { isPlaying, player } from "../state/player";

const CACHE_ROWS: {
  kind: Exclude<CacheKind, "all">;
  name: string;
  detail: string;
}[] = [
  { kind: "remote-audio", name: "远程音频", detail: "已取流的服务器曲目缓存。" },
  { kind: "remote-artwork", name: "远程封面", detail: "服务器封面图片缓存。" },
  { kind: "remote-catalog", name: "远程索引", detail: "远程浏览与分组索引。" },
  { kind: "local-artwork", name: "本地封面", detail: "本地文件的封面解析缓存。" },
];
export function SettingsPage() {
  const path = () => engine.renderPath();
  const res = () => resolvedPath();
  const gapless = () => gaplessEnabled();
  const serverList = () => servers();
  const localRoots = () => roots();
  // 切走 tab 再回来，停在原处。
  const scroll = rememberScroll("settings");

  const deviceLabel = () => {
    const r = res();
    const p = path();
    if (r?.deviceName) return r.deviceName;
    if (p?.sampleRate) return `${p.sampleRate} Hz`;
    return "—";
  };
  const clockLabel = () => {
    const p = path();
    if (!p || !p.sampleRate) return "未就绪";
    return `${p.sampleRate / 1000} kHz · ${p.channels} 声道`;
  };
  const bitDepthLabel = () => {
    const p = path();
    if (!p || !p.bitsPerSample) return "—";
    return `${p.bitsPerSample}-bit${p.dithered ? " · DITHER" : ""}`;
  };

  const stats = () => cacheStats();
  const audioDisabled = () => isPlaying() && player().path !== null;
  const clearAllOpen = signal(false);

  effect(() => {
    refreshCacheStats();
  });

  const cacheRow = (row: (typeof CACHE_ROWS)[number]) => {
    const stat = () => stats();
    const bytes = () => (row.kind === "remote-audio" ? stat().remote_audio_bytes : row.kind === "remote-artwork" ? stat().remote_artwork_bytes : row.kind === "remote-catalog" ? stat().remote_catalog_bytes : stat().local_artwork_bytes);
    const files = () => (row.kind === "remote-audio" ? stat().remote_audio_files : row.kind === "remote-artwork" ? stat().remote_artwork_files : row.kind === "remote-catalog" ? stat().remote_catalog_files : stat().local_artwork_files);
    const disabled = () => cacheBusy() || (row.kind === "remote-audio" && audioDisabled());
    return (
      <View class="flex-row items-center justify-between gap-3 border-b border-border/40 py-2 last:border-b-0">
        <View class="min-w-0 flex-1">
          <Text class="text-sm font-bold text-foreground">{row.name}</Text>
          <Text class="text-xs text-muted-foreground">{row.detail}</Text>
        </View>
        <View class="shrink-0 text-right">
          <Text class="font-mono text-xs font-bold text-foreground">{formatCacheSize(bytes())}</Text>
          <Text class="text-[11px] text-muted-foreground">{files()} 个文件</Text>
        </View>
        <View class="shrink-0">
          <Button
            testID={`cache-clear-${row.kind}`}
            disabled={disabled()}
            accessibilityLabel={`清理${row.name}`}
            onPress={() => void clearCache(row.kind)}
            class={`h-8 items-center justify-center rounded-full border px-3 active:scale-95 ${focusRing} ${
              disabled() ? "border-border bg-secondary opacity-40" : "border-border bg-secondary"
            }`}
          >
            <Text class="text-center text-xs font-medium leading-none text-foreground">
              {row.kind === "remote-audio" && audioDisabled() ? "播放中" : cacheBusy() ? "处理中" : "清理"}
            </Text>
          </Button>
        </View>
      </View>
    );
  };

  const renderCacheCard = () => (
    <View class="gap-3">
      <SectionTitle>存储与缓存</SectionTitle>
      <View class="rounded-(--radius-panel) border border-border bg-card p-4 gap-2" testID="cache-card">
        <Each of={CACHE_ROWS}>
          {(row) => cacheRow(row())}
        </Each>
        <If when={audioDisabled()}>
          <Text class="text-xs text-amber-500">播放中不可清理音频缓存</Text>
        </If>
        <View class="pt-2">
          <Button
            testID="cache-clear-all"
            disabled={cacheBusy() || audioDisabled()}
            accessibilityLabel="全部清理缓存"
            onPress={() => clearAllOpen.set(true)}
            class={`h-8 flex-row items-center justify-center rounded-full border border-destructive/40 bg-destructive/10 px-4 active:scale-95 ${focusRing} ${
              cacheBusy() || audioDisabled() ? "opacity-40" : ""
            }`}
          >
            <Text class="text-center text-xs font-bold leading-none text-destructive">全部清理</Text>
          </Button>
        </View>
      </View>
    </View>
  );
  const protocolLabel = () => {
    const r = res();
    const backend = r?.backend ? String(r.backend).toUpperCase() : "WASAPI";
    const share = r?.shareMode === 1 ? "EXCL" : "SHARED";
    return `${backend} ${share}`;
  };

  return (
    <AppPage title="设置" testID="page-settings" scroll={scroll}>
      <Scroll class="flex-1" contentClass="pb-28" contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <View class="px-6 pt-2 pb-6 gap-1" role="header">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">系统首选项</Text>
          <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            设置
          </Text>
        </View>

        <View class="px-6 gap-8">
          {/* ---- 1. 音频与输出 ---- */}
          <View class="gap-3">
            <SectionTitle>音频与输出</SectionTitle>

            {/* 当前硬件通路状态卡 */}
            <View class="rounded-(--radius-panel) border border-border bg-card p-4 gap-3">
              <View class="flex-row items-center justify-between">
                <Text class="text-xs font-bold uppercase tracking-wider text-muted-foreground">当前输出通路</Text>
                <View class="flex-row items-center gap-1.5">
                  <View
                    class="h-2 w-2 rounded-full"
                    style={{ backgroundColor: isStreamBitPerfect() ? "#34D399" : "#F59E0B" }}
                  />
                  <Text class="font-mono text-xs font-bold" style={{ color: isStreamBitPerfect() ? "#34D399" : "#F59E0B" }}>
                    {isStreamBitPerfect() ? "BIT-PERFECT" : "DSP ACTIVE"}
                  </Text>
                </View>
              </View>

              <View class="flex-row flex-wrap gap-4 py-1">
                <View class="gap-0.5">
                  <Text class="text-[10px] text-muted-foreground">设备</Text>
                  <Text class="font-mono text-xs font-bold text-foreground">
                    {deviceLabel()}
                  </Text>
                </View>
                <View class="gap-0.5">
                  <Text class="text-[10px] text-muted-foreground">时钟与声道</Text>
                  <Text class="font-mono text-xs text-foreground">
                    {clockLabel()}
                  </Text>
                </View>
                <View class="gap-0.5">
                  <Text class="text-[10px] text-muted-foreground">位深 / 抖动</Text>
                  <Text class="font-mono text-xs text-foreground">
                    {bitDepthLabel()}
                  </Text>
                </View>
                <View class="gap-0.5">
                  <Text class="text-[10px] text-muted-foreground">协议</Text>
                  <Text class="font-mono text-xs text-foreground">
                    {protocolLabel()}
                  </Text>
                </View>
              </View>

              <View class="flex-row flex-wrap gap-2 pt-2 border-t border-border/50">
                <ActionButton label="打开输出控制台" variant="outline" onPress={() => setOutputOpen(true)} />
                <ActionButton label="均衡器与 DSP (EQ)" variant="outline" onPress={() => open({ name: "eq" })} />
              </View>
            </View>

            {/* Gapless 无缝播放开关行 */}
            <View class="flex-row items-center justify-between rounded-(--radius-panel) border border-border bg-card p-4">
              <View class="gap-0.5 min-w-0 flex-1 pr-4">
                <Text class="text-sm font-bold text-foreground">无缝播放 (Gapless)</Text>
                <Text class="text-xs text-muted-foreground">
                  消除曲目切换缝隙，保持位精确直通；开启时自动旁通 Crossfade 淡入淡出。
                </Text>
              </View>
              <Button
                accessibilityLabel={gapless() ? "无缝播放已开启，点击关闭" : "无缝播放已关闭，点击开启"}
                onPress={() => setGaplessEnabled(!gapless())}
                class={`h-8 items-center justify-center rounded-full border px-4 transition duration-150 ease-out active:scale-95 ${
                  gapless() ? "border-amber-500 bg-amber-500/15" : "border-border bg-secondary"
                } ${focusRing}`}
              >
                <Text class={`text-center font-mono text-xs font-bold leading-none ${gapless() ? "text-amber-500" : "text-muted-foreground"}`}>
                  {gapless() ? "已启用 ON" : "已关闭 OFF"}
                </Text>
              </Button>
            </View>
          </View>

          {/* ---- 2. 曲库与来源管理 ---- */}
          <View class="gap-4">
            <SectionTitle>曲库与来源管理</SectionTitle>

            {/* 本地文件夹段 */}
            <View class="rounded-(--radius-panel) border border-border bg-card p-4 gap-3">
              <View class="flex-row items-center justify-between">
                <View class="flex-row items-center gap-2">
                  <Icon name="folder" size={16} color="foreground" />
                  <Text class="text-sm font-bold text-foreground">本地文件夹 ({localRoots().length})</Text>
                </View>
                <ActionButton label="添加本地文件夹" onPress={() => void addRoot()} />
              </View>

              <If
                when={localRoots().length > 0}
                else={<Text class="text-xs text-muted-foreground py-2">还没有添加本地文件夹。</Text>}
              >
                <View class="gap-2 pt-1">
                  <Each of={localRoots()}>
                    {(root) => {
                      const leaf = () => root().replace(/\\/g, "/").split("/").pop() || root();
                      return (
                        <View class="flex-row items-center justify-between rounded-(--radius) bg-secondary/50 px-3 py-2">
                          <View class="min-w-0 flex-1 pr-3">
                            <Text class="text-sm font-medium text-foreground truncate">{leaf()}</Text>
                            <Text class="text-[11px] font-mono text-muted-foreground truncate">{root()}</Text>
                          </View>
                          <View class="flex-row items-center gap-1.5 shrink-0">
                            <Button
                              onPress={() => {
                                setHomeRoot(root());
                                switchTo(TRACKS);
                              }}
                              class={`h-7 items-center justify-center rounded-(--radius) bg-secondary px-3 hover:bg-row-hover active:scale-95 ${focusRing}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-foreground">进入管理</Text>
                            </Button>
                            <Button
                              onPress={() => engine.scanFolder(root())}
                              class={`h-7 items-center justify-center rounded-(--radius) bg-secondary px-3 hover:bg-row-hover active:scale-95 ${focusRing}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-foreground">重新扫描</Text>
                            </Button>
                            <Button
                              onPress={() => removeRoot(root())}
                              class={`h-7 items-center justify-center rounded-(--radius) px-3 hover:bg-destructive/10 active:scale-95 ${focusRing}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-destructive">移除</Text>
                            </Button>
                          </View>
                        </View>
                      );
                    }}
                  </Each>
                </View>
              </If>
            </View>

            {/* 远程服务器段 */}
            <View class="rounded-(--radius-panel) border border-border bg-card p-4 gap-3">
              <View class="flex-row items-center justify-between">
                <View class="flex-row items-center gap-2">
                  <Icon name="server" size={16} color="foreground" />
                  <Text class="text-sm font-bold text-foreground">远程服务器 ({serverList().length})</Text>
                </View>
                <ActionButton label="添加远程服务器" onPress={() => setServerDialogOpen(true)} />
              </View>

              <If
                when={serverList().length > 0}
                else={<Text class="text-xs text-muted-foreground py-2">还没有添加远程服务器。</Text>}
              >
                <View class="gap-2 pt-1">
                  <Each of={serverList()}>
                    {(srv) => {
                      const count = () => cachedRemoteTracks(srv().id).length;
                      const enabled = () => isRemoteKindEnabled(srv().kind);
                      return (
                        <View class={`flex-row items-center justify-between rounded-(--radius) bg-secondary/50 px-3 py-2 ${enabled() ? "" : "opacity-60"}`}>
                          <View class="min-w-0 flex-1 pr-3">
                            <View class="flex-row items-center gap-1.5">
                              <Text class="text-sm font-medium text-foreground truncate">{srv().label}</Text>
                              <If when={!enabled()}>
                                <View class="rounded px-1.5 py-0.5 bg-amber-500/15">
                                  <Text class="text-[10px] font-bold text-amber-500">插件已停用</Text>
                                </View>
                              </If>
                            </View>
                            <Text class="text-[11px] font-mono text-muted-foreground truncate">
                              {srv().kind.toUpperCase()} · {srv().baseUrl} · 缓存 {count()} 首
                            </Text>
                          </View>
                          <View class="flex-row items-center gap-1.5 shrink-0">
                            <Button
                              disabled={!enabled()}
                              onPress={() => {
                                if (!enabled()) return;
                                selectSource(`remote:${srv().id}`);
                                switchTo(REMOTE);
                              }}
                              class={`h-7 items-center justify-center rounded-(--radius) bg-secondary px-3 hover:bg-row-hover active:scale-95 ${focusRing} ${enabled() ? "" : "opacity-40"}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-foreground">进入管理</Text>
                            </Button>
                            <Button
                              disabled={!enabled()}
                              onPress={() => {
                                if (!enabled()) return;
                                void reconnectServer(srv().id);
                              }}
                              class={`h-7 items-center justify-center rounded-(--radius) bg-secondary px-3 hover:bg-row-hover active:scale-95 ${focusRing} ${enabled() ? "" : "opacity-40"}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-foreground">增量同步</Text>
                            </Button>
                            <Button
                              onPress={() => engine.probeRemote()}
                              class={`h-7 items-center justify-center rounded-(--radius) bg-secondary px-3 hover:bg-row-hover active:scale-95 ${focusRing}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-foreground">测试连接</Text>
                            </Button>
                            <Button
                              onPress={() => void removeServerById(srv().id)}
                              class={`h-7 items-center justify-center rounded-(--radius) px-3 hover:bg-destructive/10 active:scale-95 ${focusRing}`}
                            >
                              <Text class="text-center text-xs font-medium leading-none text-destructive">移除</Text>
                            </Button>
                          </View>
                        </View>
                      );
                    }}
                  </Each>
                </View>
              </If>
            </View>
          </View>

          {/* ---- 3. 外观与交互体验 ---- */}
          <View class="gap-3">
            <SectionTitle>外观与体验</SectionTitle>

            {/* Now Playing 光效选择 */}
            <View class="rounded-(--radius-panel) border border-border bg-card p-4 gap-3">
              <View class="gap-0.5">
                <Text class="text-sm font-bold text-foreground">Now Playing 氛围光效</Text>
                <Text class="text-xs text-muted-foreground">根据音频实时频谱在底板渲染动态光波。</Text>
              </View>

              <View class="flex-row flex-wrap gap-2 pt-1">
                <Each of={TOOLBAR_EFFECTS}>
                  {(eff) => {
                    const active = () => lightEffect() === eff();
                    return (
                      <Button
                        accessibilityLabel={`光效 ${EFFECT_NAMES[eff()]}`}
                        onPress={() => setLight(eff())}
                        class={`h-8 items-center justify-center rounded-full border px-3.5 transition duration-150 ease-out active:scale-95 ${
                          active() ? "border-primary bg-primary" : "border-border bg-secondary"
                        } ${focusRing}`}
                      >
                        <Text class={`text-center text-xs leading-none ${active() ? "font-bold text-primary-foreground" : "text-muted-foreground"}`}>
                          {EFFECT_NAMES[eff()]}
                        </Text>
                      </Button>
                    );
                  }}
                </Each>
              </View>
            </View>

            {/* 侧栏收起状态开关 */}
            <View class="flex-row items-center justify-between rounded-(--radius-panel) border border-border bg-card p-4">
              <View class="gap-0.5 min-w-0 flex-1 pr-4">
                <Text class="text-sm font-bold text-foreground">侧栏模式</Text>
                <Text class="text-xs text-muted-foreground">展开完整导航栏或收起为纯图标轻量模式。</Text>
              </View>
              <Button
                accessibilityLabel={sidebarCollapsed() ? "展开侧栏" : "收起侧栏"}
                onPress={toggleSidebar}
                class={`h-8 items-center justify-center rounded-full border border-border bg-secondary px-4 active:scale-95 ${focusRing}`}
              >
                <Text class="text-center text-xs font-medium leading-none text-foreground">
                  {sidebarCollapsed() ? "当前：纯图标 (点击展开)" : "当前：完整 (点击收起)"}
                </Text>
              </Button>
            </View>
          </View>

          {/* ---- 歌词 ---- */}
          <View class="gap-3">
            <SectionTitle>歌词</SectionTitle>
            <View class="flex-row items-center justify-between rounded-(--radius-panel) border border-border bg-card p-4">
              <View class="gap-0.5 min-w-0 flex-1 pr-4">
                <Text class="text-sm font-bold text-foreground">自动检索在线歌词</Text>
                <Text class="text-xs text-muted-foreground">
                  当前曲目没有内嵌歌词时，自动向已启用的歌词源检索并采用最匹配的一份；歌词源在插件管理器中管理。
                </Text>
              </View>
              <Button
                accessibilityLabel={autoLyricsEnabled() ? "自动检索歌词已开启，点击关闭" : "自动检索歌词已关闭，点击开启"}
                onPress={() => setAutoLyricsEnabled(!autoLyricsEnabled())}
                class={`h-8 items-center justify-center rounded-full border px-4 transition duration-150 ease-out active:scale-95 ${
                  autoLyricsEnabled() ? "border-amber-500 bg-amber-500/15" : "border-border bg-secondary"
                } ${focusRing}`}
              >
                <Text class={`text-center font-mono text-xs font-bold leading-none ${autoLyricsEnabled() ? "text-amber-500" : "text-muted-foreground"}`}>
                  {autoLyricsEnabled() ? "已启用 ON" : "已关闭 OFF"}
                </Text>
              </Button>
            </View>
          </View>

          {/* ---- 4. 插件与界面壳层 ---- */}
          <View class="gap-3">
            <SectionTitle>插件与界面壳层</SectionTitle>
            <View class="flex-row items-center justify-between rounded-(--radius-panel) border border-border bg-card p-4">
              <View class="flex-row items-center gap-3 min-w-0 flex-1 pr-4">
                <View class="h-10 w-10 shrink-0 items-center justify-center rounded-lg bg-primary/10">
                  <Icon name="puzzle" size={20} color="primary" />
                </View>
                <View class="gap-0.5 min-w-0 flex-1">
                  <Text class="text-sm font-bold text-foreground">三层插件与整套界面热插拔</Text>
                  <Text class="text-xs text-muted-foreground">
                    已加载 {plugins().length} 个插件 · 当前界面壳层：{activeShellId() === "builtin.shell.studio-minimal" ? "Studio Minimal Deck" : "Industrial Rack (Default)"}
                  </Text>
                </View>
              </View>
              <Button
                accessibilityLabel="管理插件与界面壳层"
                onPress={() => setPluginDialogOpen(true)}
                class={`h-8 flex-row items-center gap-1.5 rounded-full border border-primary bg-primary/10 px-4 active:scale-95 ${focusRing}`}
              >
                <Icon name="puzzle" size={13} color="primary" />
                <Text class="text-center text-xs font-semibold leading-none text-primary">
                  打开插件管理器
                </Text>
              </Button>
            </View>
          </View>
          {renderCacheCard()}
        </View>

        <AppDialog open={clearAllOpen()} onOpenChange={(open) => clearAllOpen.set(open)} testID="cache-clear-all-dialog">
          <View class="gap-4 p-6">
            <Text class="text-lg font-bold text-foreground">确认全部清理？</Text>
            <Text class="text-sm text-muted-foreground">
              这会删除四类缓存文件；正在使用的文件可能保留到下一次清理。
            </Text>
            <View class="flex-row justify-end gap-2">
              <Button
                onPress={() => clearAllOpen.set(false)}
                class={`h-9 items-center justify-center rounded-full border border-border bg-secondary px-4 ${focusRing}`}
              >
                <Text class="text-center text-sm font-medium leading-none text-foreground">取消</Text>
              </Button>
              <Button
                testID="cache-clear-all-confirm"
                accessibilityLabel="确认全部清理"
                onPress={() => {
                  clearAllOpen.set(false);
                  void clearCache("all");
                }}
                class={`h-9 items-center justify-center rounded-full bg-destructive px-4 ${focusRing}`}
              >
                <Text class="text-center text-sm font-bold leading-none text-background">清理</Text>
              </Button>
            </View>
          </View>
        </AppDialog>
      </Scroll>
    </AppPage>
  );
}
