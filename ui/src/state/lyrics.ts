// Lyrics: what the file itself says, synced when it carries timestamps.
//
// The text comes from the track's own tag (`read_tags` → the field lofty
// calls `Lyrics`), not from a service. A file without the tag says so; a file
// with plain lines shows them unsynced rather than guessing where a line
// starts. LRC timestamps (`[01:23.45]`) are the only sync this reads, because
// they are the only sync a file carries.

import { effect, signal, Storage, untracked } from "nsh";
import { engine } from "../backend/engine";
import type { LyricsCandidate } from "../backend/types";
import { player } from "./player";

/** One lyric line: its time when the file timestamped it, else `null`. */
export interface LyricLine {
  at: number | null;
  text: string;
}

/** A track's lyrics, as far as the file reports them. */
export interface Lyrics {
  path: string;
  lines: LyricLine[];
  /** False when the file has no lyric tag at all. */
  present: boolean;
  /** False when the lines carry no usable timestamps. */
  synced: boolean;
}

const EMPTY: Lyrics = { path: "", lines: [], present: false, synced: false };

const cache = signal<Map<string, Lyrics>>(new Map());
const current = signal<Lyrics>(EMPTY);
const asking = new Set<string>();

/** The engine's field name for the lyric text (`lofty::ItemKey::Lyrics`). */
const LYRICS_KEY = "lyrics";

/** `[mm:ss]`, `[mm:ss.xx]`, `[hh:mm:ss.xx]` at the head of a line. */
const STAMP = /^\[(\d{1,2}):(\d{2})(?:[.:](\d{1,3}))?\]/;

function stampSeconds(match: RegExpMatchArray): number {
  const minutes = Number(match[1]);
  const seconds = Number(match[2]);
  const fraction = match[3] ? Number(`0.${match[3]}`) : 0;
  return minutes * 60 + seconds + fraction;
}

/** Splits lyric text into lines, reading LRC stamps where they exist.
 *
 * A line may carry several stamps (`[00:10.00][01:20.00]same words`), one per
 * repeat; each becomes its own line at its own time. */
export function parseLyrics(text: string): { lines: LyricLine[]; synced: boolean } {
  const lines: LyricLine[] = [];
  let synced = false;
  for (const raw of text.split(/\r?\n/)) {
    let rest = raw.trim();
    const stamps: number[] = [];
    while (true) {
      const match = rest.match(STAMP);
      if (!match) break;
      stamps.push(stampSeconds(match));
      rest = rest.slice(match[0].length).trim();
    }
    if (rest === "") continue;
    if (stamps.length === 0) {
      lines.push({ at: null, text: rest });
      continue;
    }
    synced = true;
    for (const at of stamps) lines.push({ at, text: rest });
  }
  if (synced) lines.sort((a, b) => (a.at ?? 0) - (b.at ?? 0));
  return { lines, synced };
}

/** The lyric field of a tag page, if it has one. */
export function lyricTextOf(fields: { key: string; value: string }[]): string {
  const hit = fields.find((field) => field.key.toLowerCase() === LYRICS_KEY);
  return hit?.value ?? "";
}

/** Asks the engine for `path`'s lyrics unless they are already known.
 *
 * Called from the Now Playing view; a file that reports no lyric tag is
 * cached as absent, so reopening the sheet does not re-read it every time. */
export function ensureLyrics(path: string): void {
  if (path === "" || cache().has(path) || asking.has(path)) return;
  asking.add(path);
  try {
    engine.readTags(path);
  } catch {
    // The engine is not connected: nothing to show, and nothing to retry
    // until it is.
    asking.delete(path);
  }
}

