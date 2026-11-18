import { expect, test } from "nsh/test";
import { EMPTY_PLAYER, type Album, type Artist, type Track } from "../src/backend/types";
import {
  albumDensity,
  albumSortField,
  artistSortField,
  artistViewMode,
  cycleAlbumSort,
  cycleArtistSort,
  cycleTrackSort,
  sortAndFilterAlbums,
  sortAndFilterArtists,
  sortAndFilterTracks,
  trackFilterPill,
  trackFilterQuery,
  trackSortField,
  trackSortOrder,
} from "../src/state/library-prefs";
import {
  adoptLyrics,
  enabledLyricsCount,
  enabledLyricsProviders,
  matchScore,
  rankedCandidates,
  similarity,
  startLyricsSearch,
  trackLyrics,
} from "../src/state/lyrics";
import { engine } from "../src/backend/engine";
import { playTrack } from "../src/state/player";

const SAMPLE_TRACKS: Track[] = [
  {
    path: "/music/track_c.flac",
    title: "C Track",
    artist: "Artist B",
    album: "Album 2",
    albumArtist: "Artist B",
    discNo: 1,
    trackNo: 3,
    year: 2021,
    genre: "Rock",
    durationMs: 180000,
    sampleRate: 96000,
    bits: 24,
    channels: 2,
  },
  {
    path: "/music/track_a.dsf",
    title: "A Track",
    artist: "Artist A",
    album: "Album 1",
    albumArtist: "Artist A",
    discNo: 1,
    trackNo: 1,
    year: 2019,
    genre: "Classical",
    durationMs: 240000,
    sampleRate: 2822400,
    bits: 1,
    channels: 2,
  },
  {
    path: "/music/track_b.mp3",
    title: "B Track",
    artist: "Artist C",
    album: "Album 3",
    albumArtist: "Artist C",
    discNo: 1,
    trackNo: 2,
    year: 2023,
    genre: "Pop",
    durationMs: 120000,
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  },
];

const SAMPLE_ALBUMS: Album[] = [
  { album: "Z Album", albumArtist: "Artist 1", year: 2018, trackCount: 5 },
  { album: "A Album", albumArtist: "Artist 2", year: 2022, trackCount: 12 },
  { album: "M Album", albumArtist: "Artist 3", year: 2020, trackCount: 8 },
];

const SAMPLE_ARTISTS: Artist[] = [
  { artist: "Charlie", trackCount: 30, albumCount: 2 },
  { artist: "Alice", trackCount: 50, albumCount: 5 },
  { artist: "Bob", trackCount: 10, albumCount: 1 },
];

test("曲目排序与 Hi-Fi 格式过滤选择器纯函数验证", () => {
  // 1. Title Sorting
  const byTitleAsc = sortAndFilterTracks(SAMPLE_TRACKS, { sortField: "title", sortOrder: "asc", filterPill: "all" });
  expect(byTitleAsc[0].title).toBe("A Track");
  expect(byTitleAsc[1].title).toBe("B Track");
  expect(byTitleAsc[2].title).toBe("C Track");

  const byTitleDesc = sortAndFilterTracks(SAMPLE_TRACKS, { sortField: "title", sortOrder: "desc", filterPill: "all" });
  expect(byTitleDesc[0].title).toBe("C Track");
  expect(byTitleDesc[2].title).toBe("A Track");

  // 2. Duration Sorting
  const byDurationAsc = sortAndFilterTracks(SAMPLE_TRACKS, { sortField: "duration", sortOrder: "asc", filterPill: "all" });
  expect(byDurationAsc[0].durationMs).toBe(120000); // B Track
  expect(byDurationAsc[2].durationMs).toBe(240000); // A Track

  // 3. Hi-Res Filter Pill
  const hiresOnly = sortAndFilterTracks(SAMPLE_TRACKS, { filterPill: "hires" });
  expect(hiresOnly.length).toBe(1);
  expect(hiresOnly[0].path).toBe("/music/track_c.flac");

  // 4. DSD Filter Pill
  const dsdOnly = sortAndFilterTracks(SAMPLE_TRACKS, { filterPill: "dsd" });
  expect(dsdOnly.length).toBe(1);
  expect(dsdOnly[0].path).toBe("/music/track_a.dsf");

  // 5. Lossless Filter Pill (FLAC and DSD are lossless, MP3 is lossy)
  const losslessOnly = sortAndFilterTracks(SAMPLE_TRACKS, { filterPill: "lossless" });
  expect(losslessOnly.length).toBe(2);
  expect(losslessOnly.some((t) => t.path.endsWith(".mp3"))).toBe(false);

  // 6. Text query search
  const searched = sortAndFilterTracks(SAMPLE_TRACKS, { query: "classical", filterPill: "all" });
  expect(searched.length).toBe(0); // genre is not in title/artist/album/path
  const searchedByTitle = sortAndFilterTracks(SAMPLE_TRACKS, { query: "c track", filterPill: "all" });
  expect(searchedByTitle.length).toBe(1);
  expect(searchedByTitle[0].title).toBe("C Track");
});

test("曲目表头循环切换排序状态 (asc -> desc -> default)", () => {
  trackSortField.set("default");
  trackSortOrder.set("asc");

  cycleTrackSort("title");
  expect(trackSortField()).toBe("title");
  expect(trackSortOrder()).toBe("asc");

  cycleTrackSort("title");
  expect(trackSortField()).toBe("title");
  expect(trackSortOrder()).toBe("desc");

  cycleTrackSort("title");
  expect(trackSortField()).toBe("default");
  expect(trackSortOrder()).toBe("asc");
});

