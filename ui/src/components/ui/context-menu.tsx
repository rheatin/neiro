// 应用自己的菜单：原生 `Menu.popup` 画的是系统菜单（系统字体、系统圆角、
// 系统配色），和这个主题不是一套。这里用应用自己的表面画同一个菜单
// （popover 底、card 圆角、border 细边、应用的 hover/focus 状态）。
//
// 两条入口，一份内容：
// - 右键：`openContextMenu(at, items)` 记下指针位置，`ContextMenuHost`
//   在根视图上把它画出来（同一时刻只有一个）。
// - "⋯" 按钮：`MenuSurface` 直接作为 `Popover` 的内容，贴着触发按钮。

import { Overlay, signal, Text, View, windowSize } from "nsh";
import { Button } from "nsh/base";
import { focusRing } from "./controls";
import { enter } from "./motion";

/** One row of the app's menu. */
export interface MenuAction {
  label: string;
  /** Called when chosen; the menu closes first. */
  onSelect: () => void;
  /** Drawn dimmed and takes no presses. */
  disabled?: boolean;
  /** A separator drawn before this item. */
  separated?: boolean;
}

/** The items themselves: one surface, used by the right-click menu and by a
 * menu a button opens. */
export function MenuSurface(props: { items: MenuAction[]; label?: string }) {
  return (
    <View
      class="min-w-44 rounded-(--radius-card) border border-border bg-popover p-1 shadow-lg"
      accessibilityLabel={props.label ?? "菜单"}
    >
      <Each of={props.items} key={(item) => item.label}>
        {(item) => (
          <>
            <If when={item().separated}>
              <View class="my-1 h-px bg-border" />
            </If>
            <Button
              disabled={item().disabled}
              onPress={() => {
                const action = item().onSelect;
                closeContextMenu();
                action();
              }}
              class={`h-8 flex-row items-center rounded-(--radius) px-2.5 transition duration-100 ease-out hover:bg-muted focus:bg-muted active:bg-muted disabled:opacity-40 ${focusRing}`}
            >
              <Text class="text-sm text-popover-foreground">{item().label}</Text>
            </Button>
          </>
        )}
      </Each>
    </View>
  );
}

interface OpenMenu {
  at: { x: number; y: number };
  items: MenuAction[];
  /** Which root draws it: the app surface, or a sheet's own root (a Sheet
   * covers everything on the surface, so a menu asked for inside one must be
   * drawn inside it). */
  layer: "root" | "sheet";
}

const open = signal<OpenMenu | null>(null);

/** Shows the menu at `at` (the root view's coordinates, as
 * `onContextMenu` reports them), replacing whatever was open. `layer` picks
 * the root that draws it: a right click inside a Sheet passes `"sheet"`,
 * because the surface's overlay sits under the Sheet and would never show. */
export function openContextMenu(
  at: { x: number; y: number },
  items: MenuAction[],
  layer: "root" | "sheet" = "root",
): void {
  if (items.length === 0) return;
  open.set({ at, items, layer });
}

export function closeContextMenu(): void {
  open.set(null);
}

/** The right-click menu, mounted once by the app root. */
/** The menu's own size, in points: its width and a row's height. Used to
 * keep it inside the window — a menu opened near the bottom edge would
 * otherwise be clipped to its first row. */
const MENU_WIDTH = 184;
const ROW = 32;
const PAD = 8;

function menuHeight(items: MenuAction[]): number {
  return PAD + items.reduce((sum, item) => sum + ROW + (item.separated ? 9 : 0), 0);
}

/** Where the menu goes: at the pointer, pulled back inside the window. */
function placeAt(at: { x: number; y: number }, items: MenuAction[]): { x: number; y: number } {
  const size = windowSize();
  const h = menuHeight(items);
  const x = Math.max(4, Math.min(at.x, size.width - MENU_WIDTH - 4));
  const y = Math.max(4, Math.min(at.y, size.height - h - 4));
  return { x, y };
}

export function ContextMenuHost(props: { layer?: "root" | "sheet" } = {}) {
  const menu = () => {
    const current = open();
    return current !== null && current.layer === (props.layer ?? "root") ? current : null;
  };
  const rise = enter("zoomIn");
  return (
    <If when={menu()}>
      {(current) => (
        <Overlay
          style={{
            position: "absolute",
            left: placeAt(current().at, current().items).x,
            top: placeAt(current().at, current().items).y,
          }}
          onPressOutside={() => closeContextMenu()}
          onCancel={() => closeContextMenu()}
        >
          <View style={rise}>
            <MenuSurface items={current().items} />
          </View>
        </Overlay>
      )}
    </If>
  );
}