/** Binds the engine's tag answers into the cache. Called once, at startup. */
export function subscribeLyrics(): () => void {
  return engine.subscribeTags((page) => {
    asking.delete(page.path);
    const text = lyricTextOf(page.fields);
    const parsed = parseLyrics(text);
    const entry: Lyrics = {
      path: page.path,
      lines: parsed.lines,
      present: text.trim() !== "",
      synced: parsed.synced,
    };
    const next = new Map(cache());
    next.set(page.path, entry);
    cache.set(next);
    if (player().path === page.path) current.set(entry);
    // A file with no lyric tag of its own is the case the automatic search
    // exists for; the tag answer is the first moment that is known.
    if (!entry.present) untracked(() => autoFetchFor(page.path));
  });
}

/** The current track's lyrics; re-read when the track changes. */
export function trackLyrics(): Lyrics {
  const path = player().path ?? "";
  const chosen = adopted().get(path);
  if (chosen) return chosen;
  const hit = cache().get(path);
  if (hit) return hit;
  const fallback = current();
  return fallback.path === path ? fallback : EMPTY;
}

// ---- 在线搜索 --------------------------------------------------------------

/** Where the words a search adopted are remembered. Kept apart from the file
 * until the write-back exists: choosing words must not edit the audio. */
const ADOPTED_KEY = "lyrics:adopted";

const adopted = signal<Map<string, Lyrics>>(new Map());

export type LyricsSearchState =
  | { status: "idle" }
  | { status: "searching" }
  | { status: "done"; candidates: LyricsCandidate[] };

export const lyricsSearch = signal<LyricsSearchState>({ status: "idle" });

let searchStop: (() => void) | null = null;
/** The path the last search was asked for: its results must be adopted under
 * that path, not whatever plays when the button is pressed later. */
let searchPath: string | null = null;
let searchOpen = false;
/** The last words asked for, and whether the narrower retry has been spent. */
let askedFor: LyricsQuery | null = null;
let fallbackTried = false;

/** Whether the search sheet is up. Opening it starts a search for the track
 * that is playing; a second press on the panel reopens the last answer. */
export const lyricsSearchOpen = signal(false);

export function stopLyricsSearch(): void {
  if (searchStop !== null) {
    try {
      searchStop();
    } catch {}
    searchStop = null;
  }
}

export function setLyricsSearchOpen(open: boolean): void {
  lyricsSearchOpen.set(open);
  if (!open) {
    stopLyricsSearch();
    return;
  }
  // The previous track's answer is not this track's: opening the sheet starts
  // a new search and shows nothing of the old one while it runs.
  if ((player().path ?? "") === "") {
    // Nothing is playing: there is no song to ask about, and leaving the
    // sheet "searching" would be a spinner that never ends.
    lyricsSearch.set({ status: "done", candidates: [] });
    return;
  }
  lyricsSearch.set({ status: "searching" });
  startLyricsSearch();
}

/** What a search asks for. The fields start as the file reports them and the
 * listener may correct them: a provider indexes the words by title and artist,
 * and a tag that says "track 07" finds nothing. */
export interface LyricsQuery {
  title: string;
  artist: string;
  album: string;
  durationS: number | null;
}

/** The song's own name: a title carrying 「〜公生とかをりの演奏 Ver.〜」 asks the
 * provider for a version it does not index, and 「feat.」 after a dash belongs
 * to the artist line. Bracketed parts and trailing dash clauses go. */
export function cleanTitle(title: string): string {
  const trimmed = title
    .replace(/[〜~][^〜~]*[〜~]/g, " ")
    .replace(/[\(（\[【][^\)）\]】]*[\)）\]】]/g, " ")
    .replace(/\s+-\s+.*$/, " ")
    .replace(/\s{2,}/g, " ")
    .trim();
  return trimmed === "" ? title.trim() : trimmed;
}

/** The fields a search starts with, from the track that is playing. */
export function lyricsQueryForCurrent(): LyricsQuery {
  const tags = player();
  return {
    title: cleanTitle(tags.title || fileTitle(tags.path ?? "")),
    artist: tags.artist ?? "",
    album: tags.album ?? "",
    durationS: tags.durationMs ? Math.round(tags.durationMs / 1000) : null,
  };
}

import { isPluginEnabled } from "./plugins";

