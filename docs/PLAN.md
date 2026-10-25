# Fork Engine Plan

One file for the whole plan, in the order it is read: the **pillars** (why the
work is ordered this way), the **near-term priorities**, the **milestones**
(what each one contains and where it stands), the **shipping queue** (the
ordered backlog the daily automation works through), and the **ongoing** rules.

These were once separate files — `STRATEGY.md`, `ROADMAP.md`, `MILESTONES.md`
and `SHIPPING_QUEUE.md` — which restated each other. They are now one, and each
idea appears once.

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
3. **Edits are durable and shareable.** An edit is a discrete, serializable
   operation over a stably identified object, not a direct mutation, so undo,
   redo, and replay all fall out of the same model. That is what makes a local
   edit trustworthy; networked collaborative editing would be the same model
   replicated, and it is not planned (see *Not planned* below).
   - *What it implies:* stable identity for everything editable; an operation
     model; and an ordered log every replica applies in the same order. It
     reuses what exists: the scene is reflection-driven, and the editor already
     routes every edit through a command stack, so this is that command stack
     made serializable.
   - *What landed:* stable component identity
     ([ADR-0045](adr/0045-component-identity.md)), the operation model
     ([ADR-0047](adr/0047-local-operation-model.md)), and the ordered log
     ([ADR-0048](adr/0048-replicated-operation-log.md)). They are shipped and
     tested, and they make undo/redo more robust on their own.
   - *Not pursued:* the transport, presence, and authoritative session that only
     existed to serve the network — a hosted service whose running cost the
     project will not carry.

The pillars are deliberately ordered by risk. Performance is the lowest-risk and
improves everything else; shipping is medium; collaborative editing was the
highest-risk because it reaches into the editor's core, which is why only its
local, headless first steps were taken and the network was left out.

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
6. **Editor polish and more platforms** — after the above.

The intent: finish the small things. The engines that last are the ones whose
authors ruthlessly complete them.

