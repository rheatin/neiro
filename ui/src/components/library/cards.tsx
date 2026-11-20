// Cards: albums and artists as tiles, in rows that scroll sideways or in a
// filling grid. A card in spotish's proportions and motion; it plays the
// item's first track (the one-track queue the engine builds) on its gold
// disc, and opens the item's page on a tap.

import { List, signal, Text, untracked, View } from "nsh";
import { Button } from "nsh/base";
import { listItemEntrance, type ListWave, WAVE } from "../ui/motion";
import { Cover } from "../ui/cover";
import { focusRing, PlayButton, SectionTitle } from "../ui/controls";
import { openContextMenu, type MenuAction } from "../ui/context-menu";
import { addToQueue } from "../../state/queue";
import { hasTrack, isCurrentTrack, isPlaying, playQueue, playTrack } from "../../state/player";
import { ready } from "../../state/engine";
import { albumCoverTrack, artistCoverTrack, itemTracks, openItem, trackForPath, type LibraryItem } from "../../state/catalog";

import type { Track } from "../../backend/types";

/** A card's width where nothing sizes it (a row), pt. */
export const CARD_WIDTH = 144;

/** A card row's height, from the card's own geometry: 10pt padding either
 * side, a square cover (the width less that padding), an 8pt gap, a title
 * line, a 4pt gap and two subtitle lines. A magic number clipped the card
 * whose subtitle used its second line. */
const rowHeight = (card: number) => 20 + (card - 20) + 8 + 20 + 4 + 32 + 8;

// The play button over a card's cover, in golden proportion: the cover
// (the card less its 10pt padding each side) is φ³ times the button, and
// the button φ² times its inset from the cover's corner.
const PHI = (1 + Math.sqrt(5)) / 2;
const playSize = (card: number) => Math.round((card - 2 * 10) / PHI ** 3);
const playInset = (card: number) => Math.round(playSize(card) / PHI ** 2);

/** The cover's file for an item (whose embedded picture the cover shows).
 * The index is what the pages read them from, so a card rows in a grid of
 * albums reads there too. */
export function itemCoverTrack(item: LibraryItem): string {
  if (item.kind === "album") return albumCoverTrack(item.album, item.albumArtist);
  return artistCoverTrack(item.artist);
}

/** The item's first track (its play button's target), while the index is
 * loaded. */
export function itemFirstTrack(item: LibraryItem): Track | undefined {
  const path = itemCoverTrack(item);
  return path ? trackForPath(path) : undefined;
}

function cardKey(i: LibraryItem): string {
  return i.kind === "album" ? `album:${i.album}\u0000${i.albumArtist}` : `artist:${i.artist}`;
}

/** One card: cover, name, what it is; the gold disc on hover. */
export function Card(props: {
  item: LibraryItem;
  index?: number;
  wave?: ListWave;
  /** About how wide it is drawn, pt: what its play button is sized by. */
  width?: number;
  /** Stretched by its container (a grid's cell) rather than `width` wide. */
  fill?: boolean;
}) {
  const width = () => props.width ?? CARD_WIDTH;
  // Made once, here: inside the Button's style it would be made (and
  // played) again whenever the Button's props re-run. Its cover flips open
  // just after the card starts to rise.
  const { style: rise, cover: coverIn } = untracked(() =>
    listItemEntrance({
      wave: props.wave,
      key: () => cardKey(props.item),
      index: () => props.index ?? 0,
      axis: "translateY",
      turns: WAVE.cards,
    }),
  );
  const first = () => itemFirstTrack(props.item);
  const rows = () => itemTracks(props.item);
  const firstIndex = () => rows().findIndex((t) => t.path === first()?.path);
  const playing = () => isCurrentTrack(first()?.path ?? "") && isPlaying();
  // 专辑/歌手的整张加入待播：卡片右键，和曲目行同一套菜单。
  const actions = (): MenuAction[] => {
    const paths = rows().map((t) => t.path);
    return [
      { label: `播放 ${props.item.name}`, onSelect: () => playQueue(rows(), 0) },
      { label: `加入待播（${paths.length} 首）`, onSelect: () => addToQueue(paths), separated: true },
      { label: "打开", onSelect: () => openItem(props.item), separated: true },
    ];
  };
  return (
    <View
      onContextMenu={(e) => openContextMenu(e.pagePosition, actions())}
      style={props.fill ? { flex: 1 } : undefined}
    >
    <Button
      accessibilityLabel={`${props.item.name}, ${props.item.subtitle}`}
      onPress={() => openItem(props.item)}
      style={props.fill ? rise : { ...rise, width: width() }}
      class={`group shrink-0 gap-1.5 rounded-(--radius-card) ${
        "p-2.5"
      } transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] motion-reduce:transition-none motion-reduce:active:scale-100 ${focusRing}`}
    >
      <View class="relative">
        {/* Layered on hover, and steady: the card doesn't move; a wash comes
            in, the cover grows a hair (1%), the play disc rises over it. */}
        <View class="transition duration-300 ease-out group-hover:scale-[1.01] motion-reduce:transition-none motion-reduce:group-hover:scale-100">
          <View style={coverIn}>
            <Cover
              path={itemCoverTrack(props.item)}
              round={props.item.kind === "artist"}
              fallback={props.item.kind === "artist" ? "user" : "disc"}
              class="shadow-md"
            />
          </View>
        </View>
        <View
          style={{ right: playInset(width()), bottom: playInset(width()) }}
          class={`absolute transition duration-200 ease-out group-hover:translate-y-0 group-hover:scale-100 group-hover:opacity-100 motion-reduce:transition-none ${
            playing() ? "translate-y-0 scale-100 opacity-100" : "translate-y-3 scale-75 opacity-0"
          }`}
        >
          <PlayButton
            playing={playing()}
            size={playSize(width())}
            disabled={!ready() || !hasTrack() || !first()}
            onPress={() => {
              if (firstIndex() < 0) return;
              playQueue(rows(), firstIndex());
            }}
          />
        </View>
      </View>
      <View class="gap-0.5">
        <Text class="line-clamp-1 text-sm font-medium text-foreground">{props.item.name}</Text>
        <Text class="line-clamp-2 text-xs text-muted-foreground">{props.item.subtitle}</Text>
      </View>
    </Button>
    </View>
  );
}

