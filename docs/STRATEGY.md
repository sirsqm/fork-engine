# Fork Engine Strategy

The roadmap is a *sequence* of milestones. This document is the *reason* for the
sequence: three capabilities that shape how every subsystem is designed, not
features to bolt on at the end.

The three pillars:

1. **Performance as a product feature.** The engine is fast by construction and
   by measurement. "Feels smooth" is a requirement, not a nice-to-have.
2. **Ship anywhere.** A game built in Fork Engine can be exported to every
   platform its players use — desktop today, web and mobile next, consoles when
   the SDKs allow.
3. **Collaborative editing.** Several people open the same project and edit it
   at the same time, seeing each other's changes and presence live, with no
   manual merge.

Each pillar below states the goal, what it forces on the architecture, an
incremental plan of milestones that each build and pass tests, how the work is
verified, and the risks. The decisions with real trade-offs are recorded as
ADRs: [0017](adr/0017-performance-strategy.md),
[0018](adr/0018-multi-platform-shipping.md),
[0019](adr/0019-collaborative-editing.md).

The pillars are deliberately ordered by risk. Performance is the lowest-risk and
improves everything else; shipping is medium; collaborative editing is the
highest-risk because it reaches into the editor's core, so it goes last and its
first steps are pure logic with no network.

---

## Pillar 1 — Performance as a product feature

### Goal

The engine holds a stable, low frame time on real scenes, and the team can
*prove* it and *protect* it. A performance claim without a number attached is
not a claim.

### Principles

- **Measure first, and keep measuring.** No optimization lands without a
  benchmark that shows the win and a regression guard that would catch its loss.
- **The authoring model is not the runtime model.** The scene graph is how
  humans author. Performance-critical systems consume a *flat, cache-friendly*
  view of that data. This is the hybrid architecture
  ([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md)) applied to the hot
  path, made concrete by a render-extraction step.
- **Keep the GPU submission boundary single-threaded.** CPU work parallelises;
  the device is still owned by one thread (the RHI contract). Parallelism is
  added behind a command list, not by letting threads touch the device.

### What it implies

- A **frame profiler**: named scopes that record into a ring buffer, plus
  counters the engine already exposes (draw calls, instances, lights, shadow
  casters). The data drives a future in-editor profiler panel.
- A **render extraction** step: one pass that walks the hierarchy once and emits
  a flat instance array, separate from issuing draws. Everything downstream
  (sorting, instancing, parallel culling) needs this seam.
- A **job system**: a small thread pool with a task graph, used by systems and
  by extraction/culling, never by the RHI.
- **Allocator discipline**: a per-frame arena for transient data (extraction
  arrays, command lists) to remove per-frame heap churn.
- A **benchmark harness** run in CI that records frame-time percentiles and
  fails on regression beyond a threshold.

### Plan

- **P1 — Measurement.** `ProfileScope`/`Profiler` in `core`; a benchmark target
  that runs the sandbox headless for N frames and reports percentiles; CI stores
  the numbers. *Verify:* a test that scopes are recorded and nest correctly; the
  benchmark runs green in CI.
- **P2 — Render extraction.** Split `Renderer2D`/`Renderer3D` into "extract a
  flat instance list" and "issue draws". *Verify:* extraction produces exactly
  the draws the current graphics tests already assert, plus a unit test on the
  extracted array (order, counts, culling flags).
- **P3 — Job system.** A `jobs` module: thread pool, `parallel_for`, and a task
  graph with dependencies. `FrameContext` gains access to it. *Verify:* parallel
  tasks produce identical results to serial; a stress test under many tasks.
- **P4 — Parallel extraction and culling.** Chunk the instance array and extract
  and cull in parallel. *Verify:* byte-identical output to the serial path.
- **P5 — Memory.** A frame arena for transient allocations; allocation counters
  to prove churn is gone. *Verify:* arena correctness test; counter assertions.
- **P6 — GPU efficiency.** Instancing, material batching, and a render-graph
  seam for passes and post-processing. *Verify:* draw-call counters drop on a
  scene with repeated meshes; images stay correct.

