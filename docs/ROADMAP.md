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

## Near-term priorities

The order the next work is actually taken in, chosen so that a small number of
things get *finished* rather than many get started. This is the roadmap's
front line; the milestones below hold the detail.

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
- [x] Asset import settings per asset type — typed settings stored in the asset's sidecar (`ImportSettings`, `AssetDatabase::import(path, scheme, settings)`) and applied at load via the `ResourceManager`; a texture's filter/wrap/mipmaps are honoured by the GPU ([ADR-0027](adr/0027-asset-import-settings.md))
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

## M2.5 — A vertical slice: one small game, end to end

**In progress.** The first complete game (`examples/breakout`) ships and is the
integration test; remaining work is the editor's authoring path over that same
game and tutorial polish.

The single highest-value next step, and deliberately inserted before the later
milestones. Infrastructure that has only ever passed its own unit tests has not
been proven to work *together*; a small finished game is what exercises the whole
engine in one artifact and exposes the missing glue. This is the project's own
guiding rule — "a few genuinely working systems beat a long list of half-finished
features" — applied at the project level, and it is the thing that attracts
contributors, because people contribute around something finished.

- [x] Build one small, complete game entirely in the engine — a breakout,
      pong, or tiny platformer — and ship it in `examples/` (or its own repo):
      `examples/breakout`, built from the public API, playable and headless
- [x] Make it the integration test: it uses the scene graph, resources, physics,
      audio, serialization, and the systems layer together (the editor's
      authoring path is exercised separately); its rules and systems are compiled
      into `fork_engine_tests` from the example's own sources, so the tests prove
      the shipped game, not a copy
- [x] Fix what it exposes; every gap it finds becomes a focused issue, not a new
      subsystem — the slice added an input channel to `FrameContext` and a
      cumulative mixer peak, and nothing else
- [x] Write a short "how this game was built" walkthrough, which doubles as the
      first real tutorial and the contributor on-ramp:
      `examples/breakout/README.md`

*Why here:* before more engine breadth. One shipped game teaches more about what
is missing than ten roadmap items, and it is the precondition for a community.

## M3 — Reflection and a real asset pipeline

- [x] Property inspector integration with editor hints (Range, Color, Resource) — typed hints on `Property` (`RangeHint`/`ColorHint`/`ResourceHint`) drive a bounded slider, a colour picker, and an asset drop target; resource edits are undoable via `SetResourceCommand` ([ADR-0028](adr/0028-editor-hint-metadata.md))
- [x] GUID-based resource references that survive file moves — a per-asset GUID
      sidecar and an asset database; see
      [ADR-0020](adr/0020-resource-identity-guid-sidecar.md)
- [x] Import settings per asset type — typed, stored in the sidecar, applied at load ([ADR-0027](adr/0027-asset-import-settings.md))
- [x] An import cache — a content-addressed store so a re-import and a rebuild are cheap ([ADR-0029](adr/0029-import-cache.md))
- [x] Hot reload of resources — the manager records each cached resource's file
      and write time, `poll_changed()` reports the ones that changed, and
      `reload()` re-loads a named one and returns the fresh instance for the
      caller to adopt ([ADR-0046](adr/0046-resource-change-detection-and-reload.md))

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
- [x] A spatial hierarchy (BVH/octree) over static geometry — `SpatialBvh` in
      `core` builds a binned-SAH binary BVH over world-space AABBs and answers
      frustum and box queries, reusing `Frustum::intersects_aabb` as its leaf test,
      with the query checked against a brute-force reference
      ([ADR-0053](adr/0053-spatial-bvh.md)); `Renderer3D` now culls through a
      cached tree, rebuilt only when an instance's world bounds change, and the
      tree path is pinned to the flat reference `cull_flat` by a test that asserts
      an identical visible set and order ([ADR-0056](adr/0056-renderer3d-spatial-cull.md))
- [x] Environment rendering and a post-processing chain — a camera-driven
      full-screen gradient background, and an opt-in exposure/tonemap/vignette
      chain over an off-screen colour target; see
      [ADR-0057](adr/0057-environment-and-post-processing.md)
- [x] A glTF importer — `GltfLoader` decodes `.gltf` (JSON) and `.glb`
      (binary) into a `Mesh` per primitive, reading through `ResourceSource` so a
      mounted pack works unchanged; see [ADR-0040](adr/0040-gltf-importer.md)

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
- [~] 2D island detection and body sleeping: bodies group into islands over the
      contact-and-joint graph and a settled island stops being solved — see
      [ADR-0026](adr/0026-physics2d-islands-sleeping.md)
