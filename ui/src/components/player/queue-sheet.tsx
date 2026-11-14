// 待播抽屉：右侧一张面板，内容就是 QueuePanel。AppSheet 带焦点、Escape
// 与返回，这里只管开合。

import { Scroll, Text, View } from "nsh";
import { Button } from "nsh/base";
import { AppSheet } from "../ui/overlays";
import { Icon } from "../ui/icons";
import { focusRing } from "../ui/controls";
import { QueuePanel } from "./queue-panel";
import { queueOpen, setQueueOpen } from "../../state/overlays";

export function QueueSheet() {
  return (
    <AppSheet open={queueOpen()} onOpenChange={setQueueOpen} side="right" testID="queue-sheet" popupWidth={360}>
      <Scroll class="flex-1" contentClass="flex w-full flex-col gap-3 border-l border-border bg-sidebar px-4 pb-4 pt-2">
        <View class="flex-row items-center justify-between">
          <Text role="header" class="text-xl font-bold text-foreground">
            待播
            <Text class="text-xs font-semibold text-muted-foreground"> 播放队列</Text>
          </Text>
          <Button
            accessibilityLabel="关闭待播"
            onPress={() => setQueueOpen(false)}
            class={`items-center justify-center rounded-(--radius) p-1 hover:bg-muted active:opacity-60 ${focusRing}`}
          >
            <Icon name="close" size={16} color="muted-foreground" />
          </Button>
        </View>
        <QueuePanel />
      </Scroll>
    </AppSheet>
  );
}
