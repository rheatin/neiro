// 远程库的状态: 服务器会话 (一组凭据，仅保存在 Storage 中 — 它们不进源码),
// `remote_listed` 数据已合并完整 (搜索会在本地过滤)，封面已连接至共享的
// artwork 栈, 流程 设为 fetch→fetched→play。

import { computed, effect, signal, Storage, untracked } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import type { RemoteKind, RemoteStatus, RemoteTrack, Track } from "../backend/types";
import { playTrack } from "./player";
import { activeSourceId, registerRemoteActivator, removeServer, servers, type RemoteServer } from "./sources";

const CONFIG_KEY = "remote:config";

/** 一个已保存的服务器 (Engine 永远看不到它所在的那台机器)。 */
export interface RemoteConfig {
  kind: RemoteKind;
  baseUrl: string;
  user: string;
  /** Jellyfin `password` — 不适用于 token 登录; `token` 适用于。 */
  password?: string;
  token?: string;
}

const config = signal<RemoteConfig | null>(null);
const configLoaded = signal(false);

/** 远程列表: 当 `remote_listed` 到达时合并; 搜索 (`listRemote` 是全
 * 列表拉取) 在 TSX 端过滤。 */
export const remoteTracks = signal<RemoteTrack[]>([]);
export const remoteFetching = signal(false);
/** 为了进度显示, 以及一个较短页面在不匹配此信号时是过时的。 */
export const remoteLoadingState = signal<"idle" | "loading" | "error">("idle");

export const remoteStatus = (): RemoteStatus => engine.remoteStatus();
export const isRemoteConfigured = (): boolean => engine.remoteStatus().configured;
export const remoteConfig = (): RemoteConfig | null => config();
export const remoteConfigReady = (): boolean => configLoaded();

/** 播放列正在获取哪一首曲目 (转圈画在该行上；比待处理的播放意图精细 —
 * 是逐行, 而不是逐条目)。 */
const fetchingIds = signal<Set<string>>(new Set());

function fetchingId(id: string): boolean {
  return fetchingIds().has(id);
}
export { fetchingId as isFetchingRemote };

// ---- 会话：凭据只进 Storage，重启后自动 setRemoteXxx 重连 -------------------

function applyRemote(cfg: RemoteConfig): void {
  if (cfg.kind === "jellyfin" && cfg.token) {
    engine.setRemoteJellyfinToken(cfg.baseUrl, cfg.token);
    return;
  }
  if (cfg.kind === "jellyfin") {
    engine.setRemoteJellyfinPassword(cfg.baseUrl, cfg.user, cfg.password ?? "");
    return;
  }
  engine.setRemoteNavidrome(cfg.baseUrl, cfg.user, cfg.password ?? "");
}

/** 保存、下发、记住：一个失败的原语（命令被拒就抛，调用方 toast）。 */
export async function saveRemoteConfig(cfg: RemoteConfig): Promise<void> {
  applyRemote(cfg);
  config.set(cfg);
  await Storage.setItem(CONFIG_KEY, JSON.stringify({ ...cfg, password: cfg.password ?? "" })).catch((error) =>
    console.warn("neiro: 远程配置没有记住:", String(error)),
  );
}

export async function disconnectRemote(): Promise<void> {
  config.set(null);
  await Storage.removeItem(CONFIG_KEY).then(undefined, (error) =>
    console.warn("neiro: 远程配置没有删除:", String(error)),
  );
  try {
    engine.clearRemote();
  } catch (error) {
    reportEngineFailure(error, "断开远程失败");
  }
  serverCache.clear();
  cacheVersion.update((v) => v + 1);
  untracked(() => {
    activeSourceId.set("local:all");
    remoteTracks.set([]);
    setRemoteQuery("");
  });
}

