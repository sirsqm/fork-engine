# Fork Engine

A modular, open-source 2D and 3D game engine written in C++20.

[![CI](https://github.com/sqmyou/fork-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/sqmyou/fork-engine/actions/workflows/ci.yml)

Fork Engine is an attempt to build a serious, long-lived engine from first
principles: a clean architecture, a high-performance runtime, and an
editor-centric workflow. It is not a clone of any existing engine. Where an
established design is the right answer, it is adopted deliberately and
documented; where there is a real benefit to doing something different, that
decision is recorded as an architecture decision record (ADR).

Three capabilities shape how the engine is designed, not just what it
eventually features:

- **Performance is a product feature.** The engine is fast by construction and
  by measurement: an authoring model that stays human-friendly and a processing
  model that stays cache-friendly, with a benchmark that guards both.
- **Games ship anywhere.** The goal is that a game built in Fork Engine reaches
  every platform its players use — desktop today, web and mobile next, consoles
  when the SDKs allow — not merely that the editor runs on several OSes. The
  supported target *today* is desktop (Linux and Windows); see
  [ADR-0032](docs/adr/0032-near-term-platform-scope.md) for why the wider claim
  is sequenced rather than asserted.
- **Editing is collaborative.** Several people open the same project, edit it
  live, see each other's changes and presence, and never run a manual merge.

The plan for these is in [`docs/STRATEGY.md`](docs/STRATEGY.md). The order the
next work is taken in — a vertical slice, scripting, then the pillars — is in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

The project is young and honest about it. The repository currently contains the
engine core through its first several milestones: a buildable library, working
2D and 3D renderers, a scene/resource/serialization/reflection core, 2D and 3D
physics, audio, an editor foundation, and a runnable example, all covered by
tests. Everything else is planned in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Status

| Area | State |
| --- | --- |
| Build system, repository layout, CI | Working |
| Platform layer (window, input, context) | Working (desktop, GLFW) |
| Logging, assertions, error handling | Working |
| RHI abstraction + OpenGL 3.3 core backend | Working |
| 2D renderer (batched sprites, camera, textures) | Working |
| 3D renderer (lit meshes, directional/point/spot lights, 3D camera, frustum culling, directional shadows) | Working (first pass) |
| Mesh resources (primitives, OBJ loader) | Working |
| Scene graph (nodes, transforms, components) | Working |
| Resource manager (load, cache, release) | Working |
| Scene serialization (JSON) | Working |
| Reflection and component serialization | Working |
| Systems and the frame schedule | Working |
| 2D physics (colliders, rigid bodies, solver, queries, contact events) | Working |
| 3D physics (box/sphere colliders, linear solver, broad phase, distance joints, queries, contact events) | Working (first cut, see [ADR-0031](docs/adr/0031-physics3d-and-backend-seam.md)) |
| Audio (device seam, WAV loader, pure mixer, mixing system) | Working (first pass) |
| Editor (docking shell, hierarchy, inspector, asset browser, 2D viewport, play-in-editor, undo/redo) | Working (foundation) |
| Project file (settings, asset root, startup scene) | Working |
| Runtime frame loop + sandbox example | Working |
| A complete example game (breakout, the M2.5 vertical slice) | Working (see [ADR-0036](docs/adr/0036-breakout-vertical-slice.md)) |
| Scripting (C++ native modules first, language-agnostic seam) | Not started (see roadmap) |
| Build/export | Not started (see roadmap) |
| Performance tooling (profiler, benchmark, CI regression gate) | Working (see [ADR-0033](docs/adr/0033-performance-regression-gate.md)) |
| Extra platforms, collaborative editing | Planned (see roadmap) |

Unit, editor, and graphics tests pass locally; see [Building and testing](#building-and-testing).

## Philosophy

- **Composition over inheritance.** Gameplay objects are composed from
  components attached to nodes. There is one obvious place for a transform, and
  no deep class hierarchy to fight.
- **Hybrid architecture, chosen per subsystem.** The scene graph is the
  editor-facing authoring model. Systems that are performance-critical are free
  to consume that data in a data-oriented way without the hierarchy dictating
  how they execute. Not every subsystem is forced into the same model.
- **Measure before optimizing.** Performance work starts with a benchmark and
  ends with a regression guard, never with clever code that cannot show a win.
- **Errors are values.** Recoverable failures travel through `fe::Error` and
  `fe::Result<T>`, not exceptions. Assertions are for programming errors only.
- **The editor is a client of the runtime, not a fork of it.** The editor talks
  to the engine through public interfaces and drives frames itself, which is what
  makes play-in-editor possible without a second, divergent code path.
- **Small and working beats large and broken.** A few genuinely finished systems
  are worth more than a long list of half-features.

The full design is in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Decisions
with real trade-offs are recorded in [`docs/adr/`](docs/adr/).

## Architecture at a glance

The engine is built from modules that depend only "downwards":

```
core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
core  <-  reflect  <-  scene, resources
core  <-  serialization  <-  scene, resources, reflect
core, scene, resources  <-  systems
core, scene, systems, reflect  <-  physics2d
core, scene, systems, reflect  <-  physics3d
core, scene, resources  <-  mesh  <-  renderer3d
core, scene, resources  <-  scene3d
core, scene, systems, resources  <-  audio
everything  <-  runtime
```

- **core** — types, math, error handling, logging, assertions, UUIDs, filesystem.
- **platform** — window creation, input, and the graphics context (GLFW).
- **rhi** — a small graphics device abstraction; the OpenGL 3.3 core backend is
  one implementation of it. Nothing above this layer includes a backend header.
- **resources** — loading, caching, and lifetime of assets (images, textures,
  meshes, audio clips).
- **scene** — the node/component hierarchy and transforms.
- **reflect** — component property descriptions and the type registry.
- **serialization** — JSON persistence of scenes and components, driven by
  reflection.
- **systems** — `System`, `SceneSchedule`, `FrameContext`, and the concrete
  gameplay systems.
- **physics2d** — 2D colliders, a rigid-body solver, queries, and contact events.
- **physics3d** — 3D box/sphere colliders, a linear rigid-body solver with a
  broad phase and distance joints, queries, and contact events. A hand-written
  first cut behind a component-and-query boundary, so a third-party solver can
  replace it later ([ADR-0031](docs/adr/0031-physics3d-and-backend-seam.md)).
- **mesh** — CPU-side geometry: `Vertex3D`, `Mesh`, primitive generators, and an
  OBJ loader.
- **scene3d** — 3D scene components (`Camera3D`, `MeshRenderer3D`,
  `DirectionalLight3D`, `PointLight3D`, `SpotLight3D`) and their reflection.
- **audio** — audio device seam, decoded clips, and a mixing system.
- **renderer2d** — batched 2D rendering on top of the RHI.
- **renderer3d** — 3D mesh rendering with a forward lit pass (directional, point,
  and spot lights) and a directional shadow map.
- **runtime** — `Engine` (subsystem lifetime) and `Application` (frame loop).

The editor lives under `editor/`, outside the engine library; it depends on the
engine, never the reverse.

Public headers live under `engine/include/fe/`, so consumers write
`#include <fe/core/Log.h>`.

## Repository layout

```
engine/           the engine library (the only thing a game links against)
  include/fe/     public headers, one directory per module
  src/            module implementations
editor/           the visual editor, built on the engine (core, UI, shell)
examples/sandbox/ a small runnable program that exercises every layer
examples/demo_project/ a minimal project to open in the editor shell
tests/            doctest-based unit, editor, and graphics test targets
benchmarks/       the performance benchmark and its CI regression gate
cmake/            dependency policy, build settings, warning interface
third_party/      vendored sources (glad, stb) with their own licenses
assets/           sample assets used by the example
tools/            small development scripts
docs/             architecture, strategy, roadmap, and ADRs
```

## Building and testing

Requirements:

- A C++20 compiler (GCC 12+, Clang 15+, or MSVC 19.3+)
- CMake 3.24+
- On Linux, the X11 or Wayland development headers that GLFW needs

Dependencies (GLFW, nlohmann/json, doctest) are fetched and pinned by
`cmake/FEDependencies.cmake`, so a clean checkout builds with one command.
Pass `-DFE_USE_SYSTEM_DEPS=ON` to prefer system-installed packages instead.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Run the tests:

```bash
cd build && ctest --output-on-failure
```

Run the example (from the repository root, so its relative asset paths resolve):

```bash
./build/bin/fork_sandbox
```

Headless runs, used by CI and useful on machines without a display:

```bash
./build/bin/fork_sandbox --headless 120        # render 120 frames, then exit
./build/bin/fork_sandbox --save-scene out.json # build the demo scene, save, exit
./build/bin/fork_sandbox --scene out.json      # load a saved scene
```

Interactive controls: `WASD`/arrows move the camera, `Q`/`E` zoom, `Esc` quits.

### Try the editor

The editor shell opens a project, loads its startup scene, and shows the scene
hierarchy, inspector, viewport, and asset browser. A minimal project ships in
`examples/demo_project`, so you can try it without authoring anything first:

```bash
./build/bin/fork_editor_shell --project examples/demo_project
```

On a machine without a display, open the same project headlessly and exit after
a few frames, which is enough to confirm the project and scene load:

```bash
./build/bin/fork_editor_shell --project examples/demo_project --headless
```

See `examples/demo_project/README.md` for what the project contains.

### Tests

- `fork_engine_tests` — pure-logic tests (math, filesystem, scene, components,
  resources, serialization, reflection, physics, mesh, systems). Run anywhere.
- `fork_editor_tests` — headless editor logic (hierarchy view model, asset
  browser view model, document, play-in-editor, command stack, property helpers,
  viewport maths). Link only `fork_editor_core`, never the UI toolkit.
- `fork_graphics_tests` — create a hidden window and a GL context, render a
  scene, and read the framebuffer back to assert what was actually drawn. They
  skip automatically when no context is available, so `ctest` stays green on
  machines without a GPU.

The editor is built with `-DFE_BUILD_EDITOR=ON` (on by default).

On a headless Linux machine the graphics tests need a virtual display and a
software renderer, for example:

```bash
xvfb-run -a -s "-screen 0 640x480x24" ./build/bin/fork_graphics_tests
```

### Benchmarks

`fork_bench` runs a synthetic 2D scene through the real frame loop and reports
frame-time percentiles, and `benchmarks/check_regression.py` compares a run
against the committed baseline so CI fails on a regression. It needs a GL
context, so run it under a virtual display on a headless machine:

```bash
xvfb-run -a -s "-screen 0 1280x720x24" ./build/bin/fork_bench --scene sprite_heavy
```

See [`benchmarks/README.md`](benchmarks/README.md) for the scenes, options, and
the gate's tolerances, and
[ADR-0033](docs/adr/0033-performance-regression-gate.md) for the design.

## Contributing

Contributions are welcome. Start with [`CONTRIBUTING.md`](CONTRIBUTING.md) for
the build workflow, coding conventions, and review expectations. In short: keep
changes small and reviewable, add tests for behaviour, update the docs when an
interface changes, and never claim something works without having run it.

By participating you agree to the [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

## License

Fork Engine is released under the [MIT License](LICENSE). Vendored third-party
code under `third_party/` keeps its own license; see the notices in each
subdirectory.
