// 写入前的确认：改的是音频文件本身，所以要问一次，并写清改哪个文件。

import { Text, View } from "nsh";
import { cancelWriteLyrics, confirmWriteLyrics, lyricsWrite } from "../../state/lyrics";
import { AppDialog } from "../ui/overlays";
import { ActionButton } from "../ui/controls";

export function LyricsWriteDialog() {
  const asked = () => lyricsWrite();
  const name = () => {
    const path = asked()?.path ?? "";
    return path.split(/[\\/]/).pop() ?? path;
  };
  return (
    <AppDialog open={asked() !== null} onOpenChange={(open) => !open && cancelWriteLyrics()}>
      <View class="w-96 gap-3 rounded-(--radius-panel) bg-popover p-5">
        <Text class="text-sm font-bold text-foreground">把歌词写进文件？</Text>
        <Text class="text-xs leading-relaxed text-muted-foreground">
          这份歌词会写进「{name()}」的标签里，覆盖文件里原有的歌词。音频本身不会变。
        </Text>
        {/* One control for both: a link and a pill put their labels on
            different baselines, which reads as a misaligned row. */}
        <View class="flex-row items-center justify-end gap-2 pt-2">
          <ActionButton label="取消" variant="outline" onPress={() => cancelWriteLyrics()} />
          <ActionButton label="写入" onPress={() => confirmWriteLyrics()} />
        </View>
      </View>
    </AppDialog>
  );
}
