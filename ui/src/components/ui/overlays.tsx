// The app's overlay surfaces: `nsh/base`'s `Dialog` and `Sheet` with the
// app's look. They bring focus, Escape and back with them; the caller owns
// `open` and follows `onOpenChange`.

import { createMotion, type JSX, type MotionSpec, View } from "nsh";
import { titleBarArea } from "./title-bar";
import { Dialog, Sheet } from "nsh/base";
import { enter } from "./motion";
import { ContextMenuHost } from "./context-menu";

/** A dialog's entrance: up a little from slightly smaller, settling
 * without overshoot, as the scrim fades. */
const RISE: MotionSpec = {
  keyframes: { opacity: [0, 1], scale: [0.97, 1], translateY: [10, 0] },
  duration: 240,
  easing: "easeOut",
};

/** The scrim behind a modal surface. Dark enough that the page behind reads
 * as a shape rather than as text the reader tries to parse through it. */
const BACKDROP = "bg-shell/92";

/** A centered dialog. Escape and a press on the scrim close it;
 * `closeOnEscape` and `closeOnBackdropPress` keep them out when a flow
 * forbids it. */
export function AppDialog(props: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  closeOnEscape?: boolean;
  closeOnBackdropPress?: boolean;
  maxWidthClass?: string;
  testID?: string;
  children?: JSX.Element;
}) {
  const maxW = props.maxWidthClass ?? "max-w-md";
  return (
    <Dialog
      open={props.open}
      testID={props.testID}
      animation="fade"
      closeOnEscape={props.closeOnEscape ?? true}
      closeOnBackdropPress={props.closeOnBackdropPress ?? true}
      onOpenChange={(e) => props.onOpenChange(e.open)}
      class={`m-auto w-full ${maxW} overflow-hidden rounded-(--radius-panel) bg-popover shadow-2xl`}
      backdropClass={BACKDROP}
    >
      {/* The panel pops in as the scrim fades. */}
      <View style={createMotion({ enter: RISE }).style}>{props.children}</View>
    </Dialog>
  );
}

/** A panel at an edge: a left drawer or a full-screen one (Now Playing). */
export function AppSheet(props: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  side: "left" | "right" | "bottom";
  /** Covers the whole screen (Now Playing). */
  full?: boolean;
  closeOnSwipe?: boolean;
  testID?: string;
  /** The content's entrance (the panel itself slides in as NSH's `Sheet`
   * does); `fadeInUp` by default. */
  entrance?: MotionSpec;
  /** 侧宽覆盖（points；只有左/右用得上，例如输出抽屉的两列蛇形）。 */
  popupWidth?: number;
  children?: JSX.Element;
}) {
  const panel = () => {
    if (props.full) return "h-full w-full bg-background";
    if (props.side === "left") return `h-full ${props.popupWidth ? `w-[${props.popupWidth}px]` : "w-80"} max-w-full bg-background`;
    if (props.side === "right") return `h-full ${props.popupWidth ? `w-[${props.popupWidth}px]` : "w-85"} max-w-full bg-background`;
    return "max-h-full w-full rounded-t-(--radius-panel) bg-popover pb-[env(safe-area-inset-bottom)]";
  };
  return (
    <Sheet
      open={props.open}
      side={props.side}
      testID={props.testID}
      closeOnSwipe={props.closeOnSwipe ?? false}
      onOpenChange={(e) => props.onOpenChange(e.open)}
      popupClass={panel()}
      backdropClass={props.full ? "bg-background" : BACKDROP}
    >
      {/* The content rises in behind the sliding panel. 侧缘的面板在引擎的
          布局里从内容取高，所以这张 wrapper 自己占据面板整高：没有它内容
          的 frame 高就是 0，抽屉的正文挂不上去（真 bug，无头测试截图取
          证过）。

          侧缘面板铺到窗口边缘，会盖住应用自己画的标题栏和窗口按钮（真机
          截图里关闭按钮正好压在系统 X 上）：留出标题栏的高度和窗口按钮
          的宽度，内容不进那两条带。 */}
      <View
        class={props.full ? "flex-1" : "h-full"}
        style={{
          ...enter(props.entrance ?? "fadeInUp", 90),
          paddingTop: props.full ? 0 : titleBarArea().height,
        }}
      >
        {props.children}
        {/* A Sheet covers everything on the app surface, so the surface's
            right-click menu would sit under it. A full sheet draws its own
            menu host, whose overlay's root is the sheet. */}
        <If when={!!props.full}>
          <ContextMenuHost layer="sheet" />
        </If>
      </View>
    </Sheet>
  );
}
