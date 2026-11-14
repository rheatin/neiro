// 多选的批量条：选中若干曲目后浮在页面底部（播放条之上），显示选中数
// 和三个动作。它是页面级的一条带，不遮内容（内容留出底部空间）。

import { Text, View } from "nsh";
import { Button } from "nsh/base";
import { ActionButton, focusRing } from "../ui/controls";
import { Icon } from "../ui/icons";
import { enter } from "../ui/motion";
import { addToQueue, endSelection, selectionCount, selectionPaths, setSelected } from "../../state/queue";
import { openAddToPlaylist } from "../library/playlist-dialogs";
import { toggleFavourite, isFavourite } from "../../state/user-catalog";
import { notify } from "../../state/feedback";

/** The bar itself: shown while a selection is open. `allPaths` is what 全选
 * selects (the page's rows); `bottom` clears the player bar. */
export function SelectionBar(props: { allPaths: string[]; bottom: number }) {
  const count = () => selectionCount();
  const all = () => props.allPaths.length > 0 && count() === props.allPaths.length;
  const rise = enter("fadeInUp");
  return (
    <View
      class="absolute left-3 right-3 flex-row items-center gap-2 rounded-(--radius-panel) border border-border bg-popover px-3 py-2 shadow-xl"
      style={{ bottom: props.bottom }}
      accessibilityLabel="批量操作"
    >
      <View style={rise} class="flex-row items-center gap-2">
        <Button
          accessibilityLabel={all() ? "取消全选" : "全选"}
          onPress={() => (all() ? setSelected([]) : setSelected(props.allPaths))}
          class={`h-7 flex-row items-center gap-1.5 rounded-(--radius) px-2 transition duration-150 ease-out hover:bg-muted ${focusRing}`}
        >
          <View
            class={`size-4 items-center justify-center rounded-sm border ${all() ? "border-primary bg-primary" : "border-input"}`}
          >
            <If when={all()}>
              <Icon name="check" size={11} color="primary-foreground" />
            </If>
          </View>
          <Text class="text-xs font-semibold text-foreground">全选</Text>
        </Button>
        <Text class="text-xs text-muted-foreground">已选 {count()} 首</Text>
      </View>
      <View class="flex-1" />
      <Button
        accessibilityLabel="加入待播"
        disabled={count() === 0}
        onPress={() => addToQueue(selectionPaths())}
        class={`h-7 flex-row items-center gap-1.5 rounded-(--radius) px-2.5 transition duration-150 ease-out hover:bg-muted active:opacity-60 disabled:opacity-40 ${focusRing}`}
      >
        <Icon name="plus" size={14} color="muted-foreground" />
        <Text class="text-xs font-semibold text-foreground">加入待播</Text>
      </Button>
      <Button
        accessibilityLabel="批量收藏"
        disabled={count() === 0}
        onPress={() => {
          const paths = selectionPaths();
          const allLiked = paths.every((p) => isFavourite(p));
          for (const path of paths) {
            if (isFavourite(path) === allLiked) toggleFavourite(path);
          }
          notify(allLiked ? `已取消收藏 ${paths.length} 首` : `已收藏 ${paths.length} 首`);
        }}
        class={`h-7 flex-row items-center gap-1.5 rounded-(--radius) px-2.5 transition duration-150 ease-out hover:bg-muted active:opacity-60 disabled:opacity-40 ${focusRing}`}
      >
        <Icon name="heart" size={14} color="muted-foreground" />
        <Text class="text-xs font-semibold text-foreground">收藏</Text>
      </Button>
      <Button
        accessibilityLabel="批量添加到曲单"
        disabled={count() === 0}
        onPress={() => {
          const paths = selectionPaths();
          if (paths.length === 1) openAddToPlaylist(paths[0]);
          else openAddToPlaylist(paths[0], paths);
        }}
        class={`h-7 flex-row items-center gap-1.5 rounded-(--radius) px-2.5 transition duration-150 ease-out hover:bg-muted active:opacity-60 disabled:opacity-40 ${focusRing}`}
      >
        <Icon name="folderPlus" size={14} color="muted-foreground" />
        <Text class="text-xs font-semibold text-foreground">曲单</Text>
      </Button>
      <View class="h-5 w-px bg-border" />
      <ActionButton label="完成" onPress={() => endSelection()} />
    </View>
  );
}
