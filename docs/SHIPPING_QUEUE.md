# Fork Engine — shipping queue

The ordered backlog the daily automation works through, one item per run.
This file is the single source of truth for *what to do next*: the automation
reads it, takes the first item whose `slug` does not appear in an `fe-task:`
commit subject, and does exactly that. When the queue is empty the automation
makes no changes and exits.

**Goal of this queue:** make a 2D game built in Fork Engine run in a browser,
then on phones, then as a downloadable desktop build. Web is first because it is
the cheapest target to verify (no store review, no device matrix) and it forces
the sandboxed-filesystem and non-desktop-graphics paths early
(docs/adr/0018-multi-platform-shipping.md).

**Rules for every item**

- The commit subject is `fe-task: <slug> - <short summary>`.
- "Done when" is the acceptance test. Satisfy it literally.
- Add or update tests. A change with no test is not finished.
- Build and test clean with `-DFE_WARNINGS_AS_ERRORS=ON` before committing.
- If an item is too large for one run, land a coherent tested first step and end
  the commit body with a `Remaining:` line.
- Do not reorder or skip items. Take the first unfinished one.

---

## Phase 1 — a 2D game renders in a browser

### `glsl-dialect-seam`

Today every shader in the engine is a hardcoded `#version 330 core` string. A
browser's WebGL2 context cannot compile that: GLSL ES 3.00 requires
`#version 300 es`, and fragment shaders must declare a float precision.

Add a dialect translation seam so the engine can emit its existing desktop
sources unchanged on desktop, and translated sources for a GLSL ES target. The
translation must be a pure function of the source string and the target dialect,
living behind the RHI so no renderer names a dialect.

- Introduce a `ShaderDialect` enum (desktop core / ES) on the RHI side.
- Add a translation function that, for the ES dialect, rewrites the version
  directive to `#version 300 es` and injects `precision highp float;` (and
  `precision highp int;`) into fragment shaders that do not already declare a
  precision.
- Desktop translation must be the identity: the bytes handed to the driver are
  unchanged, so existing rendering cannot regress.
- The translation must be idempotent-safe: never emit a second version
  directive or a duplicate precision declaration.

**Done when** a unit test proves: (a) a desktop source passes through byte for
byte; (b) an ES vertex source has the version rewritten and no precision added;
(c) an ES fragment source has the version rewritten *and* one precision
declaration added; (d) a fragment source that already declares a precision is
not given a second one; (e) translating an already-translated ES source adds
nothing. The full suite still passes.

### `shader-dialect-wiring`

Wire the seam from `glsl-dialect-seam` into the shader creation path so the
active dialect is chosen by the backend rather than by the shader author. The
OpenGL desktop backend must select the desktop dialect and behave exactly as it
does today.

- The dialect is selected once, where the device is created, not per shader.
- No renderer or shader string may name a dialect.
- The null device must continue to accept shader descriptions unchanged.

**Done when** the graphics tests pass under Xvfb with the desktop dialect, and a
test proves the dialect is applied at creation rather than left to the caller.

### `emscripten-toolchain`

Add an Emscripten build path so the engine and the player can be compiled to
WebAssembly, without disturbing the desktop build.

- A CMake toolchain file for Emscripten.
- GLFW and glad must not be required for the web target: WebGL2 provides the GL
  entry points, so the glad loader is desktop-only.
- The platform layer must compile for web: GLFW is the desktop windowing
  backend, so the web target needs its own window implementation (a later item)
  and must at least configure and compile the non-windowing modules.
- `FE_BUILD_*` options must let a web configure skip the editor, which has no
  business in a shipped game.

