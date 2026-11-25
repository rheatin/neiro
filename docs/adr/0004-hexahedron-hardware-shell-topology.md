---
status: accepted
---

# 0004 — Hexahedron Hardware Shell Topology (`BC-1` + `AMP-35`)

Neiro adopts a spatial **6-face physical hardware `Shell` topology** (`Hexahedron`, modeled after the Teenage Engineering `BC-1` Broadcast CD deck mounted onto the `K.O.-AMP 35` cube) as the flagship spatial paradigm, launching in a `3/4` hero perspective and mapping every core Neiro workflow onto a dedicated physical surface or foreground physical action.

## Context

Following `ADR-0003`, cosmetic skinning over conventional 2D SaaS bars failed to achieve the tactile physicality and instrument-grade presence required for Neiro. Studio product photography and interactive 3D prototyping demonstrated that:
1. A cold launch into a `3/4` studio perspective simultaneously surfaces playback (`Top`), library (`Front`), and output/spectrum (`Right`) without trapping the user in a menu.
2. Album inspection feels most authentic as a **foreground physical unfold (`Album Dossier`)**—sliding a kraft sleeve and printed booklet onto the desk—rather than flipping the hardware upside-down or to the rear.
3. The Right Face's `24×48` perforated speaker grille serves as a live **backlit LED matrix `Grille Spectrum`** (`BARS / MIRROR / PEAK / OFF`), uniting physical industrial form with real-time audio telemetry.

## Decision

- **Cold-Launch View**: Starts in `3/4` studio perspective (`azimuth +38°`, `elevation +29°`). Quick clicks operate controls in-place; double-clicking a face or using the tactile face dock smoothly rotates straight-on; `ESC` returns to `3/4`.
- **Surface Mapping**:
  - **Top (`BC-1 Deck`)**: Active playback — spinning CD + finger notch, `3×4` keypad + `ENTER`, red 7-segment strip + L/R bars, orange `JOG` lever, `CUE 1–3`, transport keys, `VARISPEED` & `LEVEL` knobs.
  - **Front (`CD Magazine`)**: Library & album grid — red 7-segment count, physical filter keys (`ALL / PCM / DSD / LOCAL / JELLYFIN / 歌单 / 收藏 / SEARCH`), and scrollable `2×3` (`3×3` wide) physical CD slots.
  - **Foreground Action (`Album Dossier`)**: Sliding kraft sleeve + loadable disc (`LOAD DISC ▶`) + printed tracklist booklet.
  - **Right (`AMP-35 Console`)**: Left-half **`24×48` Backlit Perforated `Grille Spectrum`**; right-half `96.0 / 24 / 04.2` telemetry, dual PPM, `DSP / RATE / EXCL / UNITY` interlocks, orange `DIRECT` Bit-Perfect latch, `LEVEL` knob, and 8-band EQ faders (+ `32-BAND RACK`).
  - **Left (`Lyrics & Queue Monitor`)**: Recessed monochrome/amber glass display (`LYRICS` + waveform / `QUEUE` / `在线搜词`) with 6 bottom micro-switches.
  - **Back (`Rear I/O Patchbay`)**: Local folders, Jellyfin/Navidrome/WebDAV network jacks, expansion slots (`ADR-0002` / `ADR-0003`), and system DIP switches.
  - **Bottom (`Chassis Base`)**: Resting baseplate with serial nameplate & `Host Core` safe-mode recovery.
- **Validation Gate**: Validate layout, per-control ergonomics, and dual-mode presentation (`3D 实体六面体` vs `正视实体机 + 功能标注`) via a studio-render-backed interactive browser demo (`neiro/docs/prototypes/neiro-studio-cube-demo.html`) prior to NSH implementation in `neiro/ui`.

## Consequences

- Every existing Neiro route (`neiro/ui/src/nav.ts`) maps cleanly to a physical face, foreground dossier, or rear patchbay jack without falling back to generic 2D card stacks.
- On `iPhone Duo` and narrow mobile viewports, the same face topology adapts seamlessly via swipeable/snappable face-on elevations while preserving `3/4` hero entry.
