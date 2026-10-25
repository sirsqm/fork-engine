# Fork Engine Shipping Queue

The ordered backlog the daily automation works through, one item per run. This
is the single source of truth for *what to do next*: the automation takes the
first item whose `slug` does not appear in an `fe-task:` commit subject and does
exactly that. When the queue is empty the automation makes no changes and exits.

**Goal of the queue:** a 2D game built in Fork Engine runs in a browser, then on
phones, then as a downloadable desktop build. Web is first because it is the
cheapest target to verify (no store review, no device matrix) and it forces the
sandboxed-filesystem and non-desktop-graphics paths early
([ADR-0071](adr/0071-web-first-2d-shipping-path.md)).

## How an item is worked

- The commit subject is `fe-task: <slug> - <short summary>`.
- "Done when" is the acceptance test. Satisfy it literally.
- Add or update tests. A change with no test is not finished.
- Build and test clean with `-DFE_WARNINGS_AS_ERRORS=ON` before committing.
- If an item is too large for one run, land a coherent tested first step and end
  the commit body with a `Remaining:` line naming what is left.
- Do not reorder or skip items. Take the first unfinished one.
- An item is finished when its slug appears in an `fe-task:` subject; that is
  the queue's state, derived from history, and it is never stored twice.

## Phases at a glance

| Phase | Ship bar |
| --- | --- |
| 1 — a 2D game renders in a browser | A game runs in a headless Chromium and is verified to draw |
| 2 — the same game on a phone | The same game accepts touch and runs on a mobile browser |
| 3 — a downloadable desktop build | A per-OS artifact a player can download and run |
| 4 — the game is worth shipping | The slice is a real game, not a demo |

Which items are done is not recorded here: a slug is done when an `fe-task:`
commit names it, and that is the only place the state lives.

---

## Phase 1 — a 2D game renders in a browser

The bar: `fork_export --target web` produces a directory, and a headless
browser loads it, runs the frame loop, and draws a non-blank frame.

### `glsl-dialect-seam`

Translate engine shader sources to a GLSL ES dialect behind the RHI, so WebGL2
can compile them, with desktop translation the identity.

**Done when** a unit test proves desktop passes through byte for byte and the ES
translation rewrites the version and adds a fragment precision exactly once.

### `shader-dialect-wiring`

Select the dialect where the device is created, not per shader.

**Done when** the graphics tests pass with the desktop dialect and a test proves
the dialect is applied at creation.

### `emscripten-toolchain`

An Emscripten CMake toolchain file that leaves the desktop build untouched.

**Done when** a documented, reproducible configure command exists and CI can run
it.

### `web-window-backend`

A canvas-backed window backend with a WebGL2 context and no GLFW.

**Done when** the web target builds and a headless smoke test constructs the
window.

### `frame-loop-stepping`

Expose `initialize()` / `step_frame()` / `shutdown()` so a browser's
`requestAnimationFrame` can drive the engine without blocking.

**Done when** a test drives N frames via `step_frame()` and observes the same
per-frame invariants as `run()`.

### `pack-fetch-source`

A `ResourceSource` that reads the pack over HTTP, because a browser has no
filesystem.

**Done when** a test serves a pack from an in-process HTTP server and reads an
asset through the fetch source, matching the disk source's bytes.

### `web-export-template`

`fork_export --target web` writes the wasm runtime, the pack, a manifest, and an
HTML/JS shell.

**Done when** the export produces that directory and the shell's expected file
names match what the export writes.

### `web-smoke-test-ci`

A CI job that configures and builds the web target so the web path cannot rot.

**Done when** the job exists, is green on a push, and runs on every change.

### `headless-browser-verification`

Load the exported build in headless Chromium and assert the game renders.

**Done when** the check runs in CI against a real export and fails when the
renderer is deliberately broken.

### `3d-in-the-browser`

Phase 1's bar is met by the 2D path, but the 3D renderer shares the whole
shipping path with it and had never been run in a browser. Three desktop-only
assumptions in it fail on a WebGL2 context, none with a build-time signal: the
shadow map's depth texture used a format/type pair GLSL ES 3.00 rejects, the
shadow pass drew into a framebuffer with a colour attachment enabled, and the
pruning tracer registered only the 2D builtins so a 3D scene traced to zero
references and shipped an empty pack. Make 3D render in a browser and cover it
in the web check.

**Done when** an exported 3D project renders a non-blank frame in headless
Chromium with no GL error, a pruned 3D export carries its mesh and texture, and
the web game check runs both renderers. See
[ADR-0076](adr/0076-3d-in-the-browser.md).

### `web-resize-and-dpr`

A browser canvas is resized by the page, not by the game, and a phone reports a
device pixel ratio above 1. Make the engine follow the canvas size and the DPR.

**Done when** a test changes the canvas size and observes the renderer's
viewport and the input coordinate space both follow.

### `web-audio-context`

A browser will not start an audio context until a user gesture. Make the web
audio device resume on the first gesture and stay silent rather than crash
before one.

**Done when** a headless test proves the device tolerates a context that starts
suspended and plays after a resume.

### `web-visibility-pause`

A backgrounded tab stops receiving animation frames, so the engine's clock
jumps on return. Clamp the frame delta when a page becomes visible again.

**Done when** a test feeds a large time step after a visibility change and
observes the simulation advance by a bounded amount.

### `web-build-in-ci-artifacts`