**Done when** a documented, reproducible Emscripten configure command exists and
CI can run it (the CI job itself is a later item; for now record the exact
command in the toolchain file's comments and in the ADR).

### `web-window-backend`

Implement the platform window backend for web: a canvas-backed surface with a
WebGL2 context, no GLFW.

- Add a `Web` value to `WindowBackend` and a `Window` implementation behind it,
  following the same shape as the null backend.
- It must report `has_context() == true` and supply the WebGL2 context.
- `get_proc_address` must resolve WebGL2 entry points as Emscripten exposes them.

**Done when** the web target builds with the web window backend and a headless
smoke test constructs the window under Node or a headless browser.

### `frame-loop-stepping`

`Application::run()` is a blocking `while` loop. A browser owns the frame
schedule: the page's `requestAnimationFrame` calls into the engine, and the
engine must not block the browser's main thread.

Refactor the frame loop so a frame can be advanced one step at a time by an
external driver, while `run()` keeps its current blocking behaviour for desktop.

- Expose `initialize()` / `step_frame()` / `shutdown()` as the primitive the loop
  is built from.
- `run()` must remain behaviourally identical on desktop: same order, same
  headless frame-count handling, same arena reset, same profiler bracketing.
- Add an Emscripten entry point that installs `requestAnimationFrame` as the
  driver and calls `step_frame()` once per callback.

**Done when** a test drives N frames via `step_frame()` and observes the same
per-frame invariants as `run()` (frame counter, arena reset, headless stop), and
the desktop suite is unchanged.

### `pack-fetch-source`

A browser has no filesystem. The engine already loads assets through
`ResourceSource`, and a pack is self-describing, so a shipped web build must
read its pack over HTTP instead of from disk.

- Implement a `ResourceSource` that fetches the pack (or pack entries) over
  HTTP and serves the same `read_binary` / `read_text` / `exists` /
  `resolve_asset` contract.
- It must be usable from the player with no change to the loaders.
- Handle the async nature of `fetch` without leaking async into the synchronous
  `ResourceSource` interface: preload the pack before the first load, which is
  what a shipped build does anyway.

**Done when** a test serves a pack from an in-process HTTP server and reads an
asset through the fetch source, matching the bytes the disk source returns.

### `web-export-template`

Extend `fork_export` to produce a web build: the player compiled to wasm, the
pack, a manifest, and an HTML/JS shell that loads them.

- The export must remain compiler-free for desktop (it copies the player); for
  web it assembles a prebuilt wasm runtime template beside the pack.
- The shell must size the canvas, load the wasm module, mount the pack, and hand
  control to the engine's frame driver.
- The manifest must record the target so a launcher knows what it has.

**Done when** `fork_export --target web` produces a directory containing the
wasm runtime, the pack, the manifest, and a shell, and the shell's expected file
names match what the export writes.

### `web-smoke-test-ci`

Add a CI job that configures and builds the web target with Emscripten, so the
web path cannot rot between runs.

- Install and cache emsdk in the job.
- Build the player for web.
- Fail the job if the web target stops compiling.

**Done when** the CI job exists, is green on a push, and the web build is
exercised on every change.

### `headless-browser-verification`

Compiling to wasm is not proof the game runs. Add verification that loads the
exported web build in a headless browser and asserts the game actually renders.

- Drive headless Chromium from a test script.
- Assert the canvas exists, the module loads without a JS error, the engine
  reports a live frame loop, and the rendered frame is not uniformly blank.
- Wire it into CI so a web regression fails the build.

**Done when** the check runs in CI against a real exported build and fails when
the renderer is deliberately broken.

---

## Phase 2 — the same game on a phone

### `webgl2-mobile-parity`

GLES3 on Android and iOS is close to WebGL2 but not identical. Confirm the ES
dialect and the RHI work under GLES3, and fix any divergence the ES path has
that only shows up on a real GLES driver.

**Done when** the ES dialect sources compile under a GLES3 context (a software
GLES implementation in CI is acceptable) and any divergence found is fixed with
a test.

### `touch-input-actions`

Input must be expressed as abstract actions so touch maps onto the same gameplay
code as keyboard and mouse. Touch is already reported through `InputState`; this
item makes it usable for gameplay rather than merely present.

**Done when** a test drives a touch sequence and observes the same gameplay
action a keyboard press produces.

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

Implement the iOS window backend: Metal or GLES surface, lifecycle, touch.

**Done when** the iOS target builds and produces a runnable app bundle. Note:
this needs a macOS host; record the constraint rather than stubbing it.

---

## Phase 3 — a downloadable desktop build

### `desktop-export-templates`

The export path copies the player built beside it. Shipping needs a *prebuilt*
runtime per desktop OS so a developer without the engine's toolchain can export.

**Done when** a documented, reproducible release build produces a per-OS runtime
artifact that `fork_export` accepts via `--player`, and the smoke test runs it.

### `desktop-packaging`

Produce the artifacts a player actually downloads: a Linux tarball/AppImage, a
macOS `.app` in a DMG, a Windows zip or MSI.

**Done when** each packaging step is scripted, reproducible in CI, and produces
an artifact that runs on a clean machine.

---

## Backlog — not yet ordered

- Console targets: no code lands without SDK access.
- Additional graphics backends (Vulkan, Metal, WebGPU) as targets demand.
- Skeletal animation/skinning (the one open M6 item), needed for animated 3D.
