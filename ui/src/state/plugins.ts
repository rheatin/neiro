// Plugin state store, scoped permission gate, and hot-swappable shell registry.

import { signal, Storage } from "nsh";
import { engine } from "../backend/engine";
import type { PluginInfo, ProviderDescriptor } from "../backend/types";

const ACTIVE_SHELL_KEY = "neiro:active-shell-id";
const DISABLED_PLUGINS_KEY = "neiro:disabled-plugins";

export const DEFAULT_SHELL_ID = "builtin.shell.industrial-rack";
export const STUDIO_MINIMAL_SHELL_ID = "builtin.shell.studio-minimal";

/** Permanent core plugins that cannot be dynamically disabled. */
export const LOCKED_PLUGIN_IDS = new Set<string>([
  DEFAULT_SHELL_ID,
  "builtin.expander.cue",
  "builtin.expander.sacd",
  "builtin.decoder.dsd",
  "builtin.dsp.soxr",
]);

/** Online lyrics provider plugins across the multi-source matrix. */
export const LYRICS_PLUGINS: PluginInfo[] = [
  {
    id: "builtin.lyrics.lrclib",
    name: "LRCLIB 同步歌词",
    version: "1.0.0",
    author: "Char ONE",
    description: "开源同步歌词库（欧美曲库强，支持逐字时间轴与纯文本）",
    tier: "ui",
    category: "lyrics.provider",
    builtin: true,
    enabled: true,
    requiresRestart: false,
    permissions: ["lyrics.read"],
  },
  {
    id: "builtin.lyrics.netease",
    name: "网易云音乐歌词",
    version: "1.0.0",
    author: "Char ONE",
    description: "网易云音乐在线歌词检索（华语、日音、ACG 覆盖极佳）",
    tier: "ui",
    category: "lyrics.provider",
    builtin: true,
    enabled: true,
    requiresRestart: false,
    permissions: ["lyrics.read"],
  },
  {
    id: "builtin.lyrics.qqmusic",
    name: "QQ 音乐歌词",
    version: "1.0.0",
    author: "Char ONE",
    description: "QQ 音乐在线歌词检索（华语流行、港台经典时间轴极全）",
    tier: "ui",
    category: "lyrics.provider",
    builtin: true,
    enabled: true,
    requiresRestart: false,
    permissions: ["lyrics.read"],
  },
  {
    id: "builtin.lyrics.kugou",
    name: "酷狗音乐歌词",
    version: "1.0.0",
    author: "Char ONE",
    description: "酷狗音乐在线歌词检索（冷门老歌、Live 现场、翻唱极强）",
    tier: "ui",
    category: "lyrics.provider",
    builtin: true,
    enabled: true,
    requiresRestart: false,
    permissions: ["lyrics.read"],
  },
];

export function isStudioMinimalShell(id: string = activeShellId()): boolean {
  return id === STUDIO_MINIMAL_SHELL_ID || id === "builtin.ui.shell.studio-minimal";
}

export function isClassicShell(id: string = activeShellId()): boolean {
  return (
    id === DEFAULT_SHELL_ID ||
    id === "builtin.ui.shell.classic" ||
    !isStudioMinimalShell(id)
  );
}

export class PermissionDeniedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PermissionDeniedError";
  }
}

export interface NeiroPluginContext {
  manifest: PluginInfo;
  hasPermission(perm: string): boolean;
  writeTags(path: string, tags: Record<string, string>): Promise<void>;
  writeLyrics(path: string, lyrics: string): Promise<void>;
  setArtwork(path: string, mime: string, data: Uint8Array): Promise<void>;
  addFolder(path: string): void;
  forgetFolder(path: string): void;
  setRemote(kind: string, params: Record<string, string>): void;
  subscribe(disposer: () => void): void;
  dispose(): void;
}

