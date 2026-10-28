# Fork Engine Shipping Queue

The ordered backlog the daily automation works through, one item per run. This
document is the single source of truth for *what to do next*.

The [strategy](STRATEGY.md) is the *why*; the [roadmap](ROADMAP.md) is the
*sequence* of capabilities; this file is the *front line* — the concrete work,
in order, that moves the engine toward a shipped game.

**How the automation reads it:** it takes the first item whose `slug` does not
appear in an `fe-task:` commit subject and does exactly that. When the queue is
empty the automation makes no changes and exits.

**State is derived from history, never stored here.** An item is done when an
`fe-task:` commit subject names its slug. That is the only place the state lives,
so this file never needs a "done" marker that can drift from the tree.

**Goal of the queue:** a 2D game built in Fork Engine runs in a browser, then on
phones, then as a downloadable desktop build, and is then a game worth playing.
Web is first because it is the cheapest target to verify (no store review, no
device matrix) and it forces the sandboxed-filesystem and non-desktop-graphics
paths early ([ADR-0071](adr/0071-web-first-2d-shipping-path.md)).

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

Each phase's bar is deliberately a *runnable* artifact, not a merge. A phase that
ends with code that compiles but has never run on its target is not finished.

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

Both hold. The check runs the 2D demo, the 3D game, a 3D touch run and a
phone-sized 2D run, and asserts a live canvas, a live loop, a non-blank frame
and a pruned export that carries its mesh and texture. The "no GL error" half
was the last to be checked: a non-blank frame is blind to a *rejected* draw, and
the two defects this item fixed — a depth-texture format GLSL ES 3.00 rejects,
and a colour-attached depth pass — both left a non-blank frame behind. The
device now has a `drain_errors()` seam (the null backend reports none), the
player reports the count as `glerr=N` and logs each error, and the web game
check fails on a non-zero count ([ADR-0095](adr/0095-web-gl-error-verification.md)).
All four runs report `glerr=0`.

### `web-gameplay-logic`

A 3D scene rendered in a browser but nothing responded to input: gameplay logic
lived only in C++ modules, and a browser has no dynamic loader, so a web export
had no logic to consume the actions the input path was already delivering.
Compile a small, useful set of behaviours into the engine and register them
in-process, so every target — including wasm — has gameplay to run.

**Done when** a web build runs a scene whose bound behaviour reads the movement
actions and moves its node, verified by the browser input probe. See
[ADR-0078](adr/0078-engine-shipped-gameplay-behaviours.md).

### `playable-3d-web-game`

A 3D test game that is playable in a browser, not merely rendered: a fly camera,
a movable player, and something to look at, driven by the engine-shipped
behaviours with a keyboard and a touchscreen.

**Done when** the project exports for the web, runs in headless Chromium, and the
input probe observes the player node move in response to a synthetic key.

### `fixed-simulation-step`

The browser input probe measured wall-clock distance, so the same run reported
different values on a loaded runner and the check was flaky by construction. Let
the engine pin a fixed simulation step so a run is reproducible.

**Done when** a test drives the frame loop with a fixed step and observes the
same delta every frame, and the web input probe reports the same distance across
repeated runs. See [ADR-0079](adr/0079-fixed-simulation-step.md).

### `web-renderer3d-frame-allocations`

The 3D renderer rebuilt its per-frame working set — the extracted instances, the
mesh-liveness list, and the light lists — on the heap every frame, and swept
cached mesh buffers with a scan that was quadratic in the number of on-screen
meshes. Reuse the buffers across frames and make the sweep linear, so a 3D game's
frame time is not dominated by allocator noise.

**Done when** a steady-state frame reuses capacity rather than allocating and the
mesh-buffer sweep visits each cached mesh once.

### `web-window-buffer-size-cache`

Every `framebuffer_width()`/`framebuffer_height()` call crossed into JavaScript —
`emscripten_webgl_get_drawing_buffer_size` and `emscripten_get_canvas_element_size`
are not free — and the frame loop asks for the size several times a frame. The
buffer only changes when the page resizes the canvas, which is exactly when the
resize callback already runs, so cache it there.

**Done when** a steady-state frame reads the drawing-buffer size from the cache
rather than querying the browser, and the resize path keeps the cache honest.

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
steers" once instead of reading raw touch points. Shipped as
`fe::player::ActionBindings`: a `fork_input` document lists actions with their
keys (by UI Events code), mouse buttons, and normalised touch regions, and
`fork_export` packs it and records its key so the player applies it after the
built-ins. `"replace": true` lets a game override the engine's default layout
rather than stack on it ([ADR-0090](adr/0090-declarative-input-bindings.md)).

