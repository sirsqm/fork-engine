# Fork Engine Strategy

The [roadmap](ROADMAP.md) is a *sequence*: what gets built, in what order, and
where each milestone stands. The [shipping queue](SHIPPING_QUEUE.md) is the
*front line*: the ordered backlog the daily automation works through.

This document is the *reason* for the sequence. It names three capabilities that
shape how every subsystem is designed — not features to bolt on at the end, but
constraints the design is checked against. A subsystem that fights a pillar is
wrong even if it works.

The three pillars:

1. **Performance is a product feature.** The engine is fast by construction and
   by measurement. "Feels smooth" is a requirement with a number attached.
2. **Games ship anywhere.** A game built here reaches the platforms its players
   use. This is about shipping *games*, not about the editor running on several
   OSes.
3. **Edits are durable and shareable.** An edit is a discrete, serializable
   operation over a stably identified object, not a direct mutation, so undo,
   redo, and replay all fall out of one model.

Each pillar below states its goal, the principles that follow, what it forces on
the architecture, an incremental plan whose steps each build and pass tests, how
the work is verified, and the risks with their mitigations. The decisions with
real trade-offs are recorded as ADRs: [0017](adr/0017-performance-strategy.md),
[0018](adr/0018-multi-platform-shipping.md), and the operation-model records
[0045](adr/0045-component-identity.md) /
[0047](adr/0047-local-operation-model.md) /
[0048](adr/0048-replicated-operation-log.md).

