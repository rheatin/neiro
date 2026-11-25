---
status: accepted
---

# 0003 — Self-Contained Paradigm `Shell`s Over Cosmetic `Surface` Skinning

Neiro abandons cosmetic `Surface` skinning (swapping colors, blurs, and border radii over a shared card-and-dock skeleton) in favor of self-contained **Paradigm `Shell`s**—each owning its own architectural grid, typographic scale, geometry, and signature audio controls—paired with `Acoustic FX` and a strict 2-step core-action usability invariant.

## Context

Prototype evaluation (`ui-sandbox.html` v1) demonstrated that separating `Surface` from `Shell` forces every aesthetic to inhabit the same conventional top-bar + two-panel + bottom-dock template, making wildly different styles feel identical and mediocre. Truly distinctive industrial-graphic designs (e.g., laboratory blueprint grids with caliper scrubbers, bleeding kinetic typography posters, or concentric polar instruments) are indivisible wholes where layout topology *is* the aesthetic.

## Considered Options

- **Option A — Keep `Shell × Surface` CSS/token decoupling**: Lower duplication, but structurally traps all designs inside conventional SaaS box layouts.
- **Option B — Self-contained Paradigm `Shell`s + `Acoustic FX` (Chosen)**: Each built-in `Shell` (`Specimen Lab`, `Kinetic Graphic`, `Radial Optical`, etc.) is free to eliminate standard bars/cards entirely while guaranteeing 2-step access to play/pause, track switching, Bit-Perfect status, and full-library search (`⌘K` / `/`).

## Consequences

- `Surface` is retired from `GLOSSARY.md` and ADR-0001 is superseded.
- Every Paradigm `Shell` must explicitly design its own Desktop, `iPhone Duo` inner-screen, and narrow phone topologies without falling back to generic card stacks.
- Core playback and search remain reachable in at most two actions regardless of how unconventional the visual composition is.
