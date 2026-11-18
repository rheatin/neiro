// 远程库的状态: 服务器会话 (一组凭据，仅保存在 Storage 中 — 它们不进源码),
// `remote_listed` 数据已合并完整 (搜索会在本地过滤)，封面已连接至共享的
// artwork 栈, 流程 设为 fetch→fetched→play。

import { cleanup, computed, effect, signal, Storage, untracked } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import type { RemoteKind, RemoteStatus, RemoteTrack, Track } from "../backend/types";
import { registerRemotePlayer, playTrack, QUEUE_AHEAD, trackToQueueItem } from "./player";
import { registerRemoteCatalog } from "./catalog";
import { ALL_ID } from "./sources";
import { activeSourceId, registerRemoteActivator, registerRemoteDisconnector, removeServer, servers, serversLoaded, type RemoteServer } from "./sources";
import { shuffle, shuffled } from "./shuffle";

const CONFIG_KEY = "remote:config";

/** 一个已保存的服务器 (Engine 永远看不到它所在的那台机器)。 */
export interface RemoteConfig {
  kind: RemoteKind;
  baseUrl: string;
  user: string;
  password?: string;
  token?: string;
  params?: Record<string, string>;
}

const config = signal<RemoteConfig | null>(null);
const configLoaded = signal(false);

/** 远程列表: 当 `remote_listed` 到达时合并; 搜索 (`listRemote` 是全
 * 列表拉取) 在 TSX 端过滤。 */
export const remoteTracks = signal<RemoteTrack[]>([]);
export const remoteFetching = signal(false);
/** 为了进度显示, 以及一个较短页面在不匹配此信号时是过时的。 */
export const remoteLoadingState = signal<"idle" | "loading" | "error">("idle");

/** The engine's last word on a remote failure. An empty list and a refused
 * server look the same on screen; this is what tells them apart. */
export const remoteError = signal<string>("");

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

/** 每个已拉过服务器的曲目列表（内存缓存；切回该来源时立即上屏，随后
 * id 重新注册的重拉在后台进行）。 */
const serverCache = new Map<string, RemoteTrack[]>();
const cacheVersion = signal(0);
let activeEngineServerId = "";
let fetchingServerId = "";
let fetchingBaseUrl = "";
let emptyHandled = false;
let lastSyncAt = 0;

function applyRemote(cfg: RemoteConfig): void {
  const params: Record<string, string> = {
    base_url: cfg.baseUrl,
    ...(cfg.user ? { user: cfg.user } : {}),
    ...(cfg.password ? { password: cfg.password } : {}),
    ...(cfg.token ? { token: cfg.token } : {}),
    ...(cfg.params ?? {}),
  };
  let effectiveKind = cfg.kind;
  if (effectiveKind === "jellyfin" && cfg.token && !cfg.password) {
    effectiveKind = "jellyfin_token";
  }
  engine.setRemote(effectiveKind, params);
}

/** 保存、下发、记住：一个失败的原语（命令被拒就抛，调用方 toast）。 */
export async function saveRemoteConfig(cfg: RemoteConfig): Promise<void> {
  applyRemote(cfg);
  config.set(cfg);
  await Storage.setItem(CONFIG_KEY, JSON.stringify(cfg)).catch((error) =>
    console.warn("neiro: 远程配置没有记住:", String(error)),
  );
}