// 引擎连上（或重试成功）后：先看 `source:active` 是否指向某台服务器（来
// 源 Tab 的恢复）；否则回退旧的单条 `remote:config`。两者都读回配置 →
// setRemoteXxx（激活路径还会重拉列表）。
let seededRemote = false;
effect(() => {
  if (engine.health().status !== "ready" || seededRemote) return;
  seededRemote = true;
  void (async () => {
    try {
      const activeId = await Storage.getItem("source:active");
      if (activeId && activeId.startsWith("remote:")) {
        const serverId = activeId.slice("remote:".length);
        // 花名册（sources.ts 启动迁移）可能还没读完；给它一小段窗口。
        for (let i = 0; i < 40 && servers().length === 0; i++) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        if (servers().some((s) => s.id === serverId)) {
          await activateServer(serverId);
          configLoaded.set(true);
          return;
        }
      }
      const saved = await Storage.getItem(CONFIG_KEY);
      if (!saved) return;
      const parsed = JSON.parse(saved) as Partial<RemoteConfig>;
      if (parsed.kind !== "jellyfin" && parsed.kind !== "navidrome") return;
      if (typeof parsed.baseUrl !== "string" || parsed.baseUrl === "") return;
      const cfg: RemoteConfig = {
        kind: parsed.kind,
        baseUrl: parsed.baseUrl,
        user: typeof parsed.user === "string" ? parsed.user : "",
        password: typeof parsed.password === "string" && parsed.password ? parsed.password : undefined,
        token: typeof parsed.token === "string" && parsed.token ? parsed.token : undefined,
      };
      config.set(cfg);
      applyRemote(cfg);
    } catch (error) {
      console.warn("neiro: 远程配置没有读回来:", String(error));
    } finally {
      configLoaded.set(true);
    }
  })();
});

// ---- 多服务器：花名册来自 sources.ts；激活 = 配置 + 重拉 -------------------

/** 每个已拉过服务器的曲目列表（内存缓存；切回该来源时立即上屏，随后
 * id 重新注册的重拉在后台进行）。 */
const serverCache = new Map<string, RemoteTrack[]>();
const cacheVersion = signal(0);

export function cachedRemoteTracks(serverId: string): RemoteTrack[] {
  cacheVersion();
  return serverCache.get(serverId) ?? [];
}

/** 当前激活的远程服务器（activeSource 指向的；未指向远程 → null）。 */
export const activeRemoteServer = computed<RemoteServer | null>(() => {
  const id = activeSourceId();
  if (!id.startsWith("remote:")) return null;
  const serverId = id.slice("remote:".length);
  return servers().find((s) => s.id === serverId) ?? null;
});

/** 激活一台服务器：配置引擎（等 remote_changed）→ 重拉列表。已连同一台
 * （baseUrl/kind/user 全同）时只重拉，不重复下发配置。 */
export async function activateServer(serverId: string): Promise<void> {
  const server = servers().find((s) => s.id === serverId);
  if (!server) {
    // 花名册里没有（被删了）：回到本地来源，清空远程视图。
    activeSourceId.set("local:all");
    remoteTracks.set([]);
    remoteLoadingState.set("idle");
    return;
  }
  const status = engine.remoteStatus();
  const sameServer =
    status.configured && status.baseUrl === server.baseUrl && remoteConfig()?.kind === server.kind &&
    remoteConfig()?.user === (server.user ?? "");
  if (!sameServer) {
    try {
      await saveRemoteConfig({
        kind: server.kind,
        baseUrl: server.baseUrl,
        user: server.user ?? "",
        password: server.password,
        token: server.token,
      });
    } catch (error) {
      // 引擎没就绪或命令被拒：错误进 toast，远程视图给可重试的空态。
      reportEngineFailure(error, "连接失败");
      remoteLoadingState.set("error");
      remoteFetching.set(false);
      return;
    }
  }
  // 有缓存的列表先上屏（封面/文件在 Base 的磁盘缓存里），随后 id 重新
  // 注册的重拉把列表与 id 表刷新。
  const cached = serverCache.get(serverId);
  remoteTracks.set(cached ? [...cached] : []);
  remoteLoadingState.set(cached && cached.length > 0 ? "idle" : "idle");
  loadRemote();
}

// sources.ts 的来源切换在这里落地（注册避免模块循环）；激活的失败不悬空。
registerRemoteActivator((serverId) => {
  activateServer(serverId).catch((error) => console.warn("neiro: 服务器没有连上:", String(error)));
});

/** 从花名册删掉一台服务器；删的是当前连着的 → 断开并回到本地来源。 */
export async function removeServerById(id: string): Promise<void> {
  if (activeRemoteServer()?.id === id) await disconnectRemote();
  await removeServer(id);
}

// 服务器就绪（remote_changed 配置为 true）时，启动读取：列表 + 播放列表。
effect(() => {
  const status = engine.remoteStatus();
  if (status.configured && remoteLoadingState() === "idle" && remoteTracks().length === 0) loadRemote();
});

// ---- 列表 ------------------------------------------------------------------

/** `listRemote` 是一页页全量拉取（默认一页 200）：来一页接一页，短页或
 * 空页结束。 */
const REMOTE_PAGE = 200;

