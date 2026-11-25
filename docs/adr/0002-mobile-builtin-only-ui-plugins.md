---
status: accepted
---

# 0002 — Equal Built-In UI Hot-Swapping Across Desktop & Mobile, Desktop-Only External Code

All built-in `Shell` and `Acoustic FX` plugins hot-swap identically across Desktop (Windows/macOS) and Mobile (iPad, iPhone, iPhone Duo), whereas loading external executable UI/engine plugins from disk remains exclusive to Desktop.

## Context

`App.tsx` previously gated shell switching behind `layout() === "desktop"` and hardcoded `<MobileShell />` on mobile. While iOS sandboxing and App Store guidelines forbid loading arbitrary executable dynamic libraries or external scripts at runtime, compiled-in NSH UI modules and pure data presets have no such restriction.

## Consequences

- `App.tsx` must dispatch `Shell` and `Acoustic FX` state uniformly on both desktop and mobile layouts.
- Mobile (`iOS` / `iPadOS` / `iPhone Duo`) exposes the full built-in `Shell × Acoustic FX` selector without hiding customization in a desktop-only drawer.
- Future mobile third-party customization is scoped strictly to declarative JSON data presets, never executable code.
