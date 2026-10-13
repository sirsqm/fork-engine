# ADR-0017: Performance strategy — measurement first, a data-oriented seam

- **Status** — Proposed
- **Supersedes** — none
- **Superseded by** — none

## Context

Performance is a product feature for this engine, not an afterthought. The goal
is a stable, low frame time on real scenes, and the ability to prove it and
protect it. That forces decisions about *how* performance work is done, before
any of it is done.

Three questions:

1. **How is performance known?** Without measurement, "fast" is an opinion and
   every regression is invisible until it is a bug report.
2. **How does a hierarchical, human-authored scene become fast to process?**
   Walking a node tree per instance per frame is pointer-chasing: cache-hostile
   and hard to parallelise. But the scene graph is the right *authoring* model.
3. **Where is parallelism allowed?** Threads can make CPU work cheaper, but the
   graphics device has a single-threaded contract.

The engine's rules apply: no premature optimization, no speculative frameworks,
and a system is only done when it is tested.

## Decision

**Measure before optimizing; introduce one data-oriented seam (render
extraction) between the scene graph and the renderers; parallelise CPU work
behind a job system while keeping the device single-threaded.**

### Measurement

- A `Profiler` in `core` with `ProfileScope` RAII scopes. Scopes record into a
  fixed ring buffer — no per-scope allocation — and nest by frame.
- The counters the engine already exposes (draw calls, instances, lights,
  shadow casters) are the first metrics; the profiler adds timings.
- A benchmark target runs the sandbox headless for N frames and reports frame
  time at the median and high percentiles. CI records the numbers and fails on a
  regression beyond a threshold.

### The extraction seam

- Both renderers gain an **extraction** step: one walk of the hierarchy that
  emits a flat array of instances (mesh/sprite, world matrix, material handle,
  visibility). Issuing draws becomes a separate step over that array.
- The array is the unit that sorting, batching, instancing, and parallel culling
  operate on. It is also the unit that a job system can chunk.
- This is [ADR-0002](0002-hybrid-scene-ecs-architecture.md) applied to the
  hot path: the authoring model (scene graph) and the processing model (flat
  array) are deliberately different, and the conversion happens in one place.

### Parallelism

- A `jobs` module: a fixed thread pool, `parallel_for`, and a small task graph
  with dependencies. `FrameContext` exposes it to systems.
- **The RHI device stays single-threaded.** Threads may extract, cull, and
  compute; only the owning thread submits. This preserves the existing RHI
  contract and avoids a locking layer inside the device.
- `std::thread`/`std::mutex` (not an external job library) for the first cut:
  the surface needed is small and the standard library is everywhere.

### Memory

- A per-frame arena for transient data (extraction arrays, command lists), reset
  each frame, to remove per-frame heap churn. Allocation counters make churn
  observable.

## Consequences

- Every optimization has a before/after number and a guard test. A change that
  cannot show a win does not land.
- The extraction seam is a prerequisite for instancing, a render graph, and
  parallel culling — each becomes a change to one step, not to the renderers'
  structure.
- The scene graph stays the authoring model. No ECS migration is required, and
  no subsystem is forced into a data-oriented shape it does not need.
- The job system adds a concurrency surface that must be tested against the
  serial path and, once it lands, run under a thread sanitizer.
- The frame arena changes how transient allocations are written; code that
  outlives a frame must not allocate from it. That is a documented rule, not an
  enforced one, and review is the guard.

## Alternatives considered

- **A full ECS for the runtime.** Rejected for now: it is a large migration that
  would reshape the authoring model and every component, for a win that the
  extraction seam captures at a fraction of the cost. The hybrid model already
  permits a data-oriented path where it pays.
- **Optimizing subsystems individually with no benchmark.** Rejected: it is how
  engines accumulate clever code that no longer helps. Measurement first.
- **A third-party job system (Taskflow, enkiTS).** Deferred: the first cut needs
  a pool, `parallel_for`, and dependencies, which `std::thread` provides without
  a new dependency. Revisit if the task graph grows.
- **Multithreaded rendering with per-thread command buffers.** Deferred: it is
  the right end state for a Vulkan-class backend, but it requires command
  buffers and resource lifetime tracking in the RHI. Keeping the device
  single-threaded is correct until a backend needs otherwise.
- **Optimizing before the extraction seam.** Rejected: without the seam, every
  optimization is entangled with the hierarchy walk and cannot be parallelised
  or benchmarked cleanly.