## Milestones

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
      new `DynamicLibrary` seam (the "dynamic libraries" row of
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
      Debouncing a build has landed too: the poll reports a module only once its
      image's write time has held across a requested number of consecutive polls,
      and the frame loop asks for two, so a link still writing the image is not
      read as a finished module ([ADR-0067](adr/0067-debouncing-a-module-build.md))
- [ ] Debugging support
- [ ] *(Deferred)* An embedded language (Lua/Wren) or a custom engine language,
      only once the seam is proven and the criteria in the ADR are met

## M8 — Ship anywhere (cross-platform targets)

The goal is that a game built in Fork Engine reaches every platform its players
use, not merely that the editor runs on several OSes. Plan and trade-offs: the
pillars above, [ADR-0018](adr/0018-multi-platform-shipping.md).

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
- [ ] The gamepad and suspend/resume rows of the platform contract, each
      implemented behind the existing seam. Touch is done end to end: the web
      backend feeds `InputState` from DOM touch events, and gameplay reaches it
      through the action layer, so a phone plays the same code a keyboard does
      ([ADR-0075](adr/0075-gameplay-actions-input-seam.md)). The dynamic-library
      row landed with `platform::DynamicLibrary`,
      [ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)
- [x] Asset packaging: a versioned pack format (`PackFile`), a writer that turns
      a project's asset tree and startup scene into one file, and a `PackSource`
      the runtime mounts so the loaders and the scene reader read from a pack as
      transparently as from disk ([ADR-0030](adr/0030-asset-pack-format.md))
- [ ] `fork_pack` tooling polish: compression and streaming a large pack.
      Reachability — packing only what the startup scene references — landed on
      `fork_pack` and `fork_export`: a scene's references are
      traced through reflection (a component's `ResourceRef` properties and a
      script's property strings, resolved against the asset database) and only
      those paths are packed ([ADR-0065](adr/0065-pack-reachability-pruning.md)).
      Pruning is now the default for both tools; `--no-prune` restores
      whole-tree packing. Following a packed asset's own references (a glTF's
      external buffers) remains
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
prove it and protect it. Plan and trade-offs: the pillars above,
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
- [x] Job system: a `jobs` module (thread pool, `parallel_for`, task graph);
      `FrameContext` exposes it to systems — `JobSystem` is a fixed `std::thread`
      pool with `parallel_for` and a dependency `TaskGraph`, and one API in two
      modes: a worker count of zero (the default) runs every job inline, so the
      module landed without changing any existing run and the serial-equivalence
      test compares the two. The submitting thread helps run the queue while it
      waits, so a nested call cannot deadlock. `FrameContext::jobs` is nullable
      like `resources`/`input`/`arena`, and `Application` owns the pool, sizes it
      from `set_worker_count`, and joins it when `run()` returns. The device stays
      single-threaded: a job extracts, culls, or computes, and only the frame
      thread submits. CI runs the pure-logic suite under ThreadSanitizer
      (`FE_ENABLE_TSAN`; ADR-0068)
- [x] Parallel extraction and culling: chunk the instance array; output must be
      identical to the serial path — `render::extract_sprites`/`extract_tiles`/
      `extract_meshes` and `cull_sprites`/`cull_meshes` take an optional
      `JobSystem*`; each chunk appends to its own list (extraction) or fills a
      byte mask (culling) and the results are joined in order, so the output is
      byte-identical to serial. Two measured thresholds keep the fan-out off
      where it does not pay: extraction above 1024 nodes (break-even ~1200),
      culling above 16384 instances (it is a loss below that). `Renderer2D`/
      `Renderer3D` thread the pool through, `Application::render_frame` supplies
      its own, and every other caller keeps the serial path unchanged
      ([ADR-0069](adr/0069-parallel-extraction-and-culling.md))
- [x] Memory: a per-frame arena for transient data, with allocation counters —
      the `core` primitive (`FrameArena`: bump allocate / `reset` /
      `mark`+`restore` / `create<T>`) with allocation counters and a test that
      asserts the steady-state allocate/reset loop performs no heap allocation
      ([ADR-0051](adr/0051-frame-arena.md)); `Application` now owns one arena,
      resets it once per frame before the systems run, and hands it to every
      system through `FrameContext::arena` (nullable, like `resources`/`input`),
      with headless tests pinning the per-frame reset and the disabled path
      ([ADR-0054](adr/0054-frame-loop-arena.md)). The extraction seam is now
      arena-backed as well: `extract_sprites_into`/`extract_tiles_into`/
      `cull_sprites_in_place` write into a caller-sized span, take their parallel
      scratch from the arena, and `Application::render_frame` runs the 2D pass
      with no heap allocation, falling back to the heap path on overflow
      ([ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md))
- [x] Parallel physics broad phase: the uniform-grid insert is chunked across the
      job pool (count-then-write, so the entry buffer and the resulting pair set
      are byte-identical to the serial build), above a measured threshold
      ([ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md))
- [x] GPU efficiency: instancing — `Renderer2D` uploads the unit-square corners
      once and a per-instance transform/UV/colour record per sprite, and issues
      one indexed instanced draw per texture batch, with the per-vertex UV
      convention preserved so the image is unchanged. Material batching is part of
      the batch key already; a render-graph seam for passes and post-processing
      remains
      ([ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md))
- [x] An in-editor profiler panel fed by the profiler data — `ProfilerPanel` +
      `ProfilerPanelViewModel` in `editor`; the editor owns an optional
      `core::Profiler`, brackets its own frame with it, and the panel draws the
      latest `FrameReport` (totals, per-name scope breakdown, counters,
      truncation) ([ADR-0055](adr/0055-editor-profiler-panel.md))

## Not planned

**Networked collaborative editing.** Several people editing one project live over
a shared server was once planned as M10. It is not planned any more: it needs a
hosted authoritative service, and that running cost is not one the project will
carry. The local steps that landed on the way — stable component identity
([ADR-0045](adr/0045-component-identity.md)), the serializable operation model
([ADR-0047](adr/0047-local-operation-model.md)), and the ordered log
([ADR-0048](adr/0048-replicated-operation-log.md)) — stay: they are shipped,
tested, and they make undo/redo more robust on their own. The transport and
session-server work that only existed to serve the network is likewise not
pursued further. Console targets are not planned either: no code lands without
SDK access.

## The shipping queue

The ordered backlog the daily automation works through, one item per run. This
section is the single source of truth for *what to do next*: the automation
takes the first item whose `slug` does not appear in an `fe-task:` commit
subject and does exactly that. When the queue is empty the automation makes no
changes and exits.

**Goal of the queue:** a 2D game built in Fork Engine runs in a browser, then on
phones, then as a downloadable desktop build. Web is first because it is the
cheapest target to verify (no store review, no device matrix) and it forces the
sandboxed-filesystem and non-desktop-graphics paths early
([ADR-0071](adr/0071-web-first-2d-shipping-path.md)).

## How an item is worked

- The commit subject is `fe-task: <slug> - <short summary>`.
- "Done when" is the acceptance test. Satisfy it literally.
- Add or update tests. A change with no test is not finished.
- Build and test clean with `-DFE_WARNINGS_AS_ERRORS=ON` before committing.
- If an item is too large for one run, land a coherent tested first step and end
  the commit body with a `Remaining:` line naming what is left.
- Do not reorder or skip items. Take the first unfinished one.
- An item is finished when its slug appears in an `fe-task:` subject; that is
  the queue's state, derived from history, and it is never stored twice.

## Phases at a glance

| Phase | Ship bar |
| --- | --- |
| 1 — a 2D game renders in a browser | A game runs in a headless Chromium and is verified to draw |
| 2 — the same game on a phone | The same game accepts touch and runs on a mobile browser |
| 3 — a downloadable desktop build | A per-OS artifact a player can download and run |
| 4 — the game is worth shipping | The slice is a real game, not a demo |

Which items are done is not recorded here: a slug is done when an `fe-task:`
commit names it, and that is the only place the state lives.

---

## Phase 1 — a 2D game renders in a browser

The bar: `fork_export --target web` produces a directory, and a headless
browser loads it, runs the frame loop, and draws a non-blank frame.

### `glsl-dialect-seam`

Translate engine shader sources to a GLSL ES dialect behind the RHI, so WebGL2
can compile them, with desktop translation the identity.

**Done when** a unit test proves desktop passes through byte for byte and the ES
translation rewrites the version and adds a fragment precision exactly once.

### `shader-dialect-wiring`

Select the dialect where the device is created, not per shader.

**Done when** the graphics tests pass with the desktop dialect and a test proves
the dialect is applied at creation.

### `emscripten-toolchain`

An Emscripten CMake toolchain file that leaves the desktop build untouched.

**Done when** a documented, reproducible configure command exists and CI can run
it.

### `web-window-backend`

A canvas-backed window backend with a WebGL2 context and no GLFW.

**Done when** the web target builds and a headless smoke test constructs the
window.

### `frame-loop-stepping`

Expose `initialize()` / `step_frame()` / `shutdown()` so a browser's
`requestAnimationFrame` can drive the engine without blocking.

**Done when** a test drives N frames via `step_frame()` and observes the same
per-frame invariants as `run()`.

### `pack-fetch-source`

A `ResourceSource` that reads the pack over HTTP, because a browser has no
filesystem.

**Done when** a test serves a pack from an in-process HTTP server and reads an
asset through the fetch source, matching the disk source's bytes.

### `web-export-template`

`fork_export --target web` writes the wasm runtime, the pack, a manifest, and an
HTML/JS shell.

**Done when** the export produces that directory and the shell's expected file
names match what the export writes.

### `web-smoke-test-ci`

A CI job that configures and builds the web target so the web path cannot rot.

**Done when** the job exists, is green on a push, and runs on every change.

### `headless-browser-verification`

Load the exported build in headless Chromium and assert the game renders.

**Done when** the check runs in CI against a real export and fails when the
renderer is deliberately broken.

### `3d-in-the-browser`

Phase 1's bar is met by the 2D path, but the 3D renderer shares the whole
shipping path with it and had never been run in a browser. Three desktop-only
assumptions in it fail on a WebGL2 context, none with a build-time signal: the
shadow map's depth texture used a format/type pair GLSL ES 3.00 rejects, the
shadow pass drew into a framebuffer with a colour attachment enabled, and the
pruning tracer registered only the 2D builtins so a 3D scene traced to zero
references and shipped an empty pack. Make 3D render in a browser and cover it
in the web check.

**Done when** an exported 3D project renders a non-blank frame in headless
Chromium with no GL error, a pruned 3D export carries its mesh and texture, and
the web game check runs both renderers. See
[ADR-0076](adr/0076-3d-in-the-browser.md).

### `web-gameplay-logic`

A 3D scene rendered in a browser but nothing responded to input: gameplay logic
lived only in C++ modules, and a browser has no dynamic loader, so a web export
had no logic to consume the actions the input path was already delivering.
Compile a small, useful set of behaviours into the engine and register them
in-process, so every target — including wasm — has gameplay to run.

**Done when** a web build runs a scene whose bound behaviour reads the movement
actions and moves its node, verified by the browser input probe. See
[ADR-0078](adr/0078-engine-shipped-gameplay-behaviours.md).

### `playable-3d-web-game`

A 3D test game that is playable in a browser, not merely rendered: a fly camera,
a movable player, and something to look at, driven by the engine-shipped
behaviours with a keyboard and a touchscreen.

**Done when** the project exports for the web, runs in headless Chromium, and the
input probe observes the player node move in response to a synthetic key.

### `web-resize-and-dpr`

A browser canvas is resized by the page, not by the game, and a phone reports a
device pixel ratio above 1. Make the engine follow the canvas size and the DPR.

**Done when** a test changes the canvas size and observes the renderer's
viewport and the input coordinate space both follow.

### `web-audio-context`

A browser will not start an audio context until a user gesture. Make the web
audio device resume on the first gesture and stay silent rather than crash
before one.

**Done when** a headless test proves the device tolerates a context that starts
suspended and plays after a resume.

### `web-visibility-pause`

A backgrounded tab stops receiving animation frames, so the engine's clock
jumps on return. Clamp the frame delta when a page becomes visible again.

**Done when** a test feeds a large time step after a visibility change and
observes the simulation advance by a bounded amount.

### `web-build-in-ci-artifacts`

The web build is compiled in CI but thrown away. Publish the exported game as a
CI artifact so a run can hand a reviewer a link.

**Done when** the CI job uploads the export directory and the artifact downloads
and runs.

---

## Phase 2 — the same game on a phone

The bar: the breakout slice accepts touch and runs in a mobile browser, then
builds for Android.

### `webgl2-mobile-parity`

GLES3 on Android and iOS is close to WebGL2 but not identical. Confirm the ES
dialect and the RHI work under a GLES3 context and fix any divergence.

**Done when** the ES dialect sources compile under a GLES3 context (a software
GLES implementation is acceptable) and any divergence found is fixed with a
test.

### `touch-input-actions`

Express input as abstract actions so touch maps onto the same gameplay code as
keyboard and mouse. This item carried the web input plumbing it depends on: DOM
keyboard, pointer and touch events feed `InputState` from the web backend, and
browser `KeyboardEvent.code` maps onto the engine's `Key` enum so bindings are
layout-independent. Touch is therefore usable, not merely present.

**Done when** a test drives a touch sequence and observes the same gameplay
action a keyboard press produces.

### `touch-gesture-regions`

Bind screen regions to actions declaratively, so a game declares "the left third
steers" once instead of reading raw touch points.

**Done when** a game declares regions and a test proves a touch in each region
fires the bound action and a touch outside fires none.

### `mobile-viewport-meta`

A mobile browser needs a viewport meta tag and a full-bleed canvas or the game
renders at desktop scale with scrollbars.

**Done when** the export shell carries the viewport meta and a test asserts the
canvas fills the window under a simulated phone size.

### `touch-fullscreen-prompt`

Offer a fullscreen request on the first touch, so a phone game is not stuck in a
browser chrome frame.

**Done when** the shell requests fullscreen from a gesture and degrades silently
where the browser refuses.

### `mobile-safe-area`

Notches and home indicators cover a full-bleed canvas. Keep interactive regions
inside the safe area.

**Done when** the shell reads the safe-area insets and a test proves a declared
region is offset by them.

### `android-window-backend`

Implement the Android window backend: native activity, EGL surface, lifecycle
(suspend/resume), and touch.

**Done when** the Android target builds and a documented command produces a
runnable APK.

### `android-export-template`

Extend `fork_export` to produce an Android build.

**Done when** `fork_export --target android` produces a directory that installs
and launches on a device or emulator.

### `ios-window-backend`

Implement the iOS window backend: a Metal or GLES surface, lifecycle, and touch.

**Done when** the iOS target builds and produces a runnable app bundle. Note:
this needs a macOS host; record the constraint rather than stubbing it.

### `ios-export-template`

Extend `fork_export` to produce an iOS build.

**Done when** `fork_export --target ios` produces an Xcode project or archive
that a macOS host can sign and run.

---

## Phase 3 — a downloadable desktop build

The bar: a player downloads one file per OS, runs it, and plays the game.

### `desktop-export-templates`

The export path copies the player built beside it. Shipping needs a *prebuilt*
runtime per desktop OS so a developer without the engine's toolchain can export.

**Done when** a documented, reproducible release build produces a per-OS runtime
artifact that `fork_export` accepts via `--player`, and the smoke test runs it.

### `linux-appimage`

Produce a single-file Linux artifact that runs on a clean machine without the
engine's shared libraries.

**Done when** the AppImage builds reproducibly in CI and launches the game on a
clean container.

### `windows-zip`

Produce a Windows artifact a player can unzip and run.

**Done when** the zip builds in CI and the game launches from it on a clean
machine.

### `macos-app-bundle`

Produce a `.app` bundle, then a DMG, with the runtime's dependencies bundled.

**Done when** the DMG builds in CI and the game launches from it on a clean
machine.

### `desktop-packaging`

Script the packaging for all three desktop targets behind one command.

**Done when** each packaging step is scripted, reproducible in CI, and produces
an artifact that runs on a clean machine.

### `release-version-stamping`

A downloadable build must say which version it is. Stamp the engine version into
the exported manifest and show it in the game.

**Done when** the export manifest carries the version from
`CMakeLists.txt` and a test proves it matches `fe::version::kEngineVersion`.

---

## Phase 4 — the game is worth shipping

The bar: the slice is a game a stranger can play, not a demo.

### `slice-title-and-menus`

A title screen, a pause, and a game-over screen, so the slice has a shape.

**Done when** a test drives the state machine through title → play → game over →
title.

### `slice-scoring-and-progression`

Score, lives, and levels that get harder, so there is a reason to keep playing.

**Done when** a test proves a level advances on a clear and the difficulty
increases.

### `slice-persistence`

Save the best score locally, so progress survives a reload.

**Done when** a test writes and reads a score through the storage seam on both
desktop and web.

### `slice-settings`

Volume and control settings, persisted, with a touch-friendly menu.

**Done when** a test changes a setting, reloads, and observes it retained.

### `scripting-native-modules`

The scripting seam plus C++ native modules as the primary language (M7), so game
logic can live outside the engine.

**Done when** a game's rules load from a module the engine did not compile in,
with a test covering load and reload.

---

## Backlog — not yet ordered

- Additional graphics backends (Vulkan, Metal, WebGPU) as targets demand.
- Skeletal animation/skinning (the one open M6 item), needed for animated 3D.
- More example games beyond the breakout slice, once the first one ships.

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
