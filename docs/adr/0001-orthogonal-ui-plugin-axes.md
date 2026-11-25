---
status: superseded by ADR-0003
---

# 0001 — Orthogonal `Shell × Surface × Acoustic FX` UI Plugin Axes

Neiro separates its standard UI extensibility into three orthogonal axes—`Shell` (spatial structure), `Surface` (visual material & typography), and `Acoustic FX` (audio-reactive GPU intensity & foley)—while retaining `Custom Shell` as an explicit escape hatch for fully bespoke `ui.shell` plugins.

## Context

Users and contributors want radically distinct layouts (crate-first browsing, stage-first immersion, mastering-desk telemetry) as well as distinct visual aesthetics (Swiss industrial, editorial gallery, hardware rack, liquid glass) and controllable GPU visualizers. Bundling every layout × aesthetic combination into standalone `ui.shell` packages creates an $N \times M$ maintenance matrix across desktop, iPad, iPhone, and foldable viewports.

## Considered Options

- **Option A — Bundled `ui.shell` per combination**: Maximum per-screen freedom, but multiplying layouts by aesthetics makes cross-device parity unsustainable.
- **Option B — Strict 3-axis composition only**: Eliminates duplication, but prevents radical single-purpose interfaces (e.g., an edge-to-edge cassette player).
- **Option C — Orthogonal `Shell × Surface × Acoustic FX` + `Custom Shell` escape hatch (Chosen)**: Standardizes 90% of experiences across all devices while keeping `ui.shell` open for total workspace takeovers.

## Consequences

- Superseded by ADR-0003 after interactive prototyping showed that decoupling `Surface` from `Shell` degenerates into superficial skinning over conventional card layouts.
