// Playback: the player's state as the UI shows it, and the intents that
// change it (spotish's store, over the Base protocol's vocabulary — no
// liked, shuffle or repeat: the engine has none, and none is invented). The
// store mirrors Base, moves the position on locally between its position
// events, and shows a control's change at once; the engine's word beats it
// once it catches up.

import { Animated, cleanup, computed, type CompositeAnimation, createAnimatedValue, effect, signal, Storage, untracked } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import {
  clock,
  positionAt,
  EMPTY_PLAYER,
  type Player,
  type QueueItem,
  type RepeatMode,
  type Track,
} from "../backend/types";
import { notify } from "./feedback";
import { activeSource } from "./sources";
import { trackForPath } from "./catalog";
import { shuffled } from "./shuffle";

let remoteHooks: {
  playRemoteByPath: (path: string) => boolean;
  withRemotePathByPath: (path: string, then: (fetched: string) => void) => boolean;
  resolveRemotePath?: (path: string) => string | null;
  cancelPlayIntent?: () => void;
} | null = null;
export function registerRemotePlayer(hooks: NonNullable<typeof remoteHooks>): void {
  remoteHooks = hooks;
}

// ---- The player as shown ----------------------------------------------------

/** A control's change on its way: shown until the engine agrees, or a
 * moment after its request is done, or a time limit. */
interface Pending {
  change: Partial<Player>;
  started: number;
  settledAt: number | null;
}

const SETTLE_GRACE = 1500;
const PENDING_LIMIT = 8000;

const pending = signal<Pending[]>([]);

function agrees(state: Player, change: Partial<Player>): boolean {
  for (const key of Object.keys(change) as (keyof Player)[]) {
    if (key === "at") continue;
    if (key === "positionMs") {
      if (Math.abs(positionAt(state, Date.now()) - (change.positionMs ?? 0)) > 2000) return false;
      continue;
    }
    if (state[key] !== change[key]) return false;
  }
  return true;
}

/** Drops the changes the engine caught up with, or that are over. */
function prune(): void {
  const state = engine.player();
  const now = Date.now();
  const kept = pending().filter(
    (p) =>
      !agrees({ ...EMPTY_PLAYER, ...state }, p.change) &&
      now - p.started < PENDING_LIMIT &&
      (p.settledAt === null || now - p.settledAt < SETTLE_GRACE),
  );
  if (kept.length !== pending().length) pending.set(kept);
}

effect(() => {
  engine.player();
  untracked(() => prune());
});


/** The optimistic layer (values still in flight over the engine's). */
const optimistic = computed<Partial<Player>>(() => Object.assign({}, ...pending().map((p) => p.change)));

/** The player as the UI shows it: the engine's state with the values of
 * controls still on their way over it. */
export const player = computed<Player>(() => ({ ...EMPTY_PLAYER, ...engine.player(), ...optimistic() }));

// ---- The position -----------------------------------------------------------

const now = signal(Date.now());

// While playing, the clock ticks so the shown times move on: once a
// second, just as the shown second changes (the bar moves natively
// through `positionFill`).
effect(() => {
  const p = player();
  if (p.state !== "playing") return;
  let timer: ReturnType<typeof setTimeout> | null = null;
  const tick = () => {
    const t = Date.now();
    now.set(t);
    const at = positionAt(p, t);
    if (p.durationMs > 0 && at >= p.durationMs) return;
    timer = setTimeout(tick, 1000 - (at % 1000) + 4);
  };
  tick();
  cleanup(() => {
    if (timer !== null) clearTimeout(timer);
  });
});

const seekDraft = signal<number | null>(null);
const volumeDraft = signal<number | null>(null);

/** The position to show: the drag in progress, else the player's. */
export const position = (): number => seekDraft() ?? positionAt(player(), now());

/** The position as a share of the track (0..1), an animated value moving
 * natively while it plays, with no JS per frame: linear timing to the end
 * over the time left, set again on every change of where the engine is.
 * The position bar draws its range from it. */
export const positionFill = createAnimatedValue(0);
let fillAnchor = "";
let fillRun: CompositeAnimation | null = null;
/** Longer than any track: the end is held this long before the hold
 * repeats (a deadline, not frames). */
