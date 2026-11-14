// 首页: a greeting, then what the library has — 专辑 as cards, 歌手 as
// round cards, and the first tracks that have arrived as a list. Data
// loads when the page shows (the lists load lazily), and asks again when
// a scan changed the index.
//
// 三态: 引擎未连接 never reaches this page (the shell paints that page);
// 空曲库 is the empty states below; a loaded index shows its rows.

import { Image, effect, List, Scroll, Text, untracked, View, windowSize } from "nsh";
import { Icon } from "../components/ui/icons";
import {
  addRoot,
  albumItemOf,
  type LibraryItem,
  albums,
  albumsState,
  artistItemOf,
  artists,
  artistsState,
  dataEpoch,
  ensureAlbums,
  ensureArtists,
  ensureTracks,
  homeAlbums,
  homeArtists,
  homeTracks,
  retryAlbums,
  retryArtists,
  trackForPath,
  visibleTracks,
  tracksState,
  visibleAlbums,
  visibleArtists,
} from "../state/catalog";
import { activeSource, allSources, selectSource } from "../state/sources";
import { openSourceDialog } from "../components/ui/server-dialog";
import {
  activeRemoteServer,
  remoteAlbumGroups,
  remoteArtistGroups,
  remoteLoadingState,
  reconnectServer,
  retryRemote,
  remoteError,
} from "../state/remote";
import { engine } from "../backend/engine";
import { AppPage } from "../components/page";
import { pageEnd, wideTracks } from "../components/layout";
import { CardRow } from "../components/library/cards";
import { RemoteCardsRow } from "../components/library/remote-cards";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { CardsSkeleton, EmptyState, ErrorState, RowsSkeleton } from "../components/ui/feedback";
import { ActionButton, SectionTitle } from "../components/ui/controls";
import { createListWave, enter, waveDelay, type ListWave } from "../components/ui/motion";
import { currentRoute, rootOpens } from "../nav";
import { mostPlayed, refreshMostPlayed } from "../state/user-catalog";
import { playTrack, isCurrentTrack, isPlaying } from "../state/player";
import { Button } from "nsh/base";
import { focusRing, } from "../components/ui/controls";
import { PlayingBars } from "../components/ui/playing";
import { Cover } from "../components/ui/cover";
import { trackArtist, trackTitle, clock, type MostPlayed as MostPlayedRow, type RemoteTrack } from "../backend/types";
import {
  isRemoteConfigured,
  playRemote,
  remoteTracks,
} from "../state/remote";
import { remoteCoverOf } from "../state/artwork";
import { RemoteTrackRow } from "./remote";
import { setServerDialogOpen } from "../state/overlays";
import { open } from "../nav";

function greeting(): string {
  const h = new Date().getHours();
  if (h < 5) return "夜深了";
  if (h < 11) return "早上好";
  if (h < 14) return "中午好";
  if (h < 18) return "下午好";
  return "晚上好";
}

/** The 添加文件夹 the 空曲库 empty states offer. */
function AddFolderButton() {
  return <ActionButton label="添加文件夹" onPress={() => void addRoot()} />;
}

export function HomePage() {
  // Lists load when the page shows; a scan (a new epoch) asks again.
  effect(() => {
    dataEpoch();
    untracked(() => {
      ensureAlbums();
      ensureArtists();
    });
  });

  // The rows' wave, played as the page is entered.
  const wave = createListWave({ isTop: () => currentRoute().name === "home", reenter: rootOpens });

  return (
    <AppPage title="首页" testID="page-home">
      <Scroll class="flex-1" contentClass={pageEnd()}>
        <Text role="header" class="px-6 pt-2 pb-4 text-4xl font-bold text-foreground" style={enter("fadeInUp")}>
          {greeting()}
        </Text>
        <SourceTabs />
        <If when={activeSource().kind === "remote"}>
          <RemoteHomeSections wave={wave} />
        </If>
        <If
          when={activeSource().kind !== "remote"}
          else={<View class="gap-0" />}
        >
          <View>
            <CardsSection
              title="专辑"
              emptyTitle="专辑是空的"
              errorLabel="专辑没有读出来。"
              items={homeAlbums().map(albumItemOf)}
              state={albumsState()}
              retry={retryAlbums}
              wave={wave}
            />
            <CardsSection
              title="歌手"
              emptyTitle="歌手是空的"
              emptyDetail="扫描把文件夹里的歌手收进来后，他们在这里。"
              errorLabel="歌手没有读出来。"
              items={homeArtists().map(artistItemOf)}
              state={artistsState()}
              retry={retryArtists}
              wave={wave.child(waveDelay(1, 4))}
            />
            <RemotePreview />
            <MostPlayedPreview />
            <TracksPreview wave={wave.child(waveDelay(2, 4))} />
          </View>
        </If>
      </Scroll>
    </AppPage>
  );
}

