// 在线歌词：字段 → 搜索 → 排序 → 预览 → 采用。
//
// 字段先按文件里的标签填好，可以改：提供方是按歌名和歌手索引歌词的。
// 结果按匹配度排序，支持多源过滤胶囊栏与提供商徽章。

import { effect, signal, Text, View, Scroll, untracked } from "nsh";
import { Button, Input } from "nsh/base";
import type { LyricsCandidate } from "../../backend/types";
import {
  adoptLyrics,
  enabledLyricsCount,
  enabledLyricsProviders,
  lyricsQueryForCurrent,
  lyricsSearch,
  lyricsSearchOpen,
  rankedCandidates,
  setLyricsSearchOpen,
  startLyricsSearch,
  type LyricsQuery,
} from "../../state/lyrics";
import { setPluginDialogOpen } from "../../state/overlays";
import { AppSheet } from "../ui/overlays";
import { Spinner } from "../ui/feedback";
import { focusRing } from "../ui/controls";
import { Icon } from "../ui/icons";

/** `3:46`, from seconds. */
function clockOf(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds <= 0) return "";
  const minutes = Math.floor(seconds / 60);
  const rest = Math.round(seconds - minutes * 60);
  return `${minutes}:${String(rest).padStart(2, "0")}`;
}

interface ProviderMeta {
  label: string;
  badgeClass: string;
}

function getProviderMeta(providerId?: string): ProviderMeta {
  switch (providerId) {
    case "builtin.lyrics.netease":
      return { label: "网易云", badgeClass: "bg-rose-500/15 text-rose-500 border border-rose-500/30" };
    case "builtin.lyrics.qqmusic":
      return { label: "QQ音乐", badgeClass: "bg-emerald-500/15 text-emerald-500 border border-emerald-500/30" };
    case "builtin.lyrics.kugou":
      return { label: "酷狗", badgeClass: "bg-sky-500/15 text-sky-400 border border-sky-500/30" };
    case "builtin.lyrics.lrclib":
      return { label: "LRCLIB", badgeClass: "bg-purple-500/15 text-purple-400 border border-purple-500/30" };
    default:
      return { label: "在线", badgeClass: "bg-secondary text-muted-foreground border border-border" };
  }
}

