// 远程 专辑/歌手 卡行与卡（首页来源 Tab 与搜索页共用）：144 卡样式，
// 封面 = 组代表曲目的远程封面（remoteCoverOf，走共享 artwork 栈）。

import { Image, List, Text, View } from "nsh";
import { Button } from "nsh/base";
import type { RemoteTrack } from "../../backend/types";
import { queueRemoteFrom } from "../../state/remote";
import { open } from "../../nav";
import { remoteCoverOf } from "../../state/artwork";
import { RemoteCover } from "../ui/cover";
import { activeRemoteServer, type RemoteAlbumGroup } from "../../state/remote";
import { focusRing } from "../ui/controls";
import type { ListWave } from "../ui/motion";
import { Icon } from "../ui/icons";

/** 远程 专辑/歌手 卡行（144 卡样式）。 */
export interface RemoteCardItem {
  key: string;
  name: string;
  sub: string;
  track: RemoteTrack;
  count: number;
  tracks?: RemoteTrack[];
}

export function RemoteCardsRow(props: {
  variant: "album" | "artist";
  items: RemoteCardItem[];
  wave?: ListWave;
  /** A page that owns the whole screen lays its cards out as a filling grid;
   * a home-page section keeps them in a row. */
  grid?: boolean;
}) {
  if (props.grid) {
    const minW = 160;
    return (
      <View class="grid" style={{ gridTemplateColumns: `repeat(auto-fill, minmax(${minW}px, 1fr))` }}>
        <Each of={props.items} key={(item) => item.key}>
          {(item, index) => <RemoteGroupCard variant={props.variant} item={item()} index={index()} fill />}
        </Each>
      </View>
    );
  }
  return (
    <List
      horizontal
      each={props.items}
      keyExtractor={(item: RemoteCardItem) => item.key}
      estimatedItemSize={160}
      snapToInterval={160}
      snapToAlignment="start"
      decelerationRate="fast"
      scrollbar={false}
      class="-mx-3 h-[228px]"
      contentClass="px-3 pt-1"
    >
      {(item, index) => <RemoteGroupCard variant={props.variant} item={item()} index={index()} />}
    </List>
  );
}

export function RemoteGroupCard(props: {
  variant: "album" | "artist";
  item: RemoteCardItem;
  index: number;
  /** Stretched by a grid cell instead of a fixed 160. */
  fill?: boolean;
}) {
  const item = () => props.item;
  const cover = () => remoteCoverOf(item().track.id, item().track.coverArt);
  const label = () => (props.variant === "album" ? `专辑 ${item().name}` : `歌手 ${item().name}`);
  return (
    <Button
      accessibilityLabel={`${label()}，${item().sub}`}
      onPress={() => {
        if (props.variant === "album") {
          open({
            name: "remoteAlbum",
            serverId: activeRemoteServer()?.id ?? "",
            album: item().name,
            artist: item().track.artist || "",
          });
        } else {
          open({ name: "remoteArtist", serverId: activeRemoteServer()?.id ?? "", artist: item().name });
        }
      }}
      class={`group shrink-0 gap-2 rounded-(--radius-card) p-2.5 transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] motion-reduce:transition-none motion-reduce:active:scale-100 ${focusRing}`}
      style={props.fill ? { flex: 1 } : { width: 160 }}
    >
      <View
        class={`relative overflow-hidden bg-secondary shadow-md items-center justify-center ${
          props.variant === "artist" ? "rounded-full" : "rounded-(--radius)"
        }`}
        style={{ width: 140, height: 140 }}
      >
        <Icon name={props.variant === "artist" ? "user" : "music"} size={56} color="muted-foreground" />
        <If when={cover()}>
          <Image src={cover()} accessibilityElementsHidden class="absolute inset-0 object-cover" />
        </If>
      </View>
      <View class="gap-0.5">
        <Text class="line-clamp-1 text-sm font-bold text-foreground">{item().name || "—"}</Text>
        <Text class="line-clamp-2 text-xs text-muted-foreground">{item().sub}</Text>
      </View>
    </Button>
  );
}

export function RemoteAlbumRow(props: { item: RemoteCardItem }) {
  const item = () => props.item;
  return (
    <View class="group flex-row items-center justify-between rounded-xl border border-border/40 bg-secondary/30 p-2.5 transition hover:border-foreground/20 hover:bg-row-hover">
      <Button
        accessibilityLabel={`专辑 ${item().name}，${item().sub}`}
        onPress={() =>
          open({
            name: "remoteAlbum",
            serverId: activeRemoteServer()?.id ?? "",
            album: item().name,
            artist: item().track.artist || "",
          })
        }
        class={`flex-row items-center gap-3 min-w-0 flex-1 text-left ${focusRing}`}
      >
        <RemoteCover id={item().track.id} coverArt={item().track.coverArt} size={40} />
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="truncate text-sm font-bold text-foreground">{item().name || "未知专辑"}</Text>
          <Text class="truncate text-xs text-muted-foreground">
            {item().track.artist || "未知歌手"} · {item().count} 首
          </Text>
        </View>
      </Button>
      <Button
        onPress={() => queueRemoteFrom(item().tracks ?? [item().track], 0)}
        accessibilityLabel={`播放 ${item().name} 全部曲目`}
        class={`shrink-0 flex-row items-center gap-1 rounded-full bg-foreground/10 px-2.5 py-1.5 opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-foreground hover:text-background active:scale-95 ${focusRing}`}
      >
        <Icon name="play" size={11} color="foreground" />
        <Text class="text-xs font-bold text-foreground group-hover:text-background">播放</Text>
      </Button>
    </View>
  );
}
