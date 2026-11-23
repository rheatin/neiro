// `engine.ts`: the one file that talks to the engine. It owns the process's
// single `Base` (a `globalThis` slot keeps a hot update from making a
// second one, spotish's rule), mirrors what Base reports into signals, and
// fans the pure events (artwork, scan, error) out to the stores that
// subscribe.
//
// Without `neiro_base.dll` the construction throws `E_ENGINE…`: this keeps
// it, sets `health` to `"failed"` and adds no listeners, so every page
// renders the honest "引擎未连接" paint instead of plausible data. `retry()`
// closes what is there and builds again.

import { signal, Storage, type Accessor } from "nsh";
import type { LyricsCandidate, RemoteProbe, TagFields } from "./types";
import {
  Base,
  type Album as NativeAlbum,
  type ArtworkBytes as NativeArtworkBytes,
  type ArtworkList as NativeArtworkList,
  type Artist as NativeArtist,
  type DeviceInfo as NativeDevice,
  type PlayerState as NativePlayer,
  type QueueInfo as NativeQueue,
  type RemoteArtwork as NativeRemoteArtwork,
  type RemoteFetched as NativeRemoteFetched,
  type RemoteItemsPage as NativeRemoteItemsPage,
  type RemotePlaylist as NativeRemotePlaylist,
  type RemoteStatus as NativeRemoteStatus,
  type RemoteTrack as NativeRemoteTrack,
  type RenderPathInfo as NativeRenderPath,
  type ScanEvent as NativeScanEvent,
  type SearchPage as NativeSearch,
  type SettingsInfo as NativeSettings,
  type Track as NativeTrack,
  type TracksPage as NativeTracksPage,
  type Favourites as NativeFavourites,
  type Playlist as NativePlaylist,
  type PlaylistEntriesPage as NativePlaylistEntriesPage,
  type MostPlayed as NativeMostPlayed,
} from "neiro/native";
import { notify } from "../state/feedback";
import {
  type VisualFrame,
  albumKey,
  EMPTY_PLAYER,
  errorOf,
  type Album,
  type Artist,
  type ArtworkList,
  type Backend,
  type Device,
  type DirectoryListing,
  type EqBand,
  type Favourites,
  type FolderAccess,
  type Health,
  type MostPlayed,
  type PickedFolder,
  type Player,
  type Playlist,
  type PlaylistEntriesPage,
  type PluginInfo,
  type ProviderDescriptor,
  type ProviderField,
  type Queue,
  type QueueItem,
  type QueueSnapshot,
  type RemoteArtwork,
  type RemoteFetched,
  type RemoteItemsPage,
  type RemoteKind,
  type RemotePlaylist,
  type RemoteStatus,
  type RemoteTrack,
  type RenderPath,
  type RepeatMode,
  type ScanEvent,
  type SearchResults,
  type Settings,
  type Track,
  type TracksPage,
} from "./types";

const SLOT_KEY = "__neiroEngine";

/** A raw failure Base reported (`error` events, an `op` names what it was
 * doing). */
export interface EngineOpError {
  op: string;
  message: string;
}

type Fanout<T> = (value: T) => void;

/** The num of one track page the engine answers with; UI pages are this big.
 * The contract caps a page at 500. */
export const TRACK_PAGE = 500;

// The mirrored state. One signal per piece (one owner), made once at module
// scope so a hot update reuses them.
const healthSig = signal<Health>({ status: "starting", error: null });
const playerSig = signal<Player>({ ...EMPTY_PLAYER, at: Date.now() });
const renderPathSig = signal<RenderPath | null>(null);
const devicesSig = signal<Device[]>([]);
const DEVICES_KEY = "audio:devices";

// The last enumeration, read back at start: enumeration opens every endpoint,
// which cannot be done while a stream holds one, and an empty list would read
// as "this machine has none".
Storage.getItem(DEVICES_KEY)
  .then((saved) => {
    if (!saved) return;
    const parsed: unknown = JSON.parse(saved);
    if (!Array.isArray(parsed)) return;
    const rows = (parsed as Device[]).filter(
      (row) => row && typeof row.id === "string" && typeof row.name === "string",
    );
    if (rows.length > 0) devicesSig.set(rows);
  })
  .catch((error) => console.warn("neiro: 端点列表没有读回来:", String(error)));
const queueSig = signal<Queue>({ queue: [], index: null });
const settingsSig = signal<Settings | null>(null);
const albumsSig = signal<Album[]>([]);
const artistsSig = signal<Artist[]>([]);
const searchSig = signal<SearchResults | null>(null);
const tracksSig = signal<TracksPage | null>(null);
const favouritesSig = signal<Favourites>({ paths: [] });
const playlistsSig = signal<Playlist[]>([]);
const playlistEntriesSig = signal<PlaylistEntriesPage | null>(null);
const mostPlayedSig = signal<MostPlayed[]>([]);
const remoteStatusSig = signal<RemoteStatus>({ configured: false, kind: null, baseUrl: "" });
const remoteTracksSig = signal<RemoteTrack[]>([]);
/** The last online lyrics search's answer. */
const lyricsFoundSig = signal<LyricsCandidate[]>([]);
/** The last 测试连接 outcome; `null` before one has run. */
const remoteProbeSig = signal<RemoteProbe | null>(null);
const remotePlaylistsSig = signal<RemotePlaylist[]>([]);
const remoteItemsSig = signal<RemoteItemsPage | null>(null);
const remoteFetchedSig = signal<RemoteFetched | null>(null);

function num(value: number | undefined | null): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

function text(value: string | null | undefined): string {
  return typeof value === "string" ? value : "";
}

/** Not a plausible default: Base reports `null` where it measured nothing
 * and the UI draws what it got. */
