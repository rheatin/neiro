// What is open over the pages: Now Playing and the 输出信息 drawer. The
// shell draws each as a `Sheet`, which owns focus, Escape and back; these
// signals only say whether each is open. The sidebar's fold is remembered
// between runs.

import { batch, signal, Storage } from "nsh";

const nowPlaying = signal(false);
const output = signal(false);
/** 服务器设置（连接/修改/断开远程库）的对话框。 */
const serverDialog = signal(false);
const pluginDialog = signal(false);

export type ModalId = "shortcuts" | "server" | "metadata" | "device" | "plugins" | null;
const activeModal = signal<ModalId>(null);
export const modalOpen = () => activeModal();
export function openModal(id: ModalId): void {
  activeModal.set(id);
  if (id === "server") serverDialog.set(true);
  if (id === "plugins") pluginDialog.set(true);
}
export function closeModal(): void {
  activeModal.set(null);
  serverDialog.set(false);
  pluginDialog.set(false);
}

export const pluginDialogOpen = () => pluginDialog() || activeModal() === "plugins";
export function setPluginDialogOpen(open: boolean): void {
  pluginDialog.set(open);
  if (open) activeModal.set("plugins");
  else if (activeModal() === "plugins") activeModal.set(null);
}
/** 待播列表抽屉（右侧）。 */
const queueSheet = signal(false);
/** Whether the 均衡器 page was opened from the output drawer: its back must
 * return there, not to whatever page was underneath (the user opened it from
 * the console, so that is where they expect to land). */
let eqFromOutput = false;

/** Whether Now Playing is open (tracked). */
export const nowPlayingOpen = () => nowPlaying();
/** Whether the 输出信息 drawer is open (tracked). */
export const outputOpen = () => output();
/** Whether the server settings dialog is open (tracked). */
export const serverDialogOpen = () => serverDialog();
/** Whether the 待播 drawer is open (tracked). */
export const queueOpen = () => queueSheet();

export function setServerDialogOpen(open: boolean): void {
  // 设置盖在别的浮层之上时，先收掉 Now Playing（一个浮层体系）。
  batch(() => {
    if (open) nowPlaying.set(false);
    serverDialog.set(open);
  });
}

export function setNowPlayingOpen(open: boolean): void {
  nowPlaying.set(open);
}

export function setOutputOpen(open: boolean): void {
  batch(() => {
    if (open) nowPlaying.set(false);
    output.set(open);
  });
}

/** Opens the equaliser, remembering whether the console sent us. */
export function setEqOpenedFromOutput(from: boolean): void {
  eqFromOutput = from;
}

export const eqCameFromOutput = (): boolean => eqFromOutput;

export function setQueueOpen(open: boolean): void {
  batch(() => {
    if (open) nowPlaying.set(false);
    queueSheet.set(open);
  });
}

/** Closes everything open over the pages (a retry of the engine). */
export function closeOverlays(): void {
  batch(() => {
    nowPlaying.set(false);
    output.set(false);
    serverDialog.set(false);
    pluginDialog.set(false);
    activeModal.set(null);
    queueSheet.set(false);
  });
}

// ---- The sidebar's fold -------------------------------------------------------

const SIDEBAR_KEY = "ui:sidebar-collapsed";
const collapsed = signal(false);

Storage.getItem(SIDEBAR_KEY)
  .then((saved) => collapsed.set(saved === "1"))
  .catch(() => {});

/** Whether the user folded the sidebar away entirely (tracked). */
export const sidebarCollapsed = () => collapsed();

/** Folds the sidebar, or brings it back, and remembers it. */
export function toggleSidebar(): void {
  const next = !collapsed();
  collapsed.set(next);
  void Storage.setItem(SIDEBAR_KEY, next ? "1" : "0").catch(() => {});
}
