// 远程 专辑/歌手 卡行与卡（首页来源 Tab 与搜索页共用）：144 卡样式，
// 封面 = 组代表曲目的远程封面（remoteCoverOf，走共享 artwork 栈）。

import { Image, List, Text, View } from "nsh";
import { Button } from "nsh/base";
import type { RemoteTrack } from "../../backend/types";
import { open } from "../../nav";
import { remoteCoverOf } from "../../state/artwork";
import { activeRemoteServer } from "../../state/remote";
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
    return (
      <View class="grid" style={{ gridTemplateColumns: "repeat(auto-fill, minmax(160px, 1fr))" }}>
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