const HOLD_MS = 86_400_000;
effect(() => {
  const draft = seekDraft();
  const p = player();
  const duration = p.durationMs;
  const anchor = `${draft}|${p.positionMs}|${p.at}|${p.state}|${duration}`;
  if (anchor === fillAnchor) return;
  fillAnchor = anchor;
  const max = Math.max(1, duration);
  const at = draft ?? positionAt(p, Date.now());
  const from = Math.min(1, Math.max(0, at / max));
  fillRun?.stop();
  fillRun = null;
  positionFill.set(from);
  if (draft === null && p.state === "playing" && duration > 0 && from < 1) {
    fillRun = Animated.loop(
      Animated.sequence([
        Animated.timing(positionFill, { toValue: 1, duration: Math.max(1, duration - at), easing: "linear" }),
        Animated.delay(HOLD_MS),
      ]),
      { resetBeforeIteration: false },
    );
    void fillRun.start();
  }
});

/** The volume to show: the drag in progress, else the engine's (a value
 * the render path has not reported is not invented — the slider disables
 * instead). */
export const volumeLevel = (): number | null => {
  const p = player();
  if (volumeDraft() !== null) return volumeDraft();
  return p.volume;
};

// ---- The queue and the ready checks ------------------------------------------

/** The engine's queue (what `play(path)` put there, and its position). */
export const queue = computed(() => engine.queue());

/** Where many views follow, each on its own: a position event changes
 * none of them (every row asks whether its path is the one playing). */
const loadedPath = computed(() => player().path);
const playingNow = computed(() => player().state === "playing");
const loadedContent = computed(() => !!(player().path || player().title));

/** Whether something is loaded at all (stopped shows an empty bar). */
export const hasTrack = (): boolean => loadedContent();

/** Whether the controls can act (tracked). */
export const controlsEnabled = (): boolean => loadedContent() && player().state !== "error" && player().state !== "deviceLost";

/** Whether `path` is the track loaded (tracked). */
export function isCurrentTrack(path: string): boolean {
  if (!path) return false;
  const current = loadedPath();
  if (!current) return false;
  if (current === path) return true;
  if (path.startsWith("remote://") || current.startsWith("remote://")) {
    const nameOf = (p: string) => {
      if (p.startsWith("remote://")) return p.slice("remote://".length);
      return p.replace(/\\/g, "/").split("/").pop() ?? "";
    };
    return nameOf(path) !== "" && nameOf(path) === nameOf(current);
  }
  return false;
}

/** Whether the loaded track is playing (tracked). */
export const isPlaying = (): boolean => playingNow();

/** The engine's queue has an entry past the current one (checked per
 * queue change, not guessed). */
export const canNext = (): boolean => {
  const q = queue();
  return q.index !== null && q.index + 1 < q.queue.length;
};

export const canPrev = (): boolean => loadedContent();

export const gaplessEnabled = (): boolean => !engine.settings()?.crossfadeEnabled;

export function setGaplessEnabled(enabled: boolean): void {
  const before = engine.settings();
  if (!before) return;
  try {
    engine.setCrossfade(!enabled, before.crossfadeDurationMs, before.crossfadeCurve);
  } catch (error) {
    reportEngineFailure(error, enabled ? "启用 Gapless 失败" : "停用 Gapless 失败");
  }
}

// ---- The intents ---------------------------------------------------------------

/** Sends a control: the change shows at once, the request goes out at the
 * same time, and the engine's word catches up. A refusal drops the change
 * and says so (one plain sentence). */
function send(change: Partial<Player>, action: () => void, label = "播放失败"): void {
  const entry: Pending | null = Object.keys(change).length > 0 ? { change, started: Date.now(), settledAt: null } : null;
  if (entry) pending.update((list) => [...list, entry]);
  try {
    action();
  } catch (error) {
    if (entry) pending.update((list) => list.filter((p) => p !== entry));
    reportEngineFailure(error, label);
  }
}

export function toggle(): void {
  const p = player();
  if (!loadedContent()) return;
  if (p.state === "playing") {
    send({ state: "paused" }, () => engine.pause(), "暂停失败");
    return;
  }
  if (p.state === "paused") {
    send({ state: "playing" }, () => engine.resume(), "继续播放失败");
    return;
  }
  // `deviceLost` / `error`: the stream is gone, so `resume` has nothing to
  // resume and the transport would be dead for the rest of the session. The
  // engine's own way back is to open the track again, which is what `play`
  // does; it starts from 0, which is honest about the stream that was lost.
  if (p.path) {
    send({ state: "playing", positionMs: 0, at: Date.now() }, () => engine.play(p.path as string), "重新播放失败");
  }
}

