// The one contract between the UI and the engine: `src/backend/engine.ts`
// is the only file that calls `"neiro/native"`; the components in `src/pages`
// and `src/state` go through `backend` and the stores only. Field names here
// mirror the contract (`local://neiro-contract.md`) and the generated
// `neiro/native` d.ts, engine-reported values stay untouched: what Base did
// not report is `""`, `0` or `null`, never a plausible guess.

import type { Accessor } from "nsh";

// ---- The engine and the session -------------------------------------------

/** Where the engine is. The UI never builds without it: `failed` paints one
 * page that says so and how to fix it. */
export type HealthStatus = "starting" | "ready" | "failed";

/** The engine's state, honest: a failure keeps its raw text for the
 * "引擎未连接" page (never paraphrased into something softer). */
export interface Health {
  status: HealthStatus;
  error: string | null;
}

// ---- The player ------------------------------------------------------------

/** One frame of playback, exactly as Base reports it. */
export interface Player {
  state: PlayerEngineState;
  path: string | null;
  title: string;
  artist: string;
  album: string;
  positionMs: number;
  durationMs: number;
  at: number;
  volume: number | null;
  bitrateKbps: number | null;
  /** The codec the engine decoded with (`flac`, `alac`, `aac`), as the
   * analysis named it. Empty before one arrives. */
  codec: string;
  /** The codec's profile (`Dolby Digital Plus + Dolby Atmos`, `LC`, …),
   * empty before the analysis arrives. */
  codecProfile: string;
  trackSerial: number;
  /** 引擎 `NAE_ResolvedPath` 的 JSON 原文（每次 track 变化/重解析都换），
   * `onResolved` 通道与它同一内容。“输出为什么”的事实（垫帧数、缓冲缝
   * 隙、bit-perfect）在这里解析，不在界面发明。 */
  resolvedJson?: string | null;
}

export type PlayerEngineState =
  | "stopped"
  | "ready"
  | "playing"
  | "paused"
  | "deviceLost"
  | "ended"
  | "error";

/** A frame the backend mirrors between messages (spotish `EMPTY_PLAYER`). */
export const EMPTY_PLAYER: Player = {
  state: "stopped",
  path: null,
  title: "",
  artist: "",
  album: "",
  positionMs: 0,
  durationMs: 0,
  at: 0,
  volume: null,
  bitrateKbps: null,
  codec: "",
  codecProfile: "",
  trackSerial: 0,
};

// ---- The library -----------------------------------------------------------

export interface Track {
  path: string;
  title: string;
  artist: string;
  album: string;
  albumArtist: string;
  discNo: number;
  trackNo: number;
  year: number;
  genre: string;
  durationMs: number;
  sampleRate: number;
  bits: number;
  channels: number;
}

export interface Album {
  album: string;
  albumArtist: string;
  year: number;
  trackCount: number;
}

export interface Artist {
  artist: string;
  trackCount: number;
  albumCount: number;
}

/** One of the albums in an artist's page that the library groups together
 * (spotish `Item`, over Base's fields). */
export interface LibraryItem {
  kind: "album" | "artist";
  /** What names it and opens it: the artist's name, or the album pair. */
  id: string;
  name: string;
  subtitle: string;
  image?: string;
  year?: number;
  count: number;
}

export const albumKey = (t: { album: string; albumArtist: string }): string =>
  `${t.album}\u0000${t.albumArtist}`;

/** What a track's title line says: the tag's title, else the file's name. */
export function trackTitle(t: Track | undefined): string {
  if (!t) return "";
  if (t.title) return t.title;
  return fileNameOf(t.path);
}

/** What a track's second line says: the artists, else the file's folder. */
export function trackArtist(t: Track | undefined): string {
  if (!t) return "";
  return t.artist || t.albumArtist || "—";
}

export function fileNameOf(path: string): string {
  return path.replace(/\\/g, "/").split("/").pop() || path;
}

/** One page of the track index as `list_tracks` answered it. */
export interface TracksPage {
  tracks: Track[];
  total: number;
  offset: number;
}

export interface SearchResults {
  query: string;
  tracks: Track[];
}

// ---- The output ---------------------------------------------------------------

export interface RenderPath {
  sourceRate: number;
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  validBits: number;
  isFloat: boolean;
  resampling: boolean;
  dithered: boolean;
  bitPerfect: boolean;
  /** The engine's `NAE_REASON_*`: 0 when bit-perfect, otherwise why not. */
  reason: number;
  latencyMs: number;
  volume: number;
  eqEnabled: boolean;
  crossfeedEnabled: boolean;
  replayGainEnabled: boolean;
  convolverEnabled: boolean;
  crossfadeEnabled: boolean;
}

/** One tag field, as Base reports it. `value` is empty for a non-text item. */
export interface TagField {
  family: string;
  key: string;
  name: string;
  value: string;
}

/** A file's tag fields, after `readTags`. */
export interface TagFields {
  path: string;
  fields: TagField[];
}

