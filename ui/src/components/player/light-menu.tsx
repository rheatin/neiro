// The music light's menu（NP 右上的 ✦）：Random 在前、一个分隔线，然后是
// 效果编号行，正在用的一行 checked。选择走 `state/visual`（持久化）。

import { signal, Text, View } from "nsh";
import { Button, Menu, MenuItem, MenuSeparator } from "nsh/base";
import { lightEffect, lightName, lightRandom, setLight } from "../../state/visual";
import { focusRing, Hint, IconButton } from "../ui/controls";
import { EFFECT_NAMES, TOOLBAR_EFFECTS } from "../ui/toolbar-fx";
import { Icon } from "../ui/icons";

export function LightMenu() {
  const open = signal(false);
  const lowered = () => lightEffect();
  return (
    <Menu
      open={open()}
      onOpenChange={(e) => open.set(e.open)}
      placement="bottom"
      align="end"
      offset={6}
      popupClass="w-52 rounded-(--radius-card) border border-border bg-popover p-1 shadow-lg"
      trigger={
        <Button
          accessibilityLabel={lightName()}
          selected={open()}
          tooltip={open() ? undefined : <Hint label={`Light: ${lightName()}`} />}
          class={`group relative items-center justify-center rounded-full ${focusRing}`}
          style={{ width: 38, height: 38 }}
        >
          <View
            class={`absolute inset-0 rounded-full bg-foreground/10 opacity-0 scale-50 transition duration-150 ease-out group-hover:scale-100 group-hover:opacity-100 group-active:scale-90 motion-reduce:transition-none ${
              open() ? "opacity-100 scale-100" : ""
            }`}
          />
          <View class="opacity-70 transition duration-150 ease-out group-hover:opacity-100 motion-reduce:transition-none">
            <Icon name="sparkles" size={18} color="foreground" />
          </View>
        </Button>
      }
    >
      <MenuItem
        role="menuitemradio"
        checked={lightRandom()}
        onSelect={() => setLight("random")}
        class="group h-8 flex-row items-center gap-2.5 rounded-(--radius) px-2.5 hover:bg-muted focus:bg-muted active:bg-muted"
      >
        <Text class="line-clamp-1 flex-1 text-sm text-popover-foreground">Random</Text>
        <If when={lightRandom()}>
          <Icon name="check" size={12} color="foreground" />
        </If>
      </MenuItem>
      <MenuSeparator class="-mx-1 my-1 h-px bg-border" />
      <Each of={TOOLBAR_EFFECTS}>
        {(effect) => (
          <MenuItem
            role="menuitemradio"
            checked={lightEffect() === effect()}
            onSelect={() => setLight(effect())}
            class="group h-8 flex-row items-center gap-2.5 rounded-(--radius) px-2.5 hover:bg-muted focus:bg-muted active:bg-muted"
          >
            <Text class="font-mono text-xs tabular-nums text-muted-foreground">
              {String(TOOLBAR_EFFECTS.indexOf(effect()) + 1).padStart(2, "0")}
            </Text>
            <Text class="line-clamp-1 flex-1 text-sm text-popover-foreground">{EFFECT_NAMES[effect()]}</Text>
            <If when={lightEffect() === effect()}>
              <Icon name="check" size={12} color="foreground" />
            </If>
          </MenuItem>
        )}
      </Each>
      <If when={lightRandom()}>
        <Text class="px-2.5 pb-1 text-[10px] text-muted-foreground">{lightName()}</Text>
      </If>
    </Menu>
  );
}