/** 探测服务器连通性与凭据有效性（用于 ServerDialog 的测试连接按钮与排障）。 */
export async function probeRemoteConfig(
  cfg: RemoteConfig,
  timeoutMs: number = 8000,
): Promise<{ ok: boolean; message: string; count: number }> {
  const url = cfg.baseUrl.replace(/\/+$/, "");
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  try {
    if (cfg.kind === "jellyfin" || cfg.kind === "jellyfin_token") {
      // 1. 探测 Jellyfin 公开服务信息
      const infoRes = await fetch(`${url}/System/Info/Public`, { signal: controller.signal });
      if (!infoRes.ok) {
        return { ok: false, message: `服务器返回状态码 ${infoRes.status}`, count: 0 };
      }
      const info = (await infoRes.json().catch(() => ({}))) as { ServerName?: string; Version?: string };
      const serverName = info.ServerName ?? "Jellyfin";

      // 2. Token 模式验证
      if (cfg.token) {
        const claim = `MediaBrowser Client="neiro", Device="desktop", DeviceId="neiro-base", Version="0.1", Token="${cfg.token}"`;
        const testRes = await fetch(`${url}/Items?Limit=1`, {
          signal: controller.signal,
          headers: {
            Authorization: claim,
            "X-Emby-Authorization": claim,
            "X-Emby-Token": cfg.token,
          },
        });
        if (!testRes.ok) {
          return { ok: false, message: `API Token 验证失败 (${testRes.status})`, count: 0 };
        }
        return { ok: true, message: `连接成功 (${serverName} ${info.Version ?? ""})`, count: 1 };
      }

      // 3. 密码模式验证
      if (cfg.password || cfg.user) {
        const claim = `MediaBrowser Client="neiro", Device="desktop", DeviceId="neiro-base", Version="0.1"`;
        const authRes = await fetch(`${url}/Users/AuthenticateByName`, {
          method: "POST",
          signal: controller.signal,
          headers: {
            Authorization: claim,
            "X-Emby-Authorization": claim,
            "Content-Type": "application/json",
          },
          body: JSON.stringify({ Username: cfg.user ?? "", Pw: cfg.password ?? "" }),
        });
        if (!authRes.ok) {
          return { ok: false, message: `用户名或密码错误 (${authRes.status})`, count: 0 };
        }
        const authData = (await authRes.json().catch(() => ({}))) as { AccessToken?: string };
        if (!authData.AccessToken) {
          return { ok: false, message: "认证未返回 AccessToken", count: 0 };
        }
        return { ok: true, message: `连接成功 (${serverName}，用户 ${cfg.user})`, count: 1 };
      }

      return { ok: true, message: `服务器可达 (${serverName})`, count: 1 };
    }

    if (cfg.kind === "navidrome") {
      const pingUrl = `${url}/rest/ping.view?u=${encodeURIComponent(cfg.user ?? "")}&p=${encodeURIComponent(
        cfg.password ?? "",
      )}&v=1.16.1&c=neiro&f=json`;
      const res = await fetch(pingUrl, { signal: controller.signal });
      if (!res.ok) return { ok: false, message: `服务器返回状态码 ${res.status}`, count: 0 };
      return { ok: true, message: "连接成功 (Navidrome / Subsonic 可达)", count: 1 };
    }

    if (cfg.kind === "webdav") {
      const res = await fetch(url, {
        method: "PROPFIND",
        signal: controller.signal,
        headers: {
          Depth: "0",
          ...(cfg.user && cfg.password
            ? { Authorization: `Basic ${btoa(`${cfg.user}:${cfg.password}`)}` }
            : {}),
        },
      });
      if (res.status >= 200 && res.status < 400) {
        return { ok: true, message: "连接成功 (WebDAV 可达)", count: 1 };
      }
      return { ok: false, message: `WebDAV 返回状态码 ${res.status}`, count: 0 };
    }

    // 默认兜底探测
    const res = await fetch(url, { signal: controller.signal });
    return { ok: res.ok, message: res.ok ? "服务器可达" : `状态码 ${res.status}`, count: res.ok ? 1 : 0 };
  } catch (err: any) {
    if (err?.name === "AbortError" || String(err).includes("abort")) {
      return { ok: false, message: "连接探测超时（8 秒内服务器未响应，请检查 IP 与端口是否可达）", count: 0 };
    }
    return { ok: false, message: `网络连接失败: ${String(err)}`, count: 0 };
  } finally {
    clearTimeout(timer);
  }
}

export async function disconnectRemote(clearCache: boolean = false): Promise<void> {
  config.set(null);
  await Storage.removeItem(CONFIG_KEY).then(undefined, (error) =>
    console.warn("neiro: 远程配置没有删除:", String(error)),
  );
  try {
    engine.clearRemote();
  } catch (error) {
    reportEngineFailure(error, "断开远程失败");
  }
  activeEngineServerId = "";
  fetchingServerId = "";
  fetchingBaseUrl = "";
  lastSyncAt = 0;
  emptyHandled = false;
  remoteError.set("");
  remoteFetching.set(false);
  if (clearCache) {
    serverCache.clear();
    cacheVersion.update((v) => v + 1);
  }
  untracked(() => {
    activeSourceId.set(ALL_ID);
    remoteTracks.set([]);
    setRemoteQuery("");
    remoteLoadingState.set("idle");
  });
}