**The pillars are ordered by risk.** Performance is the lowest-risk and improves
everything else; shipping is medium; the shareable-edit model was the
highest-risk because it reaches into the editor's core, which is why only its
local, headless first steps were taken and the network was left out (see
[Not planned](ROADMAP.md#not-planned)).

---

## Pillar 1 — Performance is a product feature

### Goal

The engine holds a stable, low frame time on real scenes, and the team can
*prove* it and *protect* it. A performance claim without a number attached is
not a claim.

### Principles

- **Measure first, and keep measuring.** No optimization lands without a
  benchmark that shows the win and a regression guard that would catch its loss.
  This is why the benchmark exists before the optimizations it guards.
- **The authoring model is not the runtime model.** The scene graph is how humans
  author. Performance-critical systems consume a *flat, cache-friendly* view of
  that data. This is the hybrid architecture
  ([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md)) applied to the hot
  path, made concrete by a render-extraction step.
- **Keep the GPU submission boundary single-threaded.** CPU work parallelises;
  the device is owned by one thread (the RHI contract). Parallelism is added
  behind a command list, never by letting threads touch the device.
- **Parallelism is opt-in and must be provably equivalent.** Every parallel path
  has a serial path beside it and a test that the two produce identical output.
  A worker count of zero runs everything inline, so the module can land without
  changing any existing run.

### What it implies

- A **frame profiler**: named scopes recording into a fixed frame ring, plus the
  counters the engine already exposes (draw calls, instances, lights, shadow
  casters). The data drives the in-editor profiler panel.
- A **render-extraction seam**: one pass that walks the hierarchy once and emits
  a flat instance array, separate from issuing draws. Everything downstream
  (sorting, instancing, parallel culling) needs this seam.
- A **job system**: a small thread pool with `parallel_for` and a dependency task
  graph, used by systems and by extraction/culling, never by the RHI.
- **Allocator discipline**: a per-frame arena for transient data (extraction
  arrays, command lists) to remove per-frame heap churn.
- A **benchmark harness** run in CI that records frame-time percentiles and fails
  on regression beyond a threshold.

### Plan

- **P1 — Measurement.** `Profiler`/`ProfileScope` in `core`, a benchmark target
  that runs synthetic scenes through the real frame loop and reports
  percentiles, and CI storing the numbers.
  *Verify:* a test that scopes record and nest correctly; the benchmark runs
  green in CI. **Landed** ([ADR-0022](adr/0022-profiler-and-frame-timing.md),
  [ADR-0024](adr/0024-profiler-instrumentation.md),
  [ADR-0033](adr/0033-performance-regression-gate.md)).
- **P2 — Render extraction.** Split `Renderer2D`/`Renderer3D` into "extract a
  flat instance list" and "issue draws".
  *Verify:* extraction produces exactly the draws the graphics tests already
  assert, plus a unit test on the extracted array (order, counts, cull flags).
  **Landed** ([ADR-0052](adr/0052-render-extraction-seam.md)).
- **P3 — Job system.** A `jobs` module: thread pool, `parallel_for`, task graph.
  `FrameContext` gains access to it.
  *Verify:* parallel tasks produce results identical to serial; a stress test
  under many tasks; the suite runs under ThreadSanitizer. **Landed**
  ([ADR-0068](adr/0068-job-system.md)).
- **P4 — Parallel extraction and culling.** Chunk the instance array; output must
  be byte-identical to the serial path, with measured thresholds so the fan-out
  stays off where it does not pay. **Landed**
  ([ADR-0069](adr/0069-parallel-extraction-and-culling.md)).
- **P5 — Memory.** A frame arena for transient allocations, with allocation
  counters to prove the churn is gone. **Landed**
  ([ADR-0051](adr/0051-frame-arena.md), [ADR-0054](adr/0054-frame-loop-arena.md),
  [ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md)).
- **P6 — GPU efficiency.** Instancing and material batching are landed for both
  renderers: the 2D sprite path
  ([ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md)) and the
  3D mesh path ([ADR-0101](adr/0101-instanced-3d-draws.md)), and the
  **render-graph seam**'s scheduler landed
  ([ADR-0100](adr/0100-render-graph-seam.md)): pass order is data, and each pass
  is profiled as `pass/<name>`. Pass culling and render-target ownership remain
  open, and are what turn the scheduler into an optimizer.

### Verification

The benchmark is the gate, not a dev tool: CI records the numbers and fails on a
large regression. The sanitizer jobs (ASan/UBSan, and TSan for the pure-logic
suite) are the second gate — a performance change that introduces a race is a
defect, not a win.

### Risks

- **Premature optimization.** Mitigated by the measure-first rule and by the
  order above: nothing is parallelised or cached until P1 can show it matters.
- **Parallelism bugs (data races).** Mitigated by keeping the device
  single-threaded, testing parallel paths against serial ones, and running the
  suite under ThreadSanitizer once the job system lands.
- **Benchmark noise.** Mitigated by coarse tolerances, a baseline recorded on the
  CI runner rather than a dev machine, and comparing against the gate job's own
  artifact (see `benchmarks/README.md`).

---

## Pillar 2 — Games ship anywhere

### Goal

A developer can choose a target platform and get a runnable build of their game
without hand-editing the engine. Desktop first, then web and mobile, then
consoles where the platform holders allow it.

This is not "the editor runs on every OS". It is "**games made here reach every
platform their players use**".

### Near-term scope, stated plainly

The target *today* is desktop: Linux and Windows through the OpenGL 3.3 core
backend. macOS is buildable but not yet supported (GL 3.3 is deprecated there),
and web, mobile, and consoles are planned, not promised. The README and the
status table state this plainly, and
[ADR-0032](adr/0032-near-term-platform-scope.md) records why the claim is
sequenced rather than asserted. A portability claim the engine cannot
demonstrate costs more than it buys.

### Principles

- **The platform layer is the complete OS contract.** Window/surface, input
  (including touch and gamepad), filesystem, threads, time, dynamic libraries,
  and application lifecycle all live behind it. Anything OS-specific that leaks
  outside is a defect.
- **A null backend proves the contract.** If the engine cannot run with no
  display and no GL context, the seam is not real. The headless backend is the
  executable proof, and it is also what CI and servers use.
- **Assets ship in a pack.** Shipping a folder of loose files is not viable on
  mobile or web. Assets are baked into a pack the runtime mounts.
- **Targets are added on demand, not speculatively.** A renderer backend lands
  when a target needs it, proven by the cheapest target first.

### What it implies

- A **complete platform layer** with everything OS-specific behind it.
- **Asset packaging**: a versioned pack format, a writer, and a source the
  loaders read from as transparently as from disk.
- **Export templates**: a prebuilt runtime per target plus a build step that
  turns a project into a runnable game, with no compiler in the runtime.
- **Renderer backends as targets demand them.** The RHI already abstracts this;
  WebGL2 covers web, GLES/Vulkan/Metal cover mobile, and consoles want their own.

### Plan

- **S1 — Platform interface audit.** Define the full contract and add a
  null/headless implementation.
  *Verify:* the engine builds and its tests pass with the null backend.
  **Landed** ([ADR-0041](adr/0041-platform-contract-and-null-backend.md)).
- **S2 — Asset packaging.** A pack format and a mountable source.
  *Verify:* the sandbox runs from a pack; a missing asset reports a clean error.
  **Landed** ([ADR-0030](adr/0030-asset-pack-format.md)).
- **S3 — Export path.** `fork_player` mounts a pack and runs the loop;
  `fork_export` assembles a runnable build. **Landed**
  ([ADR-0064](adr/0064-export-runtime-and-player.md)).
- **S4 — Web target.** Emscripten/WASM with a WebGL2 backend — the cheapest proof
  the abstraction holds, with no store gating.
  *Verify:* the demo scene runs in a headless browser; a CI job builds WASM.
  **Landed** ([ADR-0071](adr/0071-web-first-2d-shipping-path.md) and the Phase 1
  queue).
- **S5 — Mobile.** Android and iOS: lifecycle, touch, GLES/Vulkan/Metal backends,
  app packaging. **Open** — the queue's Phase 2.
- **S6 — Desktop packaging.** Per-OS runtimes, cross-compilation, and app bundles
  (`.app`, MSI, AppImage). **Partly landed** — install rules are done
  ([ADR-0084](adr/0084-install-rules.md)) and per-OS release archives with an
  installed SDK ship from a tag ([ADR-0102](adr/0102-downloadable-release-builds.md));
  the Linux single-file download is an AppImage
  ([ADR-0103](adr/0103-linux-appimage.md)). The `.app` bundle and the Windows
  installer are still the queue's Phase 3.
- **S7 — Backends as needed.** Vulkan/Metal/WebGPU, chosen by what S5 and desktop
  parity actually require.
- **S8 — Consoles.** Architecture-ready; implementation only when SDK access
  permits. No code lands without the SDKs.

### Verification

Every target is proven by running the *same* game: the breakout slice is the
portability test, and the web smoke test (headless Chromium) is the acceptance
check for the web target. A backend that compiles but has never run a frame is
not done.

### Risks

- **Scope explosion.** Mitigated by proving portability with the cheapest target
  (web) before mobile, and by only adding a backend when a target needs it.
- **Platform-specific bugs hidden in shared code.** Mitigated by the S1 audit,
  the null backend, and CI across the three desktop platforms.
- **A portability claim outrunning the code.** Mitigated by the near-term scope
  above and [ADR-0032](adr/0032-near-term-platform-scope.md).

---

## Pillar 3 — Edits are durable and shareable

### Goal

An edit is a discrete, serializable operation over a stably identified object,
not a direct mutation. Undo, redo, and replay all fall out of the same model,
which is what makes a local edit trustworthy.

### Principles

- **Everything editable has a stable identity.** Nodes have a `Uuid`; components
  have stable ids too, so a property edit addresses the same component on every
  replica and across a save/load.
- **An edit is data, not a call.** A create, delete, reparent, add/remove
  component, or set-property is an operation that can be serialized, compared,
  inverted, and replayed.
- **The command stack is the operation model.** The editor already routes every
  edit through a command stack ([ADR-0015](adr/0015-editor-undo-redo.md)) rather
  than mutating the scene directly. This pillar generalizes that stack rather
  than adding a parallel mechanism.

### What it implies

- Stable component identity, an operation model, and an ordered log every replica
  applies in the same order.
- Coalescing: a continuous gesture (a drag) becomes *one* undo step, not one per
  frame, which requires a stable coalescing key and an inverse that survives
  merging.

### What landed

- Stable component identity ([ADR-0045](adr/0045-component-identity.md)).
- The serializable operation model
  ([ADR-0047](adr/0047-local-operation-model.md)).
- The ordered log ([ADR-0048](adr/0048-replicated-operation-log.md)).
- Transform editing as the first non-component operation kind, which
  established the shape for the next one
  ([ADR-0089](adr/0089-editor-transform-editing.md)).

They are shipped and tested, and they make undo/redo more robust on their own.

### Not pursued

The transport, presence, and authoritative session that only existed to serve
networked collaboration. They need a hosted service, and that running cost is not
one the project will carry. The reasoning is recorded in
[ADR-0019](adr/0019-collaborative-editing.md) (Deprecated) and repeated in
[Not planned](ROADMAP.md#not-planned).

---

## How the pillars fit the roadmap

The pillars pull the milestones in a clear direction:

- **Performance** adds a measurement milestone and a data-oriented extraction
  seam to both renderers, then a job system — sequenced as **M9**. It is the
  lowest-risk pillar and it improves everything else, so it runs in parallel with
  feature work rather than after it.
- **Ship anywhere** makes the platform layer and asset packaging first-class,
  then export templates, then web and mobile — sequenced as **M8**, expanded from
  a single "cross-platform" line into concrete targets.
- **Edits are durable and shareable** lands after the editor is mature, as the
  local steps that also strengthen undo. Its network half is not planned.

### What comes first

The pillars are the *direction*, not the next step. The next step is the
**vertical slice** — one small game built end to end (roadmap
[M2.5](ROADMAP.md#m25--a-vertical-slice-one-small-game-end-to-end)) — and then
making that slice a game a stranger can play (the
[shipping queue](SHIPPING_QUEUE.md)'s Phase 4). Nothing in the pillars is worth
building before the engine has proven, in one finished artifact, that the parts
work together; and nothing makes the engine usable to anyone but its author until
a game in it is worth playing.

The near-term order is therefore: finish the slice, make it worth shipping, keep
scripting (M7) as the next breadth step, then resume the pillars. This is the
project's own guiding rule applied at the project level — **a few genuinely
working systems beat a long list of half-finished features.**

Nothing here is a promise of a date. It is the order in which the engine earns
each capability, with every step independently testable.