test("专辑排序、过滤与密度模式验证", () => {
  // Sort by album name
  const byAlbumAsc = sortAndFilterAlbums(SAMPLE_ALBUMS, { sortField: "album", sortOrder: "asc" });
  expect(byAlbumAsc[0].album).toBe("A Album");
  expect(byAlbumAsc[2].album).toBe("Z Album");

  // Sort by trackCount desc
  const byCountDesc = sortAndFilterAlbums(SAMPLE_ALBUMS, { sortField: "trackCount", sortOrder: "desc" });
  expect(byCountDesc[0].trackCount).toBe(12);
  expect(byCountDesc[2].trackCount).toBe(5);

  // Density preference
  albumDensity.set("standard");
  expect(albumDensity()).toBe("standard");
  albumDensity.set("compact");
  expect(albumDensity()).toBe("compact");
  albumDensity.set("standard");
});

test("歌手排序、过滤与网格/列表双视图模式验证", () => {
  // Sort by artist name
  const byNameAsc = sortAndFilterArtists(SAMPLE_ARTISTS, { sortField: "artist", sortOrder: "asc" });
  expect(byNameAsc[0].artist).toBe("Alice");
  expect(byNameAsc[2].artist).toBe("Charlie");

  // Sort by trackCount desc
  const byTracksDesc = sortAndFilterArtists(SAMPLE_ARTISTS, { sortField: "trackCount", sortOrder: "desc" });
  expect(byTracksDesc[0].artist).toBe("Alice"); // 50
  expect(byTracksDesc[1].artist).toBe("Charlie"); // 30
  expect(byTracksDesc[2].artist).toBe("Bob"); // 10

  // Dual View Mode toggle
  artistViewMode.set("grid");
  expect(artistViewMode()).toBe("grid");
  artistViewMode.set("list");
  expect(artistViewMode()).toBe("list");
  artistViewMode.set("grid");
});

test("多源在线歌词匹配度算法与提供商集合验证", () => {
  // Similarity
  expect(similarity("晴天", "晴天")).toBe(1);
  expect(similarity("晴天", "阴天")).toBe(0);
  expect(similarity("七里香", "七里香 (Live)")).toBeGreaterThan(0.5);

  // Match score
  const candidate = {
    id: 1,
    track: "晴天",
    artist: "周杰伦",
    album: "叶惠美",
    durationS: 269,
    synced: "[00:00.00]晴天",
    plain: null,
    provider: "builtin.lyrics.netease",
    providerName: "网易云音乐",
  };
  const score = matchScore(candidate, {
    title: "晴天",
    artist: "周杰伦",
    album: "叶惠美",
    durationS: 269,
  });
  expect(score).toBeGreaterThan(0.9);

  // Enabled lyrics providers
  const providers = enabledLyricsProviders();
  expect(providers.length).toBe(4);
  expect(enabledLyricsCount()).toBe(4);
});

test("startLyricsSearch 传给 engine.searchLyrics 的第 5 个参数确实是 string[] JSON", () => {
  let capturedArgs: [string, string, string, number | null, string | null | undefined] | null = null;
  const originalSearch = engine.searchLyrics;

  engine.searchLyrics = (artist: string, title: string, album: string, durationS: number | null, providersJson?: string | null) => {
    capturedArgs = [artist, title, album, durationS, providersJson];
  };

  engine.setPlayerForTest({
    state: "playing",
    path: SAMPLE_TRACKS[0].path,
    title: SAMPLE_TRACKS[0].title,
    artist: SAMPLE_TRACKS[0].artist,
    album: SAMPLE_TRACKS[0].album,
    durationMs: SAMPLE_TRACKS[0].durationMs,
  });

  try {
    startLyricsSearch({ artist: "周杰伦", title: "晴天", album: "叶惠美", durationS: 269 });

    expect(capturedArgs).not.toBeNull();
    const providersJson = capturedArgs![4];
    expect(typeof providersJson).toBe("string");
    const parsed = JSON.parse(providersJson!);
    expect(Array.isArray(parsed)).toBe(true);
    expect(parsed.length).toBe(4);
    for (const item of parsed) {
      expect(typeof item).toBe("string");
      expect(item.startsWith("builtin.lyrics.")).toBe(true);
    }
  } finally {
    engine.searchLyrics = originalSearch;
    engine.setPlayerForTest({ path: null, state: "stopped" });
  }
});

test("搜索结果采用到发起搜索的那首，切歌后不会写到新曲上", () => {
  const candidate = {
    id: 9,
    track: "晴天",
    artist: "周杰伦",
    album: "叶惠美",
    durationS: 269,
    synced: "[00:00.00]故事的小黄花",
    plain: null,
    provider: "builtin.lyrics.netease",
    providerName: "网易云音乐",
  };

  engine.setPlayerForTest({ path: "/music/a.flac", title: "晴天", state: "playing" });
  // The search remembers the track it asked about.
  startLyricsSearch({ artist: "周杰伦", title: "晴天", album: "叶惠美", durationS: 269 });

  // The track changes while that answer is on screen.
  engine.setPlayerForTest({ path: "/music/b.flac", title: "另一首", state: "playing" });
  adoptLyrics(candidate);

  // The new track must not inherit the previous search's words.
  expect(trackLyrics().present).toBe(false);

  // The track that was searched keeps them.
  engine.setPlayerForTest({ path: "/music/a.flac", title: "晴天", state: "playing" });
  expect(trackLyrics().present).toBe(true);
  expect(trackLyrics().lines[0]?.text).toBe("故事的小黄花");
});
