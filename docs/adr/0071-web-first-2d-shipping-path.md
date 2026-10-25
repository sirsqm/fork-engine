# ADR-0071: Shipping a 2D game to every platform — the web-first critical path

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0018](0018-multi-platform-shipping.md), [ADR-0030](0030-asset-pack-format.md),
  [ADR-0032](0032-near-term-platform-scope.md), [ADR-0041](0041-platform-contract-and-null-backend.md),
  [ADR-0042](0042-null-rhi-device.md), [ADR-0064](0064-export-runtime-and-player.md),
  [ADR-0065](0065-pack-reachability-pruning.md)

## Context

[ADR-0018](0018-multi-platform-shipping.md) set the direction: treat the platform
layer as the complete OS contract, package assets, and prove portability by
exporting to the cheapest target (web) before the expensive ones. It named web
as first and mobile as second. It did not say what "done" means for a 2D game,
and the work since has advanced M8 opportunistically rather than against a
defined shipping bar.

The engine can already build, run, and export a playable 2D game on desktop.
`examples/breakout` is a complete game built entirely against the public API,
`fork_pack` bakes assets into a self-describing pack, and `fork_export`
assembles a runnable build whose player mounts that pack
([ADR-0064](0064-export-runtime-and-player.md)). What does not exist is any path
from that build to a player's browser, phone, or a downloadable desktop
artifact.

An audit of the current tree against the web target found the architecture
closer than the milestone list implies, and identified the real blockers:

- **Threads are already optional.** `JobSystem` is the only `std::thread` user in
  the engine, and a default-constructed system is single-threaded with every job
  running inline. WebAssembly without SharedArrayBuffer therefore needs no new
  design, only that the web target does not ask for workers.
- **Assets already load through one interface.** `ResourceSource` is the seam,
  and `std::filesystem` appears in exactly one implementation file
  (`engine/src/core/FileSystem.cpp`). A pack is self-describing, so a browser
  needs a `fetch`-backed source, not a new resource system.
- **The RHI is already a boundary.** Nothing above `rhi/Device.h` includes a
  backend header.
- **The blockers are concrete and few.** Every shader is a hardcoded
  `#version 330 core` string, which a WebGL2 context cannot compile; `glad` is
  desktop-only and unnecessary on web where WebGL2 supplies the entry points;
  the platform layer's only window backend is GLFW; and `Application::run()` is a
  blocking loop, which a browser cannot host because the page owns the frame
  schedule.

## Decision

**Make "a 2D game built in Fork Engine runs in a browser" the shipping bar, and
drive all platform work from a single ordered queue that a daily automation
works through one item per run.**

### Web first, and specifically WebGL2

WebGL2 is the target, not WebGPU. It maps onto the existing OpenGL-shaped RHI
with no new resource model, and it is available on the browsers players actually
use. WebGPU is deferred until a target demands it.

### The blockers, in order

The queue in `docs/ROADMAP.md` (shipping queue) is the decision, made concrete. Its order
is chosen so that each item is verifiable by the next:

1. **A GLSL dialect seam.** Translate the existing desktop sources to GLSL ES
   3.00 (`#version 300 es` plus a fragment precision declaration) behind the
   RHI, with desktop translation the identity so existing rendering cannot
   regress. This is first because it is pure, fully unit-testable with no
   display, and every later item depends on it.
2. **Emscripten toolchain and a web window backend.** Configure the engine for
   wasm without disturbing the desktop build, with `glad` and GLFW excluded.
3. **A steppable frame loop.** `initialize()` / `step_frame()` / `shutdown()` as
   the primitives, with `run()` keeping its exact desktop behaviour and an
   Emscripten entry point driving `step_frame()` from `requestAnimationFrame`.
4. **A fetch-backed resource source**, so a shipped web build reads its pack over
   HTTP.
5. **A web export template**, extending `fork_export` to assemble the wasm
   runtime, the pack, the manifest, and a shell.
6. **CI and headless-browser verification.** Compiling to wasm is not proof the
   game runs. A headless Chromium check that the canvas exists, the module loads,
   and the frame is not blank is what keeps "compiles" from being mistaken for
   "works".

Mobile (GLES3 parity, touch-as-actions, Android, iOS) and desktop packaging
(prebuilt per-OS runtimes and download artifacts) follow in that order.

### Why the queue is a file in the repository

The work is performed by an automation that runs unattended, and by agents the
maintainer may run alongside it. A queue that lives in a prompt or in a chat is
lost when either changes, and it forces the agent to *infer* its task, which is
how unattended agents drift into inventing work. A file in the repository is
versioned, reviewable, and readable by every agent; an item is complete when its
slug appears in an `fe-task:` commit subject, so the queue state is derived from
history rather than stored in a second place that can disagree with it.

## Consequences

- The shipping bar is now a specific, testable statement rather than a milestone
  label. "Does a 2D game run in a browser?" has a yes/no answer, and each queue
  item has a "Done when" line that is its acceptance test.
- The dialect seam is a permanent part of the RHI. It is the price of a shader
  language that differs between desktop GL and GLES/WebGL, and it is cheaper than
  maintaining a second set of shader sources.
- `Application::run()` gaining a steppable form is a change to the engine's most
  central control flow, and the risk is desktop regression. The queue item
  therefore requires `run()` to remain behaviourally identical and be tested as
  such.
- Adding CI jobs for the web target and a headless browser increases build time
  and adds a Chromium dependency to the pipeline.
- iOS cannot be completed without a macOS host and Android needs a device matrix.
  These are recorded as constraints in the queue rather than stubbed, because a
  target that cannot be built cannot be claimed.
- The automation lands at most one item per run. Progress is therefore bounded by
  run frequency, not by the size of the backlog, and a large item is expected to
  span several runs via its `Remaining:` line.

## Alternatives considered

- **Keep advancing M8 opportunistically.** Rejected: it is what has happened so
  far, and it produced no browser build, no mobile path, and no defined bar for
  "shippable".
- **WebGPU instead of WebGL2.** Rejected for now: it is a different resource and
  pipeline model, so it would be a new backend rather than a dialect, and the
  existing RHI already matches the OpenGL shape.
- **SDL or sokol for the platform layer instead of a native web backend.**
  Rejected, consistent with [ADR-0018](0018-multi-platform-shipping.md): the
  contract is small and shaped to the engine's needs, and Emscripten's canvas and
  WebGL2 bindings are a thin implementation of it.
- **Build wasm inside each automation run.** Rejected: emsdk is a large download
  and setup would consume a meaningful fraction of a run's budget. The run writes
  the code and CI compiles it.
- **Track the queue outside the repository.** Rejected for the reasons above: it
  cannot be reviewed, and it makes the agent's task a matter of inference.