export const LYRICS_PROVIDER_IDS = [
  "builtin.lyrics.lrclib",
  "builtin.lyrics.netease",
  "builtin.lyrics.qqmusic",
  "builtin.lyrics.kugou",
];

export function enabledLyricsProviders(): { id: string; name: string }[] {
  const all = [
    { id: "builtin.lyrics.lrclib", name: "LRCLIB" },
    { id: "builtin.lyrics.netease", name: "网易云音乐" },
    { id: "builtin.lyrics.qqmusic", name: "QQ 音乐" },
    { id: "builtin.lyrics.kugou", name: "酷狗音乐" },
  ];
  return all.filter((p) => isPluginEnabled(p.id));
}

export function enabledLyricsCount(): number {
  return enabledLyricsProviders().length;
}

// ---- 自动检索 --------------------------------------------------------------

/** Whether a track with no embedded words fetches some online by itself.
 * Persisted: a search asks four services, which is a network round-trip the
 * listener opts into rather than one that happens on every track. */
const AUTO_KEY = "lyrics:auto";

export const autoLyricsEnabled = signal(true);

export function setAutoLyricsEnabled(on: boolean): void {
  autoLyricsEnabled.set(on);
  void Storage.setItem(AUTO_KEY, on ? "1" : "0").catch((error) =>
    console.warn("neiro: 自动歌词设置没有记住:", String(error)),
  );
  if (on) maybeAutoFetchLyrics();
}

/** Reads the stored preference once, at startup. */
export async function loadAutoLyricsSetting(): Promise<void> {
  try {
    const raw = await Storage.getItem(AUTO_KEY);
    // On unless the listener turned it off: a track without words should
    // fetch some by itself, which is the point of the feature.
    if (raw === "0") autoLyricsEnabled.set(false);
  } catch (error) {
    console.warn("neiro: 自动歌词设置没有读出来:", String(error));
  }
}

/** Paths the automatic search already tried, so a track is asked once a
 * session even when its answer was empty. */
const autoTried = new Set<string>();

/** The automatic search in flight, if any. The engine's answer carries no
 * request id, so the path it was asked for is the only correlation. */
let autoPending: string | null = null;
let autoStop: (() => void) | null = null;

/** Drops the in-flight automatic search: its answer is not wanted any more. */
function finishAuto(): void {
  if (autoStop !== null) {
    try {
      autoStop();
    } catch {}
    autoStop = null;
  }
  autoPending = null;
}

/** The best words above this score are adopted; a weaker answer would put
 * another song's words under this track, which is worse than none. */
const AUTO_THRESHOLD = 0.5;

/** Runs one automatic search for `path`, when the track has no words of its
 * own. A track whose tag has not been read yet is read first; the tag answer
 * calls back here. */
function autoFetchFor(path: string): void {
  if (!autoLyricsEnabled() || path === "" || autoTried.has(path)) return;
  if (adopted().has(path)) return;
  const known = cache().get(path);
  if (!known) {
    ensureLyrics(path);
    return;
  }
  if (known.present) return;
  const query = lyricsQueryForCurrent();
  if (player().path !== path || query.title.trim() === "") return;
  const enabled = enabledLyricsProviders();
  if (enabled.length === 0) return;

  // A previous track's answer is not this track's: drop it.
  finishAuto();
  autoTried.add(path);
  autoPending = path;

  autoStop = engine.subscribeLyricsFound((candidates) => {
    // Another request (or another track) came back first: leave the answer.
    if (autoPending !== path) return;
    finishAuto();
    if (player().path !== path || candidates.length === 0) return;
    const best = candidates
      .map((candidate) => ({ candidate, score: matchScore(candidate, query) }))
      .sort((left, right) => right.score - left.score)[0];
    const text = (best?.candidate.synced ?? best?.candidate.plain ?? "").trim();
    if (best && best.score >= AUTO_THRESHOLD && text !== "") {
      adoptLyrics(best.candidate, path);
    }
  });

  const providersJson = JSON.stringify(enabled.map((p) => p.id));
  try {
    engine.searchLyrics(query.artist, query.title, query.album, query.durationS, providersJson);
  } catch (error) {
    finishAuto();
    // The engine was not ready: let a later run try again.
    autoTried.delete(path);
    console.warn("neiro: 自动歌词搜索没有发出去:", String(error));
  }
}