### Risks

- **Premature optimization.** Mitigated by the measure-first rule and by the
  order above: nothing is parallelised or cached until P1 can show it matters.
- **Parallelism bugs (data races).** Mitigated by keeping the device
  single-threaded, testing parallel paths against serial ones, and running the
  suite under a thread sanitizer once the job system lands.

---

## Pillar 2 — Ship anywhere

### Goal

A developer can choose a target platform and get a runnable build of their game,
without hand-editing the engine. Desktop first, then web and mobile, then
consoles where the platform holders allow it.

This is not "the editor runs on every OS". It is "**games made here reach every
platform their players use**".

**Near-term scope.** The target *today* is desktop: Linux and Windows through
the OpenGL 3.3 core backend. macOS is buildable but not yet supported (GL 3.3 is
deprecated there), and web, mobile, and consoles are planned, not promised. The
README and the status table state this plainly; the wider goal below is the
direction, and [ADR-0032](adr/0032-near-term-platform-scope.md) records why the
claim is sequenced rather than asserted. A portability claim the engine cannot
demonstrate costs more than it buys.

### What it implies

- A **complete platform layer**: window, input (including touch and gamepad),
  filesystem, threads, time, dynamic libraries, and application lifecycle
  (suspend/resume). Anything OS-specific stays behind this module.
- **Asset packaging.** Shipping a folder of loose files is not viable on mobile
  or web. Assets are baked into a pack the runtime loads from.
- **Export templates.** A prebuilt runtime per target plus a build step that
  produces a runnable game from a project.
- **Renderer backends as targets demand them.** OpenGL 3.3 core covers desktop;
  web wants WebGL2/WebGPU, mobile wants GLES/Vulkan/Metal, and consoles want
  their own APIs. The RHI already abstracts this; the backends are added on
  demand, not speculatively.

### Plan

- **S1 — Platform interface audit.** Define the full platform contract, add a
  headless/null implementation (for CI and dedicated servers), and document what
  each backend must provide. *Verify:* the engine builds and its tests pass with
  the null backend; the graphics tests still skip cleanly without a context.
- **S2 — Asset packaging.** A pack format and a loader scheme so the runtime
  reads from a pack as transparently as from disk. *Verify:* the sandbox runs
  from a pack; a test that a missing asset reports a clean error.
- **S3 — Export templates.** Prebuilt runtime per desktop platform and a
  `fork export` path that assembles a runnable build from a project. *Verify:*
  an exported Linux build runs the demo scene.
- **S4 — Web target.** Emscripten/WASM build with a WebGL2 backend. Cheapest
  proof that the abstraction holds, with no store gating. *Verify:* the demo
  scene runs in a browser; a CI job builds the WASM target.
- **S5 — Mobile.** Android and iOS: lifecycle, touch input, GLES/Vulkan/Metal
  backends, and app packaging. *Verify:* the demo scene runs on an emulator.
- **S6 — Backends as needed.** Vulkan/Metal/WebGPU, chosen by what S4/S5 and
  desktop parity actually require.
- **S7 — Consoles.** Architecture-ready; implementation only when SDK access
  permits. No code lands without the SDKs.

### Risks

- **Scope explosion.** Mitigated by proving portability with the cheapest target
  (web) before mobile, and by only adding a backend when a target needs it.
- **Platform-specific bugs hidden in shared code.** Mitigated by the S1 audit,
  the null backend, and CI across the three desktop platforms (already running).

---

## Pillar 3 — Collaborative editing

### Goal

Two or more people open the same project, edit it simultaneously, watch each
other's changes appear live, see each other's cursors and selections, and never
run a manual merge.

### What it implies

- **Stable identity for everything that can be edited.** Nodes already have a
  `Uuid`; components need stable ids too, so a property edit addresses the same
  component on every replica.
- **An operation model.** The editor's edits become discrete, serializable
  *operations* (create node, delete node, reparent, add/remove component, set
  property) rather than direct mutations. This is the same information the
  command stack already holds — generalised and made transmittable.