/** 首页的来源 Tab：全部本地 / 每个文件夹 / 每个远程服务器，横向 chips，
 * 多了可滚；尾随 + 文件夹 / + 服务器。 */
function SourceTabs() {
  const list = () => allSources();
  const activeId = () => activeSource().id;
  return (
    <View class="flex-row items-center gap-1.5 px-3 pb-6">
      {/* The strip is exactly as tall as a chip: a taller box centred the
          chips against a shorter button and the + sat a few points low. */}
      <Scroll horizontal scrollbar={false} style={{ height: 32 }} contentStyle={{ paddingRight: 8 }}>
        <View class="flex-row items-center gap-1.5 px-1">
          <Each of={list()}>
            {(src) => {
              const active = () => activeId() === src().id;
              return (
                <Button
                  accessibilityLabel={`来源 ${src().label}${active() ? "，当前" : ""}`}
                  onPress={() => selectSource(src().id)}
                  class={`h-8 shrink-0 flex-row items-center gap-1.5 rounded-full px-3 transition duration-150 ease-out ${
                    active() ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-row-hover"
                  } ${focusRing}`}
                >
                  <Icon name={src().kind === "remote" ? "server" : "folder"} size={14} color={active() ? "foreground" : "muted-foreground"} />
                  <Text class={`text-xs font-medium ${active() ? "text-foreground" : "text-muted-foreground"}`}>
                    {src().label}
                  </Text>
                </Button>
              );
            }}
          </Each>
        </View>
      </Scroll>
      {/* One entry, the same one the sidebar has: a folder and a server are
          two answers to "add a source", not two buttons. */}
      <View class="flex-row shrink-0 pr-1">
        <Button
          accessibilityLabel="添加来源"
          onPress={() => openSourceDialog()}
          class={`h-8 w-8 shrink-0 items-center justify-center rounded-full p-0 text-muted-foreground transition duration-150 ease-out hover:bg-row-hover ${focusRing}`}
        >
          <Icon name="plus" size={16} color="muted-foreground" />
        </Button>
      </View>
    </View>
  );
}

/** 首页远程来源的内容：服务器名头 + 专辑卡行 + 歌手卡行 + 前 12 条曲目。
 * 数据来自该服务器的列表（remote.ts 的缓存 + 分页拉取）。 */
function RemoteHomeSections(props: { wave: ListWave }) {
  const server = () => activeRemoteServer();
  const groups = () => remoteAlbumGroups();
  const singerGroups = () => remoteArtistGroups();
  const rows = () => remoteTracks().slice(0, 12);
  const state = remoteLoadingState();
  return (
    <If
      when={server()}
      else={
        <EmptyState
          title="没有连接的服务器"
          detail="在侧栏的远程库段连接一个服务器，或用上面的 + 服务器。"
        >
          <ActionButton label="连接服务器…" onPress={() => setServerDialogOpen(true)} />
        </EmptyState>
      }
    >
      <View class="gap-2 px-3 pb-2" accessibilityLabel="远程来源">
        <Text class="px-3 text-xs text-muted-foreground">
          {server()!.label} · {server()!.kind === "navidrome" ? "Navidrome" : "Jellyfin"} · {remoteTracks().length} 首
        </Text>
      </View>
      {/* Configured and reachable are two different answers; the page said
          "empty" for both, which reads as a server with no music. */}
      <If when={!engine.remoteStatus().configured}>
        <View class="px-3 pb-8">
          <EmptyState title="服务器未连接" detail="引擎还没有这台服务器的登录状态：重新连接一次；密码若改过，在「添加来源」里更新它。">
            <ActionButton label="重新连接" onPress={() => void reconnectServer(server()!.id)} />
          </EmptyState>
        </View>
      </If>
      <If when={state === "loading" && remoteTracks().length === 0}>
        <View class="gap-4 px-3 pb-8">
          <CardsSkeleton rows={1} />
          <RowsSkeleton rows={4} header />
        </View>
      </If>
      <If when={state === "error" && remoteTracks().length === 0}>
        <View class="px-3 pb-8">
          <ErrorState
            message={remoteError() ? `远程库没有读出来：${remoteError()}` : "远程库没有读出来。"}
            onRetry={retryRemote}
          />
        </View>
      </If>
      <If when={remoteTracks().length > 0}>
        <View class="gap-2 px-3 pb-8">
          <SectionTitle class="px-3">专辑</SectionTitle>
          <If
            when={groups().length > 0}
            else={<EmptyState title="没有专辑" detail="服务器上的曲目还没有专辑名可分。" />}
          >
            <RemoteCardsRow
              variant="album"
              items={groups()
                .slice(0, 24)
                .map((g) => ({
                  key: `${g.album}\u0000${g.artist}`,
                  name: g.album,
                  sub: g.artist || `${g.tracks.length} 首`,
                  track: g.tracks[0],
                  count: g.tracks.length,
                }))}
              wave={props.wave}
            />
          </If>
          <If when={singerGroups().length > 0}>
            <SectionTitle class="px-3 pt-4">歌手</SectionTitle>
            <RemoteCardsRow
              variant="artist"
              items={singerGroups()
                .slice(0, 24)
                .map((g) => ({
                  key: g.artist,
                  name: g.artist,
                  sub: `${g.tracks.length} 首`,
                  track: g.tracks[0],
                  count: g.tracks.length,
                }))}
              wave={props.wave.child(waveDelay(1, 4))}
            />
          </If>
          <SectionTitle class="px-3 pt-4">曲目</SectionTitle>
          <View class="px-3">
            <Each of={rows()}>
              {(track, i) => <RemoteTrackRow track={track()} number={i()} />}
            </Each>
          </View>
        </View>
      </If>
    </If>
  );
}