**Done when** a game declares regions and a test proves a touch in each region
fires the bound action and a touch outside fires none. Both hold: a unit test
drives touches in, between, and outside the declared regions, and the web game
check dispatches a real synthetic touch at a point only the game's own document
covers, with no keys pressed, so the node moving means the document was packed,
parsed and applied end to end in a browser.

### `mobile-viewport-meta`

A mobile browser needs a viewport meta tag and a full-bleed canvas or the game
renders at desktop scale with scrollbars. Shipped as a full-bleed, non-scrolling
page: the canvas is `position: fixed; inset: 0` over the whole window, the
viewport meta carries `width=device-width` and `viewport-fit=cover`, and the page
suppresses scroll, overscroll and browser touch gestures so a drag reaches the
game ([ADR-0091](adr/0091-mobile-viewport-and-full-bleed-canvas.md)). The shell
also measures its own layout into a hidden `#layout` element, because a headless
browser can read a number but cannot judge "looks right".

**Done when** the export shell carries the viewport meta and a test asserts the
canvas fills the window under a simulated phone size. Both hold: a unit test
asserts the meta's contents and the canvas rules, and the web game check runs the
2D slice at a phone's CSS size (844x390) and fails unless the canvas box equals
the window box.

### `touch-fullscreen-prompt`

Offer a fullscreen request on the first touch, so a phone game is not stuck in a
browser chrome frame. Shipped as a best-effort request on the first touch or
pointer press on the canvas: `{ once: true }`, and every refusal — a missing API,
a rejected promise, a synchronous throw — is swallowed, because the game is
already playable in the frame it has
([ADR-0092](adr/0092-fullscreen-from-first-touch.md)).

**Done when** the shell requests fullscreen from a gesture and degrades silently
where the browser refuses. Both hold: the request is bound to the first
`touchstart`/`pointerdown` on the canvas and its failure paths are all caught;
the unit test asserts the request and the gesture binding, since a headless
Chromium cannot grant fullscreen to a synthetic event.

### `mobile-safe-area`

Notches and home indicators cover a full-bleed canvas. Keep interactive regions
inside the safe area. Shipped as a normalised seam on `ActionMap`:
`set_safe_area_insets` takes four fractions of the viewport, and the insets
*shrink the space a region is measured in* rather than being subtracted from
each region, so one declared layout stays valid on a notched and an unnotched
phone alike. The shell reads CSS `env(safe-area-inset-*)`, converts the pixels
to fractions at the boundary that measured them, and passes them as the
`--safe-area` flag; the player forwards them
([ADR-0093](adr/0093-safe-area-insets-seam.md)).

**Done when** the shell reads the safe-area insets and a test proves a declared
region is offset by them. Both hold: unit tests drive a touch into a reserved
band (fires nothing) and into the usable area's own edge (fires), and a nonsense
report is asserted to fall back to the identity mapping; the web game check
prints the value the runtime was given. The insets are always zero in a desktop
headless browser, so the behaviour is proved by the unit tests and the plumbing
by the browser.

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

### `install-rules`

Every packaging step in this phase starts from an installed engine, and there
was none: `cmake --install` did nothing, so the only way to use the tools or
build against the public headers was from the source checkout. Add install rules
for the engine library, its public headers, the editor shell, and the three
shipping tools, with the layout decided in one place (GNUInstallDirs, Windows
libraries in `bin`, a relative RPATH on Linux), and cover the installed tree with
a smoke test that runs every installed tool from the prefix. See
[ADR-0084](adr/0084-install-rules.md).

**Done when** `cmake --install` produces a tree with `bin`, `lib`, `include`, and
`share`; every installed tool runs from the prefix; and the smoke test asserts
it in CI.

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

Landed as `packaging/package_release.cmake`: one script produces a game archive
and an SDK archive per platform, checksums them, and unpacks the game archive and
plays it, so the artifact is exercised the way a download is. It runs from the
ordinary CI job as well as from the release workflow, because the release path
only fires on a tag and a mistake in it would otherwise reach a downloader before
anyone saw it. The SDK archive was made genuinely consumable in the same change:
it carries the third-party headers and archives a link needs, plus a CMake package
config, so `find_package(ForkEngine)` works against an unpacked archive. See
[ADR-0102](adr/0102-downloadable-release-builds.md).

What the queue still owes: the per-OS wrappers a downloader expects — an AppImage,
a `.app` bundle, a signed installer — are separate items below; this produces the
archives they wrap.