// 引擎连上（或重试成功）后：先看 `source:active` 是否指向某台服务器（来
// 源 Tab 的恢复）；否则回退旧的单条 `remote:config`。两者都读回配置 →
// setRemoteXxx（激活路径还会重拉列表）。
let seededRemote = false;
effect(() => {
  if (engine.health().status !== "ready" || seededRemote) return;
  if (!serversLoaded()) return;
  seededRemote = true;
  void (async () => {
    try {
      const serverList = servers();
      for (const s of serverList) {
        try {
          const raw = await Storage.getItem(`remote:tracks:${s.id}`);
          if (raw) {
            const parsed = JSON.parse(raw);
            if (Array.isArray(parsed)) {
              serverCache.set(s.id, parsed);
            }
          }
        } catch {}
      }
      cacheVersion.update((v) => v + 1);

      const activeId = activeSourceId();
      if (activeId && activeId.startsWith("remote:")) {
        const serverId = activeId.slice("remote:".length);
        if (serverList.some((s) => s.id === serverId)) {
          const cached = serverCache.get(serverId);
          if (cached && cached.length > 0) {
            remoteTracks.set([...cached]);
          }
          await activateServer(serverId);
          configLoaded.set(true);
          return;
        }
      }

      if (serverList.length > 0 && !engine.remoteStatus().configured) {
        const first = serverList[0];
        activeEngineServerId = first.id;
        const cached = serverCache.get(first.id);
        if (cached && cached.length > 0) {
          remoteTracks.set([...cached]);
        }
        const cfg: RemoteConfig = {
          kind: first.kind,
          baseUrl: first.baseUrl,
          user: first.user ?? "",
          password: first.password,
          token: first.token,
        };
        config.set(cfg);
        applyRemote(cfg);
        configLoaded.set(true);
        return;
      }

      const saved = await Storage.getItem(CONFIG_KEY);
      if (!saved) return;
      const parsed = JSON.parse(saved) as Partial<RemoteConfig>;
      if (typeof parsed.kind !== "string" || parsed.kind === "") return;
      if (typeof parsed.baseUrl !== "string" || parsed.baseUrl === "") return;
      const cfg: RemoteConfig = {
        kind: parsed.kind,
        baseUrl: parsed.baseUrl,
        user: typeof parsed.user === "string" ? parsed.user : "",
        password: typeof parsed.password === "string" && parsed.password ? parsed.password : undefined,
        token: typeof parsed.token === "string" && parsed.token ? parsed.token : undefined,
        params: parsed.params,
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

export function cachedRemoteTracks(serverId: string): RemoteTrack[] {
  cacheVersion();
  return serverCache.get(serverId) ?? [];
}

export const allRemoteTracks = computed<RemoteTrack[]>(() => {
  cacheVersion();
  const map = new Map<string, RemoteTrack>();
  for (const t of remoteTracks()) map.set(t.id, t);
  for (const list of serverCache.values()) {
    for (const t of list) {
      if (!map.has(t.id)) map.set(t.id, t);
    }
  }
  return Array.from(map.values());
});

/** 当前激活的远程服务器（activeSource 指向的；未指向远程 → null）。 */
export const activeRemoteServer = computed<RemoteServer | null>(() => {
  const id = activeSourceId();
  if (!id.startsWith("remote:")) return null;
  const serverId = id.slice("remote:".length);
  return servers().find((s) => s.id === serverId) ?? null;
});

/** 激活一台服务器：配置引擎（等 remote_changed）→ 重拉列表。已连同一台
 * （baseUrl/kind/user 全同）时只重拉，不重复下发配置。 */
/** Re-sends the server's credentials to the engine and lists again: what a
 * listener wants from 重新连接, without editing anything. */
export async function reconnectServer(serverId: string): Promise<void> {
  await activateServer(serverId, true);
  if (remoteTracks().length === 0) loadRemote(true);
}

export async function activateServer(serverId: string, force: boolean = false): Promise<void> {
  const server = servers().find((s) => s.id === serverId);
  if (!server) {
    // 花名册里没有（被删了）：回到本地来源，清空远程视图。
    activeEngineServerId = "";
    fetchingServerId = "";
    fetchingBaseUrl = "";
    freshList = null;
    remoteFetching.set(false);
    activeSourceId.set(ALL_ID);
    remoteTracks.set([]);
    remoteLoadingState.set("idle");
    return;
  }
  // 切换服务器时立即同步替换当前屏幕上的曲目列表（不保留上一台服务器的列表），
  // 并作废上一台服务器尚未落地的分页拉取。
  activeEngineServerId = serverId;
  fetchingServerId = serverId;
  fetchingBaseUrl = server.baseUrl;
  freshList = null;
  remoteFetching.set(false);
  remoteError.set("");
  const cached = serverCache.get(serverId);
  remoteTracks.set(cached ? [...cached] : []);
  remoteLoadingState.set(cached && cached.length > 0 ? "idle" : "loading");

  const status = engine.remoteStatus();
  const sameServer =
    !force &&
    remoteTracks().length > 0 &&
    remoteError() === "" &&
    status.configured &&
    status.baseUrl === server.baseUrl &&
    remoteConfig()?.kind === server.kind &&
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
  if (activeEngineServerId !== serverId) return;
  loadRemote(true);
}

// sources.ts 的来源切换在这里落地（注册避免模块循环）；激活的失败不悬空。
registerRemoteActivator((serverId) => {
  activateServer(serverId).catch((error) => console.warn("neiro: 服务器没有连上:", String(error)));
});

registerRemoteDisconnector(() => {
  void disconnectRemote();
});

/** 从花名册删掉一台服务器；删的是当前连着的 → 断开并回到本地来源。 */
export async function removeServerById(id: string): Promise<void> {
  serverCache.delete(id);
  cacheVersion.update((v) => v + 1);
  void Storage.removeItem(`remote:tracks:${id}`).catch(() => {});
  if (activeRemoteServer()?.id === id) await disconnectRemote();
  await removeServer(id);
}

// 服务器就绪（remote_changed 配置为 true）时启动读取——每个服务器只启动一次。
//
// 以前的条件读了 `remoteLoadingState`/`remoteTracks`：一次空的回复把状态放回
// `idle`，条件又成立，于是再发一次请求。屏幕上就是转圈走到一半归零、永远不
// 落地。
effect(() => {
  const status = engine.remoteStatus();
  const empty = remoteTracks().length === 0;
  const busy = remoteFetching();
  if (!status.configured || !empty || busy) return;
  // A first attempt can be dropped while the engine is still subscribing.
  // Retrying is safe because an attempt stamps `lastSyncAt`, so this runs at
  // most once per freshness window — not once per state change.
  if (Date.now() - lastSyncAt < 5_000) return;
  untracked(() => loadRemote());
});

// ---- 列表 ------------------------------------------------------------------

/** `listRemote` 是一页页全量拉取（默认一页 200）：来一页接一页，短页或
 * 空页结束。 */
const REMOTE_PAGE = 200;

/** 一页并入列表；再没到头就发下一页的请求。 */
/** A refresh collects into this while the cached list stays on screen; the
 * swap happens when the last page lands, so a re-listing never blanks the
 * page and never mixes two listings. */
let freshList: RemoteTrack[] | null = null;

/** How long a listing is trusted before the page refreshes it again. */
const SYNC_TTL_MS = 5 * 60 * 1000;

/** Whether the cached listing is fresh enough to show without asking again. */
export const remoteIsFresh = (): boolean =>
  remoteTracks().length > 0 && Date.now() - lastSyncAt < SYNC_TTL_MS;

function tracksEqual(a: RemoteTrack[], b: RemoteTrack[]): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    const x = a[i];
    const y = b[i];
    if (
      x.id !== y.id ||
      x.title !== y.title ||
      x.artist !== y.artist ||
      x.album !== y.album ||
      x.container !== y.container ||
      x.durationMs !== y.durationMs ||
      x.size !== y.size ||
      x.coverArt !== y.coverArt
    ) {
      return false;
    }
  }
  return true;
}

let listEpoch = 0;

function handleListed(tracks: RemoteTrack[]): void {
  const currentEpoch = listEpoch;
  // 丢弃上一台服务器在切换前已经在路上的旧分页结果。
  const currentStatusUrl = engine.remoteStatus().baseUrl;
  if (fetchingBaseUrl && currentStatusUrl && currentStatusUrl !== fetchingBaseUrl) {
    return;
  }
  if (fetchingServerId && activeEngineServerId && fetchingServerId !== activeEngineServerId) {
    return;
  }
  const targetServerId = fetchingServerId;
  // 分页期间 `remoteFetching` 必须保持 true：订阅条件 `if (remoteFetching())`
  // 是"这是当前这一轮拉取的页"的标记；第一页就置 false 会把第二页起的所有
  // 页丢掉（曾经只拉到 200 首）。
  remoteLoadingState.set("idle");
  if (tracks.length === 0) {
    // A listing that arrives with no items and a password on file is the one
    // case worth repeating: the engine may be holding a token from an older
    // sign-in. Signing in again costs one request and answers the question.
    // The engine's own config may be a token; the roster is where the
    // password lives. Either way, one empty answer is worth one fresh
    // sign-in before the page claims the server is empty.
    const cfg = remoteConfig();
    const roster = activeRemoteServer();
    const password = (cfg?.password ?? "") !== "" ? cfg?.password : roster?.password;
    if (!emptyHandled && (freshList === null || freshList.length === 0) && cfg && (password ?? "") !== "") {
      emptyHandled = true;
      void saveRemoteConfig({
        kind: cfg.kind,
        baseUrl: cfg.baseUrl,
        user: (cfg.user ?? "") !== "" ? cfg.user : roster?.user ?? "",
        password,
      }).then(() => loadRemote(true));
      return;
    }
    if (currentEpoch !== listEpoch) return;
    // A listing with songs is the proof the session works; an empty one has
    // now been answered by a fresh sign-in, and asking again would be the
    // same question on a loop.
    if (freshList !== null && freshList.length > 0) {
      emptyHandled = false;
      if (!tracksEqual(remoteTracks(), freshList)) {
        remoteTracks.set(freshList);
      }
    } else if (freshList !== null) {
      if (remoteTracks().length > 0) remoteTracks.set([]);
    }
    freshList = null;
    lastSyncAt = Date.now();
    remoteFetching.set(false);
    finishServerCache(targetServerId);
    return;
  }
  if (currentEpoch !== listEpoch) return;
  if (freshList !== null) freshList.push(...tracks);
  if (tracks.length >= REMOTE_PAGE) {
    try {
      engine.listRemote(freshList !== null ? freshList.length : remoteTracks().length, REMOTE_PAGE);
    } catch (error) {
      if (currentEpoch !== listEpoch) return;
      console.warn("neiro: 下一页远程列表没有发出去:", String(error));
      remoteFetching.set(false);
      finishServerCache(targetServerId);
    }
  } else {
    // The listing is whole: one swap, so the page reads one listing.
    if (currentEpoch !== listEpoch) return;
    if (freshList !== null) {
      emptyHandled = false;
      if (!tracksEqual(remoteTracks(), freshList)) {
        remoteTracks.set(freshList);
      }
    }
    freshList = null;
    lastSyncAt = Date.now();
    remoteFetching.set(false);
    finishServerCache(targetServerId);
  }
}

/** 一轮拉取结束：把完整列表存进当前远程来源的缓存（切 Tab 免重拉上屏，并持久化到 Storage）。 */
function finishServerCache(targetServerId?: string): void {
  let serverId = targetServerId || activeEngineServerId;
  if (!serverId) {
    const id = activeSourceId();
    if (id.startsWith("remote:")) {
      serverId = id.slice("remote:".length);
    } else {
      const first = servers()[0];
      if (first) serverId = first.id;
    }
  }
  if (!serverId) return;
  const current = [...remoteTracks()];
  serverCache.set(serverId, current);
  cacheVersion.update((v) => v + 1);
  void Storage.setItem(`remote:tracks:${serverId}`, JSON.stringify(current)).catch((e) =>
    console.warn("neiro: 远程曲目缓存没有存下:", String(e)),
  );
}

export function loadRemote(force = false): void {
  // Only a request that is still in flight blocks a new one. A flag left set
  // by a request that never answered used to make the page empty forever.
  if (!force && remoteFetching() && remoteLoadingState() === "loading") return;
  const thisEpoch = ++listEpoch;
  const srv =
    activeRemoteServer() ??
    servers().find((s) => s.id === activeEngineServerId) ??
    servers()[0] ??
    null;
  if (srv) {
    fetchingServerId = srv.id;
    fetchingBaseUrl = srv.baseUrl;
  } else {
    fetchingServerId = activeEngineServerId;
    fetchingBaseUrl = remoteConfig()?.baseUrl ?? "";
  }
  remoteFetching.set(true);
  // The cached rows stay on screen while this runs; `freshList` replaces them
  // in one swap when the last page lands.
  freshList = [];
  lastSyncAt = Date.now();
  remoteError.set("");
  remoteLoadingState.set(remoteTracks().length > 0 ? "idle" : "loading");
  try {
    engine.listRemote(0, REMOTE_PAGE);
  } catch (error) {
    if (thisEpoch !== listEpoch) return;
    reportEngineFailure(error, "远程列表失败");
    remoteFetching.set(false);
    remoteLoadingState.set("error");
  }
}

export function retryRemote(): void {
  if (remoteLoadingState() === "error" || remoteTracks().length === 0) loadRemote(true);
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
    remoteError.set(error.message);
    remoteFetching.set(false);
    remoteLoadingState.set("error");
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
  let name = path;
  if (name.startsWith("remote://")) {
    name = name.slice("remote://".length);
  } else {
    name = name.replace(/\\/g, "/").split("/").pop() ?? "";
  }
  const dot = name.lastIndexOf(".");
  if (dot <= 0) return null;
  const id = name.slice(0, dot);
  const container = name.slice(dot + 1).toLowerCase();
  const lists: RemoteTrack[][] = [remoteTracks(), ...serverCache.values()];
  for (const list of lists) {
    const found = list.find((t) => t.id === id && (t.container.toLowerCase() === container || !t.container));
    if (found) return found;
  }
  return null;
}

export const remoteTrackForPath = remoteTrackForCachePath;

// ---- 播放：fetch → fetched → play --------------------------------------------

export function remoteAsTrack(rt: RemoteTrack): Track {
  const ext = (rt.container || "flac").toLowerCase();
  const path = cachedRemotePath(rt) ?? `remote://${rt.id}.${ext}`;
  return asLocalTrack(rt, path);
}

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

/** 点远程列表的第 `index` 首:取流播放,并把后面 `QUEUE_AHEAD` 首依次取流
 * 后排队。远程曲目在缓存里才有本地路径,所以排队就是下载——一次只有一首在
 * 下,拿到路径就入队,顺序不会乱。 */
export function queueRemoteFrom(rows: RemoteTrack[], index: number): void {
  const at = Math.min(Math.max(index, 0), Math.max(rows.length - 1, 0));
  if (rows.length === 0) return;
  const first = rows[at];
  const other = rows.filter((_, i) => i !== at);
  const queuedOther = shuffle() ? shuffled(other) : rows.slice(at + 1);
  const rest = queuedOther.slice(0, QUEUE_AHEAD);

  withRemotePath(first, (firstPath) => {
    playTrack(asLocalTrack(first, firstPath));
    const step = (i: number) => {
      if (i >= rest.length) return;
      withRemotePath(rest[i], (path) => {
        try {
          engine.playLater([trackToQueueItem(asLocalTrack(rest[i], path))]);
        } catch (e) {
          console.warn("neiro: 远程排队失败:", String(e));
        }
        step(i + 1);
      });
    };
    step(0);
  });
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
 * instead of being refused. Multiple concurrent callers queue their callbacks. */
const fetchedActions = new Map<string, Array<(path: string) => void>>();

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
    const actions = fetchedActions.get(track.id);
    fetchedActions.delete(track.id);
    if (actions && actions.length > 0) {
      for (const act of actions) {
        try {
          act(path);
        } catch (e) {
          console.warn("neiro: withRemotePath callback failed:", String(e));
        }
      }
    } else {
      playTrack(asLocalTrack(track, path));
    }
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
 * If already cached, invokes `then` synchronously. Multiple callers for the same
 * row during download are queued and all invoked when the file arrives. */
export function withRemotePath(track: RemoteTrack, then: (path: string) => void): void {
  const cached = cachedRemotePath(track);
  if (cached) {
    then(cached);
    return;
  }
  const current = fetchedActions.get(track.id) ?? [];
  current.push(then);
  fetchedActions.set(track.id, current);

  if (fetchingId(track.id)) return;
  fetchingIds.update((set) => new Set(set).add(track.id));
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
    if (stale.size > 0) {
      for (const id of stale) {
        fetchedActions.delete(id);
        unfetchId(id);
      }
    }
  }, 30_000);
  cleanup(() => clearTimeout(timer));
});

registerRemoteCatalog(
  () => remoteTracks().map(remoteAsTrack),
  (path) => {
    const rt = remoteTrackForCachePath(path);
    return rt ? asLocalTrack(rt, path) : undefined;
  },
  () => remoteLoadingState(),
);

registerRemotePlayer({
  playRemoteByPath: (path) => {
    const rt = remoteTrackForCachePath(path);
    if (!rt) return false;
    playRemote(rt);
    return true;
  },
  withRemotePathByPath: (path, then) => {
    const rt = remoteTrackForCachePath(path);
    if (!rt) return false;
    withRemotePath(rt, then);
    return true;
  },
  resolveRemotePath: (path) => {
    const rt = remoteTrackForCachePath(path);
    return rt ? cachedRemotePath(rt) : null;
  },
});
