// What the app tells the user: short toasts at the bottom right. Each
// message has an id, so showing one again replaces it rather than stacking
// a copy; details of failures go to the log, never into a toast.

import { effect, signal, untracked } from "nsh";

export interface ToastAction {
  label: string;
  run: () => void;
}

export interface Toast {
  id: string;
  text: string;
  action?: ToastAction;
  /** When it disappears, epoch ms. */
  until: number;
}

/** How many show at once, and how long each stays up. */
const LIMIT = 3;
const TIMEOUT = 5000;

const toastsSig = signal<Toast[]>([]);
let nextId = 0;

// While anything shows, drop the toasts whose time is up.
effect(() => {
  const list = toastsSig();
  if (list.length === 0) return;
  const timer = setTimeout(() => {
    const now = Date.now();
    const kept = list.filter((t) => t.until > now);
    if (kept.length !== list.length) toastsSig.set(kept);
  }, 250);
  return () => clearTimeout(timer);
});

/** Shows `text` again replacing the same text; `action` is a button that
 * closes it and runs. */
export function notify(text: string, action?: ToastAction): void {
  const id = `t${nextId++}`;
  toastsSig.update((list) =>
    [...list.filter((t) => t.text !== text || t.action), { id, text, action, until: Date.now() + (action ? TIMEOUT * 2 : TIMEOUT) }].slice(-LIMIT),
  );
}

export const toasts = (): Toast[] => toastsSig();

export function dismissToast(id: string): void {
  toastsSig.update((list) => list.filter((t) => t.id !== id));
}

/** Runs a toast's action and closes it. */
export function chooseToast(id: string): void {
  const toast = untracked(toastsSig).find((t) => t.id === id);
  dismissToast(id);
  toast?.action?.run();
}
