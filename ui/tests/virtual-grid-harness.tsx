// 大曲库：专辑网格必须只渲染视口附近的卡片（虚拟化，AGENTS §4）。
import { mount, Text, View } from "nsh";
import { Button } from "nsh/base";
import { MainStack } from "../src/shells/main";
import { switchTo, ALBUMS } from "../src/nav";
import { setTracksForTest } from "../src/state/catalog";
import { engine } from "../src/backend/engine";
import { health } from "../src/state/engine";
import type { Track } from "../src/backend/types";

if (health().status === "starting") engine.connect();

// 400 张专辑，每张 5 首。
setTracksForTest(
  Array.from({ length: 2000 }, (_, i) => ({
    path: `/music/track-${String(i).padStart(4, "0")}.flac`,
    title: `Track ${String(i).padStart(4, "0")}`,
    artist: "V Artist",
    albumArtist: "V Artist",
    album: `V Album ${String(Math.floor(i / 5)).padStart(3, "0")}`,
    durationMs: 180000,
    discNo: 1,
    trackNo: i + 1,
    year: 2024,
    genre: "Pop",
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  })) as Track[],
);

mount(() => (
  <View class="flex-1 bg-background">
    <Button accessibilityLabel="专辑页" onPress={() => switchTo(ALBUMS)}>
      <Text>专辑</Text>
    </Button>
    <MainStack class="flex-1" />
  </View>
));