/** Plays (the system's media controls); nothing when it plays already. */
export function play(): void {
  if (!isPlaying()) toggle();
}

export function pause(): void {
  if (isPlaying()) toggle();
}

export function trackToQueueItem(t: Track): QueueItem {
  return {
    id: t.path,
    path: t.path,
    title: t.title,
    artist: t.artist,
    durationMs: t.durationMs,
    duration_ms: t.durationMs,
  };
}

const SHUFFLE_KEY = "player:shuffle";
const REPEAT_KEY = "player:repeat";

export const repeatMode = signal<RepeatMode>("off");
export const shuffleMode = signal<boolean>(false);

export type PlaybackMode = "sequential" | "shuffle" | "repeatAll" | "repeatOne";

/** The one transport mode implied by the mutually exclusive engine modes. */
export const playbackMode = (): PlaybackMode => {
  if (repeatMode() === "one") return "repeatOne";
  if (repeatMode() === "all") return "repeatAll";
  if (shuffleMode()) return "shuffle";
  return "sequential";
};

// 启动读回持久化的 repeat 和 shuffle 设置，并在连接后同步给引擎
void (async () => {
  try {
    const savedShuffle = await Storage.getItem(SHUFFLE_KEY);
    if (savedShuffle !== null) {
      shuffleMode.set(savedShuffle === "1");
    }
    const savedRepeat = await Storage.getItem(REPEAT_KEY);
    if (savedRepeat === "off" || savedRepeat === "all" || savedRepeat === "one") {
      repeatMode.set(savedRepeat);
    }
    // 旧版存过随机 + 循环同时开。现在只有一个模式按钮：循环优先
    // （与 playbackMode() 的顺序一致），随机落回关闭，按钮与引擎一致。
    if (repeatMode() !== "off" && shuffleMode()) {
      shuffleMode.set(false);
      void Storage.setItem(SHUFFLE_KEY, "0").catch((e) => console.warn("neiro: shuffle 没有记住:", String(e)));
    }
  } catch (error) {
    console.warn("neiro: 播放模式持久化读取失败:", String(error));
  }
})();

// 当引擎就绪时，将持久化的模式同步给引擎
let syncedInitialModes = false;
effect(() => {
  if (engine.health().status !== "ready" || syncedInitialModes) return;
  syncedInitialModes = true;
  untracked(() => {
    try {
      if (shuffleMode()) engine.setShuffleMode(true);
      if (repeatMode() !== "off") engine.setRepeatMode(repeatMode());
    } catch (e) {
      console.warn("neiro: 初始化同步 shuffle/repeat 失败:", String(e));
    }
  });
});

// 当引擎上报 snapshot 时，单一事实源自动同步回信号
effect(() => {
  const snap = engine.queue().snapshot;
  if (!snap) return;
  untracked(() => {
    const nextRepeat = snap.repeatMode ?? repeatMode();
    // One mode at a time: an engine that reports both is folded to repeat,
    // the same way playbackMode() reads them.
    const nextShuffle = (snap.shuffleEnabled ?? shuffleMode()) && nextRepeat === "off";
    if (nextRepeat !== repeatMode()) repeatMode.set(nextRepeat);
    if (nextShuffle !== shuffleMode()) shuffleMode.set(nextShuffle);
  });
});

export function cycleRepeatMode(): void {
  const current = repeatMode();
  const next: RepeatMode = current === "off" ? "all" : current === "all" ? "one" : "off";
  setRepeatMode(next);
}

export function setRepeatMode(mode: RepeatMode): void {
  if (mode === repeatMode() && (mode === "off" || !shuffleMode())) return;
  if (mode !== "off" && shuffleMode()) setShuffle(false);
  repeatMode.set(mode);
  void Storage.setItem(REPEAT_KEY, mode).catch((e) => console.warn("neiro: repeat 没有记住:", String(e)));
  try {
    engine.setRepeatMode(mode);
  } catch (e) {
    reportEngineFailure(e, "设置循环模式失败");
  }
}