function gain(value: number | null | undefined): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

let trackLookup: ((path: string) => { title?: string; artist?: string; album?: string } | undefined) | null = null;

export function registerTrackLookup(
  lookup: (path: string) => { title?: string; artist?: string; album?: string } | undefined,
): void {
  trackLookup = lookup;
}

/** A remote track's cached file is named by the server's id; the listing is
 * the metadata the listener browsed. It wins over the file's own tags, so a
 * stale/preloaded analysis cannot put the previous song's title on it — and
 * the cover (keyed by the same listing) stays in step with the title. */
let remoteTrackLookup:
  | ((path: string) => { title?: string; artist?: string; album?: string } | undefined)
  | null = null;

export function registerRemoteTrackLookup(
  lookup: (path: string) => { title?: string; artist?: string; album?: string } | undefined,
): void {
  remoteTrackLookup = lookup;
}

function playerOf(n: NativePlayer | null | undefined): Player {
  if (!n) return { ...EMPTY_PLAYER, at: Date.now() };
  let title = text(n.title);
  let artist = text(n.artist);
  let album = text(n.album);
  const path = n.path ?? null;
  if (path && (!title || !artist || !album)) {
    const fallback = trackLookup?.(path);
    if (fallback) {
      if (!title && fallback.title) title = fallback.title;
      if (!artist && fallback.artist) artist = fallback.artist;
      if (!album && fallback.album) album = fallback.album;
    }
  }
  if (path) {
    const remote = remoteTrackLookup?.(path);
    if (remote) {
      if (remote.title) title = remote.title;
      if (remote.artist) artist = remote.artist;
      if (remote.album) album = remote.album;
    }
  }
  return {
    state: (text(n.state) || "stopped") as Player["state"],
    path,
    title,
    artist,
    album,
    positionMs: num(n.positionMs),
    durationMs: num(n.durationMs),
    at: num(n.at) || Date.now(),
    volume: gain(n.volume),
    bitrateKbps: gain(n.bitrateKbps),
    codec: text(n.codec),
    codecProfile: text(n.codecProfile),
    trackSerial: num(n.trackSerial),
    resolvedJson: n.resolvedJson ?? null,
  };
}

function trackOf(n: NativeTrack): Track {
  return {
    path: text(n.path),
    title: text(n.title),
    artist: text(n.artist),
    album: text(n.album),
    albumArtist: text(n.albumArtist),
    discNo: num(n.discNo),
    trackNo: num(n.trackNo),
    year: num(n.year),
    genre: text(n.genre),
    durationMs: num(n.durationMs),
    sampleRate: num(n.sampleRate),
    bits: num(n.bits),
    channels: num(n.channels),
  };
}

function tracksPageOf(n: NativeTracksPage): TracksPage {
  return {
    tracks: (n.tracks ?? []).map(trackOf),
    total: num(n.total),
    offset: num(n.offset),
  };
}

// Album rows keep the identity the library pages group by.
function albumOf(n: NativeAlbum): Album {
  return {
    album: text(n.album),
    albumArtist: text(n.albumArtist),
    year: num(n.year),
    trackCount: num(n.trackCount),
  };
}

function artistOf(n: NativeArtist): { artist: string; trackCount: number; albumCount: number } {
  return { artist: text(n.artist), trackCount: num(n.trackCount), albumCount: num(n.albumCount) };
}

function renderPathOf(n: NativeRenderPath | null | undefined): RenderPath | null {
  if (!n) return null;
  return {
    sourceRate: num(n.sourceRate),
    sampleRate: num(n.sampleRate),
    channels: num(n.channels),
    bitsPerSample: num(n.bitsPerSample),
    validBits: num(n.validBits),
    isFloat: n.isFloat ?? false,
    resampling: n.resampling ?? false,
    dithered: n.dithered ?? false,
    bitPerfect: n.bitPerfect ?? false,
    reason: num(n.reason),
    latencyMs: num(n.latencyMs),
    volume: num(n.volume),
    eqEnabled: n.eqEnabled ?? false,
    crossfeedEnabled: n.crossfeedEnabled ?? false,
    replayGainEnabled: n.replayGainEnabled ?? false,
    convolverEnabled: n.convolverEnabled ?? false,
    crossfadeEnabled: n.crossfadeEnabled ?? false,
  };
}

function deviceOf(n: NativeDevice): Device {
  return {
    id: text(n.id),
    name: text(n.name),
    sampleRate: num(n.sampleRate),
    bitsPerSample: num(n.bitsPerSample),
    validBits: num(n.validBits),
    channels: num(n.channels),
    exclusive: n.exclusive ?? false,
    shared: n.shared ?? false,
    supportedSampleRates: (n.supportedSampleRates ?? []).map(num).filter((rate) => rate > 0),
  };
}

function queueOf(n: NativeQueue | null | undefined): Queue {
  let snapshot: QueueSnapshot | null = null;
  if (n?.snapshotJson) {
    try {
      const parsed = JSON.parse(n.snapshotJson);
      snapshot = {
        current: parsed.current ?? null,
        userQueue: parsed.user_queue ?? parsed.userQueue ?? [],
        contextQueue: parsed.context_queue ?? parsed.contextQueue ?? [],
        history: parsed.history ?? [],
        repeatMode: (parsed.repeat_mode ?? parsed.repeatMode ?? "off").toLowerCase() as RepeatMode,
        shuffleEnabled: parsed.shuffle_enabled ?? parsed.shuffleEnabled ?? false,
      };
    } catch (e) {
      console.warn("neiro: 解析 queue snapshotJson 失败:", String(e));
    }
  }
  return {
    queue: (n?.queue ?? []).map(text),
    index: typeof n?.index === "number" ? n.index : null,
    snapshot,
  };
}

