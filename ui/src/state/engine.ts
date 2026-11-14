// The engine singleton's gates for the shell: its health (ready vs 未连
// 接), and the retry that rebuilds it.

import { engine } from "../backend/engine";
import { closeOverlays } from "./overlays";

/** The engine's health (tracked). */
export const health = () => engine.health();

/** Whether the pages may load and the controls act (tracked). */
export const ready = () => engine.health().status === "ready";

/** The engine's own error text while `failed`. */
export function healthError(): string {
  const h = engine.health();
  return h.error ?? "E_ENGINE: 引擎未连接";
}

/** The engine's health, aliased (visualiser 的 guard 从这里读）。 */
export const engineHealth = health;

/** Tries the engine again (重试): a fresh Base, the same stores. */
export function retryEngine(): void {
  closeOverlays();
  engine.retry();
}