/** A titled row of cards that scrolls sideways. With the page's `wave`,
 * its cards come in as a wave of their own, `delay` ms after the page's,
 * each time the page is entered; cards scrolled to come in on their own. */
export function CardRow(props: { title?: string; items: LibraryItem[]; class?: string; wave?: ListWave; delay?: number }) {
  const wave = untracked(() => props.wave?.child(props.delay ?? 0));
  // Scrolling stops with a card's start at the gutter (cards are 144pt
  // apart); the row runs out to the page's edges and insets its cards by
  // the page's gutter, so a snapped card stops 12pt in. The title belongs
  // to the caller when it wraps cards in a section that already names it.
  return (
    <View class={`gap-2 ${props.class ?? ""}`}>
      <If when={!!props.title}>
        <SectionTitle>{props.title}</SectionTitle>
      </If>
      <List
        horizontal
        each={props.items}
        keyExtractor={cardKey}
        estimatedItemSize={144}
        snapToInterval={144}
        snapToAlignment="start"
        decelerationRate="fast"
        scrollbar={false}
        class="-mx-3" style={{ height: rowHeight(CARD_WIDTH) }}
        contentClass="px-3 pt-1"
      >
        {(item, index) => <Card item={item()} index={index()} wave={wave} />}
      </List>
    </View>
  );
}

/** Cards that wrap into a filling grid (a page's or an artist's list).
 * Virtualized: it lays out at its full length in the page's `Scroll`
 * (`viewport="parent"`) and only the rows near the viewport exist, so a
 * library of thousands of albums opens as fast as one of ten. */
export function CardGrid(props: { items: LibraryItem[]; class?: string; wave?: ListWave; delay?: number }) {
  const wave = untracked(() => props.wave?.child(props.delay ?? 0));
  const minWidth = CARD_WIDTH;
  // As many columns as fit at a card's width, widened to share the row out
  // so no room is left at its end. The width one card gets is worked out
  // for its play button, and the row's height from that card.
  const room = signal(0);
  const columns = () => {
    const w = room();
    return w > 0 ? Math.max(1, Math.floor(w / minWidth)) : 4;
  };
  const cardWidth = () => {
    const w = room();
    return w > 0 ? w / columns() : minWidth;
  };
  return (
    <View class={props.class} onLayout={(e) => room.set(e.bounds.size.width)}>
      <List
        viewport="parent"
        numColumns={columns()}
        each={props.items}
        keyExtractor={cardKey}
        estimatedItemSize={rowHeight(cardWidth())}
      >
        {(item, index) => <Card item={item()} index={index()} wave={wave} width={cardWidth()} fill />}
      </List>
    </View>
  );
}
