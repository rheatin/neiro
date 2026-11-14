// 专辑页 / 歌手页: a pushed page from a card, over the header + its track
// table. It needs the whole index (the album's or artist's share of it),
// so it asks the store for that on entry and waits for it; the header's
// cover is the album's (or artist's) first track's embedded picture.

import { effect, List, Text, untracked, View } from "nsh";
import { clock, trackTitle, type Track } from "../backend/types";
import {
  albumCoverTrack,
  artistCoverTrack,
  ensureTracks,
  tracksOfAlbum,
  tracksOfArtist,
  tracksState,
} from "../state/catalog";
import { isCurrentTrack, isPlaying, playQueue, playTrack, toggle } from "../state/player";
import { AppPage } from "../components/page";
import { barSize, pageEnd, wideTracks } from "../components/layout";
import { SelectionBar } from "../components/library/selection-bar";
import { selectionActive } from "../state/queue";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { CollectionSkeleton, EmptyState } from "../components/ui/feedback";
import { Cover } from "../components/ui/cover";
import { PlayButton } from "../components/ui/controls";
import { createListWave, enter } from "../components/ui/motion";
import { currentRoute } from "../nav";

/** The album's page. */
export function AlbumPage(props: { album: string; albumArtist: string }) {
  return (
    <DetailPage
      title={props.album}
      kind="专辑"
      coverPath={albumCoverTrack(props.album, props.albumArtist)}
      rows={() => tracksOfAlbum(props.album, props.albumArtist)}
    />
  );
}

/** The artist's page. */
export function ArtistPage(props: { artist: string }) {
  return (
    <DetailPage
      title={props.artist}
      kind="歌手"
      coverPath={artistCoverTrack(props.artist)}
      rows={() => tracksOfArtist(props.artist)}
    />
  );
}

function DetailPage(props: { title: string; kind: "专辑" | "歌手"; coverPath: string; rows: () => Track[] }) {
  // The shared index: it is the page's data, both its table's rows and its
  // header's quality facts.
  effect(() => {
    untracked(() => ensureTracks());
  });
  const wave = createListWave({
    isTop: () => {
      const route = currentRoute();
      return route.name === "album" || route.name === "artist";
    },
  });
  const complete = () => tracksState() === "complete";
  const count = () => props.rows().length;
  const first = () => props.rows()[0];
  const playingHere = () => isCurrentTrack(first()?.path ?? "") && isPlaying();
  const totalTime = () => props.rows().reduce((sum, t) => sum + t.durationMs, 0);
  const showAlbum = wideTracks();

  return (
    <AppPage title={props.title} testID={`page-${props.kind}`}>
      <List
        each={complete() ? props.rows() : []}
        keyExtractor={(t) => t.path}
        estimatedItemSize={48}
        header={() =>
          complete() && count() === 0 ? undefined : (
            <View class="pt-2">
              <Header
                kind={props.kind}
                title={props.title}
                coverPath={props.coverPath}
                rows={props.rows}
                firstTrack={first}
                count={count()}
                totalTime={totalTime()}
                playing={playingHere()}
              />
              <TrackHeader showAlbum={showAlbum} />
            </View>
          )
        }
        footer={() => <View class={pageEnd()} />}
        empty={() => (
          <If when={complete()} else={<CollectionSkeleton />}>
            <EmptyState title="这里还没有曲目" detail="曲库里没有这一页的文件。" />
          </If>
        )}
        class="flex-1"
      >
        {(track, index) => <TrackRow track={track()} number={index()} showAlbum={showAlbum} wave={wave} />}
      </List>
      <If when={selectionActive()}>
        <SelectionBar
          allPaths={props.rows().map((t) => t.path)}
          bottom={barSize() === "stacked" ? 128 : 96}
        />
      </If>
    </AppPage>
  );
}

/** A header's layers come in one after another: its cover zooms, then the
 * words, then the play disc. */
function Header(props: {
  kind: string;
  title: string;
  coverPath: string;
  rows: () => Track[];
  firstTrack: () => Track | undefined;
  count: number;
  totalTime: number;
  playing: boolean;
}) {
  const coverIn = untracked(() => enter("zoomIn"));
  const kindIn = enter("fadeInUp", 70);
  const nameIn = enter("fadeInUp", 150);
  const restIn = enter("fadeInUp", 250);
  const playIn = enter("zoomIn");
  return (
    <View class="flex-row items-end gap-4 pl-[30px] pr-6 pb-4">
      <View style={coverIn}>
        <Cover path={props.coverPath} size={96} round={props.kind === "歌手"} class="shadow-xl" />
      </View>
      <View class="min-w-0 flex-1 gap-1 self-stretch justify-center">
        <View style={kindIn}>
          <Text class="text-xs font-bold uppercase tracking-wider text-muted-foreground">{props.kind}</Text>
        </View>
        <View style={nameIn}>
          <Text role="header" class="line-clamp-1 text-3xl font-bold text-foreground">
            {props.title || "—"}
          </Text>
        </View>
        <View class="gap-1" style={restIn}>
          <Text class="text-xs text-muted-foreground">
            {props.count} 首{props.totalTime > 0 ? ` · ${clock(props.totalTime)}` : ""}
          </Text>
        </View>
      </View>
      <View class="shrink-0" style={playIn}>
        <PlayButton
          playing={props.playing}
          size={44}
          disabled={!props.firstTrack()}
          onPress={() => {
            const track = props.firstTrack();
            if (!track) return;
            if (isCurrentTrack(track.path)) toggle();
            else {
              const index = props.rows().findIndex((t) => t.path === track.path);
              if (index < 0) playTrack(track);
              else playQueue(props.rows(), index);
            }
          }}
        />
      </View>
    </View>
  );
}