- [~] 2D physics refinements: collider offsets and oriented-box colliders are
      done — a box collider carries the node's Z rotation and a local offset, and
      the narrow phase is a separating-axis test over the oriented shapes, so a
      rotated box is collided as its true shape rather than its bounding box —
      see [ADR-0037](adr/0037-physics2d-oriented-boxes-offsets.md). Continuous
      collision detection remains.
- [x] 3D physics (first cut): box and sphere colliders, dynamic and static
      bodies, a linear sequential-impulse solver (gravity, restitution,
      friction, damping), a uniform-grid broad phase, distance joints, scene
      queries, and contact events (`Collider3D`, `RigidBody3D`,
      `DistanceJoint3D`, `Physics3DSystem`) — a hand-written core kept behind a
      component-and-query boundary so a third-party solver can replace it later;
      see [ADR-0031](adr/0031-physics3d-and-backend-seam.md)
- [~] 3D physics refinements: angular dynamics are done — a body carries an
      angular velocity, torque, and a shape-derived (isotropic) inertia, the
      solver integrates the orientation as a quaternion and applies angular
      impulses from contacts and joints (`RigidBody3D` angular fields, `Quat` in
      `core/Math.h`) — see [ADR-0043](adr/0043-physics3d-angular-dynamics.md).
      Oriented colliders, more joint types, and islands / sleeping remain,
      mirroring the 2D cuts; deferred behind the backend seam rather than chased
      to parity
- [x] Integration with the scene and the editor: the play schedule runs
      `Physics3DSystem`, and the editor copies `Collider3D` / `RigidBody3D` into
      the play scene, so a 3D scene plays with physics through the same path a
      shipped game uses — see
      [ADR-0058](adr/0058-editor-physics3d-and-debug-draw.md)
- [x] Debug drawing: a `debugdraw` module builds collider wireframes for a 2D or
      a 3D scene (`DebugDrawList`) and a `DebugRenderer` draws them as a single
      line list over the world camera, built on a new RHI line primitive — see
      [ADR-0058](adr/0058-editor-physics3d-and-debug-draw.md)

## M6 — Animation and audio

- [x] 2D sprite-sheet animation (frame playback; the first system) — see M2 prerequisite
- [x] Audio playback and mixing: `AudioDevice` with a real (miniaudio) and a null
      backend, `AudioClip` + `WavLoader`, `AudioSource`, and `AudioSystem` with a
      pure mixer — see [ADR-0016](adr/0016-audio-module.md)
- [~] Audio refinements: 3D positional attenuation and audio buses / mixing
      groups — see [ADR-0034](adr/0034-audio-attenuation-buses.md)
- [x] Streaming / large-file playback and compressed codecs: mu-law, A-law, and
      IMA ADPCM decoding in `WavLoader`, and a pull-based `AudioStream` /
      `WavStream` a source opts into with `AudioSource::stream` — see
      [ADR-0060](adr/0060-compressed-audio-and-streaming.md)
- [x] Tilemaps — a `Tilemap` component holds a grid of tile indices over a
      tileset texture (`columns` x `rows`, `tile_size`, tint, layer). Cell
      (0, 0) is the map's top-left and +Y goes down, the convention every tile
      editor uses; `kEmpty` draws nothing, so a sparse map is cheap. Extraction
      emits one `render::SpriteInstance` per non-empty cell into the *same*
      array the sprites fill, so a tile reuses the sprite draw path, the shared
      cull, and the `(layer, texture)` batcher — a 100x100 level is one
      component and one draw call per texture, not ten thousand nodes. The grid
      persists as the component's own compact JSON through the `grid` reflection
      property, the trade `AnimationStateMachine` already made
      ([ADR-0062](adr/0062-tilemaps.md)). Tile collisions, mixed tile sizes, and
      a dedicated tile editor remain
- [ ] Skeletal animation and skinning
- [x] Animation state machines — a named-state graph over the sprite animator:
      states (a clip: first frame, frame count, fps, loop) and transitions (from,
      to, a require-finished gate, and trigger conditions) in an
      `AnimationStateMachine` component, advanced by
      `AnimationStateMachineSystem`, which drives the node's `SpriteAnimator`
      cell. Blend trees, continuous parameters, and skeletal states build on this
      shape and remain ([ADR-0059](adr/0059-animation-state-machines.md))

## M7 — Scripting