- **A replicated operation log.** Every replica applies the same ordered log and
  converges to the same scene. Undo becomes log-based.
- **An authoritative session.** A session server assigns sequence numbers,
  serialises structural operations, and resolves property edits by
  last-writer-wins per property.
- **Presence.** Cursors, selections, and per-user viewport cameras, broadcast
  through the session.
- **Asset synchronisation.** A content-addressed store so a collaborator gets
  the assets the scene references.

### Why this reuses what exists

The scene is already reflection-driven and serialization is already
reflection-driven ([ADR-0005](adr/0005-reflection-and-serialization.md)). A
property operation is exactly `(entity id, component id, property path, value)`,
which is what `SetPropertyCommand` already resolves through the registry. The
editor already routes every edit through a command stack
([ADR-0015](adr/0015-editor-undo-redo.md)) rather than mutating the scene
directly. Collaboration is the command stack, made serializable and shared.

### Plan

- **C1 — Component identity.** Give components stable ids. *Verify:* ids survive
  serialization round-trips and undo; existing tests stay green.
- **C2 — Operation model (local only).** Generalise `Command` into a serializable
  `Operation`; route local edits through it. *Verify:* apply/undo/serialize/
  replay an operation; the editor's existing undo tests pass through the new
  layer.
- **C3 — Replicated log (local only).** A `Session` holding a document and an
  ordered log, applied deterministically; undo is log-based. *Verify:* two
  in-process replicas fed the same interleaved log converge to identical scenes.
- **C4 — Transport and server.** A `net` module with a transport abstraction
  (in-process loopback for tests, WebSocket in production) and a session server
  that orders structural operations and applies last-writer-wins to property
  edits. *Verify:* two in-process clients editing concurrently converge; a fuzz
  test interleaves random operations.
- **C5 — Presence.** Cursors, selections, and viewport cameras per user. *Verify:*
  presence messages round-trip through the loopback transport.
- **C6 — Conflict policy refinement.** Node-level locking for structural edits
  (the simplest model that avoids structural conflicts) if last-writer-wins
  proves insufficient; CRDT merging for properties only if a concrete need
  appears. *Verify:* a test that reproduces the conflict the change is meant to
  fix.
- **C7 — Asset sync.** Content-addressed store, dedup, and delta transfer.
  *Verify:* a second replica requests and receives a missing asset.

### Risks

- **Highest-risk pillar: it touches the editor's core.** Mitigated by doing
  C1–C3 *locally and headlessly* first — no network — which also makes undo/redo
  more robust, and by adding the server only once the local log converges.
- **Convergence bugs are subtle.** Mitigated by fuzz-testing interleaved
  operations across replicas and asserting identical resulting scenes.
- **Latency and UX.** Mitigated by optimistic local application (edits apply
  immediately, then reconcile), so the editor never blocks on the network.

---

## How the pillars fit the roadmap

The three pillars pull the milestones in a clear direction:

- **Performance** adds a measurement milestone early and a data-oriented
  extraction seam to both renderers, then a job system — sequenced in the
  roadmap as **M9**.
- **Ship anywhere** makes the platform layer and asset packaging first-class,
  then export templates, then web/mobile — sequenced as **M8**, expanded from a
  single "cross-platform" line into concrete targets.
- **Collaborative editing** lands after the editor is mature, as **M10**, with
  its first steps (component identity, operations, log) also improving undo.

**What comes first.** The pillars are the *direction*, not the next step. The
next step is a **vertical slice** — one small game built end to end in the engine
(roadmap M2.5) — followed by the **scripting seam** (M7). Nothing in the pillars
is worth building before the engine has proven, in one finished artifact, that
the parts work together; and nothing makes the engine usable to anyone but its
author until scripting exists. So the near-term order is: finish a game, add
scripting, then resume the pillars. Collaborative editing in particular is a
north star and stays out of the next several milestones.

Nothing here is a promise of a date. It is the order in which the engine earns
each capability, with every step independently testable, in keeping with the
guiding rule: a few genuinely working systems beat a long list of half-finished
features.
