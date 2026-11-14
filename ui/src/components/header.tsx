// The header's controls, in the title bar on the desktops: the sidebar's
// toggle, Back and Forward at the left; the search field at the right.
// A page's header (where the title bar has none — never on the native
// desktop, only the one honest notice page) would come from `PageHeader`.

import { createMotion, signal, token, untracked, View } from "nsh";
import { Input } from "nsh/base";
import { open, back, canBack, canForward, currentRoute, forward, SEARCH } from "../nav";
import { searchQuery } from "../state/catalog";
import { sidebarCollapsed, toggleSidebar } from "../state/overlays";
import { Icon } from "./ui/icons";
import { IconButton } from "./ui/controls";
import { DISTANCE, DURATION, EASING } from "./ui/tokens";

/** The desktop's search, in the header's top-right corner: a Search button
 * that opens into a field. Return searches (opens Search with what was
 * typed); on the search page the field stays open and the results follow
 * the typing. Leaving it empty (it loses the focus, or Escape) folds it
 * back. */
export function SearchField() {
  const onSearch = () => currentRoute().name === "search";
  const expanded = signal(onSearch() && searchQuery().trim() !== "");
  return (
    <If when={expanded() || onSearch()} else={<IconButton name="search" label="搜索" hint onPress={() => expanded.set(true)} />}>
      <SearchBox
        onSearch={onSearch()}
        onDone={() => expanded.set(false)}
      />
    </If>
  );
}

/** The opened field: it grows out from the right, focused. */
function SearchBox(props: { onSearch: boolean; onDone: () => void }) {
  const draft = signal(untracked(() => searchQuery()));
  const grow = untracked(() =>
    createMotion({
      enter: {
        keyframes: { opacity: [0, 1], scaleX: [0.7, 1], translateX: [DISTANCE.large, 0] },
        duration: DURATION.base,
        easing: EASING.out,
      },
    }).style,
  );
  const submit = (text: string) => {
    const term = text.trim();
    if (!term) return;
    searchQuery.set(text);
    if (!props.onSearch) open(SEARCH);
  };
  return (
    <View
      class="h-8 w-72 flex-row items-center gap-2 rounded-full border border-secondary bg-secondary px-3.5 transition duration-150 ease-out focus-within:border-foreground/40 motion-reduce:transition-none"
      style={grow}
    >
      <Icon name="search" size={16} color="muted-foreground" />
      <Input
        value={props.onSearch ? searchQuery() : draft()}
        onChange={({ value }) => (props.onSearch ? searchQuery.set(value) : draft.set(value))}
        onSubmit={({ text }) => submit(text)}
        onBlur={() => {
          if (!(props.onSearch ? searchQuery() : draft()).trim()) props.onDone();
        }}
        onKeyPress={({ key }) => {
          if (key === "Escape" && !props.onSearch) props.onDone();
        }}
        placeholder="搜索曲目、专辑或歌手"
        accessibilityLabel="搜索曲库"
        returnKeyType="search"
        submitBehavior="submit"
        autoFocus
        class="flex-1 text-sm text-foreground"
        style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
      />
    </View>
  );
}

/** Getting around: the sidebar's toggle, Back and Forward. */
export function NavControls() {
  return (
    <>
      {/* Folds the sidebar away entirely, or brings it back. In a rail
          window it is pointless (railSidebar keeps it at any width), but
          the user can still hide it. */}
      <IconButton
        name="sidebar"
        label={sidebarCollapsed() ? "展开侧栏" : "收起侧栏"}
        hint
        onPress={toggleSidebar}
      />
      <IconButton name="chevronLeft" label="后退" disabled={!canBack()} onPress={back} />
      <IconButton name="chevronRight" label="前进" disabled={!canForward()} onPress={forward} />
    </>
  );
}

/** The trailing controls: the search field. */
export function TrailingControls() {
  return <SearchField />;
}
