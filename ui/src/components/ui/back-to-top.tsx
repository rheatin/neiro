// 回到顶部：页面滚离顶部后，在右下角（播放条之上）浮出一个圆钮；点一下
// 动画回到顶部。页面自己决定它出不出（`PageScroll.showTop`）。

import { View } from "nsh";
import { Button } from "nsh/base";
import { barSize } from "../layout";
import { focusRing } from "./controls";
import { Icon } from "./icons";
import { enter } from "./motion";

export function BackToTop(props: { onPress: () => void }) {
  return (
    <View style={{ position: "absolute", right: 20, bottom: barSize() === "stacked" ? 128 : 96 }}>
      <View style={enter("fadeInUp", 120)}>
        <Button
          testID="back-to-top"
          accessibilityLabel="回到顶部"
          onPress={() => props.onPress()}
          class={`group items-center justify-center rounded-full border border-border bg-card shadow-lg transition duration-150 ease-out hover:bg-muted active:scale-95 motion-reduce:transition-none motion-reduce:active:scale-100 ${focusRing}`}
          style={{ width: 40, height: 40 }}
        >
          <Icon name="arrowUp" size={18} color="foreground" />
        </Button>
      </View>
    </View>
  );
}