/** Turns shuffle on or off without repeating the same engine command. */
export function setShuffle(enabled: boolean): void {
  if (enabled === shuffleMode()) return;
  if (enabled && repeatMode() !== "off") setRepeatMode("off");
  shuffleMode.set(enabled);
  void Storage.setItem(SHUFFLE_KEY, enabled ? "1" : "0").catch((e) =>
    console.warn("neiro: shuffle 没有记住:", String(e)),
  );
  try {
    engine.setShuffleMode(enabled);
  } catch (e) {
    reportEngineFailure(e, "设置随机播放失败");
  }
}

export function toggleShuffle(): void {
  setShuffle(!shuffleMode());
}

export function setPlaybackMode(mode: PlaybackMode): void {
  setShuffle(mode === "shuffle");
  setRepeatMode(mode === "repeatOne" ? "one" : mode === "repeatAll" ? "all" : "off");
}

export function cyclePlaybackMode(): void {
  const modes: PlaybackMode[] = ["sequential", "shuffle", "repeatAll", "repeatOne"];
  const next = modes[(modes.indexOf(playbackMode()) + 1) % modes.length];
  setPlaybackMode(next);
}

export function playNextTrack(track: Track): void {
  if (track.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(track.path, (cachedPath) => {
        playNextTrack({ ...track, path: cachedPath });
      })
    ) {
      return;
    }
  }
  engine.playNext([trackToQueueItem(track)]);
  notify(`已设为下一首播放：${track.title || "曲目"}`);
}

export function playLaterTrack(track: Track): void {
  if (track.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(track.path, (cachedPath) => {
        playLaterTrack({ ...track, path: cachedPath });
      })
    ) {
      return;
    }
  }
  engine.playLater([trackToQueueItem(track)]);
  notify(`已设为稍后播放：${track.title || "曲目"}`);
}

/** Plays one track: the queue becomes it and it starts. What shows the
 * moment it is picked is itself, playing, until the engine names its own
 * state. */
export function playTrack(track: Track): void {
  // A new play supersedes a remote fetch still on its way: that answer must
  // not take over the track the listener just chose.
  remoteHooks?.cancelPlayIntent?.();
  if (track.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(track.path, (cachedPath) => {
        playTrack({ ...track, path: cachedPath });
      })
    ) {
      return;
    }
  }
  const at = Date.now();
  send(
    {
      path: track.path,
      title: track.title,
      artist: track.artist,
      album: track.album,
      state: "playing",
      positionMs: 0,
      durationMs: track.durationMs,
      at,
    },
    () => {
      engine.playContext([trackToQueueItem(track)], 0);
    },
    "播放失败",
  );
}

/** 点一首歌时,后面跟着进队列的曲目数。
 *
 * 十首:一张列表里点歌的人要的是"接着放",不是"把四千首放进来"——队列面
 * 板要能一眼看完,而且引擎的队列是可编辑的,长队只会让编辑变难。 */
export const QUEUE_AHEAD = 10;

/** Tracks the one active context for progressive remote queueing. A new
 * click replaces it; an old fetch that lands after that is ignored. */
interface PlayContext {
  rows: Track[];
  pathIndex: Map<string, number>;
  cursor: number;
  current: number;
  queued: Set<string>;
  inFlight: Set<string>;
}

let playContext: PlayContext | null = null;
let contextFilling = false;

/** 从列表点第 `index` 首:它开始播,后面 `QUEUE_AHEAD` 首按列表顺序进队
 * 列。换列表就换一批——队列属于"我现在在听的这张列表"。 */
export function playFromRows(rows: Track[], index: number): void {
  if (rows.length === 0) return;
  playQueue(rows, index);
}

/** “播放全部”:顺序时从第一首开始;随机时首曲也随机,然后由 playQueue
 * 以 shuffleMode 的一次性请求序接着排。 */
export function playAll(rows: Track[]): void {
  toggleShuffle();
  const start = shuffleMode() && rows.length > 0 ? Math.floor(Math.random() * rows.length) : 0;
  playQueue(rows, start);
}

/** Plays the clicked row immediately and fills its context in order. An
 * uncached remote row is fetched before queueing, and one fill stops at the
 * first such row: the next one is fetched when playback reaches it. */