/** The trigger for the track that is playing now. */
export function maybeAutoFetchLyrics(): void {
  if (!autoLyricsEnabled()) return;
  const path = player().path ?? "";
  if (path !== "") autoFetchFor(path);
}

// Read each track's tags once, whether or not the lyrics panel is open: the
// automatic search needs the "no lyric tag" answer, and the panel should have
// the words ready when it is opened.
effect(() => {
  const path = player().path ?? "";
  const engineReady = engine.health().status === "ready";
  if (path === "" || !engineReady) return;
  untracked(() => ensureLyrics(path));
});

// A track change, or the setting turned on, checks once. `untracked` keeps the
// store's internal reads from re-running this on every cache write.
effect(() => {
  const on = autoLyricsEnabled();
  const path = player().path ?? "";
  const engineReady = engine.health().status === "ready";
  if (!on || !engineReady || path === "") return;
  untracked(() => autoFetchFor(path));
});

/** Asks the provider for the current track's words. */
export function startLyricsSearch(query?: LyricsQuery): void {
  const path = player().path ?? "";
  if (path === "") return;
  searchPath = path;

  const enabled = enabledLyricsProviders();
  if (enabled.length === 0) {
    lyricsSearch.set({ status: "done", candidates: [] });
    return;
  }
  const providersJson = JSON.stringify(enabled.map((p) => p.id));

  if (searchStop === null) {
    searchStop = engine.subscribeLyricsFound((candidates) => {
      // A provider indexes 「奇洛李维斯回信」 under a name this file does not
      // carry, and an artist it spells differently: an empty answer with three
      // fields filled is worth one more ask, with the title alone.
      const asked = askedFor;
      if (candidates.length === 0 && !fallbackTried && asked && (asked.artist !== "" || asked.album !== "")) {
        fallbackTried = true;
        engine.searchLyrics("", asked.title, "", null, providersJson);
        return;
      }
      lyricsSearch.set({ status: "done", candidates });
    });
  }
  lyricsSearch.set({ status: "searching" });
  const asked = query ?? lyricsQueryForCurrent();
  askedFor = asked;
  fallbackTried = false;
  try {
    engine.searchLyrics(asked.artist, asked.title, asked.album, asked.durationS, providersJson);
  } catch (error) {
    console.warn("neiro: 歌词搜索没有发出去:", String(error));
    lyricsSearch.set({ status: "done", candidates: [] });
  }
}

// ---- 匹配度 ----------------------------------------------------------------

/** Letters, digits and CJK only: a provider writes 「キラメキ」 and 「キラメキ - EP
 * Version」 as different strings, and punctuation is what differs. */
function normalize(text: string): string {
  return text
    .toLowerCase()
    .replace(/[\(（\[【].*?[\)）\]】]/g, "")
    .replace(/[^\p{L}\p{N}]+/gu, "");
}

/** Sørensen–Dice over character bigrams.
 *
 * Bigrams rather than words because the titles here are Japanese and Chinese:
 * one has no spaces to split on, and a wrong character in a two-character word
 * still leaves the rest matching. `1` is the same string, `0` shares nothing. */
export function similarity(left: string, right: string): number {
  const a = normalize(left);
  const b = normalize(right);
  if (a === "" || b === "") return 0;
  if (a === b) return 1;
  if (a.length < 2 || b.length < 2) return 0;
  const grams = new Map<string, number>();
  for (let i = 0; i < a.length - 1; i += 1) {
    const gram = a.slice(i, i + 2);
    grams.set(gram, (grams.get(gram) ?? 0) + 1);
  }
  let hits = 0;
  for (let i = 0; i < b.length - 1; i += 1) {
    const gram = b.slice(i, i + 2);
    const left = grams.get(gram) ?? 0;
    if (left > 0) {
      grams.set(gram, left - 1);
      hits += 1;
    }
  }
  return (2 * hits) / (a.length - 1 + (b.length - 1));
}

