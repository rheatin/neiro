// 资料库: the Library tab's root on mobile. The desktop sidebar lists these
// categories directly; a phone's tab bar has four places instead of nine, so
// the categories live one level down, as rows that push the same pages the
// sidebar opens. The route is a root so the tab keeps its own stack: going
// into 专辑 and back returns here, not to whatever tab was last used.

import { Scroll, Text, View } from "nsh";
import { Button } from "nsh/base";
import { AppPage } from "../components/page";
import { pageEnd } from "../components/layout";
import { focusRing } from "../components/ui/controls";
import { Icon, type IconName } from "../components/ui/icons";
import { rememberScroll } from "../state/page-scroll";
import { ALBUMS, ARTISTS, LIKED, PLAYLISTS, TRACKS, open, type Route } from "../nav";
import { albums, artists, visibleTracks } from "../state/catalog";
import { favourites, playlists } from "../state/user-catalog";

/** One category. 44pt tall with its glyph in a 30pt column, the geometry the
 * sidebar's rows use, so the two read as the same list. */
function CategoryRow(props: {
  icon: IconName;
  label: string;
  detail: string;
  route: Route;
}) {
  return (
    <Button
      accessibilityLabel={props.label}
      onPress={() => open(props.route)}
      class={`h-14 flex-row items-center gap-3 rounded-(--radius-inset) px-2 hover:bg-row-hover active:opacity-60 ${focusRing}`}
    >
      <View class="w-8 items-center">
        <Icon name={props.icon} size={20} color="muted-foreground" />
      </View>
      <View class="min-w-0 flex-1">
        <Text class="text-base font-semibold text-foreground">{props.label}</Text>
      </View>
      <Text class="text-xs text-muted-foreground">{props.detail}</Text>
      <Icon name="chevronRight" size={16} color="muted-foreground" />
    </Button>
  );
}

export function LibraryPage() {
  const scroll = rememberScroll("library");
  return (
    <AppPage title="资料库" testID="page-library" scroll={scroll} gear>
      <Scroll class="flex-1" contentClass={pageEnd()} contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <View class="gap-1 px-4 pt-2">
          <CategoryRow icon="disc" label="专辑" detail={`${albums().length} 张`} route={ALBUMS} />
          <CategoryRow icon="user" label="艺术家" detail={`${artists().length} 位`} route={ARTISTS} />
          <CategoryRow icon="music" label="歌曲" detail={`${visibleTracks().length} 首`} route={TRACKS} />
          <CategoryRow icon="heart" label="收藏" detail={`${favourites().length} 首`} route={LIKED} />
          <CategoryRow icon="layers" label="播放列表" detail={`${playlists().length} 个`} route={PLAYLISTS} />
        </View>
      </Scroll>
    </AppPage>
  );
}