### `release-version-stamping`

A downloadable build must say which version it is. Stamp the engine version into
the exported manifest and show it in the game. Shipped as an optional
`engine_version` field on the export manifest, stamped by `fork_export` from
`fe::version::kEngineVersionString` so it comes from the one source of truth
(the root `CMakeLists.txt`, through the generated `Version.h`) and cannot be
typed in wrong. The web page's `<title>` carries it as `Name 0.1.0`, and the
player logs one line when a manifest recorded one. The field is optional and
`format_version` is not bumped: an added optional field a reader may ignore does
not change the document's shape ([ADR-0094](adr/0094-export-version-stamping.md)).

**Done when** the export manifest carries the version from
`CMakeLists.txt` and a test proves it matches `fe::version::kEngineVersion`.
Both hold: a unit test round-trips the field and rejects a wrong type, and an
end-to-end export of `examples/demo_project` writes `"engine_version": "0.1.0"`
and a `<title>Demo 0.1.0</title>`. A manifest written before the field existed
still loads, reading no version rather than a guess.

---

## Phase 4 — the game is worth shipping

The bar: the slice is a game a stranger can play, not a demo.

### `text-label`

The engine cannot draw a character: text lives only in the editor's ImGui
panels and the web shell's HTML. Nothing in Phase 4 can be built without it —
a title, a score, a menu, and a settings screen are all strings. Add a
`TextLabel` component that emits one glyph quad per character through the
existing 2D extraction seam, and a generated bitmap-font atlas behind it
([ADR-0083](adr/0083-in-engine-text-rendering.md)).

**Done when** a test lays out a string, extracts it, and observes one instance
per glyph with the expected advance, and the breakout slice draws its score and
lives on screen in a headless browser run.

### `in-game-ui`

Text alone cannot make a menu. A `TextLabel` is a world-space object: its
glyphs are placed by the node transform and drawn through the world camera, so
a HUD built from labels drifts out of its corner when the window's aspect ratio
changes, and nothing can be pointed at, so no button and no menu is possible.
Add screen-space UI as scene components — a `UiCanvas` and a `UiElement` — laid
out by one pure solver that drawing and hit testing both consume, drawn as quads
through the existing 2D seam, and driven by one input system
([ADR-0086](adr/0086-in-game-ui-layout-and-extraction.md)).

**Done when** a test asserts that the rectangle the renderer draws a button in
equals the rectangle the input system hit tests, and the breakout HUD is a
screen-space canvas whose score and lives stay in the window's corners at any
aspect ratio.

### `collision-filtering-and-triggers`

Every collider collides with every other collider, so a game cannot say "these
two never touch" or "this shape is a zone, not a solid". A pickup the player
passes through, a projectile that ignores its owner, and team collision are all
the same missing idea. Add `layer`/`mask` filtering and a trigger flag to
`Collider2D` and `Collider3D`, applied in one place in each solver
([ADR-0087](adr/0087-collision-filtering-and-triggers.md)).

**Done when** a test proves two colliders with non-overlapping masks pass
through each other and report no contact, and a trigger reports a contact
without applying an impulse, in both 2D and 3D.

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

Volume and control settings, persisted, with a touch-friendly menu
([ADR-0098](adr/0098-persisted-settings-and-a-settings-screen.md)).

**Done when** a test changes a setting, reloads, and observes it retained.

### `scripting-native-modules`

The scripting seam plus C++ native modules as the primary language (M7), so game
logic can live outside the engine.

**Done when** a game's rules load from a module the engine did not compile in,
with a test covering load and reload.

---
## Backlog — not yet ordered

- Additional graphics backends (Vulkan, Metal, WebGPU) as targets demand.
- Skeletal animation/skinning (the one open M6 item), needed for animated 3D.
- More example games beyond the breakout slice, once the first one ships.

## Ongoing

- Keep the build reproducible and warning-clean.
- Keep the tests green; every fix gets a test that would have caught it.
- Run the suite under AddressSanitizer and UndefinedBehaviorSanitizer while the
  codebase is small enough to keep them cheaply green (`-DFE_ENABLE_ASAN=ON`);
  a dedicated CI job does this ([ADR-0035](adr/0035-ci-quality-gates.md)).
- Keep the benchmark a regression gate, not just a dev tool: CI records the
  numbers and fails on a large regression (see `benchmarks/` and
  [ADR-0033](adr/0033-performance-regression-gate.md)).
- Keep the architecture documentation and ADRs current.
- Review dependency licenses for compatibility with MIT.
