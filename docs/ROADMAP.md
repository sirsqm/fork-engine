# Fork Engine Roadmap

The engine is built incrementally. Each milestone is independently testable and
must build and pass its tests before the next one starts. This document is a
plan, not a promise; milestones may be reordered as the design becomes clearer,
and changes are recorded in the ADRs.

The guiding rule: a few genuinely working systems beat a long list of
half-finished features.

Three capabilities shape the sequence and are planned in depth in
[`STRATEGY.md`](STRATEGY.md): **performance** (a product feature, measured and
protected), **shipping to every platform players use** (not just running the
editor on several OSes), and **collaborative editing** (several people in one
project, live, no manual merge). Their milestones are **M9**, **M8**, and
**M10** below; the trade-offs are in ADRs
[0017](adr/0017-performance-strategy.md),
[0018](adr/0018-multi-platform-shipping.md), and
[0019](adr/0019-collaborative-editing.md).

## M1 — Foundation (done)

The smallest useful engine: something that builds, runs, renders, and is tested.

- [x] Build system, repository structure, CI
- [x] Application entry point and platform abstraction (window, input, context)
- [x] Logging, assertions, and error handling
- [x] Memory and resource ownership conventions
- [x] RHI abstraction with an OpenGL 3.3 core backend
- [x] Minimal 2D renderer: batched, textured sprites with a camera
- [x] Scene representation with JSON serialization
- [x] A runnable example (the sandbox)
- [x] Unit tests and graphics tests

## M2 — Editor foundation

The editor becomes real before the engine grows much further, so the runtime is
built editor-friendly from the start rather than adapted later.

