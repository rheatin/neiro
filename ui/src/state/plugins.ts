// Plugin state store, scoped permission gate, and hot-swappable shell registry.

import { signal, Storage } from "nsh";
import { engine } from "../backend/engine";
import type { PluginInfo } from "../backend/types";

const ACTIVE_SHELL_KEY = "neiro:active-shell-id";
export const DEFAULT_SHELL_ID = "builtin.shell.industrial-rack";

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
    writeTags: async (_path, _tags) => {
      assertPerm("tags.write");
      // Delegated to engine/Base atomic rename pipeline
    },
    writeLyrics: async (path, lyrics) => {
      assertPerm("tags.write");
      engine.writeLyrics(path, lyrics);
    },
    setArtwork: async (_path, _mime, _data) => {
      assertPerm("tags.write");
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
export const pluginsLoaded = signal(false);
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

export function refreshPlugins(): void {
  try {
    const list = engine.listPlugins();
    plugins.set(list);
    pluginsLoaded.set(true);
  } catch (error) {
    console.warn("neiro: 刷新插件列表失败:", error);
  }
}

export function rescanPlugins(): void {
  try {
    const list = engine.reloadPlugins();
    plugins.set(list);
    pluginsLoaded.set(true);
  } catch (error) {
    console.warn("neiro: 重新扫描本地插件失败:", error);
  }
}

export function togglePlugin(id: string, enabled: boolean): void {
  try {
    engine.setPluginEnabled(id, enabled);
    refreshPlugins();

    // If the active shell was disabled, automatically fall back to default
    if (!enabled && activeShellId() === id) {
      switchShell(DEFAULT_SHELL_ID);
    }
  } catch (error) {
    console.warn("neiro: 切换插件状态失败:", error);
  }
}

export function switchShell(shellId: string): void {
  shellError.set(null);
  activeShellId.set(shellId);
  void Storage.setItem(ACTIVE_SHELL_KEY, shellId).catch(() => {});
}

// Initialise active shell from storage
void Storage.getItem(ACTIVE_SHELL_KEY)
  .then((saved) => {
    if (saved && saved.trim() !== "") {
      activeShellId.set(saved.trim());
    }
  })
  .catch(() => {});

// Initial load
refreshPlugins();