/** How well a candidate answers the query, `0`..`1`.
 *
 * The three facts weigh in the order a listener notices a wrong answer: the
 * words of another song (title), the words of another singer, then a version
 * whose timing differs. Length is the loosest — a live take is the same song —
 * so it can only move a result, never decide it. */
export function matchScore(candidate: LyricsCandidate, query: LyricsQuery): number {
  const title = similarity(candidate.track, query.title);
  const artist =
    query.artist.trim() === "" ? 0.55 : similarity(candidate.artist, query.artist);
  const album =
    query.album.trim() === "" || candidate.album.trim() === ""
      ? 0.5
      : similarity(candidate.album, query.album);
  let length = 0.5;
  if (query.durationS && candidate.durationS > 0) {
    length = Math.max(0, 1 - Math.abs(candidate.durationS - query.durationS) / 45);
  }
  return Math.min(1, title * 0.5 + artist * 0.3 + album * 0.1 + length * 0.1);
}

/** The candidates of the last search, best first. A tie keeps the provider's
 * order, which is its own relevance ranking. */
export function rankedCandidates(query: LyricsQuery): { candidate: LyricsCandidate; score: number }[] {
  const state = lyricsSearch();
  if (state.status !== "done") return [];
  return state.candidates
    .map((candidate) => ({ candidate, score: matchScore(candidate, query) }))
    .sort((left, right) => right.score - left.score);
}

/** The file's own name, without directory or extension: the fallback title
 * when a file carries no tag. */
function fileTitle(path: string): string {
  const name = path.split(/[\\/]/).pop() ?? "";
  return name.replace(/\.[^.]+$/, "");
}

/** Use one candidate's words for `path`. The file itself is not touched. */
export function adoptLyrics(candidate: LyricsCandidate, path?: string): void {
  // The words belong to the track the search asked about; a track change
  // while the sheet is open must not write them under the new one.
  const target = path ?? searchPath ?? player().path ?? "";
  if (target === "") return;
  const text = candidate.synced ?? candidate.plain ?? "";
  const parsed = parseLyrics(text);
  const entry: Lyrics = { path: target, lines: parsed.lines, present: true, synced: parsed.synced };
  const next = new Map(adopted());
  next.set(target, entry);
  adopted.set(next);
  persistAdopted(next);
  const cacheNext = new Map(cache());
  cacheNext.set(target, entry);
  cache.set(cacheNext);
  if (player().path === target) current.set(entry);
}

async function persistAdopted(all: Map<string, Lyrics>): Promise<void> {
  const plain: Record<string, { text: string; synced: boolean }> = {};
  for (const [path, lyrics] of all) {
    // The lines are stored as the text they came from, so a later parser
    // change reads the same words with the new rules.
    const text = lyrics.lines
      .map((line) => (line.at === null ? line.text : `[${stampOf(line.at)}]${line.text}`))
      .join("\n");
    plain[path] = { text, synced: lyrics.synced };
  }
  await Storage.setItem(ADOPTED_KEY, JSON.stringify(plain)).catch((error) =>
    console.warn("neiro: 选中的歌词没有记住:", String(error)),
  );
}

function stampOf(seconds: number): string {
  const minutes = Math.floor(seconds / 60);
  const rest = seconds - minutes * 60;
  return `${String(minutes).padStart(2, "0")}:${rest.toFixed(2).padStart(5, "0")}`;
}