Direction (decided, ADR to follow): **C++ is the primary language for now**, via
native gameplay modules that build against the engine and hot-reload, and the
scripting layer is built as a **language-agnostic seam** so another language can
be added later without the engine core naming it. A purpose-built engine
scripting language (a GDScript-style option) is a *possible* future, deliberately
**not started** — it is a multi-year commitment and only worth it if it is
dramatically better for the engine's use case than embedding a proven language,
not merely different. The seam is the cheap, reversible decision; the language
is the expensive one, and it waits.

- [x] Choose and document the scripting strategy (ADR first): C++ native modules
      as the primary path, the seam's shape, and the criteria under which an
      embedded language or a custom language would be added
      ([ADR-0044](adr/0044-scripting-seam.md))
- [x] A language-agnostic scripting seam: the engine core talks to an interface
      (`Script`/`ScriptHost`/`ScriptRegistry`), never to a concrete language
      (`fe/script/`, driven by `ScriptSystem` through the ordinary schedule)
- [x] C++ native gameplay modules: compile against the engine, load, and
      hot-reload without restarting the editor — a module is a shared library
      that registers behaviours through a versioned entry point
      (`ScriptModuleApi.h`); `ScriptLibrary` opens it through the platform layer's
      new `DynamicLibrary` seam (the deferred "dynamic libraries" row of
      [ADR-0041](adr/0041-platform-contract-and-null-backend.md)) and reloads it
      by destroying the live scripts first and rebinding them to the new code
      ([ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)). Automatic
      file-watching landed with editor integration
      ([ADR-0066](adr/0066-editor-driven-module-hot-reload.md))
- [x] Reflection-backed bindings, so a script's exposed properties flow through
      the existing reflection and serialization path
      ([ADR-0063](adr/0063-reflection-backed-script-properties.md))
- [ ] Editor integration (attach, inspect, hot reload) — the headless half has
      landed: the editor owns a `ScriptCatalog` (a `ScriptHost` plus a
      `ScriptRegistry`) and the document persists its bindings through the
      serializer's `ScriptBinding` seam, so a scene saved from the editor comes
      back scripted, the inspector lists a node's behaviour and its reflected
      properties, and every reflected script field — a scalar or a resource
      reference — is an undoable `SetScriptProperty`/`SetScriptResource` operation
      ([ADR-0063](adr/0063-reflection-backed-script-properties.md)).
      File-watching hot reload has also landed: the editor's `ScriptCatalog` owns
      a `ScriptLibrary`, loads gameplay modules named on the command line
      (`--module`), and polls them each frame, hot-reloading a rebuilt one and
      rebinding the nodes it scripted — including the nodes a scene load scripted
      ([ADR-0066](adr/0066-editor-driven-module-hot-reload.md)).
      Still to do: debouncing a build
- [ ] Debugging support
- [ ] *(Deferred)* An embedded language (Lua/Wren) or a custom engine language,
      only once the seam is proven and the criteria in the ADR are met

## M8 — Ship anywhere (cross-platform targets)

The goal is that a game built in Fork Engine reaches every platform its players
use, not merely that the editor runs on several OSes. Plan and trade-offs:
[`STRATEGY.md`](STRATEGY.md), [ADR-0018](adr/0018-multi-platform-shipping.md).

- [x] Platform interface audit: the full OS contract (window/surface, input
      incl. touch and gamepad, filesystem, threads, time, dynamic libraries,
      lifecycle) is recorded concern by concern, with what is implemented now
      and what is deferred, and a **null/headless** window backend exists for CI
      and servers ([ADR-0041](adr/0041-platform-contract-and-null-backend.md)).
      The engine's frame loop and its systems run with no display and no GL
      context; the `fork_headless` example is the executable proof.
- [x] Headless rendering: a null RHI device so the render extraction, batching,
      and draw submission run with no context, selected by an explicit
      `Engine::Config::headless_render` opt-in ([ADR-0042](adr/0042-null-rhi-device.md)).
- [ ] The touch/gamepad, dynamic-library, and suspend/resume rows of the
      platform contract, each implemented behind the existing seam
- [x] Asset packaging: a versioned pack format (`PackFile`), a writer that turns
      a project's asset tree and startup scene into one file, and a `PackSource`
      the runtime mounts so the loaders and the scene reader read from a pack as
      transparently as from disk ([ADR-0030](adr/0030-asset-pack-format.md))
- [ ] `fork_pack` tooling polish: compression and streaming a large pack.
      Reachability — packing only what the startup scene references — landed as an
      opt-in `--prune` on `fork_pack` and `fork_export`: a scene's references are
      traced through reflection (a component's `ResourceRef` properties and a
      script's property strings, resolved against the asset database) and only
      those paths are packed ([ADR-0065](adr/0065-pack-reachability-pruning.md)).
      Following a packed asset's own references (a glTF's external buffers) and
      making pruning the default remain
