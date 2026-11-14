// Lyrics: what the file itself says, synced when it carries timestamps.
//
// The text comes from the track's own tag (`read_tags` → the field lofty
// calls `Lyrics`), not from a service. A file without the tag says so; a file
// with plain lines shows them unsynced rather than guessing where a line
// starts. LRC timestamps (`[01:23.45]`) are the only sync this reads, because
// they are the only sync a file carries.

import { signal } from "nsh";
import { engine } from "../backend/engine";
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
  });
}

/** The current track's lyrics; re-read when the track changes. */
export function trackLyrics(): Lyrics {
  const path = player().path ?? "";
  const hit = cache().get(path);
  if (hit) return hit;
  const fallback = current();
  return fallback.path === path ? fallback : EMPTY;
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
