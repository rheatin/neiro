import { NavStack } from "nsh/base";
import { effect, Text, View, mount, signal } from "nsh";
import { nav } from "../src/nav";
import { engine } from "../src/backend/engine";
import { TracksPage } from "../src/pages/tracks";
import { health } from "../src/state/engine";
import { setTracksForTest } from "../src/state/catalog";
import { shuffleMode } from "../src/state/player";

if (health().status === "starting") engine.connect();
shuffleMode.set(false);
const playCalls = signal(0);
const originalPlayContext = (engine as any).playContext.bind(engine);
(engine as any).playContext = () => {
  playCalls.set(playCalls() + 1);
};

setTracksForTest([
  {
    path: "/music/shuffle-a.flac",
    title: "Shuffle A",
    artist: "Artist A",
    albumArtist: "Artist A",
    album: "Album A",
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Pop",
    durationMs: 100000,
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  },
  {
    path: "/music/shuffle-b.flac",
    title: "Shuffle B",
    artist: "Artist B",
    albumArtist: "Artist B",
    album: "Album B",
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Rock",
    durationMs: 110000,
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  },
]);

function ShuffleToolbarHarness() {
  effect(() => {
    const count = playCalls();
    const mode = shuffleMode() ? "on" : "off";
    return () => `Play calls: ${count}; Shuffle: ${mode}`;
  });

  return (
    <View class="flex-1 bg-background">
      <View class="flex-row items-center justify-between bg-secondary p-2" role="header">
        <Text>全部曲目</Text>
        <Text>Play calls: {playCalls()}; Shuffle: {shuffleMode() ? "on" : "off"}</Text>
      </View>
      <NavStack state={nav} class="flex-1">
        {() => <TracksPage />}
      </NavStack>
    </View>
  );
}

mount(ShuffleToolbarHarness);