The web build is compiled in CI but thrown away. Publish the exported game as a
CI artifact so a run can hand a reviewer a link.

**Done when** the CI job uploads the export directory and the artifact downloads
and runs.

---

## Phase 2 — the same game on a phone

The bar: the breakout slice accepts touch and runs in a mobile browser, then
builds for Android.

### `webgl2-mobile-parity`

GLES3 on Android and iOS is close to WebGL2 but not identical. Confirm the ES
dialect and the RHI work under a GLES3 context and fix any divergence.

**Done when** the ES dialect sources compile under a GLES3 context (a software
GLES implementation is acceptable) and any divergence found is fixed with a
test.

### `touch-input-actions`

Express input as abstract actions so touch maps onto the same gameplay code as
keyboard and mouse. This item carried the web input plumbing it depends on: DOM
keyboard, pointer and touch events feed `InputState` from the web backend, and
browser `KeyboardEvent.code` maps onto the engine's `Key` enum so bindings are
layout-independent. Touch is therefore usable, not merely present.

**Done when** a test drives a touch sequence and observes the same gameplay
action a keyboard press produces.

### `touch-gesture-regions`

Bind screen regions to actions declaratively, so a game declares "the left third
steers" once instead of reading raw touch points.

**Done when** a game declares regions and a test proves a touch in each region
fires the bound action and a touch outside fires none.

### `mobile-viewport-meta`

A mobile browser needs a viewport meta tag and a full-bleed canvas or the game
renders at desktop scale with scrollbars.

**Done when** the export shell carries the viewport meta and a test asserts the
canvas fills the window under a simulated phone size.

### `touch-fullscreen-prompt`

Offer a fullscreen request on the first touch, so a phone game is not stuck in a
browser chrome frame.

**Done when** the shell requests fullscreen from a gesture and degrades silently
where the browser refuses.

### `mobile-safe-area`

Notches and home indicators cover a full-bleed canvas. Keep interactive regions
inside the safe area.

**Done when** the shell reads the safe-area insets and a test proves a declared
region is offset by them.

### `android-window-backend`

Implement the Android window backend: native activity, EGL surface, lifecycle
(suspend/resume), and touch.

**Done when** the Android target builds and a documented command produces a
runnable APK.

### `android-export-template`

Extend `fork_export` to produce an Android build.

**Done when** `fork_export --target android` produces a directory that installs
and launches on a device or emulator.

### `ios-window-backend`

Implement the iOS window backend: a Metal or GLES surface, lifecycle, and touch.

**Done when** the iOS target builds and produces a runnable app bundle. Note:
this needs a macOS host; record the constraint rather than stubbing it.

### `ios-export-template`

Extend `fork_export` to produce an iOS build.

**Done when** `fork_export --target ios` produces an Xcode project or archive
that a macOS host can sign and run.

---

## Phase 3 — a downloadable desktop build

The bar: a player downloads one file per OS, runs it, and plays the game.

### `desktop-export-templates`

The export path copies the player built beside it. Shipping needs a *prebuilt*
runtime per desktop OS so a developer without the engine's toolchain can export.

**Done when** a documented, reproducible release build produces a per-OS runtime
artifact that `fork_export` accepts via `--player`, and the smoke test runs it.

### `linux-appimage`

Produce a single-file Linux artifact that runs on a clean machine without the
engine's shared libraries.

**Done when** the AppImage builds reproducibly in CI and launches the game on a
clean container.

### `windows-zip`

Produce a Windows artifact a player can unzip and run.

**Done when** the zip builds in CI and the game launches from it on a clean
machine.

### `macos-app-bundle`

Produce a `.app` bundle, then a DMG, with the runtime's dependencies bundled.

**Done when** the DMG builds in CI and the game launches from it on a clean
machine.

### `desktop-packaging`

Script the packaging for all three desktop targets behind one command.

**Done when** each packaging step is scripted, reproducible in CI, and produces
an artifact that runs on a clean machine.

### `release-version-stamping`

A downloadable build must say which version it is. Stamp the engine version into
the exported manifest and show it in the game.

**Done when** the export manifest carries the version from
`CMakeLists.txt` and a test proves it matches `fe::version::kEngineVersion`.

---

## Phase 4 — the game is worth shipping

The bar: the slice is a game a stranger can play, not a demo.

### `slice-title-and-menus`

A title screen, a pause, and a game-over screen, so the slice has a shape.

**Done when** a test drives the state machine through title → play → game over →
title.

### `slice-scoring-and-progression`

Score, lives, and levels that get harder, so there is a reason to keep playing.

**Done when** a test proves a level advances on a clear and the difficulty
increases.

### `slice-persistence`

Save the best score locally, so progress survives a reload.

**Done when** a test writes and reads a score through the storage seam on both
desktop and web.

### `slice-settings`

Volume and control settings, persisted, with a touch-friendly menu.

**Done when** a test changes a setting, reloads, and observes it retained.

### `scripting-native-modules`

The scripting seam plus C++ native modules as the primary language (M7), so game
logic can live outside the engine.

**Done when** a game's rules load from a module the engine did not compile in,
with a test covering load and reload.

---

## Backlog — not yet ordered

- Console targets: no code lands without SDK access.
- Additional graphics backends (Vulkan, Metal, WebGPU) as targets demand.
- Skeletal animation/skinning (the one open M6 item), needed for animated 3D.
- More example games beyond the breakout slice, once the first one ships.