function settingsOf(n: NativeSettings | null | undefined): Settings | null {
  if (!n) return null;
  return {
    volume: num(n.volume),
    eqEnabled: n.eqEnabled ?? false,
    eqPreampDb: num(n.eqPreampDb),
    eqBands: (n.eqBands ?? []).map((b) => ({
      kind: num(b.kind),
      freqHz: num(b.freqHz),
      gainDb: num(b.gainDb),
      q: num(b.q),
      enabled: b.enabled ?? false,
      channelMask: num(b.channelMask),
    })),
    crossfeedEnabled: n.crossfeedEnabled ?? false,
    crossfeedAmount: num(n.crossfeedAmount),
    crossfeedDelayMs: num(n.crossfeedDelayMs),
    crossfeedCutoffHz: num(n.crossfeedCutoffHz),
    replayGainMode: num(n.replayGainMode),
    replayGainPreampDb: num(n.replayGainPreampDb),
    replayGainPreventClipping: n.replayGainPreventClipping ?? false,
    replayGainFallbackDb: num(n.replayGainFallbackDb),
    convolverEnabled: n.convolverEnabled ?? false,
    convolverIrPath: text(n.convolverIrPath),
    convolverWet: num(n.convolverWet),
    convolverDry: num(n.convolverDry),
    convolverGainDb: num(n.convolverGainDb),
    convolverNormalize: n.convolverNormalize ?? false,
    crossfadeEnabled: n.crossfadeEnabled ?? false,
    crossfadeDurationMs: num(n.crossfadeDurationMs),
    crossfadeCurve: num(n.crossfadeCurve),
  };
}

function scanOf(n: NativeScanEvent): ScanEvent {
  return {
    kind: (text(n.kind) || "started") as ScanEvent["kind"],
    root: text(n.root),
    added: n.added ?? [],
    modified: n.modified ?? [],
    removed: n.removed ?? [],
    roots: n.roots ?? [],
    rescan: n.rescan ?? false,
  };
}

function artworkListOf(n: NativeArtworkList): ArtworkList {
  return {
    path: text(n.path),
    pictures: (n.pictures ?? []).map((p) => ({ role: text(p.role), mime: p.mime ?? null, size: num(p.size) })),
  };
}

// ---- 远程库（v2） -----------------------------------------------------------

function remoteStatusOf(n: NativeRemoteStatus | undefined | null): RemoteStatus {
  const kind = text(n?.kind);
  return {
    configured: n?.configured ?? false,
    kind: kind === "jellyfin" || kind === "navidrome" || kind === "webdav" ? (kind as RemoteKind) : null,
    baseUrl: text(n?.baseUrl),
  };
}

function lyricsCandidateOf(n: any): LyricsCandidate {
  return {
    id: typeof n?.id === "number" ? n.id : 0,
    track: text(n?.track),
    artist: text(n?.artist),
    album: text(n?.album),
    durationS: typeof n?.durationS === "number" ? n.durationS : 0,
    synced: typeof n?.synced === "string" ? n.synced : null,
    plain: typeof n?.plain === "string" ? n.plain : null,
    provider: typeof n?.provider === "string" ? n.provider : undefined,
    providerName: typeof n?.providerName === "string" ? n.providerName : undefined,
  };
}

function remoteTrackOf(n: NativeRemoteTrack): RemoteTrack {
  return {
    id: text(n.id),
    title: text(n.title),
    artist: text(n.artist),
    album: text(n.album),
    container: text(n.container),
    durationMs: num(n.durationMs),
    size: typeof n.size === "number" ? n.size : null,
    coverArt: typeof n.coverArt === "string" ? n.coverArt : null,
  };
}

function remotePlaylistOf(n: NativeRemotePlaylist): RemotePlaylist {
  return { id: text(n.id), name: text(n.name), trackCount: num(n.trackCount), durationMs: num(n.durationMs) };
}

function remoteItemsOf(n: NativeRemoteItemsPage | undefined | null): RemoteItemsPage | null {
  if (!n) return null;
  return { id: text(n.id), tracks: (n.tracks ?? []).map(remoteTrackOf) };
}

function remoteFetchedOf(n: NativeRemoteFetched | undefined | null): RemoteFetched | null {
  if (!n || !n.track) return null;
  return { track: remoteTrackOf(n.track), path: text(n.path) };
}

function remoteArtworkOf(n: NativeRemoteArtwork | undefined | null): RemoteArtwork | null {
  if (!n) return null;
  return { id: text(n.id), mime: n.mime ?? null, data: n.data ?? null };
}

// ---- 曲库的用户层（v3） --------------------------------------------------------

function favouritesOf(n: NativeFavourites | undefined | null): Favourites {
  return { paths: (n?.paths ?? []).filter((p) => typeof p === "string") };
}

function playlistOf(n: NativePlaylist): Playlist {
  return { id: num(n.id), name: text(n.name), trackCount: num(n.trackCount) };
}

function playlistEntriesOf(n: NativePlaylistEntriesPage | undefined | null): PlaylistEntriesPage | null {
  if (!n) return null;
  return {
    id: num(n.id),
    entries: (n.entries ?? []).map((entry) => ({ path: text(entry.path), position: num(entry.position) })),
  };
}

function mostPlayedOf(n: NativeMostPlayed): MostPlayed {
  return { path: text(n.path), playCount: num(n.playCount) };
}