export function createPluginContext(manifest: PluginInfo): NeiroPluginContext {
  const disposers: (() => void)[] = [];
  const perms = new Set(manifest.permissions ?? []);

  const assertPerm = (perm: string) => {
    if (!perms.has(perm)) {
      throw new PermissionDeniedError(`Missing permission: ${perm}`);
    }
  };

  return {
    manifest,
    hasPermission: (perm: string) => perms.has(perm),
    writeTags: async (path, _tags) => {
      assertPerm("tags.write");
      if (!path) throw new Error("path is required");
      console.warn("neiro: writeTags: 底层引擎尚未开放标签批量重写 ABI");
    },
    writeLyrics: async (path, lyrics) => {
      assertPerm("tags.write");
      engine.writeLyrics(path, lyrics);
    },
    setArtwork: async (path, _mime, _data) => {
      assertPerm("tags.write");
      if (!path) throw new Error("path is required");
      console.warn("neiro: setArtwork: 底层引擎尚未开放内嵌封面重写 ABI");
    },
    addFolder: (path: string) => {
      assertPerm("library.manage");
      engine.watchFolder(path);
    },
    forgetFolder: (path: string) => {
      assertPerm("library.manage");
      engine.forgetFolder(path);
    },
    setRemote: (kind: string, params: Record<string, string>) => {
      assertPerm("remote.manage");
      engine.setRemote(kind, params);
    },
    subscribe: (disposer: () => void) => {
      disposers.push(disposer);
    },
    dispose: () => {
      for (const d of disposers.splice(0)) {
        try {
          d();
        } catch (e) {
          console.warn("neiro: plugin disposer error:", e);
        }
      }
    },
  };
}

export const plugins = signal<PluginInfo[]>([]);
export const remoteProviders = signal<ProviderDescriptor[]>([]);
export const pluginsLoaded = signal(false);
export const disabledPlugins = signal<Set<string>>(new Set());
export const activeShellId = signal<string>(DEFAULT_SHELL_ID);
export const shellError = signal<{ shellId: string; message: string } | null>(null);
export const devWatchEnabled = signal(false);

const externalShells = new Map<string, (ctx: NeiroPluginContext) => unknown>();

export function registerExternalUiShell(id: string, render: (ctx: NeiroPluginContext) => unknown): void {
  externalShells.set(id, render);
}

export function getExternalUiShell(id: string): ((ctx: NeiroPluginContext) => unknown) | undefined {
  return externalShells.get(id);
}

/** Check whether a plugin is enabled. Permanent core plugins are always true. */
export function isPluginEnabled(id: string): boolean {
  if (LOCKED_PLUGIN_IDS.has(id)) return true;
  return !disabledPlugins().has(id);
}

/** Check whether a remote protocol is enabled. */
export function isRemoteKindEnabled(kind: string): boolean {
  if (kind === "jellyfin" || kind === "jellyfin_token") {
    return isPluginEnabled("jellyfin");
  }
  if (kind === "navidrome") {
    return isPluginEnabled("navidrome");
  }
  if (kind === "webdav") {
    return isPluginEnabled("webdav");
  }
  return isPluginEnabled(kind);
}

/** Check whether the 60Hz FFT visualizer plugin is enabled. */
export function isVisualizerPluginEnabled(): boolean {
  return isPluginEnabled("builtin.visualizer.signal-chain");
}

/** Merges engine plugins, folds jellyfin/jellyfin_token into one, appends lyrics plugins, and marks locked status. */
function mergePluginList(raw: PluginInfo[]): PluginInfo[] {
  const list: PluginInfo[] = [];
  let hasJellyfin = false;

  for (const p of raw) {
    if (p.id === "jellyfin" || p.id === "jellyfin_token") {
      if (!hasJellyfin) {
        hasJellyfin = true;
        list.push({
          id: "jellyfin",
          name: "Jellyfin 媒体服务器",
          version: "1.0.0",
          author: "Char ONE",
          description: "支持账号密码与 API Token 双模式连接 Jellyfin 远程无损音乐库",
          tier: "base",
          category: "source.connector",
          builtin: true,
          enabled: isPluginEnabled("jellyfin"),
          locked: false,
          requiresRestart: false,
          permissions: ["remote.read", "remote.manage"],
        });
      }
    } else {
      const locked = LOCKED_PLUGIN_IDS.has(p.id);
      list.push({
        ...p,
        locked,
        enabled: locked ? true : isPluginEnabled(p.id),
      });
    }
  }

  // Ensure the 4 built-in online lyrics plugins are included
  for (const lyr of LYRICS_PLUGINS) {
    if (!list.some((p) => p.id === lyr.id)) {
      list.push({
        ...lyr,
        locked: false,
        enabled: isPluginEnabled(lyr.id),
      });
    }
  }

  return list;
}

