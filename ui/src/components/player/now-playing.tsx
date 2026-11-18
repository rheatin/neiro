// Now Playing: the cover large, what plays and the full transport, in a
// full-screen sheet (Escape, back, the down chevron and a swipe down close
// it). A flat near-black sheet: the cover carries what colour there is;
// the controls are quiet.

import { cleanup, createMotion, effect, signal, token, View, windowSize } from "nsh";
import { MusicLight } from "../ui/toolbar-fx";
import { lightEffect } from "../../state/visual";
import { isPlayingSpectrum } from "../../state/visualiser";
import { animateLayoutOn, enter } from "../ui/motion";
import { titleBarArea } from "../ui/title-bar";
import { setNowPlayingOpen, setOutputOpen, setQueueOpen, nowPlayingOpen } from "../../state/overlays";
import { AppSheet } from "../ui/overlays";
import { Button } from "nsh/base";
import { IconButton } from "../ui/controls";
import { Cover } from "../ui/cover";
import { LightMenu } from "./light-menu";
import { NowPlayingSummary, SeekBar, TransportControls, VolumeControl, NEW_COVER } from "./parts";
import { NowPlayingBadges } from "../ui/format-badges";
import { LyricsPanel } from "./lyrics";
import { hasTrack, player } from "../../state/player";
import { openContextMenu } from "../ui/context-menu";
import { rowActions } from "../library/tracks";
import { trackForPath, openItem } from "../../state/catalog";
import { playTick } from "../../state/foley";
import { isFavourite, toggleFavourite } from "../../state/user-catalog";

/** Now Playing 的红心：与播放条同一集合（liked 色实心）。It sits after
 * 下一曲 — the track, then what you can do to it — rather than in the
 * sheet's chrome, where it read as a page action. */
function NowPlayingHeart(props: { size?: number }) {
  const liked = () => isFavourite(player().path ?? "");
  return (
    <If when={hasTrack()}>
      <IconButton
        name={liked() ? "heartFilled" : "heart"}
        label={liked() ? "取消收藏" : "收藏"}
        pressed={liked()}
        pressedColor="liked"
        size={props.size ?? 22}
        onPress={() => toggleFavourite(player().path ?? "")}
      />
    </If>
  );
}

/** Whether the lyrics are showing. Module scope, because this component is
 * re-invoked on every visualiser frame (the light subscribes to them) and a
 * signal created inside it would be reset by its own re-render — the sheet
 * opened the words and then closed them on the next frame. */
const lyricsOpen = signal(false);

/** Now Playing, open while the overlay store says so. */
export function NowPlayingSheet() {
  return (
    <AppSheet open={nowPlayingOpen()} onOpenChange={setNowPlayingOpen} side="bottom" full testID="now-playing">
      <NowPlayingContent />
    </AppSheet>
  );
}

