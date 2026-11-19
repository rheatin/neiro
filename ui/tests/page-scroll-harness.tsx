// 滚动位置记忆的测试台：三个 tab（首页 / 专辑 / 曲目）和一个塞满行的曲库，
// 用来验证切走再回来页面停在原处、回到顶部按钮出现和消失。
import { mount, Text, View } from "nsh";
import { Button } from "nsh/base";
import { MainStack } from "../src/shells/main";
import { switchTo, HOME, ALBUMS, TRACKS } from "../src/nav";
import { setTracksForTest } from "../src/state/catalog";
import { engine } from "../src/backend/engine";
import { health } from "../src/state/engine";
import type { Track } from "../src/backend/types";

if (health().status === "starting") engine.connect();

setTracksForTest(
  Array.from({ length: 120 }, (_, i) => ({
    path: `/music/track-${String(i).padStart(3, "0")}.flac`,
    title: `Track ${String(i).padStart(3, "0")}`,
    artist: "Scroll Artist",
    albumArtist: "Scroll Artist",
    album: "Scroll Album",
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

function TestHarness() {
  return (
    <View class="flex-1 bg-background">
      <View class="flex-row gap-2 p-2 bg-secondary" role="toolbar">
        <Button accessibilityLabel="首页页" onPress={() => switchTo(HOME)}>
          <Text>首页</Text>
        </Button>
        <Button accessibilityLabel="专辑页" onPress={() => switchTo(ALBUMS)}>
          <Text>专辑</Text>
        </Button>
        <Button accessibilityLabel="曲目页" onPress={() => switchTo(TRACKS)}>
          <Text>曲目</Text>
        </Button>
      </View>
      <MainStack class="flex-1" />
    </View>
  );
}

mount(TestHarness);