/** 一页并入列表；再没到头就发下一页的请求。 */
function handleListed(tracks: RemoteTrack[]): void {
  // 分页期间 `remoteFetching` 必须保持 true：订阅条件 `if (remoteFetching())`
  // 是"这是当前这一轮拉取的页"的标记；第一页就置 false 会把第二页起的所有
  // 页丢掉（曾经只拉到 200 首）。
  remoteLoadingState.set("idle");
  if (tracks.length === 0) {
    remoteFetching.set(false);
    finishServerCache();
    return;
  }
  remoteTracks.update((prev) => [...prev, ...tracks]);
  if (tracks.length >= REMOTE_PAGE) {
    try {
      engine.listRemote(remoteTracks().length, REMOTE_PAGE);
    } catch (error) {
      console.warn("neiro: 下一页远程列表没有发出去:", String(error));
      remoteFetching.set(false);
      finishServerCache();
    }
  } else {
    remoteFetching.set(false);
    finishServerCache();
  }
}

/** 一轮拉取结束：把完整列表存进当前远程来源的缓存（切 Tab 免重拉上屏）。 */
function finishServerCache(): void {
  const id = activeSourceId();
  if (!id.startsWith("remote:")) return;
  serverCache.set(id.slice("remote:".length), [...remoteTracks()]);
  cacheVersion.update((v) => v + 1);
}

export function loadRemote(): void {
  // Only a request that is still in flight blocks a new one. A flag left set
  // by a request that never answered used to make the page empty forever.
  if (remoteFetching() && remoteLoadingState() === "loading") return;
  remoteFetching.set(true);
  remoteTracks.set([]);
  remoteLoadingState.set("loading");
  try {
    engine.listRemote(0, REMOTE_PAGE);
  } catch (error) {
    reportEngineFailure(error, "远程列表失败");
    remoteFetching.set(false);
    remoteLoadingState.set("error");
  }
}

export function retryRemote(): void {
  if (remoteLoadingState() === "error" || remoteTracks().length === 0) loadRemote();
}

// remote_listed 从引擎镜像落进来（先于首呼的缓存值由引擎自己给出）。空数
// 组也应到达（一个整页后的空页就是结束），不过滤。
effect(() => {
  const page = engine.remoteTracks();
  untracked(() => {
    if (page === null) return;
    if (remoteFetching()) handleListed(page);
  });
});

// 一次尝试失败了（连不上/认证失败——op 是 remote）：转圈变成可重试的空态。
engine.subscribeEngineError((error) => {
  if (error.op !== "remote") return;
  untracked(() => {
    if (remoteFetching()) {
      remoteFetching.set(false);
      remoteLoadingState.set("error");
    }
  });
});

// ---- 分组（首页远程来源的 专辑/歌手 段） ------------------------------------

/** 远程专辑组：按 album（+artist 消歧）分组；组内曲目保持服务器列表顺序。 */
export interface RemoteAlbumGroup {
  album: string;
  artist: string;
  tracks: RemoteTrack[];
}

export const remoteAlbumGroups = computed<RemoteAlbumGroup[]>(() => {
  const map = new Map<string, RemoteAlbumGroup>();
  for (const track of remoteTracks()) {
    const album = track.album || "";
    if (album === "") continue;
    const artist = track.artist || "";
    const key = `${album}\u0000${artist}`;
    const group = map.get(key);
    if (group) group.tracks.push(track);
    else map.set(key, { album, artist, tracks: [track] });
  }
  return [...map.values()];
});

/** 远程歌手组：按 artist 分组（空 artist 跳过——无名字不画卡）。 */
export interface RemoteArtistGroup {
  artist: string;
  tracks: RemoteTrack[];
}

export const remoteArtistGroups = computed<RemoteArtistGroup[]>(() => {
  const map = new Map<string, RemoteArtistGroup>();
  for (const track of remoteTracks()) {
    const artist = track.artist || "";
    if (artist === "") continue;
    const group = map.get(artist);
    if (group) group.tracks.push(track);
    else map.set(artist, { artist, tracks: [track] });
  }
  return [...map.values()];
});

// ---- 搜索 ------------------------------------------------------------------

export const remoteQuery = signal("");

/** 全量键入完成 (200/页列表在背景拉) 后在列表上本地过滤。 */
export function filteredRemoteTracks(): RemoteTrack[] {
  const query = remoteQuery().trim().toLowerCase();
  const list = remoteTracks();
  if (!query) return list;
  return list.filter(
    (t) => t.title.toLowerCase().includes(query) || t.artist.toLowerCase().includes(query) || t.album.toLowerCase().includes(query),
  );
}