/** A titled row of cards with its load states: loading → skeleton, error →
 * 错误 + Retry, empty → what to do, else the cards. */
function CardsSection(props: {
  title: string;
  emptyTitle: string;
  emptyDetail?: string;
  errorLabel: string;
  items: LibraryItem[];
  state: "idle" | "loading" | "error";
  retry: () => void;
  wave?: ListWave;
}) {
  return (
    <View class="gap-2 px-3 pb-8">
      <SectionTitle class="px-3">{props.title}</SectionTitle>
      <If
        when={props.state === "error"}
        else={
          <If
            when={props.items.length > 0}
            else={
              <If
                when={props.state === "loading"}
                else={
                  <EmptyState title={props.emptyTitle} detail={props.emptyDetail}>
                    <AddFolderButton />
                  </EmptyState>
                }
              >
                <CardsSkeleton rows={1} />
              </If>
            }
          >
            {/* 标题已由上面的 SectionTitle 画过；CardRow 不再带名。 */}
            <CardRow items={props.items} wave={props.wave} />
          </If>
        }
      >
        <ErrorState message={props.errorLabel} onRetry={props.retry} />
      </If>
    </View>
  );
}

/** The first tracks that have arrived (the index loads in the background
 * below; the 全部曲目 page is the whole one), or the empty prompt. */
function TracksPreview(props: { wave: ListWave }) {
  // Only the index; albums and artists already asked above.
  effect(() => {
    untracked(() => ensureTracks());
  });
  const sample = () => homeTracks().slice(0, 12);
  // 单个文件夹来源：空态说的是这个文件夹，不是整个曲库。
  const emptyTitle = () => (activeSource().root ? "这个文件夹还没有曲目" : "曲库是空的");
  const emptyDetail = () =>
    activeSource().root ? "扫描把这个文件夹里的文件收进曲库。" : "添加一个音乐文件夹，扫描把里面的文件收进曲库。";
  const showAlbum = wideTracks();
  return (
    <View class="gap-2">
      <SectionTitle class="px-6">曲目</SectionTitle>
      <If
        when={sample().length > 0}
        else={
          <If
            when={tracksState() === "loading"}
            else={
              <If
                when={tracksState() === "error"}
                else={
                  <EmptyState title={emptyTitle()} detail={emptyDetail()}>
                    <AddFolderButton />
                  </EmptyState>
                }
              >
                <RowsSkeleton rows={4} header />
              </If>
            }
          >
            <Text class="px-6 text-xs text-muted-foreground">正在读曲库…</Text>
          </If>
        }
      >
        <View class="px-3">
          <TrackHeader showAlbum={showAlbum} />
          <Each of={sample()}>
            {(track, i) => <TrackRow track={track()} number={i()} showAlbum={showAlbum} wave={props.wave} />}
          </Each>
        </View>
      </If>
    </View>
  );
}

/** 首页远程库：远程配置好且列表非空时横向远程行卡（144 卡样式，点击=
 * playRemote 与远程页同一链）；未配置/空列表整段不渲染。 */
