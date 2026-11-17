import { mount, Text, View } from "nsh";
import { Button } from "nsh/base";
import { MainStack } from "../src/shells/main";
import { back, open } from "../src/nav";
import { remoteTracks } from "../src/state/remote";
import { setTracksForTest } from "../src/state/catalog";
import { engine } from "../src/backend/engine";
import { health } from "../src/state/engine";
import type { Track } from "../src/backend/types";

if (health().status === "starting") engine.connect();

// Seed remote tracks
remoteTracks.set([
  {
    id: "rem-alpha-1",
    title: "Remote Track Alpha 1",
    artist: "Artist One",
    album: "Remote Album Alpha",
    container: "flac",
    durationMs: 180000,
    size: 25000000,
    coverArt: null,
  },
  {
    id: "rem-beta-1",
    title: "Remote Track Beta 1",
    artist: "Artist Two",
    album: "Remote Album Beta",
    container: "flac",
    durationMs: 200000,
    size: 30000000,
    coverArt: null,
  },
]);

// Seed local tracks
setTracksForTest([
  {
    path: "/music/local-alpha-1.flac",
    title: "Local Track Alpha 1",
    artist: "Local Artist Alpha",
    albumArtist: "Local Artist Alpha",
    album: "Local Album Alpha",
    durationMs: 180000,
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Pop",
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  } as Track,
  {
    path: "/music/local-beta-1.flac",
    title: "Local Track Beta 1",
    artist: "Local Artist Beta",
    albumArtist: "Local Artist Beta",
    album: "Local Album Beta",
    durationMs: 210000,
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Rock",
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  } as Track,
]);

function TestHarness() {
  return (
    <View class="flex-1 bg-background">
      <View class="flex-row gap-2 p-2 bg-secondary" role="toolbar">
        <Button
          accessibilityLabel="打开远程专辑1"
          onPress={() =>
            open({
              name: "remoteAlbum",
              serverId: "srv1",
              album: "Remote Album Alpha",
              artist: "Artist One",
            })
          }
        >
          <Text>打开远程专辑1</Text>
        </Button>
        <Button
          accessibilityLabel="打开远程专辑2"
          onPress={() =>
            open({
              name: "remoteAlbum",
              serverId: "srv1",
              album: "Remote Album Beta",
              artist: "Artist Two",
            })
          }
        >
          <Text>打开远程专辑2</Text>
        </Button>
        <Button
          accessibilityLabel="打开远程歌手1"
          onPress={() =>
            open({
              name: "remoteArtist",
              serverId: "srv1",
              artist: "Artist One",
            })
          }
        >
          <Text>打开远程歌手1</Text>
        </Button>
        <Button
          accessibilityLabel="打开远程歌手2"
          onPress={() =>
            open({
              name: "remoteArtist",
              serverId: "srv1",
              artist: "Artist Two",
            })
          }
        >
          <Text>打开远程歌手2</Text>
        </Button>
        <Button
          accessibilityLabel="打开本地专辑1"
          onPress={() =>
            open({
              name: "album",
              album: "Local Album Alpha",
              albumArtist: "Local Artist Alpha",
            })
          }
        >
          <Text>打开本地专辑1</Text>
        </Button>
        <Button
          accessibilityLabel="打开本地专辑2"
          onPress={() =>
            open({
              name: "album",
              album: "Local Album Beta",
              albumArtist: "Local Artist Beta",
            })
          }
        >
          <Text>打开本地专辑2</Text>
        </Button>
        <Button accessibilityLabel="返回" onPress={() => back()}>
          <Text>返回</Text>
        </Button>
      </View>
      <MainStack class="flex-1" />
    </View>
  );
}

mount(TestHarness);
