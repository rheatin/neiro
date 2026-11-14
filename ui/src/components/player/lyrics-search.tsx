// 在线歌词：搜到的候选 → 预览 → 采用。
//
// 采用只改界面里的歌词，**不写文件**：写回是另一个动作，等 Base 能在 ID3v2
// 里落 USLT 之后才会有，并且必须由人确认。所以这个面板只回答一个问题——
// "这几份词，用哪一份"。

import { signal, Text, View, Scroll } from "nsh";
import { Button } from "nsh/base";
import type { LyricsCandidate } from "../../backend/types";
import {
  adoptLyrics,
  lyricsSearch,
  lyricsSearchOpen,
  setLyricsSearchOpen,
  startLyricsSearch,
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

  const state = () => lyricsSearch();
  const candidates = () => {
    const now = state();
    return now.status === "done" ? now.candidates : [];
  };

  return (
    <AppSheet
      open={lyricsSearchOpen()}
      onOpenChange={(open) => {
        if (!open) chosen.set(null);
        setLyricsSearchOpen(open);
      }}
      side="right"
      popupWidth={420}
      testID="lyrics-search"
    >
      <View class="h-full flex-col">
        <View class="flex-row items-center justify-between px-5 pt-4 pb-3">
          <Text class="text-sm font-semibold text-foreground">在线歌词</Text>
          <Button
            accessibilityLabel="关闭"
            onPress={() => setLyricsSearchOpen(false)}
            class={`h-7 w-7 items-center justify-center rounded-full text-muted-foreground hover:bg-row-hover ${focusRing}`}
          >
            <Icon name="close" size={14} color="muted-foreground" />
          </Button>
        </View>

        <View class="min-h-0 flex-1">
          <If when={state().status === "searching"}>
            <View class="flex-row items-center gap-3 px-5 py-4">
              <Spinner label="正在搜索歌词" />
              <Text class="text-xs text-muted-foreground">正在搜索歌词…</Text>
            </View>
          </If>

          <If when={state().status === "done" && candidates().length === 0}>
            <View class="items-start gap-3 px-5 py-4">
              <Text class="text-sm text-muted-foreground">没有找到匹配的歌词。</Text>
              <Button
                onPress={() => startLyricsSearch()}
                class={`h-8 flex-row items-center rounded-full bg-row-hover px-3 ${focusRing}`}
              >
                <Text class="text-xs font-medium text-foreground">再搜一次</Text>
              </Button>
            </View>
          </If>

          <Scroll class="min-h-0 flex-1" contentClass="gap-1 px-3 pb-4">
            <Each of={candidates()} key={(item) => String(item.id)}>
              {(c) => {
                const selected = () => chosen()?.id === c().id;
                return (
                  <View class="gap-1">
                    <Button
                      onPress={() => chosen.set(selected() ? null : c())}
                      selected={selected()}
                      class={`w-full flex-col items-start gap-1 rounded-(--radius-inset) px-3 py-2 text-left transition duration-150 ease-out hover:bg-row-hover ${focusRing} ${
                        selected() ? "bg-row-hover" : ""
                      }`}
                    >
                      <Text class="truncate text-sm font-medium text-foreground">
                        {c().track || "未命名"}
                      </Text>
                      <Text class="truncate text-xs text-muted-foreground">
                        {[c().artist, c().album, clockOf(c().durationS)].filter((part) => part !== "").join(" · ")}
                      </Text>
                      <Text class="text-[10px] uppercase tracking-[0.14em] text-muted-foreground/70">
                        {c().synced ? "有时间轴" : "无时间轴"}
                      </Text>
                    </Button>
                    <If when={selected()}>
                      <View class="gap-3 rounded-(--radius-inset) bg-row-hover px-3 py-3">
                        <Scroll class="max-h-64" contentClass="gap-1">
                          <Each of={(c().synced ?? c().plain ?? "").split(/\r?\n/).filter((line) => line.trim() !== "")}>
                            {(line) => (
                              <Text class="text-xs leading-relaxed text-muted-foreground">{line()}</Text>
                            )}
                          </Each>
                        </Scroll>
                        <Button
                          onPress={() => {
                            adoptLyrics(c());
                            setLyricsSearchOpen(false);
                          }}
                          class={`h-8 flex-row items-center self-end rounded-full bg-foreground px-3 ${focusRing}`}
                        >
                          <Text class="text-xs font-semibold text-background">用这份歌词</Text>
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