export function playQueue(rows: Track[], firstIndex = 0): void {
  if (rows.length === 0) return;
  const idx = Math.min(Math.max(firstIndex, 0), rows.length - 1);
  const first = rows[idx];
  const start = shuffleMode() ? 0 : idx;
  const requestedOrder = shuffleMode()
    ? [first, ...shuffled(rows.filter((_, i) => i !== idx))]
    : rows;
  const firstCachedPath = first.path.startsWith("remote://") ? remoteHooks?.resolveRemotePath?.(first.path) ?? null : null;
  const pathIndex = new Map<string, number>();
  requestedOrder.forEach((row, at) => pathIndex.set(row.path, at));
  if (firstCachedPath) pathIndex.set(firstCachedPath, start);
  playContext = {
    rows: requestedOrder,
    pathIndex,
    cursor: start,
    current: start,
    queued: new Set(firstCachedPath ? [first.path, firstCachedPath] : [first.path]),
    inFlight: new Set(),
  };

  if (first.path.startsWith("remote://") && !remoteHooks?.resolveRemotePath?.(first.path)) {
    const context = playContext;
    const startRemote = () => {
      context.inFlight.add(first.path);
      const handled = remoteHooks?.withRemotePathByPath(first.path, (cachedPath) => {
        if (playContext !== context) return;
        context.inFlight.delete(first.path);
        context.queued.add(first.path);
        context.queued.add(cachedPath);
        context.pathIndex.set(cachedPath, start);
        playTrack({ ...first, path: cachedPath });
        fillPlayContext();
      }) ?? false;
      if (!handled) context.inFlight.delete(first.path);
      return handled;
    };
    if (startRemote()) return;
    reportEngineFailure(new Error("E_REMOTE: 服务器没有回这个文件的缓存路径"), "取流失败");
    const next = requestedOrder.find((row, at) => {
      if (at <= start) return false;
      if (!row.path.startsWith("remote://")) return true;
      return remoteHooks?.resolveRemotePath?.(row.path) !== null;
    });
    if (!next) return;
    const nextAt = requestedOrder.indexOf(next);
    const nextCachedPath = next.path.startsWith("remote://") ? remoteHooks?.resolveRemotePath?.(next.path) ?? null : null;
    playContext.cursor = nextAt;
    playContext.current = nextAt;
    playContext.pathIndex.set(next.path, nextAt);
    if (nextCachedPath) playContext.pathIndex.set(nextCachedPath, nextAt);
    playContext.queued.add(next.path);
    if (nextCachedPath) playContext.queued.add(nextCachedPath);
    playTrack(next);
    fillPlayContext();
    return;
  }
  playTrack(first);
  fillPlayContext();
}

/** Enqueues rows after the cursor in original order. One uncached remote
 * row may be in flight; `playLater` only receives local cached paths. */
function fillPlayContext(): void {
  if (!playContext || contextFilling) return;
  const context = playContext;
  contextFilling = true;
  try {
    while (context.cursor < context.rows.length) {
      if (context.cursor - context.current >= QUEUE_AHEAD) return;
      const row = context.rows[context.cursor];
      const isRemote = row.path.startsWith("remote://");
      const cached = isRemote ? remoteHooks?.resolveRemotePath?.(row.path) ?? null : null;
      if (isRemote && !cached) {
        if (context.inFlight.has(row.path)) return;
        context.inFlight.add(row.path);
        const handled = remoteHooks?.withRemotePathByPath(row.path, (cachedPath) => {
          if (playContext !== context) return;
          context.inFlight.delete(row.path);
          context.queued.add(row.path);
          context.queued.add(cachedPath);
          context.pathIndex.set(cachedPath, context.cursor);
          try {
            engine.playLater([trackToQueueItem({ ...row, path: cachedPath })]);
          } catch (error) {
            console.warn("neiro: 远程排队失败:", String(error));
          }
          context.cursor += 1;
        }) ?? false;
        if (!handled) {
          context.inFlight.delete(row.path);
          context.cursor += 1;
          continue;
        }
        return;
      }
      const queueable = isRemote && cached ? { ...row, path: cached } : row;
      if (queueable.path !== row.path) context.pathIndex.set(queueable.path, context.cursor);
      if (!context.queued.has(queueable.path)) {
        context.queued.add(queueable.path);
        try {
          engine.playLater([trackToQueueItem(queueable)]);
        } catch (error) {
          console.warn("neiro: 排队失败:", String(error));
        }
      } else {
        context.queued.add(queueable.path);
      }
      context.cursor += 1;
    }
  } finally {
    contextFilling = false;
  }
}