function NowPlayingContent() {
  // Lyrics start hidden. The cover is the switch: on a wide window the words
  // take the right half while the rest moves left; when there is not room for
  // two columns the cover goes away and the words take the whole sheet.
  // A new track's cover settles in, as in the bar.
  const coverIn = createMotion();
  let shown = player().path;
  effect(() => {
    const now = player().path;
    if (now !== shown) void coverIn.play(NEW_COVER);
    shown = now;
  });
  cleanup(() => coverIn.stop());

  // Responsive: the cover takes what room the window leaves. Tall, it
  // sits above the rest; wide and short, beside it; with lyrics open on a
  // narrow window, it is dropped entirely so the words are never clipped.
  const side = () => windowSize().width >= 640 && windowSize().height < 560;
  const coverSize = () => {
    const { width, height } = windowSize();
    const size = side() ? Math.min(height - 120, width * 0.38) : Math.min(width - 48, height - 400);
    const fitted = Math.min(320, Math.floor(size));
    return fitted < 96 ? 0 : fitted;
  };
  /** Two columns need a cover plus a lyric column wide enough to read.
   * Narrower than this, the words take the whole sheet. */
  const TWO_COLUMN = 880;
  const lyricsOnly = () => lyricsOpen() && windowSize().width < TWO_COLUMN;
  // Rearranging (above / beside / no cover), it glides there.
  animateLayoutOn(() => `${side()}|${coverSize() > 0}|${lyricsOpen()}|${lyricsOnly()}`);

  const track = () => trackForPath(player().path ?? "");
  const openAlbum = () => {
    const t = track();
    if (!t) return;
    if (t.album) openItem({ kind: "album", album: t.album, albumArtist: t.albumArtist ?? t.artist ?? "", name: t.album, subtitle: t.albumArtist ?? t.artist ?? "", count: 0 });
    else if (t.artist) openItem({ kind: "artist", artist: t.artist, name: t.artist, subtitle: "", count: 0 });
  };
  const cover = (size: number) => (
    // The press target is a Button (the cover is a control here — it opens the
    // words), and the wrapper owns the right-click menu, which ButtonProps does
    // not carry.
    <View
      style={coverIn.style}
      onContextMenu={(e) =>
        openContextMenu(e.pagePosition, [
          // The press is the lyrics switch, so the jump it used to make lives
          // here — one gesture each, and neither hides the other.
          ...(track()?.album || track()?.artist
            ? [{ label: track()?.album ? "查看专辑" : "查看歌手", onSelect: openAlbum }]
            : []),
          ...rowActions(track()),
        ], "sheet")
      }
    >
      <Button
        testID="now-playing-cover"
        accessibilityLabel={lyricsOpen() ? "隐藏歌词" : "显示歌词"}
        onPress={() => {
          const next = !lyricsOpen();
          lyricsOpen.set(next);
          if (next) playTick();
        }}
        class="rounded-2xl active:opacity-85"
      >
        <Cover path={player().path ?? ""} size={size} class="rounded-2xl shadow-xl" />
      </Button>
    </View>
  );
  const details = () => (
    <>
      <View class="w-full gap-2" style={enter("fadeInUp", 140)}>
        <NowPlayingSummary large cover={0} center={lyricsOpen()} />
      </View>
      <View style={enter("fadeInUp", 200)}>
        {/* Between the two clocks: the sheet's one point equidistant from
            both ends of the track. */}
        <SeekBar
          quiet
          center={
            // `self-center`: the plate row would otherwise take the whole slot
            // and leave its one plate at the slot's left edge.
            <View class="self-center">
              <NowPlayingBadges wrap={false} />
            </View>
          }
        />
      </View>
      <View class="flex-row items-center justify-center gap-6" style={enter("fadeInUp", 260)}>
        <TransportControls large quiet />
        <NowPlayingHeart size={22} />
      </View>
    </>
  );

  return (
    <View class="relative flex-1 bg-sidebar pb-[env(safe-area-inset-bottom)]">
      {/* The music light: the page's background, rising from the bottom and
          driven by the engine's own frames. It runs only while playing and is
          still otherwise. */}
      <MusicLight
        effect={lightEffect()}
        playing={isPlayingSpectrum()}
        // Kept low: it is the floor's light, not a second layer over the
        // controls. Two thirds of the sheet used to put peaks on the transport.
        reach={windowSize().height * 0.55}
      />
      {/* The top row starts under the window controls (a band that drags
          the window); the close chevron sits clear of the caption
          buttons. */}
      <View class="z-10 flex-row items-center justify-between px-3" desktop={{ windowDrag: true }} style={{ paddingTop: titleBarArea().height > 0 ? titleBarArea().height : 16 }}>
        <IconButton name="chevronDown" label="关闭 Now Playing" size={24} onPress={() => setNowPlayingOpen(false)} />
        <View class="flex-1" />
        <View class="flex-row items-center gap-1">
          <IconButton name="queue" label="待播" size={22} hint onPress={() => setQueueOpen(true)} />
          <IconButton name="info" label="输出信息" size={22} hint onPress={() => setOutputOpen(true)} />
          <If when={lyricsOnly()}>
            <IconButton name="disc" label="显示封面" size={22} onPress={() => lyricsOpen.set(false)} />
          </If>
          <VolumeControl quiet />
          <LightMenu />
        </View>
      </View>
      <If when={lyricsOnly()}>
        {/* Narrow: the words take the whole sheet. A 320pt cover in a 200pt
            column was overlapping the lines; dropping it leaves the transport
            so playback stays controllable. */}
        <View class="min-h-0 flex-1 gap-4 px-8 pb-8">
          <View class="min-h-0 flex-1">
            <LyricsPanel large />
          </View>
          <View class="gap-3">
            <SeekBar
          quiet
          center={
            // `self-center`: the plate row would otherwise take the whole slot
            // and leave its one plate at the slot's left edge.
            <View class="self-center">
              <NowPlayingBadges wrap={false} />
            </View>
          }
        />
            <View class="flex-row items-center justify-center gap-6">
              <TransportControls large quiet />
              <NowPlayingHeart size={22} />
            </View>
          </View>
        </View>
      </If>
      <If when={!lyricsOnly()}>
      <If
        when={side()}
        else={
          // The row is as tall as the sheet; each column decides what to do
          // with that height. The lyrics need it whole (their scroll has no
          // other source of height); the cover block sits at the top when the
          // words are out, centred when they are not.
          <View class="min-h-0 flex-1 items-center justify-center px-8 pb-8">
          <View class="w-full flex-row items-center justify-center gap-10" style={{ maxWidth: 1120 }}>
            {/* Left: the cover over its title, the bar and the controls. With
                the lyrics open it narrows to a third and the whole block
                starts at the same line as the words, as Apple Music does. */}
            <View
              class={`min-h-0 items-center gap-5 ${lyricsOpen() ? "justify-start" : "justify-center"}`}
              style={{ flexBasis: lyricsOpen() ? "34%" : "100%", flexGrow: lyricsOpen() ? 0 : 1 }}
            >
              <If when={coverSize() > 0}>
                <View style={enter("zoomIn", 60)}>
                  {cover(lyricsOpen() ? Math.min(300, coverSize()) : coverSize())}
                </View>
              </If>
              <View class="w-full max-w-160 gap-5">{details()}</View>
            </View>
            <If when={lyricsOpen()}>
              {/* Capped: a lyric column stretched to the window edge leaves all
                  the text mass on the left of the sheet, which reads as a
                  layout that never finished centring. */}
              <View class="min-h-0 flex-1" style={{ minHeight: 420, maxHeight: "82%", maxWidth: 660 }}>
                <LyricsPanel large />
              </View>
            </If>
          </View>
          </View>
        }
      >
        {/* Wide and short: the cover beside the rest, and the lyrics after
            both when they are open. */}
        <View class="min-h-0 flex-1 flex-row items-center justify-center gap-8 px-8 pb-6">
          <View style={enter("zoomIn", 60)}>{cover(Math.min(240, coverSize() || 240))}</View>
          <View
            class="min-h-0 flex-row items-center gap-6"
            style={{ flexBasis: lyricsOpen() ? "60%" : "auto", flexGrow: 1 }}
          >
            <View class="min-w-0 max-w-140 flex-1 gap-5">{details()}</View>
            <If when={lyricsOpen()}>
              {/* Stretched, not centred: the words line up with the cover's top
                  and run down beside it, instead of floating mid-column. */}
              <View class="min-h-0 flex-1 self-stretch">
                <LyricsPanel large />
              </View>
            </If>
          </View>
        </View>
      </If>
      </If>
    </View>
  );
}