export function setRemoteQuery(value: string): void {
  remoteQuery.set(value);
}

// ---- 收藏/曲单里的远程缓存文件 ----------------------------------------------

/** A downloaded remote track is named `{id}.{container}` by the engine's
 * `cache_path`, so a favourite or playlist entry that names one can be
 * resolved back to the listing row — title, artist and album come from the
 * server, not from a file name. Only the lists already in memory answer:
 * a collection must not turn into a network fetch of its own. */
export function remoteTrackForCachePath(path: string): RemoteTrack | null {
  const name = path.replace(/\\/g, "/").split("/").pop() ?? "";
  const dot = name.lastIndexOf(".");
  if (dot <= 0) return null;
  const id = name.slice(0, dot);
  const container = name.slice(dot + 1).toLowerCase();
  const lists: RemoteTrack[][] = [remoteTracks(), ...serverCache.values()];
  for (const list of lists) {
    const found = list.find((t) => t.id === id && t.container.toLowerCase() === container);
    if (found) return found;
  }
  return null;
}

// ---- 播放：fetch → fetched → play --------------------------------------------

/** 把远程行变成播放意图所需的形状（path 是引擎缓存后的本地文件路径）。 */
export function asLocalTrack(track: RemoteTrack, path: string): Track {
  return {
    path,
    title: track.title,
    artist: track.artist,
    album: track.album,
    albumArtist: "",
    discNo: 0,
    trackNo: 0,
    year: 0,
    genre: "",
    durationMs: track.durationMs,
    sampleRate: 0,
    bits: 0,
    channels: 0,
  };
}

/** 点击一行：`fetchRemote(id)` 进缓存，`remote_fetched` 带回 path，直
 * 接 `play(path)`。等待中给该行转圈。 */
export function playRemote(track: RemoteTrack): void {
  if (fetchingId(track.id)) return;
  fetchingIds.update((set) => new Set(set).add(track.id));
  try {
    engine.fetchRemote(track.id);
  } catch (error) {
    reportEngineFailure(error, "取流失败");
    unfetchId(track.id);
  }
}

function unfetchId(id: string): void {
  fetchingIds.update((set) => {
    const next = new Set(set);
    next.delete(id);
    return next;
  });
}

/** What to do once a row's file is in the cache: play it, or hand its path
 * to an action that needs one (收藏, 加入曲单). A remote row has no local
 * path until the engine fetched it, so those actions wait for the download
 * instead of being refused. */
const fetchedActions = new Map<string, (path: string) => void>();

engine.subscribeRemoteFetched(({ track, path }) => {
  untracked(() => {
    if (!fetchingId(track.id)) return;
    unfetchId(track.id);
    if (!path) {
      fetchedActions.delete(track.id);
      reportEngineFailure(new Error("E_REMOTE: 服务器没有回这个文件的缓存路径"), "取流失败");
      return;
    }
    fetchedPaths.set(track.id, path);
    const action = fetchedActions.get(track.id);
    fetchedActions.delete(track.id);
    if (action) action(path);
    else playTrack(asLocalTrack(track, path));
  });
});

/** The cache paths this run knows, by track id: the engine reports one
 * whenever it finishes a download. */
const fetchedPaths = new Map<string, string>();

/** The cached file of a row, when this run already fetched it (its path
 * came back on `remote_fetched`). `null` until then: the file may well be
 * on disk from an earlier run, and only the engine's own answer is a fact. */
export function cachedRemotePath(track: RemoteTrack): string | null {
  return fetchedPaths.get(track.id) ?? null;
}

/** Fetches a remote row and runs `then` with the cached path (no play).
 * A second request for the same row while it downloads is dropped. */
export function withRemotePath(track: RemoteTrack, then: (path: string) => void): void {
  if (fetchingId(track.id)) return;
  fetchingIds.update((set) => new Set(set).add(track.id));
  fetchedActions.set(track.id, then);
  try {
    engine.fetchRemote(track.id);
  } catch (error) {
    fetchedActions.delete(track.id);
    unfetchId(track.id);
    reportEngineFailure(error, "取流失败");
  }
}

// 一条取了很久没有人来接（引擎静默了）：转圈撤下，不挂死。
effect(() => {
  if (fetchingIds().size === 0) return;
  const timer = setTimeout(() => {
    const stale = fetchingIds();
    if (stale.size > 0) for (const id of stale) unfetchId(id);
  }, 30_000);
  return () => clearTimeout(timer);
});
