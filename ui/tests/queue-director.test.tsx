import { expect, test } from "nsh/test";
import {
  canPrev,
  cycleRepeatMode,
  playLaterTrack,
  playNextTrack,
  playTrack,
  prev,
  repeatMode,
  setRepeatMode,
  shuffleMode,
  toggleShuffle,
  trackToQueueItem,
} from "../src/state/player";
import { engine } from "../src/backend/engine";
import type { Track } from "../src/backend/types";

test("QueueDirector UI 状态：三态 Repeat 循环 (Off -> All -> One -> Off)", () => {
  engine.connect();
  setRepeatMode("off");
  expect(repeatMode()).toBe("off");

  cycleRepeatMode();
  expect(repeatMode()).toBe("all");

  cycleRepeatMode();
  expect(repeatMode()).toBe("one");

  cycleRepeatMode();
  expect(repeatMode()).toBe("off");
});

test("QueueDirector UI 状态：Shuffle 状态即时切换", () => {
  engine.connect();
  const initial = shuffleMode();
  toggleShuffle();
  expect(shuffleMode()).toBe(!initial);
  toggleShuffle();
  expect(shuffleMode()).toBe(initial);
});

function createDummyTrack(path: string, title: string, artist: string, album: string, durationMs: number): Track {
  return {
    path,
    title,
    artist,
    album,
    albumArtist: artist,
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Pop",
    durationMs,
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  };
}

test("QueueDirector UI 状态：上一曲由 engine.previous 统一裁决，不再硬编码为 seek(0)", () => {
  engine.connect();
  const dummyTrack = createDummyTrack("/music/track2.flac", "Track 2", "Artist 2", "Album 2", 200000);

  let calledPrevious = false;
  const originalPrevious = (engine as any).previous.bind(engine);
  (engine as any).previous = () => {
    calledPrevious = true;
  };

  try {
    playTrack(dummyTrack);
    expect(canPrev()).toBe(true);
    prev();
    expect(calledPrevious).toBe(true);
  } finally {
    (engine as any).previous = originalPrevious;
  }
});

test("QueueDirector UI 状态：Track 转换为标准的 QueueItem 并支持 Play Next / Play Later", () => {
  engine.connect();
  const dummyTrack = createDummyTrack("/music/track1.flac", "Track 1", "Artist 1", "Album 1", 180000);

  const item = trackToQueueItem(dummyTrack);
  expect(item.id).toBe("/music/track1.flac");
  expect(item.path).toBe("/music/track1.flac");
  expect(item.title).toBe("Track 1");
  expect(item.artist).toBe("Artist 1");
  expect(item.durationMs).toBe(180000);

  let sentNext = false;
  let sentLater = false;
  const originalPlayNext = (engine as any).playNext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);

  (engine as any).playNext = (items: any[]) => {
    sentNext = items.length === 1 && items[0].id === dummyTrack.path;
  };
  (engine as any).playLater = (items: any[]) => {
    sentLater = items.length === 1 && items[0].id === dummyTrack.path;
  };

  try {
    playNextTrack(dummyTrack);
    expect(sentNext).toBe(true);

    playLaterTrack(dummyTrack);
    expect(sentLater).toBe(true);
  } finally {
    (engine as any).playNext = originalPlayNext;
    (engine as any).playLater = originalPlayLater;
  }
});
