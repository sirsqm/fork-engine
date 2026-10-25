# Fork Engine Roadmap

The front line of the plan: the strategy (why the work is sequenced this way),
the near-term priorities, and the shipping queue (the ordered backlog the daily
automation works through). The milestone detail it points into is in
[MILESTONES.md](MILESTONES.md), split out so neither file is too large to read in
one sitting.

The plan was once three files — `STRATEGY.md`, `ROADMAP.md` and
`SHIPPING_QUEUE.md` — which restated each other. It is now two that do not: this
file is *what to do next and why*, and `MILESTONES.md` is *what each milestone
contains*.

The engine is built incrementally. Each milestone is independently testable and
must build and pass its tests before the next one starts. This document is a
plan, not a promise; milestones may be reordered as the design becomes clearer,
and changes are recorded in the ADRs.

The guiding rule: a few genuinely working systems beat a long list of
half-finished features.

## The three pillars

Three capabilities shape how every subsystem is designed, not just what the
engine eventually features. They are the *direction*; the milestones below are
the *sequence*.

1. **Performance is a product feature.** The engine is fast by construction and
   by measurement. "Feels smooth" is a requirement, not a nice-to-have.
   - *What it implies:* a frame profiler; a render-extraction step so the
     authoring model (the scene graph) and the processing model (a flat instance
     array) meet at one seam; a job system for CPU work, with the GPU submission
     boundary kept single-threaded; allocator discipline (a per-frame arena);
     and a benchmark harness in CI that fails on regression.
   - *Risks:* premature optimization, mitigated by the measure-first rule and by
     the order of the work (nothing is parallelised or cached before P1 can show
     it matters); parallelism bugs, mitigated by keeping the device
     single-threaded, testing parallel paths against serial ones, and running the
     suite under a thread sanitizer once the job system lands.
   - *Sequenced as* **M9**; trade-offs in
     [ADR-0017](adr/0017-performance-strategy.md).
2. **Games ship anywhere.** A developer can choose a target platform and get a
   runnable build of their game without hand-editing the engine. This is about
   shipping *games*, not about the editor running on several OSes.
   - *What it implies:* a complete platform layer (window, input including touch
     and gamepad, filesystem, threads, time, dynamic libraries, application
     lifecycle), with everything OS-specific behind it; asset packaging (a
     self-describing pack the runtime mounts); export templates (a prebuilt
     runtime per target plus a build step that turns a project into a runnable
     game); and renderer backends added as targets demand them.
   - *Risks:* scope explosion, mitigated by proving portability with the
     cheapest target (web) before mobile and by only adding a backend when a
     target needs one; platform-specific bugs hidden in shared code, mitigated by
     the platform audit, the null backend, and CI across the three desktop
     platforms.
   - *Sequenced as* **M8**; trade-offs in
     [ADR-0018](adr/0018-multi-platform-shipping.md). The near-term target is
     desktop, and [ADR-0032](adr/0032-near-term-platform-scope.md) records why
     the wider claim is sequenced rather than asserted.
3. **Editing is collaborative.** Several people open the same project, edit it
   simultaneously, see each other's changes and presence live, and never run a
   manual merge.
   - *What it implies:* stable identity for everything editable; an operation
     model (edits as discrete, serializable operations rather than direct
     mutations); a replicated operation log every replica applies in the same
     order; an authoritative session that orders structural operations and
     resolves property edits by last-writer-wins; presence (cursors, selections,
     viewports); and asset synchronisation. It reuses what exists: the scene is
     reflection-driven, and the editor already routes every edit through a
     command stack, so collaboration is that command stack made serializable and
     shared.
   - *Risks:* it touches the editor's core, mitigated by doing the first steps
     locally and headlessly with no network (which also makes undo/redo more
     robust); convergence bugs, mitigated by fuzz-testing interleaved operations
     across replicas; latency, mitigated by optimistic local application.
   - *Sequenced as* **M10**; trade-offs in
     [ADR-0019](adr/0019-collaborative-editing.md).

The pillars are deliberately ordered by risk. Performance is the lowest-risk and
improves everything else; shipping is medium; collaborative editing is the
highest-risk because it reaches into the editor's core, so it goes last and its
first steps are pure logic with no network.

## Near-term priorities

The order the next work is actually taken in, chosen so that a small number of
things get *finished* rather than many get started. This is the roadmap's
front line; the milestones in [MILESTONES.md](MILESTONES.md) hold the detail.

1. **A vertical slice** — one small game built end to end in the engine (M2.5).
   Everything else is secondary to having something real that exercises the whole
   stack together.
2. **Honest scope** — the "ship anywhere" claim is scoped to desktop for now, in
   [ADR-0032](adr/0032-near-term-platform-scope.md), so the README does not
   promise more than the engine delivers.
3. **Scripting** — the language-agnostic seam plus C++ native modules as the
   primary language (M7). This is what turns the engine into something people can
   make games with.
4. **Guard the performance claims** — the benchmark as a CI regression gate
   (M9) and sanitizer builds (ASan/UBSan), while the codebase is still small
   enough to keep them cheaply green.
5. **Content pipeline depth** — glTF instead of only OBJ, and skeletal animation
   (M4, M6), once a game needs real 3D content.
6. **Editor polish, more platforms, collaborative editing** — after the above.

The intent: finish the small things. The engines that last are the ones whose
authors ruthlessly complete them.

## Shipping queue

