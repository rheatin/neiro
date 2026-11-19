// 均衡器页：就是 02B 的 32 段机架本身。
//
// 抽屉里 EQ 节点的「设置」进这里；抽屉里的 EQ 段只切预设。一个编辑器只有
// 一处，两个入口指向同一个组件（`surface/rack-eq`），所以两边不会漂移。

import { Scroll, View } from "nsh";
import { Button } from "nsh/base";
import { AppPage } from "../components/page";
import { rememberScroll } from "../state/page-scroll";
import { pageEnd } from "../components/layout";
import { RackEq } from "../components/player/surface/rack-eq";
import { HARDWARE } from "../components/ui/hardware";
import { Icon } from "../components/ui/icons";
import { focusRing } from "../components/ui/controls";
import { back } from "../nav";
import { eqCameFromOutput, setEqOpenedFromOutput, setOutputOpen } from "../state/overlays";
import { Text } from "nsh";

export function EqPage() {
  const scroll = rememberScroll("eq");
  return (
    <AppPage title="均衡器" testID="page-eq" scroll={scroll}>
      <Scroll class="flex-1" contentClass="px-6 pb-28" contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <View class="flex-row items-center gap-2 pt-2 pb-3">
          <Button
            accessibilityLabel="返回"
            onPress={() => {
              // Came from the console: go back there, not to the page under it.
              if (eqCameFromOutput()) {
                setEqOpenedFromOutput(false);
                setOutputOpen(true);
                back();
                return;
              }
              back();
            }}
            class={`items-center justify-center rounded-(--radius) p-1.5 hover:bg-row-hover active:opacity-60 ${focusRing}`}
          >
            <Icon name="chevronLeft" size={16} color="muted-foreground" />
          </Button>
          <View class="min-w-0 flex-1 gap-0.5">
            <Text class="text-[10px] font-bold uppercase tracking-[0.2em]" style={{ color: HARDWARE.amber }}>
              02B // RACK UNIT A
            </Text>
            <Text role="header" class="text-2xl font-bold text-foreground">
              32 段机架均衡器
            </Text>
          </View>
        </View>
        <RackEq />
        <View class={pageEnd()} />
      </Scroll>
    </AppPage>
  );
}