- [x] A windowing/UI layer for tools (docking, panels) — ADR-0007, `fork_editor`
- [x] Scene hierarchy panel — `HierarchyViewModel` + `HierarchyPanel`
- [x] Property inspector, driven by the reflection system — `PropertyEditor` + `InspectorPanel`
- [x] 2D viewport rendering into an editor panel — `Viewport` + `ViewportPanel` (off-screen render target)
- [x] Asset browser — `AssetBrowserViewModel` (headless) + `AssetBrowserPanel`, listing the asset database's GUID-tagged assets with search and a copyable reference
- [x] Undo/redo — `Command` + `CommandStack` + concrete commands in `fork_editor_core`; coalescing drags into one step ([ADR-0015](adr/0015-editor-undo-redo.md))
- [x] Play-in-editor (embed an `Engine`, drive frames, run the scene's `SceneSchedule`) — `PlaySession`
- [x] Project asset root — `EditorContext::set_project_root` + `--project`, indexing the asset database so the browser and GUID references resolve
- [x] Project settings — `ProjectSettings` (headless) + `fork.project.json`, opened with `--project`/`--project-file`; name, asset root, and startup scene, with portable relative paths ([ADR-0025](adr/0025-project-file.md))
- [ ] Asset import settings per asset type (M3 groundwork; the sidecar's `scheme` is the seed — [ADR-0020](adr/0020-resource-identity-guid-sidecar.md))
- [ ] Build/export path — moved to M8, where the packaging, pack format, and per-target export templates live ([ADR-0018](adr/0018-multi-platform-shipping.md))

### M2 prerequisite — reflection (done)

The inspector is driven by reflection, so the reflection registry landed before
the editor panels. See [ADR-0006](adr/0006-component-reflection.md).

- [x] A reflection registry: types, properties, metadata, editor hints
- [x] Serialization driven by reflection instead of per-component code

### M2 prerequisite — systems and the frame schedule (done)

The hybrid execution model ([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md))
needed a concrete mechanism before gameplay could grow. See
[ADR-0008](adr/0008-systems-and-frame-schedule.md).

- [x] `System`, `SceneSchedule`, and `FrameContext` (ordered, single-threaded, no job graph)
- [x] The runtime drives the schedule once per frame, after `on_update` and before rendering
- [x] First concrete system: sprite-sheet animation (`SpriteAnimator` + `SpriteAnimationSystem`), reflected so animators serialize and are inspector-editable

## M3 — Reflection and a real asset pipeline

- [ ] Property inspector integration with editor hints (Range, Color, Resource)
- [x] GUID-based resource references that survive file moves — a per-asset GUID
      sidecar and an asset database; see
      [ADR-0020](adr/0020-resource-identity-guid-sidecar.md)
- [ ] Import settings per asset type, and an import cache
- [ ] Hot reload of resources

## M4 — 3D rendering

- [x] Meshes, materials, cameras, and a 3D renderer — the first lit pass; see
      [ADR-0010](adr/0010-3d-rendering-foundation.md)
- [x] A directional light
- [x] Frustum culling (world-AABB, conservative) — see
      [ADR-0011](adr/0011-frustum-culling.md)
- [x] Directional, point, and spot lights — see
      [ADR-0012](adr/0012-forward-lighting-model.md)
- [x] Shadow mapping (single-cascade directional) — see
      [ADR-0014](adr/0014-directional-shadow-mapping.md)
- [ ] A spatial hierarchy (BVH/octree) over static geometry
- [ ] Environment rendering and a post-processing chain
- [ ] A glTF importer

## M5 — Physics

- [x] 2D physics: box and circle colliders, dynamic and static bodies, a
      sequential-impulse solver (gravity, restitution, friction), as a system
      (`Collider2D`, `RigidBody2D`, `Physics2DSystem`) — see
      [ADR-0009](adr/0009-physics2d-system.md)
- [x] Physics queries (raycast, overlap) and contact events — see
      [ADR-0013](adr/0013-physics2d-queries-events-angular.md)
- [x] 2D angular dynamics: angular velocity, torque, and shape-derived inertia,
      integrated by the solver — see
      [ADR-0013](adr/0013-physics2d-queries-events-angular.md)
- [x] 2D joints: distance and revolute constraints solved by the same
      sequential-impulse loop as contacts, with node-id body references that
      serialize — see [ADR-0021](adr/0021-physics2d-joints.md)
- [x] A uniform-grid (spatial hash) broad phase: candidate pairs are found once
      per step instead of once per iteration, so a step is O(candidates *
      iterations) rather than O(bodies² * iterations) — see
      [ADR-0023](adr/0023-physics2d-broadphase.md)
- [ ] 2D physics refinements: collider offsets, oriented-box colliders,
      continuous collision detection, sleeping
- [ ] 3D physics
- [ ] Integration with the scene and the editor
- [ ] Debug drawing

## M6 — Animation and audio

- [x] 2D sprite-sheet animation (frame playback; the first system) — see M2 prerequisite
- [x] Audio playback and mixing: `AudioDevice` with a real (miniaudio) and a null
      backend, `AudioClip` + `WavLoader`, `AudioSource`, and `AudioSystem` with a
      pure mixer — see [ADR-0016](adr/0016-audio-module.md)
- [ ] Audio refinements: 3D positional attenuation, streaming / large-file
      playback, compressed codecs, and audio buses / mixing groups
- [ ] Tilemaps
- [ ] Skeletal animation and skinning
- [ ] Animation state machines

## M7 — Scripting

- [ ] Choose and document the language and embedding strategy (ADR first)
- [ ] Reflection-backed bindings
- [ ] Editor integration (attach, inspect, hot reload)
- [ ] Debugging support

## M8 — Ship anywhere (cross-platform targets)

The goal is that a game built in Fork Engine reaches every platform its players
use, not merely that the editor runs on several OSes. Plan and trade-offs:
[`STRATEGY.md`](STRATEGY.md), [ADR-0018](adr/0018-multi-platform-shipping.md).

- [ ] Platform interface audit: define the full OS contract (window/surface,
      input incl. touch and gamepad, filesystem, threads, time, dynamic
      libraries, lifecycle) and add a **null/headless** backend for CI and
      servers
- [ ] Asset packaging: a pack format and a loader scheme so the runtime reads
      from a pack as transparently as from disk
- [ ] Export templates: a prebuilt runtime per desktop target and a `fork
      export` path that assembles a runnable build from a project
- [ ] Desktop parity: macOS and Windows backends brought to parity with Linux
- [ ] Web target: Emscripten/WASM build with a WebGL2 backend (the cheapest
      proof that the abstraction holds)
- [ ] Mobile: Android and iOS lifecycle, touch input, GLES/Vulkan/Metal
      backends, and app packaging
- [ ] Additional graphics backends as targets require them (Vulkan/Metal/WebGPU)
- [ ] Consoles: architecture-ready; implementation only when SDK access permits

## M9 — Performance (a product feature)

The engine holds a stable, low frame time on real scenes, and the team can
prove it and protect it. Plan and trade-offs: [`STRATEGY.md`](STRATEGY.md),
[ADR-0017](adr/0017-performance-strategy.md).

- [x] Profiler: `Profiler` + `ProfileScope` in `core` (ADR-0022), recording named
      scopes and counters into a fixed frame ring
- [x] Profiler instrumentation and runtime integration (ADR-0024):
      `FE_PROFILE_SCOPE`/`FE_PROFILE_FUNCTION` macros with a free disabled build;
      `Application` brackets `frame`/`frame/update`/`frame/render` and
      `SceneSchedule` times each system as `system/<name>`; a by-value
      `FrameReport` (per-name totals, counters, summary/to_string) is the stats
      surface
- [ ] Benchmark: a headless target reporting frame-time percentiles; CI records
      the numbers and fails on regression
- [ ] Render extraction: split both renderers into "extract a flat instance
      list" and "issue draws" (the data-oriented seam)
- [ ] Job system: a `jobs` module (thread pool, `parallel_for`, task graph);
      `FrameContext` exposes it to systems
- [ ] Parallel extraction and culling: chunk the instance array; output must be
      identical to the serial path
- [ ] Memory: a per-frame arena for transient data, with allocation counters
- [ ] GPU efficiency: instancing, material batching, and a render-graph seam for
      passes and post-processing
- [ ] An in-editor profiler panel fed by the profiler data

## M10 — Collaborative editing

Several people edit one project live, with presence and no manual merge. It
lands after the editor is mature because it reaches into the editor's core. Plan
and trade-offs: [`STRATEGY.md`](STRATEGY.md),
[ADR-0019](adr/0019-collaborative-editing.md).

- [ ] Component identity: stable ids that survive serialization, reimport, and
      undo
- [ ] Operation model (local only): generalise `Command` into a serializable
      `Operation`; route local edits through it
- [ ] Replicated log (local only): a `Session` holding a document and an ordered
      log; two in-process replicas converge; undo becomes log-based
- [ ] Transport and server: a `net` module with an in-process loopback transport
      (tests) and WebSocket (production), plus an authoritative session server
      (ordered structural operations, last-writer-wins per property)
- [ ] Presence: cursors, selections, and per-user viewport cameras
- [ ] Conflict policy refinement: node-level locking for structural edits, or
      property CRDTs, only against a reproduced conflict
- [ ] Asset sync: a content-addressed store with dedup and delta transfer

## Ongoing

- Keep the build reproducible and warning-clean.
- Keep the tests green; every fix gets a test that would have caught it.
- Keep the architecture documentation and ADRs current.
- Review dependency licenses for compatibility with MIT.