effect(() => {
  const path = player().path;
  const context = playContext;
  if (!context || !path) return;
  const at = context.pathIndex.get(path) ?? -1;
  if (at < 0) {
    playContext = null;
    return;
  }
  context.current = at;
  if (at > context.cursor) context.cursor = at;
  if (context.rows[at].path.startsWith("remote://") && !context.queued.has(context.rows[at].path)) {
    context.queued.add(context.rows[at].path);
    context.cursor = Math.max(context.cursor, at + 1);
  }
  untracked(() => fillPlayContext());
});

/** A remote download failed before the context ever heard a path. When it
 * was the row playback was waiting on, take the next row that can play
 * (local, or a remote file already cached) instead of stalling. */
export function notifyRemoteFetchFailed(id: string): void {
  const context = playContext;
  if (!context) return;
  const prefix = `remote://${id}.`;
  const path = Array.from(context.pathIndex.keys()).find((candidate) => candidate.startsWith(prefix));
  if (!path) return;
  const at = context.pathIndex.get(path) ?? -1;
  context.inFlight.delete(path);
  if (at < 0) return;
  if (at !== context.current) {
    // A prefetch failed: move past it so the queue keeps filling.
    if (at >= context.cursor) {
      context.cursor = at + 1;
      fillPlayContext();
    }
    return;
  }
  const nextAt = context.rows.findIndex(
    (row, index) =>
      index > at &&
      (!row.path.startsWith("remote://") || (remoteHooks?.resolveRemotePath?.(row.path) ?? null) !== null),
  );
  if (nextAt < 0) return;
  const next = context.rows[nextAt];
  context.cursor = nextAt;
  context.current = nextAt;
  context.pathIndex.set(next.path, nextAt);
  context.queued.add(next.path);
  playTrack(next);
  fillPlayContext();
}

/** Test seam: advance the context by the exact index of the playing path
 * and fill again (the same logic the player effect uses). */export function advancePlayContextToPath(path: string | null): void {
  if (!playContext || !path) return;
  const at = playContext.pathIndex.get(path) ?? -1;
  if (at < 0) {
    playContext = null;
    return;
  }
  playContext.current = at;
  if (at > playContext.cursor) playContext.cursor = at;
  if (playContext.rows[at].path.startsWith("remote://") && !playContext.queued.has(playContext.rows[at].path)) {
    playContext.queued.add(playContext.rows[at].path);
    playContext.cursor = Math.max(playContext.cursor, at + 1);
  }
  fillPlayContext();
}

export function playQueueAt(index: number): void {
  const q = queue();
  if (index < 0 || index >= q.queue.length) return;
  const target = q.queue[index];
  const after = q.queue.slice(index + 1);
  const tr = trackForPath(target);
  if (tr) {
    playTrack(tr);
  } else {
    send(
      {
        path: target,
        state: "playing",
        positionMs: 0,
        at: Date.now(),
      },
      () => engine.play(target),
      "播放失败",
    );
  }
  if (after.length > 0) {
    try {
      engine.enqueue(after);
    } catch (e) {
      reportEngineFailure(e, "入队失败");
    }
  }
}

export function prev(): void {
  if (!canPrev()) return;
  try {
    engine.previous();
  } catch (error) {
    reportEngineFailure(error, "上一曲失败");
  }
}

export function next(): void {
  if (!canNext()) return;
  try {
    engine.next();
  } catch (error) {
    reportEngineFailure(error, "下一曲失败");
  }
}

/** The position drag moved: shown, not sent. */
export function dragSeek(ms: number): void {
  seekDraft.set(ms);
}

/** Seeks to `ms` (the drag ended there). */
export function seek(ms: number): void {
  seekDraft.set(null);
  send({ positionMs: ms, at: Date.now() }, () => engine.seek(ms / 1000), "跳转失败");
}

/** The volume drag moved: shown, not sent. */
export function dragVolume(value: number | null): void {
  volumeDraft.set(value);
}

/** Sets the volume (the drag ended there). */
export function setVolume(value: number): void {
  volumeDraft.set(null);
  send({ volume: value }, () => engine.setVolume(value), "音量失败");
}

/** Volume up one notch (the keyboard's command). */
export function bumpVolume(amount: number): void {
  const current = volumeLevel();
  if (current === null) return;
  setVolume(Math.min(1, Math.max(0, current + amount)));
}

/** `m:ss` of where it is now, for the clock beside the bar. */
export const positionClock = (): string => clock(position());

export const durationClock = () => clock(player().durationMs, hasTrack());
