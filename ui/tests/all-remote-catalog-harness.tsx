import { NavStack } from "nsh/base";
import { Text, View, mount } from "nsh";
import { nav } from "../src/nav";
import { engine } from "../src/backend/engine";
import { TracksPage } from "../src/pages/tracks";
import { health } from "../src/state/engine";
import { setTracksForTest } from "../src/state/catalog";
import { remoteTracks, setCachedRemoteTracksForTest } from "../src/state/remote";
import { selectSource } from "../src/state/sources";

if (health().status === "starting") engine.connect();

setTracksForTest([
  {
    path: "/music/local-all.flac",
    title: "Local In All",
    artist: "Local Artist",
    albumArtist: "Local Artist",
    album: "Local Album",
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Pop",
    durationMs: 100000,
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  },
]);

remoteTracks.set([]);
setCachedRemoteTracksForTest("srv-cached", [
  {
    id: "remote-cached-1",
    title: "Remote In All",
    artist: "Remote Artist",
    album: "Remote Album",
    container: "flac",
    durationMs: 120000,
    size: null,
    coverArt: null,
  },
]);

selectSource("all");

function AllRemoteCatalogHarness() {
  return (
    <View class="flex-1 bg-background">
      <View class="flex-row items-center justify-between bg-secondary p-2" role="header">
        <Text>全部曲目</Text>
      </View>
      <NavStack state={nav} class="flex-1">
        {() => <TracksPage />}
      </NavStack>
    </View>
  );
}

mount(AllRemoteCatalogHarness);
