// The mobile shell alone, for the tests: the app's own four places, with the
// engine started as the app starts it and a couple of seeded tracks so the
// tabs have something to show. Opened with `platform: "ios"` the shell is
// the mobile application (see `components/layout.tsx`).

import { mount, View } from "nsh";
import { MobileShell } from "../src/shells/mobile";
import { engine } from "../src/backend/engine";
import { health } from "../src/state/engine";
import { setTracksForTest } from "../src/state/catalog";
import type { Track } from "../src/backend/types";

if (health().status === "starting") engine.connect();

setTracksForTest([
  {
    path: "/music/mobile-alpha.flac",
    title: "Mobile Track Alpha",
    artist: "Mobile Artist",
    albumArtist: "Mobile Artist",
    album: "Mobile Album Alpha",
    durationMs: 180000,
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Pop",
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  } as Track,
]);

function TestHarness() {
  return (
    <View class="flex-1 flex-row">
      <MobileShell />
    </View>
  );
}

mount(TestHarness);