export function LyricsSearchSheet() {
  const chosen = signal<LyricsCandidate | null>(null);
  const fields = signal<LyricsQuery>({ title: "", artist: "", album: "", durationS: null });
  const selectedProvider = signal<string>("all");

  // Filled when the sheet opens, from the track that is playing then.
  effect(() => {
    if (!lyricsSearchOpen()) return;
    untracked(() => {
      fields.set(lyricsQueryForCurrent());
      chosen.set(null);
      selectedProvider.set("all");
    });
  });

  const state = () => lyricsSearch();
  const allResults = () => rankedCandidates(fields());

  // Filtered by selected provider pill
  const results = () => {
    const list = allResults();
    const filter = selectedProvider();
    if (filter === "all") return list;
    return list.filter((r) => r.candidate.provider === filter);
  };

  const enabledCount = () => enabledLyricsCount();
  const enabledProviders = () => enabledLyricsProviders();

  return (
    <AppSheet
      open={lyricsSearchOpen()}
      onOpenChange={(open) => {
        setLyricsSearchOpen(open);
      }}
      side="right"
      popupWidth={440}
      testID="lyrics-search"
    >
      <View class="h-full flex-col">
        {/* Header */}
        <View class="flex-row items-center justify-between px-5 pt-4 pb-2">
          <View class="flex-row items-center gap-2">
            <Text class="text-sm font-bold text-foreground">在线歌词</Text>
            <View class="rounded bg-secondary px-1.5 py-0.5">
              <Text class="font-mono text-[10px] text-muted-foreground">
                {enabledCount()} 个歌词源就绪
              </Text>
            </View>
          </View>
          <Button
            accessibilityLabel="关闭"
            onPress={() => setLyricsSearchOpen(false)}
            class={`h-7 w-7 items-center justify-center rounded-full text-muted-foreground hover:bg-row-hover ${focusRing}`}
          >
            <Icon name="close" size={14} color="muted-foreground" />
          </Button>
        </View>

        {/* Search Inputs */}
        <View class="gap-1.5 px-5 pb-3">
          <Input
            value={fields().title}
            onChange={({ value }: { value: string }) => fields.set({ ...fields(), title: value })}
            placeholder="歌名"
            accessibilityLabel="歌词搜索的歌名"
            class="h-8 rounded-(--radius-inset) bg-row-hover px-2 text-xs text-foreground"
          />
          <View class="flex-row gap-2">
            <Input
              value={fields().artist}
              onChange={({ value }: { value: string }) => fields.set({ ...fields(), artist: value })}
              placeholder="歌手"
              accessibilityLabel="歌词搜索的歌手"
              class="h-8 min-w-0 flex-1 rounded-(--radius-inset) bg-row-hover px-2 text-xs text-foreground"
            />
            <Input
              value={fields().album}
              onChange={({ value }: { value: string }) => fields.set({ ...fields(), album: value })}
              placeholder="专辑"
              accessibilityLabel="歌词搜索的专辑"
              class="h-8 min-w-0 flex-1 rounded-(--radius-inset) bg-row-hover px-2 text-xs text-foreground"
            />
          </View>
          <View class="flex-row items-center justify-between">
            <Text class="text-[10px] text-muted-foreground">
              {fields().durationS ? `按 ${clockOf(fields().durationS!)} 的时长比对` : "没有时长，只按文字比对"}
            </Text>
            <Button
              accessibilityLabel="搜索歌词"
              disabled={state().status === "searching" || enabledCount() === 0}
              onPress={() => {
                chosen.set(null);
                selectedProvider.set("all");
                startLyricsSearch(fields().title.trim() === "" ? undefined : fields());
              }}
              class={`h-8 flex-row items-center gap-1.5 rounded-full bg-foreground px-3.5 ${focusRing} ${
                state().status === "searching" || enabledCount() === 0 ? "opacity-70" : ""
              }`}
            >
              <If when={state().status === "searching"}>
                <Spinner label="搜索中" />
              </If>
              <Text class="text-xs font-bold text-background">
                {state().status === "searching" ? "搜索中…" : "多源并发搜索"}
              </Text>
            </Button>
          </View>
        </View>

        {/* Content Area */}
        <View class="min-h-0 flex-1">
          {/* All lyrics plugins disabled empty state */}
          <If when={enabledCount() === 0}>
            <View class="flex-1 items-center justify-center gap-3 p-8 text-center">
              <View class="h-10 w-10 items-center justify-center rounded-full bg-muted">
                <Icon name="fileText" size={20} color="muted-foreground" />
              </View>
              <Text class="text-sm font-bold text-foreground">所有在线歌词源插件均已停用</Text>
              <Text class="text-xs text-muted-foreground max-w-xs text-center leading-relaxed">
                当前没有已启用的歌词源（LRCLIB、网易云、QQ音乐、酷狗）。请在插件管理器中重新开启。
              </Text>
              <Button
                onPress={() => {
                  setLyricsSearchOpen(false);
                  setPluginDialogOpen(true);
                }}
                class={`mt-2 rounded-full border border-border bg-card px-4 py-2 hover:bg-muted ${focusRing}`}
              >
                <Text class="text-xs font-bold text-foreground">打开插件管理器</Text>
              </Button>
            </View>
          </If>

          <If when={enabledCount() > 0}>
            <If when={state().status === "searching"}>
              <View class="flex-row items-center gap-3 px-5 py-4">
                <Spinner label="正在搜索歌词" />
                <Text class="text-xs text-muted-foreground">正在并发检索各歌词源…</Text>
              </View>
            </If>

            <If when={state().status === "done" && allResults().length === 0}>
              <View class="px-5 py-8 items-center justify-center gap-2">
                <Text class="text-sm font-medium text-foreground">没有找到匹配的歌词</Text>
                <Text class="text-xs text-muted-foreground">尝试简化歌名或清空歌手/专辑后重新搜索</Text>
              </View>
            </If>

            <If when={state().status === "done" && allResults().length > 0}>
              {/* Filter Pills Bar */}
              <View class="border-b border-border/60 px-4 pb-2">
                <Scroll horizontal class="w-full">
                  <View class="flex-row items-center gap-1.5">
                    {/* All Pill */}
                    <Button
                      selected={selectedProvider() === "all"}
                      onPress={() => selectedProvider.set("all")}
                      class={`rounded-full px-2.5 py-1 text-xs transition ${
                        selectedProvider() === "all"
                          ? "bg-foreground text-background font-bold shadow-xs"
                          : "bg-secondary text-muted-foreground hover:text-foreground"
                      } ${focusRing}`}
                    >
                      <Text class={`text-[11px] ${selectedProvider() === "all" ? "text-background font-bold" : "text-muted-foreground"}`}>
                        全部 ({allResults().length})
                      </Text>
                    </Button>

                    {/* Per-Provider Pills */}
                    <Each of={enabledProviders()}>
                      {(p) => {
                        const count = () => allResults().filter((r) => r.candidate.provider === p().id).length;
                        const isCurrent = () => selectedProvider() === p().id;
                        return (
                          <If when={count() > 0}>
                            <Button
                              selected={isCurrent()}
                              onPress={() => selectedProvider.set(p().id)}
                              class={`rounded-full px-2.5 py-1 text-xs transition ${
                                isCurrent()
                                  ? "bg-foreground text-background font-bold shadow-xs"
                                  : "bg-secondary text-muted-foreground hover:text-foreground"
                              } ${focusRing}`}
                            >
                              <Text class={`text-[11px] ${isCurrent() ? "text-background font-bold" : "text-muted-foreground"}`}>
                                {getProviderMeta(p().id).label} ({count()})
                              </Text>
                            </Button>
                          </If>
                        );
                      }}
                    </Each>
                  </View>
                </Scroll>
              </View>
            </If>

            {/* Candidate List */}
            <Scroll class="min-h-0 flex-1" contentClass="gap-1.5 px-3 py-3">
              <Each of={results()} key={(row) => String(row.candidate.id)}>
                {(row) => {
                  const candidate = () => row().candidate;
                  const selected = () => chosen()?.id === candidate().id;
                  const meta = () => getProviderMeta(candidate().provider);
                  const syncType = () => {
                    if (candidate().synced) return "时间轴同步";
                    if (candidate().plain) return "纯文本";
                    return "歌词预览";
                  };

                  return (
                    <View class="gap-1">
                      <Button
                        accessibilityLabel={`候选 ${candidate().track || "未命名"}`}
                        onPress={() => chosen.set(selected() ? null : candidate())}
                        selected={selected()}
                        class={`w-full flex-col items-start gap-1.5 rounded-lg border border-border/40 p-3 text-left transition duration-150 ease-out hover:border-foreground/20 hover:bg-row-hover ${focusRing} ${
                          selected() ? "border-primary bg-row-hover" : "bg-card/50"
                        }`}
                      >
                        <View class="flex-row w-full items-center justify-between gap-2">
                          <View class="flex-row items-center gap-1.5 min-w-0 flex-1">
                            {/* Provider Badge */}
                            <View class={`rounded px-1.5 py-0.5 ${meta().badgeClass}`}>
                              <Text class="font-mono text-[9px] font-bold">
                                {meta().label}
                              </Text>
                            </View>
                            <Text class="truncate text-sm font-bold text-foreground">
                              {candidate().track || "未命名"}
                            </Text>
                          </View>
                          <Text class="shrink-0 font-mono text-[10px] font-bold tabular-nums text-primary">
                            {Math.round(row().score * 100)}% 匹配
                          </Text>
                        </View>

                        <Text class="truncate text-xs text-muted-foreground">
                          {[candidate().artist, candidate().album, clockOf(candidate().durationS)]
                            .filter((part) => part !== "")
                            .join(" · ")}
                        </Text>

                        <View class="flex-row items-center gap-2">
                          <View class="rounded bg-secondary px-1.5 py-0.5">
                            <Text class="text-[9px] font-medium text-muted-foreground">
                              {syncType()}
                            </Text>
                          </View>
                        </View>
                      </Button>

                      <If when={selected()}>
                        <View class="gap-3 rounded-lg border border-border bg-secondary/80 p-3">
                          <Scroll class="max-h-56" contentClass="gap-1">
                            <Each
                              of={(candidate().synced ?? candidate().plain ?? "")
                                .split(/\r?\n/)
                                .filter((line) => line.trim() !== "")}
                            >
                              {(line) => (
                                <Text class="text-xs leading-relaxed text-foreground/80">{line()}</Text>
                              )}
                            </Each>
                          </Scroll>
                          <View class="flex-row items-center justify-between border-t border-border/50 pt-2">
                            <Text class="text-[11px] text-muted-foreground">
                              来源: {meta().label} ({candidate().providerName ?? candidate().provider ?? "在线"})
                            </Text>
                            <Button
                              onPress={() => {
                                adoptLyrics(candidate());
                                setLyricsSearchOpen(false);
                              }}
                              class={`h-8 flex-row items-center rounded-full bg-foreground px-4 shadow-sm hover:opacity-90 ${focusRing}`}
                            >
                              <Text class="text-xs font-bold text-background">用这份歌词</Text>
                            </Button>
                          </View>
                        </View>
                      </If>
                    </View>
                  );
                }}
              </Each>
            </Scroll>
          </If>
        </View>
      </View>
    </AppSheet>
  );
}