The ordered backlog the daily automation works through, one item per run. This
is the single source of truth for *what to do next*: the automation takes the
first item whose `slug` does not appear in an `fe-task:` commit subject and does
exactly that. When the queue is empty the automation makes no changes and exits.

**Goal of this queue:** make a 2D game built in Fork Engine run in a browser,
then on phones, then as a downloadable desktop build. Web is first because it is
the cheapest target to verify (no store review, no device matrix) and it forces
the sandboxed-filesystem and non-desktop-graphics paths early
([ADR-0071](adr/0071-web-first-2d-shipping-path.md)).

**Rules for every item**

- The commit subject is `fe-task: <slug> - <short summary>`.
- "Done when" is the acceptance test. Satisfy it literally.
- Add or update tests. A change with no test is not finished.
- Build and test clean with `-DFE_WARNINGS_AS_ERRORS=ON` before committing.
- If an item is too large for one run, land a coherent tested first step and end
  the commit body with a `Remaining:` line.
- Do not reorder or skip items. Take the first unfinished one.

### Phase 1 — a 2D game renders in a browser

#### `glsl-dialect-seam`

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

#### `shader-dialect-wiring`

Wire the seam from `glsl-dialect-seam` into the shader creation path so the
active dialect is chosen by the backend rather than by the shader author. The
OpenGL desktop backend must select the desktop dialect and behave exactly as it
does today.

- The dialect is selected once, where the device is created, not per shader.
- No renderer or shader string may name a dialect.
- The null device must continue to accept shader descriptions unchanged.

**Done when** the graphics tests pass under Xvfb with the desktop dialect, and a
test proves the dialect is applied at creation rather than left to the caller.

#### `emscripten-toolchain`

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

#### `web-window-backend`

Implement the platform window backend for web: a canvas-backed surface with a
WebGL2 context, no GLFW.

- Add a `Web` value to `WindowBackend` and a `Window` implementation behind it,
  following the same shape as the null backend.
- It must report `has_context() == true` and supply the WebGL2 context.
- `get_proc_address` must resolve WebGL2 entry points as Emscripten exposes them.

**Done when** the web target builds with the web window backend and a headless
smoke test constructs the window under Node or a headless browser.

#### `frame-loop-stepping`

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

#### `pack-fetch-source`

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

#### `web-export-template`

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

#### `web-smoke-test-ci`

Add a CI job that configures and builds the web target with Emscripten, so the
web path cannot rot between runs.

- Install and cache emsdk in the job.
- Build the player for web.
- Fail the job if the web target stops compiling.

**Done when** the CI job exists, is green on a push, and the web build is
exercised on every change.

#### `headless-browser-verification`

Compiling to wasm is not proof the game runs. Add verification that loads the
exported web build in a headless browser and asserts the game actually renders.

- Drive headless Chromium from a test script.
- Assert the canvas exists, the module loads without a JS error, the engine
  reports a live frame loop, and the rendered frame is not uniformly blank.
- Wire it into CI so a web regression fails the build.

**Done when** the check runs in CI against a real exported build and fails when
the renderer is deliberately broken.

### Phase 2 — the same game on a phone

#### `webgl2-mobile-parity`

GLES3 on Android and iOS is close to WebGL2 but not identical. Confirm the ES
dialect and the RHI work under GLES3, and fix any divergence the ES path has
that only shows up on a real GLES driver.

**Done when** the ES dialect sources compile under a GLES3 context (a software
GLES implementation in CI is acceptable) and any divergence found is fixed with
a test.

#### `touch-input-actions`

Input must be expressed as abstract actions so touch maps onto the same gameplay
code as keyboard and mouse. Touch is already reported through `InputState`; this
item makes it usable for gameplay rather than merely present.

**Done when** a test drives a touch sequence and observes the same gameplay
action a keyboard press produces.

#### `android-window-backend`

Implement the Android window backend: native activity, EGL surface, lifecycle
(suspend/resume), and touch.

**Done when** the Android target builds and a documented command produces a
runnable APK.

#### `android-export-template`

Extend `fork_export` to produce an Android build.

**Done when** `fork_export --target android` produces a directory that installs
and launches on a device or emulator.

#### `ios-window-backend`

Implement the iOS window backend: Metal or GLES surface, lifecycle, touch.

**Done when** the iOS target builds and produces a runnable app bundle. Note:
this needs a macOS host; record the constraint rather than stubbing it.

### Phase 3 — a downloadable desktop build

#### `desktop-export-templates`

The export path copies the player built beside it. Shipping needs a *prebuilt*
runtime per desktop OS so a developer without the engine's toolchain can export.

**Done when** a documented, reproducible release build produces a per-OS runtime
artifact that `fork_export` accepts via `--player`, and the smoke test runs it.

#### `desktop-packaging`

Produce the artifacts a player actually downloads: a Linux tarball/AppImage, a
macOS `.app` in a DMG, a Windows zip or MSI.

**Done when** each packaging step is scripted, reproducible in CI, and produces
an artifact that runs on a clean machine.

### Backlog — not yet ordered

- Console targets: no code lands without SDK access.
- Additional graphics backends (Vulkan, Metal, WebGPU) as targets demand.
- Skeletal animation/skinning (the one open M6 item), needed for animated 3D.

## Milestones

What each milestone contains, and where it stands, is in
[MILESTONES.md](MILESTONES.md). The milestones are the *sequence*; the pillars
above are the *direction*, and the shipping queue above is the front line.

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