function RemotePreview() {
  const configured = () => isRemoteConfigured();
  const rows = () => remoteTracks().slice(0, 12);
  return (
    <If when={configured() && rows().length > 0}>
      <View class="gap-2 px-3 pb-8" accessibilityLabel="远程库">
        <SectionTitle class="px-3">远程库</SectionTitle>
        <List
          horizontal
          each={rows()}
          keyExtractor={(row: RemoteTrack) => row.id}
          estimatedItemSize={160}
          snapToInterval={160}
          snapToAlignment="start"
          decelerationRate="fast"
          scrollbar={false}
          class="-mx-3 h-[196px]"
          contentClass="px-3 pt-1"
        >
          {(row, index) => <RemoteHomeCard row={row()} index={index()} />}
        </List>
      </View>
    </If>
  );
}

function RemoteHomeCard(props: { row: RemoteTrack; index: number }) {
  const row = () => props.row;
  const cover = () => remoteCoverOf(row().id, row().coverArt);
  const sub = () =>
    [row().artist, row().album].filter((part) => part).join(" · ") || "—";
  return (
    <Button
      accessibilityLabel={`${row().title}，${sub()}`}
      onPress={() => playRemote(row())}
      class={`group shrink-0 gap-2 rounded-(--radius-card) p-2.5 transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] motion-reduce:transition-none motion-reduce:active:scale-100 ${focusRing}`}
      style={{ width: 160 }}
    >
      <View class="relative overflow-hidden rounded-(--radius) shadow-md items-center justify-center bg-secondary" style={{ width: 140, height: 140 }}>
        <Icon name="music" size={56} color="muted-foreground" />
        <If when={cover()}>
          <Image src={cover()} accessibilityElementsHidden class="absolute inset-0 object-cover" />
        </If>
        <If when={row().container}>
          <Text
            accessibilityLabel={`容器 ${row().container}`}
            class="absolute bottom-1 right-1 rounded-sm bg-muted-foreground px-1 py-0.5 text-[9px] font-mono font-bold lowercase text-center text-background" style={{ minWidth: 42 }}
          >
            {row().container.toUpperCase().slice(0, 5)}
          </Text>
        </If>
      </View>
      <View class="gap-0.5">
        <Text class="line-clamp-1 text-sm font-bold text-foreground">{row().title || "—"}</Text>
        <Text class="line-clamp-2 text-xs text-muted-foreground">{sub()}</Text>
      </View>
    </Button>
  );
}

/** 首页最常播放：横排的曲目卡（listMostPlayed(12) 驱动；无数据不占
 * 位）。卡显示封面、标题、播放次数；点击播放该行。 */
function MostPlayedPreview() {
  // 一次页面进门拉一页（请求后引擎还会自动重发）。
  effect(() => {
    untracked(() => refreshMostPlayed(12));
  });
  const rows = mostPlayed();
  return (
    <If when={rows.length > 0}>
      <View class="gap-2 px-3 pb-8" accessibilityLabel="最常播放">
        <SectionTitle class="px-3">最常播放</SectionTitle>
        <List
          horizontal
          each={rows}
          keyExtractor={(row) => row.path}
          estimatedItemSize={200}
          snapToInterval={200}
          snapToAlignment="start"
          decelerationRate="fast"
          scrollbar={false}
          class="-mx-3 h-[196px]"
          contentClass="px-3 pt-1"
        >
          {(row, index) => <MostPlayedCard row={row()} index={index()} />}
        </List>
      </View>
    </If>
  );
}

function MostPlayedCard(props: { row: MostPlayedRow; index: number }) {
  const row = () => props.row;
  const track = () => trackForPath(row().path);
  const playing = () => isCurrentTrack(row().path) && isPlaying();
  return (
    <Button
      accessibilityLabel={`${trackTitle(track())}，播放 ${row().playCount} 次`}
      onPress={() => {
        const t = track();
        if (t) playTrack(t);
      }}
      class={`group shrink-0 gap-2 rounded-(--radius-card) p-2.5 transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] ${focusRing}`}
      style={{ width: 180, ...(props.index ? {} : {}) }}
    >
      <View class="relative overflow-hidden rounded-(--radius) shadow-md">
        <Cover path={row().path} size={128} fallback="music" />
        <If when={playing()}>
          <View class="absolute bottom-2 right-2 rounded-full bg-background/70 p-1"><PlayingBars playing size={16} /></View>
        </If>
      </View>
      <View class="gap-0.5">
        <Text class="line-clamp-1 text-sm font-bold text-foreground">{trackTitle(track()) || "—"}</Text>
        <Text class="line-clamp-1 text-xs text-muted-foreground">{trackArtist(track()) || "—"}</Text>
        <Text class="text-[11px] font-bold text-liked">{row().playCount} 次播放</Text>
      </View>
    </Button>
  );
}