/** Reads what earlier choices remembered. Called once, at startup. */
export async function loadAdoptedLyrics(): Promise<void> {
  try {
    const raw = await Storage.getItem(ADOPTED_KEY);
    if (!raw) return;
    const stored = JSON.parse(raw) as Record<string, { text: string; synced: boolean }>;
    const next = new Map<string, Lyrics>();
    for (const [path, entry] of Object.entries(stored)) {
      const parsed = parseLyrics(entry.text);
      next.set(path, { path, lines: parsed.lines, present: true, synced: parsed.synced });
    }
    adopted.set(next);
  } catch (error) {
    console.warn("neiro: 记住的歌词没有读出来:", String(error));
  }
}

// ---- 写回文件 --------------------------------------------------------------

/** The write the listener is being asked to confirm: the text, and where it
 * would go. Nothing reaches the file before this is answered. */
export const lyricsWrite = signal<{ path: string; text: string } | null>(null);

/** The current words as the text a file stores: stamps kept when the lines
 * have them, so a written copy is still in step with the audio. */
export function lyricsTextOf(lines: LyricLine[]): string {
  return lines
    .map((line) => (line.at === null ? line.text : `[${stampOf(line.at)}]${line.text}`))
    .join("\n");
}

export function askWriteLyrics(): void {
  const path = player().path ?? "";
  const lyrics = trackLyrics();
  if (path === "" || !lyrics.present) return;
  lyricsWrite.set({ path, text: lyricsTextOf(lyrics.lines) });
}

/** The listener answered no: nothing reaches the file. */
export function cancelWriteLyrics(): void {
  lyricsWrite.set(null);
}

/** The listener answered yes. The file is edited; the panel then re-reads it,
 * because the file is now what the words come from. */
export function confirmWriteLyrics(): void {
  const asked = lyricsWrite();
  lyricsWrite.set(null);
  if (!asked) return;
  try {
    engine.writeLyrics(asked.path, asked.text);
    const next = new Map(adopted());
    next.delete(asked.path);
    adopted.set(next);
    persistAdopted(next);
    asking.add(asked.path);
    const reread = new Map(cache());
    reread.delete(asked.path);
    cache.set(reread);
  } catch (error) {
    console.warn("neiro: 歌词没有写进文件:", String(error));
  }
}

// ---- 手动对轴 --------------------------------------------------------------

/** How far ahead (positive) or behind the words this track's timing sits, in
 * milliseconds. Providers stamp a version, a listener hears another: the fix
 * is a shift, and it belongs to the track, not to the file. */
const OFFSET_KEY = "lyrics:offset";

const offsets = signal<Map<string, number>>(new Map());

export const lyricOffset = (): number => offsets().get(player().path ?? "") ?? 0;

export function setLyricOffset(ms: number): void {
  const path = player().path ?? "";
  if (path === "") return;
  const clamped = Math.max(-10_000, Math.min(10_000, Math.round(ms / 100) * 100));
  const next = new Map(offsets());
  if (clamped === 0) next.delete(path);
  else next.set(path, clamped);
  offsets.set(next);
  void Storage.setItem(OFFSET_KEY, JSON.stringify(Object.fromEntries(next))).catch((error) =>
    console.warn("neiro: 歌词偏移没有记住:", String(error)),
  );
}

/** Shifts the words by one step and says how far they now sit. */
export function nudgeLyricOffset(stepMs: number): void {
  setLyricOffset(lyricOffset() + stepMs);
}

export async function loadLyricOffsets(): Promise<void> {
  try {
    const raw = await Storage.getItem(OFFSET_KEY);
    if (!raw) return;
    const stored = JSON.parse(raw) as Record<string, number>;
    offsets.set(new Map(Object.entries(stored)));
  } catch (error) {
    console.warn("neiro: 歌词偏移没有读出来:", String(error));
  }
}

/** The line that is playing at `positionMs`, or -1 when the file is not
 * timestamped (or before the first line). */
export function activeLine(lines: LyricLine[], positionMs: number): number {
  const seconds = positionMs / 1000;
  let found = -1;
  for (let i = 0; i < lines.length; i += 1) {
    const at = lines[i].at;
    if (at === null) continue;
    if (at <= seconds + 0.05) found = i;
    else break;
  }
  return found;
}