- [x] The export path: `fork_player` mounts a pack and runs the game loop, and
      `fork_export` assembles a runnable build (the player, a fresh pack, and a
      manifest) from a project ([ADR-0064](adr/0064-export-runtime-and-player.md)).
      The runtime is generic and driven by the pack, so export has no compiler in
      it. The exported build is exercised end to end in CI.
- [ ] Per-desktop-target runtimes and app packaging: a prebuilt `fork_player`
      for each OS, cross-compilation, and platform app bundles (`.app`, MSI)
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
- [x] Benchmark: a headless target reporting frame-time percentiles; CI records
      the numbers and fails on regression — `benchmarks/scene_bench.cpp` runs
      synthetic scenes through the real frame loop and reports p50/p95/p99;
      `benchmarks/check_regression.py` compares against the committed baseline
      and fails past a coarse tolerance, and a dedicated CI job runs it
      ([ADR-0033](adr/0033-performance-regression-gate.md))
- [x] Render extraction: split both renderers into "extract a flat instance
      list" and "issue draws" (the data-oriented seam) — a shared `render` module
      holds `render::SpriteInstance`/`MeshInstance` and the pure extract/cull
      steps; each renderer's `render()` is extract → cull → draw, and the draw
      step does no culling ([ADR-0052](adr/0052-render-extraction-seam.md))
- [ ] Job system: a `jobs` module (thread pool, `parallel_for`, task graph);
      `FrameContext` exposes it to systems
- [ ] Parallel extraction and culling: chunk the instance array; output must be
      identical to the serial path
- [x] Memory: a per-frame arena for transient data, with allocation counters —
      the `core` primitive (`FrameArena`: bump allocate / `reset` /
      `mark`+`restore` / `create<T>`) with allocation counters and a test that
      asserts the steady-state allocate/reset loop performs no heap allocation
      ([ADR-0051](adr/0051-frame-arena.md)); `Application` now owns one arena,
      resets it once per frame before the systems run, and hands it to every
      system through `FrameContext::arena` (nullable, like `resources`/`input`),
      with headless tests pinning the per-frame reset and the disabled path
      ([ADR-0054](adr/0054-frame-loop-arena.md)). Moving the extraction seam onto
      the arena is the follow-on
- [ ] GPU efficiency: instancing, material batching, and a render-graph seam for
      passes and post-processing
- [x] An in-editor profiler panel fed by the profiler data — `ProfilerPanel` +
      `ProfilerPanelViewModel` in `editor`; the editor owns an optional
      `core::Profiler`, brackets its own frame with it, and the panel draws the
      latest `FrameReport` (totals, per-name scope breakdown, counters,
      truncation) ([ADR-0055](adr/0055-editor-profiler-panel.md))

## M10 — Collaborative editing

Several people edit one project live, with presence and no manual merge. It
lands after the editor is mature because it reaches into the editor's core. Plan
and trade-offs: [`STRATEGY.md`](STRATEGY.md),
[ADR-0019](adr/0019-collaborative-editing.md).

- [x] Component identity: stable ids that survive serialization, reimport, and
      undo — a `Uuid` on every `Component`, assigned at creation and written to
      the scene document (format version 3); `Scene::find_component` resolves one
      anywhere in the tree ([ADR-0045](adr/0045-component-identity.md))
- [x] Operation model (local only): a serializable `Operation` generalising
      `Command`, naming objects by stable id, with JSON round-trip, `apply`, and
      a document-derived `inverse`; local edits route through an
      `OperationCommand` adapter ([ADR-0047](adr/0047-local-operation-model.md))
- [x] Replicated log (local only): a `Session` holding a document and an ordered
      log; two in-process replicas converge; undo becomes log-based
      ([ADR-0048](adr/0048-replicated-operation-log.md))
- [ ] Transport and server: a `net` module with an in-process loopback transport
      (tests) and WebSocket (production), plus an authoritative session server
      (ordered structural operations, last-writer-wins per property)
      — the loopback transport, the `Transport` seam, the authoritative
      `SessionServer`, and the `SessionClient` have landed
      ([ADR-0049](adr/0049-net-transport-and-session-server.md)); the WebSocket
      backend and optimistic local apply remain
- [ ] Presence: cursors, selections, and per-user viewport cameras
- [ ] Conflict policy refinement: node-level locking for structural edits, or
      property CRDTs, only against a reproduced conflict
- [ ] Asset sync: a content-addressed store with dedup and delta transfer

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
