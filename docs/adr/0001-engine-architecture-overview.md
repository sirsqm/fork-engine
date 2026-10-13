# ADR-0001: Engine architecture overview

- **Status:** Accepted
- **Date:** 2026-01

## Context

Fork Engine is a new engine intended to grow into a serious, long-lived
open-source project supporting 2D and 3D game development with a visual editor.
It starts from an empty repository, so the first decision is the overall shape of
the system: how code is partitioned, how modules depend on each other, and what
the runtime and the editor each own.

The risks of getting this wrong are severe. A monolithic engine where
platform code, rendering, and gameplay are tangled cannot be ported, cannot be
tested in isolation, and becomes impossible to change after a few years. A
layering that is too abstract up front wastes effort on speculative
generality.

## Decision

Partition the engine into modules with explicit responsibilities and a strict,
acyclic, downward-only dependency graph:

```
core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
core  <-  serialization  <-  scene
everything  <-  runtime
```

Key properties:

1. `core` depends on nothing.
2. The graphics API is hidden behind `rhi`; no backend header is included above
   the `rhi` module.
3. `scene` is pure data and has no dependency on rendering, physics, or the
   runtime.
4. The platform library is hidden behind `platform`; windowing-library types do
   not appear in public headers except as forward-declared handles.
5. `Engine` owns subsystem lifetime; `Application` owns the frame loop. The two
   are separate so the editor can embed an `Engine` and drive frames itself.

New dependency edges must point downward; adding one is an architectural change
and requires an ADR.

## Consequences

**Easier:**

- A subsystem can be tested without the whole engine (the pure-logic tests run
  with no window or GPU).
- A second graphics backend is a new `rhi` implementation, not a rewrite.
- The editor can be built as a client of the engine without forking the runtime.
- New platforms are new `platform` backends.

**Harder:**

- Some conveniences require reaching "through" a boundary (for example, a
  renderer needs resource handles), so interfaces must be designed with the
  consumers in mind.
- A change deep in the stack can require touching the interfaces above it.

## Alternatives considered

- **A single monolithic library with no module boundaries.** Simplest to start,
  but it does not scale and cannot be tested or ported cleanly. Rejected.
- **Over-abstracting up front** (plugin ABI, dynamic module loading, a fully
  generic render graph before there is anything to render). Rejected as
  speculative complexity; the module boundaries leave room to add these later.
- **Merging `Engine` and `Application`.** Rejected: the editor needs to drive
  frames without the standalone loop, and separating them costs almost nothing.
