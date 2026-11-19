// The main content: one navigation stack per root, all kept alive, the
// active one shown — or, while the engine is not connected, the one page
// that says so with 重试.

import { View } from "nsh";
import { NavStack } from "nsh/base";
import { activeRootName, openedRoots, stackOf, type Route } from "../nav";
import { AlbumsPage } from "../pages/albums";
import { AlbumPage, ArtistPage } from "../pages/detail";
import { ArtistsPage } from "../pages/artists";
import { HomePage } from "../pages/home";
import { RemoteAlbumPage, RemoteArtistPage, RemotePage, RemotePlaylistPage } from "../pages/remote";
import { LikedPage, PlaylistPage, PlaylistsPage } from "../pages/user-collections";
import { EqPage } from "../pages/eq";
import { SearchPage } from "../pages/search";
import { TracksPage } from "../pages/tracks";
import { SettingsPage } from "../pages/settings";
import { EngineDownPage } from "../pages/engine-down";
import { health } from "../state/engine";

function page(route: Route) {
  switch (route.name) {
    case "home":
      return <HomePage />;
    case "search":
      return <SearchPage />;
    case "albums":
      return <AlbumsPage />;
    case "artists":
      return <ArtistsPage />;
    case "tracks":
      return <TracksPage />;
    case "settings":
      return <SettingsPage />;
    case "remote":
      return <RemotePage />;
    case "liked":
      return <LikedPage />;
    case "playlists":
      return <PlaylistsPage />;
    case "eq":
      return <EqPage />;
    case "playlist":
      return <PlaylistPage id={route.id} playlist={route.playlist} />;
    case "remotePlaylist":
      return <RemotePlaylistPage id={route.id} name={route.playlist} />;
    case "remoteAlbum":
      return <RemoteAlbumPage serverId={route.serverId} album={route.album} artist={route.artist} />;
    case "remoteArtist":
      return <RemoteArtistPage serverId={route.serverId} artist={route.artist} />;
    case "album":
      return <AlbumPage album={route.album} albumArtist={route.albumArtist} />;
    case "artist":
      return <ArtistPage artist={route.artist} />;
  }
}

export function MainStack(props: { class?: string }) {
  return (
    <View class={`min-h-0 flex-1 ${props.class ?? ""}`}>
      <If
        when={health().status !== "failed"}
        else={<EngineDownPage />}
      >
        <View class="relative min-h-0 flex-1">
          {/* One layer per visited root: the others stay mounted (and laid
              out) so a switch is only a visibility change — no rebuild, no
              re-layout. 不用 display:none：它把子树移出布局，回来时整页重
              排（大列表几百毫秒）。切换的剩余成本是 1–2 帧的首次呈现。 */}
          <Each of={openedRoots()} key={(root) => root.name}>
            {(root) => {
              const shown = () => activeRootName() === root().name;
              return (
                <View
                  class="absolute inset-0"
                  style={{ visibility: shown() ? "visible" : "hidden" }}
                >
                  <NavStack state={stackOf(root())} class="min-h-0 flex-1" testID={`main-${root().name}`}>
                    {(route) => page(route)}
                  </NavStack>
                </View>
              );
            }}
          </Each>
        </View>
      </If>
    </View>
  );
}
