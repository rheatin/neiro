// The main content: the navigation stack of pages — or, while the engine
// is not connected, the one page that says so with 重试.

import { View } from "nsh";
import { NavStack } from "nsh/base";
import { nav, type Route } from "../nav";
import { AlbumsPage } from "../pages/albums";
import { AlbumPage, ArtistPage } from "../pages/detail";
import { ArtistsPage } from "../pages/artists";
import { HomePage } from "../pages/home";
import { RemoteAlbumPage, RemoteArtistPage, RemotePage, RemotePlaylistPage } from "../pages/remote";
import { LikedPage, PlaylistPage } from "../pages/user-collections";
import { EqPage } from "../pages/eq";
import { SearchPage } from "../pages/search";
import { TracksPage } from "../pages/tracks";
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
    case "remote":
      return <RemotePage />;
    case "liked":
      return <LikedPage />;
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
        <NavStack state={nav} class="flex-1" testID="main">
          {(route) => page(route)}
        </NavStack>
      </If>
    </View>
  );
}
