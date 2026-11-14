// 在线歌词：字段 → 搜索 → 排序 → 预览 → 采用。
//
// 字段先按文件里的标签填好，可以改：提供方是按歌名和歌手索引歌词的，标签写
// 成「track 07」就什么也搜不到。结果按匹配度排序（歌名、歌手、专辑、时长），
// 每行都写出分数，因为排序是建议，不是结论。
//
// 采用只改界面里的歌词，**不写文件**：写回是另一个动作，等 Base 能在 ID3v2
// 里落 USLT 之后才会有，并且必须由人确认。

import { effect, signal, Text, View, Scroll, untracked } from "nsh";
import { Button, Input } from "nsh/base";
import type { LyricsCandidate } from "../../backend/types";
import {
  adoptLyrics,
  lyricsQueryForCurrent,
  lyricsSearch,
  lyricsSearchOpen,
  rankedCandidates,
  setLyricsSearchOpen,
  startLyricsSearch,
  type LyricsQuery,
} from "../../state/lyrics";
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

export function LyricsSearchSheet() {
  const chosen = signal<LyricsCandidate | null>(null);
  const fields = signal<LyricsQuery>({ title: "", artist: "", album: "", durationS: null });

  // Filled when the sheet opens, from the track that is playing then: the
  // board is built once, before anything plays, so an initial value would be
  // empty for the first search and stale for every later one.
  effect(() => {
    if (!lyricsSearchOpen()) return;
    untracked(() => {
      fields.set(lyricsQueryForCurrent());
      chosen.set(null);
    });
  });

  const state = () => lyricsSearch();
  const results = () => rankedCandidates(fields());

  return (
    <AppSheet
      open={lyricsSearchOpen()}
      onOpenChange={(open) => {
        setLyricsSearchOpen(open);
      }}
      side="right"
      popupWidth={420}
      testID="lyrics-search"
    >
      <View class="h-full flex-col">
        <View class="flex-row items-center justify-between px-5 pt-4 pb-2">
          <Text class="text-sm font-bold text-foreground">在线歌词</Text>
          <Button
            accessibilityLabel="关闭"
            onPress={() => setLyricsSearchOpen(false)}
            class={`h-7 w-7 items-center justify-center rounded-full text-muted-foreground hover:bg-row-hover ${focusRing}`}
          >
            <Icon name="close" size={14} color="muted-foreground" />
          </Button>
        </View>

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
              onPress={() => {
                chosen.set(null);
                // An empty title would ask the provider for nothing; the
                // track's own fields are the answer in that case.
                startLyricsSearch(fields().title.trim() === "" ? undefined : fields());
              }}
              class={`h-8 flex-row items-center rounded-full bg-foreground px-3 ${focusRing}`}
            >
              <Text class="text-xs font-bold text-background">搜索</Text>
            </Button>
          </View>
        </View>

        <View class="min-h-0 flex-1">
          <If when={state().status === "searching"}>
            <View class="flex-row items-center gap-3 px-5 py-4">
              <Spinner label="正在搜索歌词" />
              <Text class="text-xs text-muted-foreground">正在搜索歌词…</Text>
            </View>
          </If>

          <If when={state().status === "done" && results().length === 0}>
            <View class="px-5 py-4">
              <Text class="text-sm text-muted-foreground">没有找到匹配的歌词。</Text>
            </View>
          </If>

          <Scroll class="min-h-0 flex-1" contentClass="gap-1 px-3 pb-4">
            <Each of={results()} key={(row) => String(row.candidate.id)}>
              {(row) => {
                const candidate = () => row().candidate;
                const selected = () => chosen()?.id === candidate().id;
                return (
                  <View class="gap-1">
                    <Button
                      accessibilityLabel={`候选 ${candidate().track || "未命名"}`}
                      onPress={() => chosen.set(selected() ? null : candidate())}
                      selected={selected()}
                      class={`w-full flex-col items-start gap-1 rounded-(--radius-inset) px-3 py-2 text-left transition duration-150 ease-out hover:bg-row-hover ${focusRing} ${
                        selected() ? "bg-row-hover" : ""
                      }`}
                    >
                      <View class="flex-row w-full items-center justify-between gap-2">
                        <Text class="truncate flex-1 text-sm font-medium text-foreground">
                          {candidate().track || "未命名"}
                        </Text>
                        <Text class="shrink-0 text-[10px] tabular-nums text-muted-foreground">
                          {Math.round(row().score * 100)}%
                        </Text>
                      </View>
                      <Text class="truncate text-xs text-muted-foreground">
                        {[candidate().artist, candidate().album, clockOf(candidate().durationS)]
                          .filter((part) => part !== "")
                          .join(" · ")}
                      </Text>
                      <Text class="text-[10px] uppercase tracking-[0.14em] text-muted-foreground/70">
                        {candidate().synced ? "有时间轴" : "无时间轴"}
                      </Text>
                    </Button>
                    <If when={selected()}>
                      <View class="gap-3 rounded-(--radius-inset) bg-row-hover px-3 py-3">
                        <Scroll class="max-h-64" contentClass="gap-1">
                          <Each
                            of={(candidate().synced ?? candidate().plain ?? "")
                              .split(/\r?\n/)
                              .filter((line) => line.trim() !== "")}
                          >
                            {(line) => (
                              <Text class="text-xs leading-relaxed text-muted-foreground">{line()}</Text>
                            )}
                          </Each>
                        </Scroll>
                        <Button
                          onPress={() => {
                            adoptLyrics(candidate());
                            setLyricsSearchOpen(false);
                          }}
                          class={`h-8 flex-row items-center self-end rounded-full bg-foreground px-3 ${focusRing}`}
                        >
                          <Text class="text-xs font-bold text-background">用这份歌词</Text>
                        </Button>
                      </View>
                    </If>
                  </View>
                );
              }}
            </Each>
          </Scroll>
        </View>
      </View>
    </AppSheet>
  );
}
