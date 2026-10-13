# Fork Engine

A modular, open-source 2D and 3D game engine written in C++20.

Fork Engine is an attempt to build a serious, long-lived engine from first
principles: a clean architecture, a high-performance runtime, and an
editor-centric workflow. It is not a clone of any existing engine. Where an
established design is the right answer, it is adopted deliberately and
documented; where there is a real benefit to doing something different, that
decision is recorded as an architecture decision record (ADR).

The project is young and honest about it. This repository currently contains the
**foundation milestone**: a buildable engine library, a working 2D renderer, a
scene/resource/serialization core, and a runnable example, all covered by tests.
Everything else is planned in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Status

| Area | State |
| --- | --- |
| Build system, repository layout, CI | Working |
| Platform layer (window, input, context) | Working (desktop, GLFW) |
| Logging, assertions, error handling | Working |
| RHI abstraction + OpenGL 3.3 core backend | Working |
| 2D renderer (batched sprites, camera, textures) | Working |
| 3D renderer (lit meshes, directional light, 3D camera, frustum culling) | Working (first pass) |
| Mesh resources (primitives, OBJ loader) | Working |
| Scene graph (nodes, transforms, components) | Working |
| Resource manager (load, cache, release) | Working |
| Scene serialization (JSON) | Working |
| Runtime frame loop + sandbox example | Working |
| Physics, editor, scripting | Not started (see roadmap) |

Unit and graphics tests pass locally; see [Building and testing](#building-and-testing).

## Philosophy

- **Composition over inheritance.** Gameplay objects are composed from
  components attached to nodes. There is one obvious place for a transform, and
  no deep class hierarchy to fight.
- **Hybrid architecture, chosen per subsystem.** The scene graph is the
  editor-facing authoring model. Systems that are performance-critical are free
  to consume that data in a data-oriented way without the hierarchy dictating
  how they execute. Not every subsystem is forced into the same model.
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
core  <-  serialization  <-  scene
core, scene, resources  <-  mesh  <-  renderer3d
core, scene, resources  <-  scene3d
everything  <-  runtime
```

- **core** — types, math, error handling, logging, assertions, UUIDs, filesystem.
- **platform** — window creation, input, and the graphics context (GLFW).
- **rhi** — a small graphics device abstraction; the OpenGL 3.3 core backend is
  one implementation of it. Nothing above this layer includes a backend header.
- **resources** — loading, caching, and lifetime of assets (images, textures,
  meshes).
- **scene** — the node/component hierarchy and transforms.
- **serialization** — JSON persistence of scenes and components.
- **mesh** — CPU-side geometry: `Vertex3D`, `Mesh`, primitive generators, and an
  OBJ loader.
- **scene3d** — 3D scene components (`Camera3D`, `MeshRenderer3D`,
  `DirectionalLight3D`) and their reflection.
- **renderer2d** — batched 2D rendering on top of the RHI.
- **renderer3d** — 3D mesh rendering with a single directional-light pass.
- **runtime** — `Engine` (subsystem lifetime) and `Application` (frame loop).

Public headers live under `engine/include/fe/`, so consumers write
`#include <fe/core/Log.h>`.

## Repository layout

```
engine/           the engine library (the only thing a game links against)
  include/fe/     public headers, one directory per module
  src/            module implementations
examples/sandbox/ a small runnable program that exercises every layer
tests/            doctest-based unit tests and a separate graphics test target
cmake/            dependency policy, build settings, warning interface
third_party/      vendored sources (glad, stb) with their own licenses
assets/           sample assets used by the example
tools/            small development scripts
docs/             architecture, roadmap, and ADRs
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

### Tests

- `fork_engine_tests` — pure-logic tests (math, filesystem, scene, resources,
  serialization, input). Run anywhere.
- `fork_graphics_tests` — create a hidden window and a GL context, render a
  scene, and read the framebuffer back to assert what was actually drawn. They
  skip automatically when no context is available, so `ctest` stays green on
  machines without a GPU.

On a headless Linux machine the graphics tests need a virtual display and a
software renderer, for example:

```bash
xvfb-run -a -s "-screen 0 640x480x24" ./build/bin/fork_graphics_tests
```

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
