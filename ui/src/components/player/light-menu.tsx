// The music light's menu（播放条右侧与 NP 右上的 ✦）：Random 在前、一个分隔线，
// 然后是 14 个效果编号行（超出高度可滚动，打开时定位在当前效果），正在用的
// 一行 checked（Random 模式下被选中的那项显示 shuffle 标记）。选择走 `state/visual`（持久化）。

import { Scroll, signal, Text, View } from "nsh";
import { Button, Menu, MenuItem, MenuSeparator } from "nsh/base";
import { lightEffect, lightName, lightRandom, setLight } from "../../state/visual";
import { focusRing, Hint, LAYER, menuItem, popupSurface } from "../ui/controls";
import { EFFECT_NAMES, TOOLBAR_EFFECTS } from "../ui/toolbar-fx";
import { Icon, iconMotion } from "../ui/icons";

// A row's height (`menuItem`'s h-8) and the list's (`max-h-72`).
const ROW = 32;
const VIEW = 288;

export function LightMenu(props?: { placement?: "top" | "bottom"; size?: number }) {
  const open = signal(false);
  const hover = iconMotion();
  const placement = () => props?.placement ?? "bottom";
  const size = () => props?.size ?? 18;
  const label = () => `Light: ${lightName()}`;
  // The effects' list: rows of ROW points in a window of VIEW. Where it
  // starts, so the effect in use is in the middle of it.
  const openAt = () => {
    const i = TOOLBAR_EFFECTS.indexOf(lightEffect());
    const most = Math.max(0, TOOLBAR_EFFECTS.length * ROW - VIEW);
    return Math.min(most, Math.max(0, i * ROW - (VIEW - ROW) / 2));
  };
  return (
    <Menu
      open={open()}
      onOpenChange={(e) => open.set(e.open)}
      placement={placement()}
      align="end"
      offset={6}
      popupClass={`w-52 ${popupSurface}`}
      trigger={
        <Button
          accessibilityLabel={label()}
          selected={open()}
          tooltip={open() ? undefined : <Hint label={label()} />}
          class={`group relative items-center justify-center rounded-full ${focusRing}`}
          style={{ width: size() + 16, height: size() + 16 }}
        >
          <View
            class={`absolute inset-0 rounded-full bg-foreground/10 opacity-0 scale-50 ${LAYER} group-hover:scale-100 group-hover:opacity-100 group-selected:scale-100 group-selected:opacity-100 group-active:scale-90 ${
              open() ? "opacity-100 scale-100" : ""
            }`}
          />
          <View
            onMouseEnter={() => hover.play("sparkles")}
            class={`${LAYER} opacity-70 group-hover:opacity-100 group-selected:opacity-100 group-active:scale-90`}
          >
            <View style={hover.style}>
              <Icon name="sparkles" t={hover.t()} size={size()} color="foreground" />
            </View>
          </View>
        </Button>
      }
    >
      <MenuItem
        role="menuitemradio"
        checked={lightRandom()}
        onSelect={() => setLight("random")}
        class={`group ${menuItem} gap-2.5`}
        indicatorClass="hidden"
      >
        <View class="w-[15px] items-start">
          <Icon name="shuffle" size={12} color="muted-foreground" />
        </View>
        <Text class="line-clamp-1 flex-1 text-sm text-popover-foreground group-checked:font-semibold">Random</Text>
        <If when={lightRandom()}>
          <Icon name="check" size={12} color="foreground" />
        </If>
      </MenuItem>
      <MenuSeparator class="-mx-1 my-1 h-px bg-border" />
      <Scroll class="max-h-72" contentOffset={{ y: openAt() }}>
        <Each of={TOOLBAR_EFFECTS} key={(e) => e}>
          {(effect) => (
            <MenuItem
              role="menuitemradio"
              checked={!lightRandom() && lightEffect() === effect()}
              onSelect={() => setLight(effect())}
              class={`group ${menuItem} gap-2.5`}
              indicatorClass="hidden"
            >
              <Text class="font-mono text-xs tabular-nums text-muted-foreground">
                {String(TOOLBAR_EFFECTS.indexOf(effect()) + 1).padStart(2, "0")}
              </Text>
              <Text class="line-clamp-1 flex-1 text-sm text-popover-foreground group-checked:font-semibold">
                {EFFECT_NAMES[effect()]}
              </Text>
              <If when={!lightRandom() && lightEffect() === effect()}>
                <Icon name="check" size={12} color="foreground" />
              </If>
              <If when={lightRandom() && lightEffect() === effect()}>
                <Icon name="shuffle" size={12} color="muted-foreground" />
              </If>
            </MenuItem>
          )}
        </Each>
      </Scroll>
    </Menu>
  );
}