export interface Device {
  id: string;
  name: string;
  sampleRate: number;
  bitsPerSample: number;
  validBits: number;
  channels: number;
  exclusive: boolean;
  shared: boolean;
  /** The rates this endpoint opens, ascending, as Base measured by opening
   * it. Empty means "not probed", never "supports nothing". */
  supportedSampleRates: number[];
}

export interface Queue {
  queue: string[];
  index: number | null;
}

/** 应用 DSP 参数，按 wire 的原样回读（引擎不发明默认值）。 */
export interface Settings {
  volume: number;
  eqEnabled: boolean;
  eqPreampDb: number;
  eqBands: EqBand[];
  crossfeedEnabled: boolean;
  crossfeedAmount: number;
  crossfeedDelayMs: number;
  crossfeedCutoffHz: number;
  /** `0` off、`1` track、`2` album。 */
  replayGainMode: number;
  replayGainPreampDb: number;
  replayGainPreventClipping: boolean;
  replayGainFallbackDb: number;
  convolverEnabled: boolean;
  convolverIrPath: string;
  convolverWet: number;
  convolverDry: number;
  convolverGainDb: number;
  convolverNormalize: boolean;
  crossfadeEnabled: boolean;
  crossfadeDurationMs: number;
  /** `0` linear、`1` equal power。 */
  crossfadeCurve: number;
}

/** 一段 EQ：wire 的原样字段（`freqHz`，不是 `freq`）。`kind` 为滤型枚举，
 * `channelMask` 的为位掩码。 */
export interface EqBand {
  kind: number;
  freqHz: number;
  gainDb: number;
  q: number;
  enabled: boolean;
  channelMask: number;
}

// ---- Artwork and the scan ----------------------------------------------------

export interface ArtworkList {
  path: string;
  pictures: { role: string; mime: string | null; size: number }[];
}

export interface ScanEvent {
  kind: "started" | "done" | "watched" | "changed";
  root: string;
  added: string[];
  modified: string[];
  removed: string[];
  roots: string[];
  rescan: boolean;
}

// ---- 可视化（频谱/包络；帧只在播放与订阅时到达） --------------------------------

/** 一帧可视化：`bins` 为线性幅值（`fftSize / 2` 个）；`envelope` 时域包络，
 * 老的在前；`framesFed` 与上帧比，知道是不是新音频。 */
export interface VisualFrame {
  bins: number[];
  fftSize: number;
  sampleRate: number;
  /** The maximum across channels; the per-channel pair is below. */
  peak: number;
  /** Per-channel peaks, or `null` when the engine does not report them: the
   * panel then draws one mono ladder instead of two invented ones. */
  peakL: number | null;
  peakR: number | null;
  rms: number;
  framesFed: number;
  envelope: number[];
  envelopeHopMs: number;
}

// ---- 曲库的用户层（v3） ------------------------------------------------------

/** The favourite paths, as `favourites_changed` delivered them (the whole
 * set; the UI diffs). */
export interface Favourites {
  paths: string[];
}

export interface Playlist {
  id: number;
  name: string;
  trackCount: number;
}

/** One entry of a playlist: where the file sits (position dense, kept so
 * by the engine on removal). */
export interface PlaylistEntry {
  path: string;
  position: number;
}

export interface PlaylistEntriesPage {
  id: number;
  entries: PlaylistEntry[];
}

/** One row of 最常播放, sorted by plays (the engine orders them). */
export interface MostPlayed {
  path: string;
  playCount: number;
}

// ---- 远程库（v2） ------------------------------------------------------------

/** One remote server's shape. */
export type RemoteKind = "jellyfin" | "navidrome";

/** Whether a server is configured (and which); never credentials. */
export interface RemoteStatus {
  configured: boolean;
  kind: RemoteKind | null;
  baseUrl: string;
}

/** One row of a remote listing. */
export interface RemoteTrack {
  id: string;
  title: string;
  artist: string;
  album: string;
  container: string;
  durationMs: number;
  size: number | null;
  coverArt: string | null;
}

export interface RemotePlaylist {
  id: string;
  name: string;
  trackCount: number;
  durationMs: number;
}

/** One playlist's items, as `remote_playlist_items` answered. */
export interface RemoteItemsPage {
  id: string;
  tracks: RemoteTrack[];
}

/** One downloaded remote track: the cached path is what `play(path)`
 * wants. */
export interface RemoteFetched {
  track: RemoteTrack;
  path: string;
}

/** One cover's answer: `data === null` means the server has no picture —
 * not an error. */
export interface RemoteArtwork {
  id: string;
  mime: string | null;
  data: string | null;
}

// ---- The contract -----------------------------------------------------------

/** What plays the music over the one Base the process owns (`engine.ts`).
 * Requests are sync: they throw `E_*` when the engine is gone. */
export interface Backend {
  readonly health: Accessor<Health>;
  /** Connects to the engine once (a fresh `Base`, its snapshot absorbed).
   * `failed` keeps its error; `retry()` can try again. */
  connect(): void;
  /** Stops listening and frees the one Base (a hot update or shutdown). */
  dispose(): void;
  /** Tries the engine again after `failed`. */
  retry(): void;