/** The engine store: the one Base, the signals it feeds, and the requests. */
export class Engine implements Backend {
  readonly health: Accessor<Health> = () => healthSig();
  readonly player: Accessor<Player> = () => playerSig();
  readonly renderPath: Accessor<RenderPath | null> = () => renderPathSig();
  readonly devices: Accessor<Device[]> = () => devicesSig();
  /** One page of the track index as the engine last answered it: the
   * catalog store stitches pages together. */
  readonly tracksPage: Accessor<TracksPage | null> = () => tracksSig();
  readonly albums: Accessor<Album[]> = () => albumsSig();
  readonly artists: Accessor<Artist[]> = () => artistsSig();
  readonly search: Accessor<SearchResults | null> = () => searchSig();
  readonly queue: Accessor<Queue> = () => queueSig();
  readonly settings: Accessor<Settings | null> = () => settingsSig();
  readonly remoteStatus: Accessor<RemoteStatus> = () => remoteStatusSig();
  readonly remoteTracks: Accessor<RemoteTrack[]> = () => remoteTracksSig();
  readonly remoteProbe: Accessor<RemoteProbe | null> = () => remoteProbeSig();
  readonly remotePlaylists: Accessor<RemotePlaylist[]> = () => remotePlaylistsSig();
  readonly remoteItems: Accessor<RemoteItemsPage | null> = () => remoteItemsSig();
  readonly remoteFetched: Accessor<RemoteFetched | null> = () => remoteFetchedSig();
  readonly favourites: Accessor<Favourites> = () => favouritesSig();
  readonly playlists: Accessor<Playlist[]> = () => playlistsSig();
  readonly playlistEntries: Accessor<PlaylistEntriesPage | null> = () => playlistEntriesSig();
  readonly mostPlayed: Accessor<MostPlayed[]> = () => mostPlayedSig();

  #base: Base | null = null;
  #stops: (() => void)[] = [];
  #resolvedSubs = new Set<Fanout<string>>();
  #tagSubs = new Set<Fanout<TagFields>>();
  #visualSubs = new Set<Fanout<VisualFrame>>();
  #artListSubs = new Set<Fanout<ArtworkList>>();
  #artDataSubs = new Set<Fanout<NativeArtworkBytes>>();
  #scanSubs = new Set<Fanout<ScanEvent>>();
  #errorSubs = new Set<Fanout<EngineOpError>>();
  #remoteFetchedSubs = new Set<Fanout<RemoteFetched>>();
  #remoteArtworkSubs = new Set<Fanout<RemoteArtwork | null>>();
  #lyricsFoundSubs = new Set<Fanout<LyricsCandidate[]>>();
  #remoteProbeSubs = new Set<Fanout<RemoteProbe>>();