/** Merges and filters remote providers. */
function mergeProviderList(raw: ProviderDescriptor[]): ProviderDescriptor[] {
  const list: ProviderDescriptor[] = [];
  let hasJellyfin = false;
  for (const prov of raw) {
    if (prov.id === "jellyfin" || prov.id === "jellyfin_token") {
      if (!hasJellyfin) {
        hasJellyfin = true;
        list.push({
          id: "jellyfin",
          name: "Jellyfin 媒体服务器",
          fields: [
            { key: "base_url", label: "服务器地址", kind: "url", placeholder: "http://192.168.1.2:8096", required: true, secret: false },
            { key: "user", label: "用户名", kind: "text", placeholder: "admin", required: false, secret: false },
            { key: "password", label: "密码", kind: "password", placeholder: "", required: false, secret: true },
            { key: "token", label: "API Key / Token", kind: "password", placeholder: "可选 API Token", required: false, secret: true },
          ],
        });
      }
    } else {
      list.push(prov);
    }
  }
  return list;
}

export function refreshPlugins(): void {
  try {
    const rawList = engine.listPlugins();
    const rawProviders = engine.remoteProviders();
    plugins.set(mergePluginList(rawList));
    remoteProviders.set(mergeProviderList(rawProviders));
    pluginsLoaded.set(true);
  } catch (error) {
    console.warn("neiro: 刷新插件列表失败:", error);
  }
}

export function rescanPlugins(): void {
  try {
    const rawList = engine.reloadPlugins();
    const rawProviders = engine.remoteProviders();
    plugins.set(mergePluginList(rawList));
    remoteProviders.set(mergeProviderList(rawProviders));
    pluginsLoaded.set(true);
  } catch (error) {
    console.warn("neiro: 重新扫描本地插件失败:", error);
  }
}

export function togglePlugin(id: string, enabled: boolean): void {
  if (LOCKED_PLUGIN_IDS.has(id)) {
    return;
  }
  try {
    // Persist in disabled set
    disabledPlugins.update((set) => {
      const next = new Set(set);
      if (enabled) {
        next.delete(id);
        if (id === "jellyfin") next.delete("jellyfin_token");
      } else {
        next.add(id);
        if (id === "jellyfin") next.add("jellyfin_token");
      }
      void Storage.setItem(DISABLED_PLUGINS_KEY, JSON.stringify([...next])).catch(() => {});
      return next;
    });

    // Notify backend engine if applicable
    try {
      engine.setPluginEnabled(id, enabled);
      if (id === "jellyfin") {
        try {
          engine.setPluginEnabled("jellyfin_token", enabled);
        } catch {}
      }
    } catch {}

    refreshPlugins();

    // If active shell was disabled, automatically fall back to default industrial rack
    if (!enabled && activeShellId() === id) {
      switchShell(DEFAULT_SHELL_ID);
    }
  } catch (error) {
    console.warn("neiro: 切换插件状态失败:", error);
  }
}

export function switchShell(shellId: string): void {
  // Disallow switching to disabled shell
  if (!isPluginEnabled(shellId)) {
    return;
  }
  shellError.set(null);
  activeShellId.set(shellId);
  void Storage.setItem(ACTIVE_SHELL_KEY, shellId).catch(() => {});
}

// Initialise active shell and disabled plugins from storage
void Promise.all([
  Storage.getItem(ACTIVE_SHELL_KEY).catch(() => null),
  Storage.getItem(DISABLED_PLUGINS_KEY).catch(() => null),
]).then(([savedShell, savedDisabled]) => {
  if (savedDisabled) {
    try {
      const arr = JSON.parse(savedDisabled) as string[];
      if (Array.isArray(arr)) {
        disabledPlugins.set(new Set(arr));
      }
    } catch {}
  }
  if (savedShell && savedShell.trim() !== "") {
    const trimmed = savedShell.trim();
    if (isPluginEnabled(trimmed)) {
      activeShellId.set(trimmed);
    } else {
      activeShellId.set(DEFAULT_SHELL_ID);
    }
  }
  refreshPlugins();
});

// Initial load
refreshPlugins();