  /** Plays and its parts (the requests the protocol queues). */
  play(path: string): void;
  /** Appends paths to the existing queue (each one pre-analyzed). The order
   * is Base's business: play(第一首) + enqueue(其余) 就是"随机播放"的排列。 */
  enqueue(paths: string[]): void;
  /** Takes one entry out of the queue (the playing one is refused). */
  removeFromQueue(index: number): void;
  /** Moves one queue entry; the playing entry follows. */
  moveInQueue(from: number, to: number): void;
  /** Drops every entry but the one playing. */
  clearQueue(): void;
  pause(): void;
  resume(): void;
  stop(): void;
  next(): void;
  seek(seconds: number): void;
  setVolume(gain: number): void;
  listDevices(): void;
  setDevice(deviceId: string | null): void;
  /** 输出策略（引擎字段名的 JSON，例如 `{"rate":{"mode":"fixed",
   * "rate_hz":96000},"quality":"very_high","share":"exclusive"}`）。解析
   * 失败 E_*；引擎在当前进度重开流，结果回 `render_path`/resolved。 */
  setPolicy(policyJson: string): void;
  setEq(enabled: boolean, preampDb: number, bands: EqBand[]): void;
  setCrossfeed(enabled: boolean, amount: number, delayMs: number, cutoffHz: number): void;
  /** `mode`: `0` off、`1` track、`2` album。 */
  setReplayGain(mode: number, preampDb: number, preventClipping: boolean, fallbackDb: number): void;
  setConvolver(enabled: boolean, irPath: string, wet: number, dry: number, gainDb: number, normalize: boolean): void;
  /** `curve`: `0` linear、`1` equal power。 */
  setCrossfade(enabled: boolean, durationMs: number, curve: number): void;
  /** 频谱帧的转发开关；`null` 停。帧只在播放中到达（订阅期）——照
   * spotish 的性能规则：只在 Now Playing 可见且播放时开。 */
  setVisualiser(intervalMs: number | null): void;
  /** 无缓存的事件：只把它们丢给订阅者（过期的帧没有价值）。 */
  subscribeVisualFrame(sub: (frame: VisualFrame) => void): () => void;
  scanFolder(root: string): void;
  watchFolder(root: string): void;
  unwatchFolder(root: string): void;
  setReconcileInterval(seconds: number): void;
  pickFolder(): Promise<string | null>;

  // Data requests and their events (the fire-and-forget round trip).
  listTracks(offset: number, limit: number): void;
  listAlbums(): void;
  listArtists(): void;
  searchLibrary(query: string, limit: number): void;
  readArtworkList(path: string): void;
  readArtworkBytes(path: string, index: number): void;

  // 远程库（v2）
  setRemoteJellyfinPassword(baseUrl: string, user: string, password: string): void;
  setRemoteJellyfinToken(baseUrl: string, token: string): void;
  setRemoteNavidrome(baseUrl: string, user: string, password: string): void;
  clearRemote(): void;
  listRemote(start: number, limit: number): void;
  fetchRemote(id: string): void;
  fetchRemoteArtwork(id: string): void;
  listRemotePlaylists(): void;
  listRemotePlaylistItems(id: string, start: number, limit: number): void;

  // 曲库的用户层（v3）
  setFavourite(path: string, on: boolean): void;
  listFavourites(): void;
  listPlaylists(): void;
  createPlaylist(name: string): void;
  renamePlaylist(id: number, name: string): void;
  deletePlaylist(id: number): void;
  addToPlaylist(id: number, path: string): void;
  removeFromPlaylist(id: number, path: string): void;
  listPlaylistEntries(id: number): void;
  listMostPlayed(limit: number): void;
  recordPlay(path: string, atMs: number): void;
}

// ---- Helpers on the models --------------------------------------------------

/** The `E_*` of a rejection, with its message; `E_UNKNOWN` for anything
 * that is not in that shape. */
export interface EngineErrorInfo {
  code: string;
  message: string;
}

export function errorOf(error: unknown): EngineErrorInfo {
  const text = error instanceof Error ? error.message : String(error);
  const match = /^(E_[A-Z0-9_]+):\s*([\s\S]*)$/.exec(text);
  if (match) return { code: match[1], message: match[2] };
  return { code: "E_UNKNOWN", message: text };
}

/** Where `p` is at the time `t`: moved on from when it was sampled while it
 * plays, never past its end. */
export function positionAt(p: Player, t: number): number {
  const playing = p.state === "playing";
  const elapsed = playing && p.at > 0 ? Math.max(0, t - p.at) : 0;
  const position = p.positionMs + elapsed;
  return p.durationMs > 0 ? Math.min(position, p.durationMs) : position;
}

/** `m:ss`, or `h:mm:ss` past an hour; a duration Base never gave shows —. */
export function clock(ms: number, unknown = false): string {
  if (!ms && unknown) return "—";
  const seconds = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds / 60) % 60);
  const s = seconds % 60;
  const pad = (n: number) => String(n).padStart(2, "0");
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
}