  /** Builds the Base, absorbs its snapshot through the first listener calls
   * and reports `failed` with the engine's own words when there is none. */
  connect(): void {
    this.dispose();
    try {
      this.#base = new Base();
    } catch (error) {
      const text = error instanceof Error ? error.message : String(error);
      healthSig.set({ status: "failed", error: text });
      return;
    }
    const base = this.#base;
    this.#stops.push(
      base.onPlayer((p) => playerSig.set(playerOf(p))),
      base.onRenderPath((r) => renderPathSig.set(renderPathOf(r))),
      base.onDevices((d) => {
        const rows = (d ?? []).map(deviceOf);
        devicesSig.set(rows);
        if (rows.length > 0) {
          void Storage.setItem(DEVICES_KEY, JSON.stringify(rows)).catch((error) =>
            console.warn("neiro: 端点列表没有记住:", String(error)),
          );
        }
      }),
      base.onQueue((q) => queueSig.set(queueOf(q))),
      base.onSettings((s) => settingsSig.set(settingsOf(s))),
      base.onTracks((page) => tracksSig.set(page ? tracksPageOf(page) : null)),
      base.onAlbums((a) => albumsSig.set((a ?? []).map(albumOf))),
      base.onArtists((a) => artistsSig.set((a ?? []).map(artistOf))),
      base.onSearch((s) =>
        searchSig.set({ query: text(s?.query), tracks: (s?.tracks ?? []).map(trackOf) }),
      ),
      base.onArtworkList((list) => this.#fan(this.#artListSubs, artworkListOf(list))),
      base.onArtworkData((data) => this.#fan(this.#artDataSubs, data)),
      base.onScan((event) => this.#fan(this.#scanSubs, scanOf(event))),
      base.onError((e) => this.#fan(this.#errorSubs, { op: text(e?.op), message: text(e?.message) })),
      base.onRemote((s) => remoteStatusSig.set(remoteStatusOf(s))),
      base.onRemoteTracks((t) => remoteTracksSig.set((t ?? []).map(remoteTrackOf))),
      base.onRemoteProbed((p) => {
        const probed: RemoteProbe = {
          ok: p?.ok === true,
          message: text(p?.message),
          count: typeof p?.count === "number" ? p.count : 0,
        };
        remoteProbeSig.set(probed);
        this.#fan(this.#remoteProbeSubs, probed);
      }),
      base.onLyricsFound((list) => {
        lyricsFoundSig.set((list ?? []).map(lyricsCandidateOf));
        this.#fan(this.#lyricsFoundSubs, lyricsFoundSig());
      }),
      base.onRemotePlaylists((p) => remotePlaylistsSig.set((p ?? []).map(remotePlaylistOf))),
      base.onRemoteItems((page) => remoteItemsSig.set(remoteItemsOf(page))),
      base.onRemoteFetched((f) => {
        const mapped = remoteFetchedOf(f);
        remoteFetchedSig.set(mapped);
        if (mapped) this.#fan(this.#remoteFetchedSubs, mapped);
      }),
      base.onRemoteArtwork((a) => this.#fan(this.#remoteArtworkSubs, remoteArtworkOf(a))),
      base.onFavourites((f) => favouritesSig.set(favouritesOf(f))),
      base.onPlaylists((p) => playlistsSig.set((p ?? []).map(playlistOf))),
      base.onPlaylistEntries((page) => playlistEntriesSig.set(playlistEntriesOf(page))),
      base.onMostPlayed((m) => mostPlayedSig.set((m ?? []).map(mostPlayedOf))),
      base.onResolved((text) => this.#fan(this.#resolvedSubs, typeof text === "string" ? text : "")),
      base.onTags((page) =>
        this.#fan(this.#tagSubs, {
          path: text(page?.path),
          fields: (page?.fields ?? []).map((field) => ({
            family: text(field?.family),
            key: text(field?.key),
            name: text(field?.name),
            value: text(field?.value),
          })),
        }),
      ),
      base.onVisualFrame((frame) => {
        if (!frame) return;
        this.#fan(
          this.#visualSubs,
          {
            bins: (frame.bins ?? []).map((b) => num(b)),
            fftSize: num(frame.fftSize),
            sampleRate: num(frame.sampleRate),
            peak: num(frame.peak),
            peakL: frame.peakL === null || frame.peakL === undefined ? null : num(frame.peakL),
            peakR: frame.peakR === null || frame.peakR === undefined ? null : num(frame.peakR),
            rms: num(frame.rms),
            framesFed: num(frame.framesFed),
            envelope: (frame.envelope ?? []).map((e) => num(e)),
            envelopeHopMs: num(frame.envelopeHopMs),
          } as VisualFrame,
        );
      }),
    );
    healthSig.set({ status: "ready", error: null });
  }

  /** Stops listening and frees the one Base (never on a hot update's
   * behalf: the slot below keeps the store alive). */
  dispose(): void {
    for (const stop of this.#stops.splice(0)) stop();
    if (this.#base) {
      this.#base.shutdown();
      this.#base.dispose();
      this.#base = null;
    }
  }

  /** Tries the engine again: a fresh Base, its snapshot, the same signals. */
  retry(): void {
    this.connect();
  }

  #fan<T>(subs: Set<Fanout<T>>, value: T): void {
    for (const sub of subs) sub(value);
  }

  /** The engine's own page of pure events: Base answers every artwork ask
   * on one channel keyed by path, and reports the scan and its errors as
   * they happen. The stores subscribe; the store is responsible for
   * unsubscribing when it goes. */
  subscribeArtworkList(sub: Fanout<ArtworkList>): () => void {
    this.#artListSubs.add(sub);
    return () => this.#artListSubs.delete(sub);
  }

  /** Asks the server one item; the outcome arrives on `subscribeRemoteProbe`. */
  probeRemote(): void {
    this.#command((base) => base.probeRemote());
  }

  subscribeRemoteProbe(sub: Fanout<RemoteProbe>): () => void {
    this.#remoteProbeSubs.add(sub);
    return () => this.#remoteProbeSubs.delete(sub);
  }

  /** The answer to one online lyrics search, as the provider returned it. */
  subscribeLyricsFound(sub: Fanout<LyricsCandidate[]>): () => void {
    this.#lyricsFoundSubs.add(sub);
    return () => this.#lyricsFoundSubs.delete(sub);
  }

  subscribeArtworkData(sub: Fanout<NativeArtworkBytes>): () => void {
    this.#artDataSubs.add(sub);
    return () => this.#artDataSubs.delete(sub);
  }

  subscribeScan(sub: Fanout<ScanEvent>): () => void {
    this.#scanSubs.add(sub);
    return () => this.#scanSubs.delete(sub);
  }

  subscribeEngineError(sub: Fanout<EngineOpError>): () => void {
    this.#errorSubs.add(sub);
    return () => this.#errorSubs.delete(sub);
  }

  /** One remote track just landed in the cache (path included): the
   * player plays it from here. */
  subscribeRemoteFetched(sub: Fanout<RemoteFetched>): () => void {
    this.#remoteFetchedSubs.add(sub);
    return () => this.#remoteFetchedSubs.delete(sub);
  }

  /** One remote cover answered (data === null: the server has no
   * picture). */
  subscribeRemoteArtwork(sub: Fanout<RemoteArtwork | null>): () => void {
    this.#remoteArtworkSubs.add(sub);
    return () => this.#remoteArtworkSubs.delete(sub);
  }

  /** One re-resolve: the whole diagnostics JSON for the current stream, as
   * a string (它与 `player().resolvedJson` 同一内容，事件线给它"为什么"
   * 的最新值——播放中的每一次重新解析都到）。 */
  /** A file's tag fields, answered once per `readTags`. */
  subscribeTags(sub: Fanout<TagFields>): () => void {
    this.#tagSubs.add(sub);
    return () => this.#tagSubs.delete(sub);
  }

  subscribeResolved(sub: Fanout<string>): () => void {
    this.#resolvedSubs.add(sub);
    return () => this.#resolvedSubs.delete(sub);
  }

  /** 可视化帧：无缓存、无首呼（一个过期帧没有价值）；只在播放与已订阅
   * 时到达。订阅者自己也负责 life time（视觉状态机挂在 Now Playing 的
   * open/playing 上）。 */
  subscribeVisualFrame(sub: Fanout<VisualFrame>): () => void {
    this.#visualSubs.add(sub);
    return () => this.#visualSubs.delete(sub);
  }

  // ---- Requests ------------------------------------------------------------

  /** Sends a command; the engine reports the change as an event. When there
   * is no engine (`failed`, or before `connect`), a refusal that says so. */
  #command(run: (base: Base) => void): void {
    const base = this.#base;
    if (!base) throw new Error("E_ENGINE: 引擎未连接。先构建 ../Neiro/base，然后重试。");
    run(base);
  }

  play(path: string): void {
    this.#command((base) => base.play(path));
  }

  enqueue(paths: string[]): void {
    this.#command((base) => base.enqueue(paths));
  }

  removeFromQueue(index: number): void {
    this.#command((base) => base.removeFromQueue(index));
  }

  moveInQueue(from: number, to: number): void {
    this.#command((base) => base.moveInQueue(from, to));
  }

  clearQueue(): void {
    this.#command((base) => base.clearQueue());
  }

  pause(): void {
    this.#command((base) => base.pause());
  }

  resume(): void {
    this.#command((base) => base.resume());
  }

  stop(): void {
    this.#command((base) => base.stop());
  }

  next(): void {
    this.#command((base) => base.next());
  }

  previous(): void {
    this.#command((base) => base.previous());
  }

  playContext(items: QueueItem[], startIndex = 0): void {
    const serialized = items.map((i) => ({
      ...i,
      duration_ms: i.duration_ms ?? i.durationMs ?? 0,
      durationMs: i.durationMs ?? i.duration_ms ?? 0,
    }));
    this.#command((base) => base.playContext(JSON.stringify(serialized), startIndex));
  }

  playNext(items: QueueItem[]): void {
    const serialized = items.map((i) => ({
      ...i,
      duration_ms: i.duration_ms ?? i.durationMs ?? 0,
      durationMs: i.durationMs ?? i.duration_ms ?? 0,
    }));
    this.#command((base) => base.playNext(JSON.stringify(serialized)));
  }

  playLater(items: QueueItem[]): void {
    const serialized = items.map((i) => ({
      ...i,
      duration_ms: i.duration_ms ?? i.durationMs ?? 0,
      durationMs: i.durationMs ?? i.duration_ms ?? 0,
    }));
    this.#command((base) => base.playLater(JSON.stringify(serialized)));
  }

  setRepeatMode(mode: RepeatMode): void {
    const num = mode === "all" ? 1 : mode === "one" ? 2 : 0;
    this.#command((base) => base.setRepeatMode(num));
  }

  setShuffleMode(enabled: boolean): void {
    this.#command((base) => base.setShuffleMode(enabled));
  }

  removeQueueItem(id: string): void {
    this.#command((base) => base.removeQueueItem(id));
  }

  clearUserQueue(): void {
    this.#command((base) => base.clearUserQueue());
  }

  moveUserQueueItem(from: number, to: number): void {
    this.#command((base) => base.moveUserQueueItem(from, to));
  }

  seek(seconds: number): void {
    this.#command((base) => base.seek(seconds));
  }

  releaseDevice(): void {
    this.#command((base) => base.releaseDevice());
  }

  setVolume(gainValue: number): void {
    this.#command((base) => base.setVolume(gainValue));
  }

  listDevices(): void {
    this.#command((base) => base.listDevices());
  }

  /** Asks for one file's tag fields; answered on `subscribeTags`. */
  readTags(path: string): void {
    this.#command((base) => base.readTags(path));
  }

  setDevice(deviceId: string | null): void {
    this.#command((base) => base.setDevice(deviceId));
  }

  /** Changes the output policy (rate 模式 native/device/fixed、质量、共
   * 享)。`policyjson` 由界面按引擎的字段名组合；引擎做下一次在当前进度
   * 重开流。 */
  setPolicy(policyJson: string): void {
    this.#command((base) => base.setPolicy(policyJson));
  }

  setEq(enabled: boolean, preampDb: number, bands: EqBand[]): void {
    this.#command((base) => base.setEq(enabled, preampDb, bands));
  }

  setCrossfeed(enabled: boolean, amount: number, delayMs: number, cutoffHz: number): void {
    this.#command((base) => base.setCrossfeed(enabled, amount, delayMs, cutoffHz));
  }

  /** `mode`: `0` off、`1` track、`2` album。 */
  setReplayGain(mode: number, preampDb: number, preventClipping: boolean, fallbackDb: number): void {
    this.#command((base) => base.setReplayGain(mode, preampDb, preventClipping, fallbackDb));
  }

  setConvolver(enabled: boolean, irPath: string, wet: number, dry: number, gainDb: number, normalize: boolean): void {
    this.#command((base) => base.setConvolver(enabled, irPath, wet, dry, gainDb, normalize));
  }

  /** `curve`: `0` linear、`1` equal power。 */
  setCrossfade(enabled: boolean, durationMs: number, curve: number): void {
    this.#command((base) => base.setCrossfade(enabled, durationMs, curve));
  }

  /** `null` 停；16 = 60 Hz。只在订阅的 UI 可见时开（Now Playing 且播放）。 */
  setVisualiser(intervalMs: number | null): void {
    this.#command((base) => base.setVisualiser(intervalMs));
  }

  scanFolder(root: string): void {
    this.#command((base) => base.scanFolder(root));
  }

  watchFolder(root: string): void {
    this.#command((base) => base.watchFolder(root));
  }

  /** Stop watching a folder and delete what was indexed under it. */
  forgetFolder(root: string): void {
    this.#command((base) => base.forgetFolder(root));
  }

  unwatchFolder(root: string): void {
    this.#command((base) => base.unwatchFolder(root));
  }

  setReconcileInterval(seconds: number): void {
    this.#command((base) => base.setReconcileInterval(seconds));
  }

  /** The system's file dialog for a convolver IR; `null` on cancel. */
  async pickIrFile(): Promise<string | null> {
    const base = this.#base;
    if (!base) throw new Error("E_ENGINE: 引擎未连接");
    return base.pickIrFile();
  }

  /** The system's folder dialog; `null` when the user cancelled. */
  async pickFolder(): Promise<string | null> {
    const base = this.#base;
    if (!base) throw new Error("E_ENGINE: 引擎未连接");
    return base.pickFolder();
  }

  /** iOS's folder picker, with the security-scoped bookmark the library
   * keeps instead of a path; `null` when the user cancelled. */
  async pickFolderBookmark(): Promise<PickedFolder | null> {
    const base = this.#base;
    if (!base) throw new Error("E_ENGINE: 引擎未连接");
    const picked = await base.pickFolderBookmark();
    return picked
      ? { path: text(picked.path), bookmark: text(picked.bookmark), label: text(picked.label) }
      : null;
  }

  /** Resolves a stored bookmark and opens the app's access to the folder;
   * `null` when it no longer resolves (pick it again). */
  resolveFolderBookmark(bookmark: string): FolderAccess | null {
    const base = this.#base;
    if (!base) throw new Error("E_ENGINE: 引擎未连接");
    const access = base.resolveFolderBookmark(bookmark);
    return access
      ? { path: text(access.path), bookmark: text(access.bookmark), stale: access.stale }
      : null;
  }

  /** Stops the access a resolved bookmark opened; nothing to do without an
   * engine (its process held no access). */
  releaseFolderBookmark(bookmark: string): void {
    this.#base?.releaseFolderBookmark(bookmark);
  }

  listTracks(offset: number, limit: number): void {
    this.#command((base) => base.listTracks(offset, limit));
  }

  listAlbums(): void {
    this.#command((base) => base.listAlbums());
  }

  listArtists(): void {
    this.#command((base) => base.listArtists());
  }

  searchLibrary(query: string, limit: number): void {
    this.#command((base) => base.search(query, limit));
  }

  readArtworkList(path: string): void {
    this.#command((base) => base.readArtworkList(path));
  }

  readArtworkBytes(path: string, index: number): void {
    this.#command((base) => base.readArtworkBytes(path, index));
  }

  /** The cover a previous run cached on disk: `""` when there is no entry,
   * `"none"` when that run found the file has no picture. */
  coverCacheGet(path: string): string {
    const base = this.#base;
    if (!base) return "";
    try {
      return base.coverCacheGet(path);
    } catch (error) {
      console.warn("neiro: 封面缓存没有读出来:", String(error));
      return "";
    }
  }

  /** Writes a cover answer to the disk cache (the `data:` URI, or `"none"`). */
  coverCachePut(path: string, uri: string): void {
    const base = this.#base;
    if (!base) return;
    try {
      base.coverCachePut(path, uri);
    } catch (error) {
      console.warn("neiro: 封面缓存没有写进去:", String(error));
    }
  }

  /** The cache buckets' size and count, as the Rust host reports them. */
  cacheStats(): string | null {
    const base = this.#base;
    if (!base) return null;
    try {
      const json = base.cacheStats();
      return typeof json === "string" ? json : JSON.stringify(json);
    } catch (error) {
      console.warn("neiro: 缓存统计没有读出来:", String(error));
      return null;
    }
  }

  /** The linked core's ABI version on iOS; 0 where the core is loaded at run time. */
  baseAbiVersion(): number {
    const base = this.#base;
    if (!base) return 0;
    try {
      return base.baseAbiVersion();
    } catch (error) {
      console.warn("neiro: core ABI 版本读不出来:", String(error));
      return 0;
    }
  }

  /** The iOS test tone: writes it once, then plays it through the core. */
  playTone(): void {
    this.#command((base) => base.playTone());
  }

  /** Deletes one cache bucket; the answer is the stats after the clean. */
  clearCache(kind: string): string | null {
    const base = this.#base;
    if (!base) return null;
    try {
      const json = base.clearCache(kind);
      return typeof json === "string" ? json : JSON.stringify(json);
    } catch (error) {
      console.warn("neiro: 缓存没有清理:", String(error));
      return null;
    }
  }

  /** Sets the remote audio disk cache budget (`0` = stream-only, `null` = unlimited). */
  setAudioCacheBudget(budgetBytes: number | null): void {
    const base = this.#base;
    if (!base) return;
    try {
      base.setAudioCacheBudget(budgetBytes);
    } catch (error) {
      console.warn("neiro: 设置缓存上限失败:", String(error));
    }
  }

  // ---- 远程库（v2，契约：缓存目录由 Rust 固定，不传参） --------------------------

  /** This install's remote identity. A probe can run with no Base loaded
   * (the engine-down page still offers 设置), so that case answers empty and
   * the caller keeps the historical fallback. */
  deviceId(): string {
    try {
      return this.#base?.deviceId() ?? "";
    } catch (error) {
      console.warn("neiro: 设备标识没有读到:", String(error));
      return "";
    }
  }

  /** The Keychain blob for one server. A missing handle or a refused read
   * answers `null`: the caller then asks the user to re-enter the
   * credential instead of failing the whole page. */
  secretGet(server: string): string | null {
    try {
      return this.#base?.secretGet(server) ?? null;
    } catch (error) {
      console.warn("neiro: 钥匙串没有读到:", String(error));
      return null;
    }
  }

  secretSet(server: string, value: string): void {
    this.#command((base) => base.secretSet(server, value));
  }

  secretDelete(server: string): void {
    this.#command((base) => base.secretDelete(server));
  }

  setRemoteJellyfinPassword(baseUrl: string, user: string, password: string): void {
    this.#command((base) => base.setRemoteJellyfinPassword(baseUrl, user, password));
  }

  setRemoteJellyfinToken(baseUrl: string, token: string): void {
    this.#command((base) => base.setRemoteJellyfinToken(baseUrl, token));
  }

  setRemoteNavidrome(baseUrl: string, user: string, password: string): void {
    this.#command((base) => base.setRemoteNavidrome(baseUrl, user, password));
  }

  setRemoteWebdav(baseUrl: string, user: string, password: string): void {
    this.#command((base) => base.setRemoteWebdav(baseUrl, user, password));
  }

  /** 断开：丢弃内存层，磁盘缓存保留 —— 引擎自己清。 */
  clearRemote(): void {
    this.#command((base) => base.clearRemote());
  }

  /** One page of the remote index (default 200; a short page ends it). */
  /** Writes words into the file's own tag. The caller confirms first: this
   * is the only call that changes an audio file. */
  writeLyrics(path: string, text: string): void {
    this.#command((base) => base.writeLyrics(path, text));
  }

  /** Asks the online provider for this track's words; the answer arrives on
   * `subscribeLyricsFound`. */
  searchLyrics(artist: string, title: string, album: string, durationS: number | null, providersJson?: string | null): void {
    this.#command((base) => {
      base.searchLyrics(artist, title, album ?? null, durationS ?? null, providersJson ?? null);
    });
  }

  listRemote(start: number, limit: number): void {
    this.#command((base) => base.listRemote(start, limit));
  }

  fetchRemote(id: string): void {
    this.#command((base) => base.fetchRemote(id));
  }

  fetchRemoteArtwork(id: string): void {
    this.#command((base) => base.fetchRemoteArtwork(id));
  }

  listRemotePlaylists(): void {
    this.#command((base) => base.listRemotePlaylists());
  }

  listRemotePlaylistItems(id: string, start: number, limit: number): void {
    this.#command((base) => base.listRemotePlaylistItems(id, start, limit));
  }

  remoteProviders(): ProviderDescriptor[] {
    const base = this.#base;
    if (!base) return [];
    try {
      const json = base.remoteProviders();
      const parsed = typeof json === "string" ? JSON.parse(json) : json;
      return Array.isArray(parsed) ? (parsed as ProviderDescriptor[]) : [];
    } catch (e) {
      console.warn("neiro: 读取 remoteProviders 失败:", String(e));
      return [];
    }
  }

  setRemote(kind: string, params: Record<string, string>): void {
    this.#command((base) => base.setRemote(kind, JSON.stringify(params)));
  }

  listPlugins(): PluginInfo[] {
    const base = this.#base;
    if (!base) return [];
    try {
      const raw = base.listPlugins();
      return typeof raw === "string" ? (JSON.parse(raw) as PluginInfo[]) : (raw as PluginInfo[]);
    } catch (e) {
      console.warn("neiro: 读取 listPlugins 失败:", String(e));
      return [];
    }
  }

  setPluginEnabled(id: string, enabled: boolean): void {
    const base = this.#base;
    if (!base) return;
    try {
      base.setPluginEnabled(id, enabled);
    } catch (e) {
      console.warn("neiro: setPluginEnabled 失败:", String(e));
    }
  }

  reloadPlugins(): PluginInfo[] {
    const base = this.#base;
    if (!base) return [];
    try {
      const raw = base.reloadPlugins();
      return typeof raw === "string" ? (JSON.parse(raw) as PluginInfo[]) : (raw as PluginInfo[]);
    } catch (e) {
      console.warn("neiro: reloadPlugins 失败:", String(e));
      return [];
    }
  }

  async listDirectory(path: string): Promise<DirectoryListing> {
    const base = this.#base;
    if (!base) return { path, folders: [], tracks: [] };
    try {
      const json = base.listDirectory(path);
      const parsed = typeof json === "string" ? JSON.parse(json) : json;
      return {
        path: typeof parsed.path === "string" ? parsed.path : path,
        folders: Array.isArray(parsed.folders) ? parsed.folders : [],
        tracks: Array.isArray(parsed.tracks) ? parsed.tracks.map(trackOf) : [],
      };
    } catch (e) {
      console.warn("neiro: listDirectory 失败:", String(e));
      return { path, folders: [], tracks: [] };
    }
  }

  // ---- 曲库的用户层（v3） ----------------------------------------------------

  setFavourite(path: string, on: boolean): void {
    this.#command((base) => base.setFavourite(path, on));
  }

  listFavourites(): void {
    this.#command((base) => base.listFavourites());
  }

  listPlaylists(): void {
    this.#command((base) => base.listPlaylists());
  }

  createPlaylist(name: string): void {
    this.#command((base) => base.createPlaylist(name));
  }

  renamePlaylist(id: number, name: string): void {
    this.#command((base) => base.renamePlaylist(id, name));
  }

  deletePlaylist(id: number): void {
    this.#command((base) => base.deletePlaylist(id));
  }

  addToPlaylist(id: number, path: string): void {
    this.#command((base) => base.addToPlaylist(id, path));
  }

  removeFromPlaylist(id: number, path: string): void {
    this.#command((base) => base.removeFromPlaylist(id, path));
  }

  listPlaylistEntries(id: number): void {
    this.#command((base) => base.listPlaylistEntries(id));
  }

  listMostPlayed(limit: number): void {
    this.#command((base) => base.listMostPlayed(limit));
  }

  recordPlay(path: string, atMs: number): void {
    this.#command((base) => base.recordPlay(path, atMs));
  }

  setPlayerForTest(p: Partial<Player>): void {
    playerSig.update((prev) => ({ ...prev, ...p }));
  }

  /** Tests: pretend the engine is (or is no longer) configured for a server. */
  setRemoteStatusForTest(status: RemoteStatus): void {
    remoteStatusSig.set(status);
  }
}

/** The engine the stores share: made once (the slot keeps a hot update from
 * making a second one), connected by the app root. */
export const engine: Engine = ((globalThis as Record<string, unknown>)[SLOT_KEY] as Engine) ?? new Engine();
(globalThis as Record<string, unknown>)[SLOT_KEY] = engine;

/** Reports a refused command as a toast; the pages stay up. */
export function reportEngineFailure(error: unknown, label = "操作失败"): void {
  const { code, message } = errorOf(error);
  console.warn(`neiro: ${code}: ${message}`);
  notify(`${label}：${message || "引擎没有接受这次操作"}`);
}
