# Fork Engine Roadmap

The [strategy](STRATEGY.md) says *why* the work is ordered this way — three
pillars that shape the design. The [shipping queue](SHIPPING_QUEUE.md) says
*what to do next*, one item per run.

This document is the *sequence*: what each milestone contains, what it depends
on, where it stands, and how you know it is done. It is a plan, not a promise;
milestones may be reordered as the design becomes clearer, and a change to a
decision is recorded in an ADR.

## How to read this document

Each milestone is a coherent capability, not a feature list, and each is
independently testable: it must build and pass its tests before the next one
starts. The status of each item is one of:

- `[x]` — done and tested. The code is in the tree.
- `[~]` — partly done. The done part is named, and what remains is named with it.
- `[ ]` — not started.

A milestone's **Done when** line is the acceptance test. It is written so that a
reader can check it without trusting the prose: it names a test or an artifact.
Where a milestone is large, the work is broken into steps that each land
separately.

## The guiding rule

**A few genuinely working systems beat a long list of half-finished features.**
This is why M2.5 (a vertical slice) sits before the breadth milestones, and why
the shipping queue is a single ordered line rather than a set of parallel tracks.
A milestone that cannot be demonstrated end to end is not finished, no matter how
many of its items are checked.

## Milestone map

| # | Milestone | Depends on | Status |
| --- | --- | --- | --- |
| M1 | Foundation | — | done |
| M2 | Editor foundation | M1 | mostly done (export moved to M8) |
| M2.5 | A vertical slice | M2 | in progress |
| M3 | Reflection and a real asset pipeline | M2 | done |
| M4 | 3D rendering | M1, M3 | mostly done (pass culling open) |
| M5 | Physics | M1, M3 | mostly done (3D parity open) |
| M6 | Animation and audio | M4 | mostly done (audio refinements open) |
| M7 | Scripting | M2, M5 | core done (debugger integration open) |
| M8 | Ship anywhere | M2.5, M3 | partly done (web done, mobile open) |
| M9 | Performance | M1 | mostly done (pass culling open) |

The dependencies are real: M4 needs the asset pipeline from M3 to load a mesh,
and M7 needs a scene with gameplay systems (M5) before a script has anything to
drive.

## Near-term priorities

The order the next work is actually taken in, chosen so that a small number of
things get *finished* rather than many get started. This is the roadmap's front
line; the milestones below hold the detail.

1. **Make the slice a game worth shipping** — the breakout slice becomes a game
   a stranger can play: a title, a score, lives, levels, persistence, and
   settings (M2.5 and the [shipping queue](SHIPPING_QUEUE.md)'s Phase 4).
   Everything else is secondary to having one thing that is genuinely finished.
2. **Honest scope** — the "ship anywhere" claim is scoped to desktop for now, in
   [ADR-0032](adr/0032-near-term-platform-scope.md), so the README does not
   promise more than the engine delivers.
3. **Scripting depth** — the language-agnostic seam and C++ native modules are
   in place (M7), and the in-engine debugging surface has landed ([ADR-0109](adr/0109-in-engine-debugging-surface.md)):
   a bounded log buffer, recorded script failures, and an editor console. What
   remains is a debugger-integration story for a developer with a local build.
4. **Guard the performance claims** — the benchmark as a CI regression gate
   (M9) and sanitizer builds (ASan/UBSan, TSan), while the codebase is still
   small enough to keep them cheaply green.
5. **Editor and rendering depth** — a 3D authoring view (M2) and the render
   graph (M4, M9), which together unblock skeletal animation and instancing.
6. **More platforms** — desktop artifacts and mobile (M8), after the above.

The intent: finish the small things. The engines that last are the ones whose
authors ruthlessly complete them.

---

## M1 — Foundation (done)

The smallest useful engine: something that builds, runs, renders, and is tested.
Everything later assumes this exists.

- [x] Build system, repository structure, CI
- [x] Application entry point and platform abstraction (window, input, context)
- [x] Logging, assertions, and error handling ([ADR-0003](adr/0003-error-handling.md))
- [x] Memory and resource ownership conventions ([ADR-0004](adr/0004-resource-ownership-weak-cache.md))
- [x] RHI abstraction with an OpenGL 3.3 core backend
- [x] Minimal 2D renderer: batched, textured sprites with a camera
- [x] Scene representation with JSON serialization ([ADR-0001](adr/0001-engine-architecture-overview.md))
- [x] A runnable example (the sandbox)
- [x] Unit tests and graphics tests

**Done when** the sandbox runs, draws sprites, and `ctest` is green. *Met.*

## M2 — Editor foundation

The editor becomes real before the engine grows much further, so the runtime is
built editor-friendly from the start rather than adapted later. Retrofitting an
editor onto an engine that never expected one is the expensive path; this is the
cheap one.

**Why before breadth:** the editor is the first consumer of reflection,
serialization, and the command stack. Building it early forces those seams to be
usable, not merely present.

- [x] A windowing/UI layer for tools (docking, panels) — [ADR-0007](adr/0007-editor-ui-toolkit.md), `fork_editor`
- [x] Scene hierarchy panel — `HierarchyViewModel` + `HierarchyPanel`
- [x] Property inspector, driven by the reflection system — `PropertyEditor` + `InspectorPanel`
- [x] 2D viewport rendering into an editor panel — `Viewport` + `ViewportPanel` (off-screen render target)
- [x] Asset browser — `AssetBrowserViewModel` (headless) + `AssetBrowserPanel`, listing the asset database's GUID-tagged assets with search and a copyable reference
- [x] Undo/redo — `Command` + `CommandStack` + concrete commands in `fork_editor_core`; coalescing drags into one step ([ADR-0015](adr/0015-editor-undo-redo.md))
- [x] Editing a node's transform — a Transform section in the inspector and drag-to-move in the viewport, both routed through the command stack as the `SetTransform` operation ([ADR-0089](adr/0089-editor-transform-editing.md))
- [x] Play-in-editor (embed an `Engine`, drive frames, run the scene's `SceneSchedule`) — `PlaySession`
- [x] Project asset root — `EditorContext::set_project_root` + `--project`, indexing the asset database so the browser and GUID references resolve
- [x] Project settings — `ProjectSettings` (headless) + `fork.project.json`, opened with `--project`/`--project-file`; name, asset root, and startup scene, with portable relative paths ([ADR-0025](adr/0025-project-file.md))
- [x] Asset import settings per asset type — typed settings stored in the asset's sidecar (`ImportSettings`, `AssetDatabase::import(path, scheme, settings)`) and applied at load via the `ResourceManager`; a texture's filter/wrap/mipmaps are honoured by the GPU ([ADR-0027](adr/0027-asset-import-settings.md))
- [ ] **3D authoring view** — the editor's viewport is hardcoded 2D (`Camera2D` + `Renderer2D`), so a 3D scene cannot be seen or placed in the editor even though the 3D renderer and physics exist. This is the largest remaining editor gap and the precondition for authoring a 3D game.
- [ ] Build/export path — moved to M8, where the packaging, pack format, and per-target export templates live ([ADR-0018](adr/0018-multi-platform-shipping.md))

**Done when** a scene can be built, edited, saved, reloaded, and played from the
editor without hand-editing a file. *Met for 2D; 3D authoring is open.*

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
integration test. What remains is not engine work but game work: making the slice
a game a stranger can play (the [shipping queue](SHIPPING_QUEUE.md)'s Phase 4).

The single highest-value next step, and deliberately inserted before the later
milestones. Infrastructure that has only ever passed its own unit tests has not
been proven to work *together*; a small finished game is what exercises the whole
engine in one artifact and exposes the missing glue. This is the project's
guiding rule applied at the project level, and it is the thing that attracts
contributors, because people contribute around something finished.

- [x] Build one small, complete game entirely in the engine — a breakout, pong,
      or tiny platformer — and ship it in `examples/`: `examples/breakout`,
      built from the public API, playable and headless
- [x] Make it the integration test: it uses the scene graph, resources, physics,
      audio, serialization, and the systems layer together; its rules and systems
      are compiled into `fork_engine_tests` from the example's own sources, so
      the tests prove the shipped game, not a copy
- [x] Fix what it exposes; every gap it finds becomes a focused issue, not a new
      subsystem — the slice added an input channel to `FrameContext` and a
      cumulative mixer peak, and nothing else
- [x] Write a short "how this game was built" walkthrough, which doubles as the
      first real tutorial and the contributor on-ramp: `examples/breakout/README.md`
- [~] Make the slice a *game*, not a demo — a title, a score, lives, levels that
      get harder, persistence, and settings. The engine pieces this needs
      (`TextLabel`, `UiCanvas`/`UiElement`) have landed; the game-side state
      machine and scoring are the queue's Phase 4 items
      (`slice-title-and-menus`, `slice-scoring-and-progression`,
      `slice-persistence`, `slice-settings`).

*Why here:* before more engine breadth. One shipped game teaches more about what
is missing than ten roadmap items, and it is the precondition for a community.

## M3 — Reflection and a real asset pipeline

A game is mostly content. Without an asset pipeline that survives a rename, a
re-import, and a rebuild, every later milestone is slower to build.

- [x] Property inspector integration with editor hints (Range, Color, Resource) — typed hints on `Property` (`RangeHint`/`ColorHint`/`ResourceHint`) drive a bounded slider, a colour picker, and an asset drop target; resource edits are undoable via `SetResourceCommand` ([ADR-0028](adr/0028-editor-hint-metadata.md))
- [x] GUID-based resource references that survive file moves — a per-asset GUID sidecar and an asset database; see [ADR-0020](adr/0020-resource-identity-guid-sidecar.md)
- [x] Import settings per asset type — typed, stored in the sidecar, applied at load ([ADR-0027](adr/0027-asset-import-settings.md))
- [x] An import cache — a content-addressed store so a re-import and a rebuild are cheap ([ADR-0029](adr/0029-import-cache.md))
- [x] Hot reload of resources — the manager records each cached resource's file and write time, `poll_changed()` reports the ones that changed, and `reload()` re-loads a named one and returns the fresh instance for the caller to adopt ([ADR-0046](adr/0046-resource-change-detection-and-reload.md))

**Done when** an asset can be imported, referenced by GUID, re-imported cheaply,
and hot-reloaded without restarting the editor. *Met.*

## M4 — 3D rendering

- [x] Meshes, materials, cameras, and a 3D renderer — the first lit pass; see [ADR-0010](adr/0010-3d-rendering-foundation.md)
- [x] A directional light
- [x] Frustum culling (world-AABB, conservative) — see [ADR-0011](adr/0011-frustum-culling.md)
- [x] Directional, point, and spot lights — see [ADR-0012](adr/0012-forward-lighting-model.md)
- [x] Shadow mapping (single-cascade directional) — see [ADR-0014](adr/0014-directional-shadow-mapping.md)
- [x] A spatial hierarchy (BVH) over static geometry — `SpatialBvh` in `core` builds a binned-SAH binary BVH over world-space AABBs and answers frustum and box queries, reusing `Frustum::intersects_aabb` as its leaf test, with the query checked against a brute-force reference ([ADR-0053](adr/0053-spatial-bvh.md)); `Renderer3D` now culls through a cached tree, rebuilt only when an instance's world bounds change, and the tree path is pinned to the flat reference `cull_flat` by a test that asserts an identical visible set and order ([ADR-0056](adr/0056-renderer3d-spatial-cull.md))
- [x] Environment rendering and a post-processing chain — a camera-driven full-screen gradient background, and an opt-in exposure/tonemap/vignette chain over an off-screen colour target; see [ADR-0057](adr/0057-environment-and-post-processing.md)
- [x] A glTF importer — `GltfLoader` decodes `.gltf` (JSON) and `.glb` (binary) into a `Mesh` per primitive, reading through `ResourceSource` so a mounted pack works unchanged; metallic-roughness materials resolve relative to the file; see [ADR-0040](adr/0040-gltf-importer.md)
- [~] **A render-graph seam** for passes and post-processing — `render::RenderGraph` makes pass order data: passes are declared by name with their dependencies, `order()` returns a stable run order (declaration-order ties, cycles refused with the passes named), and `run()` invokes each pass in a profiler scope named `pass/<name>`. `Renderer2D::render_frame` drives the world and UI passes through it. **Pass culling landed**: a pass declares the resources it reads and writes, a resource can be marked an output, and a pass whose declared writes are unread is dropped (a pass with no declared writes is never culled, so the presenting pass and every existing graph are unchanged) — see [ADR-0110](adr/0110-render-graph-pass-culling.md). See [ADR-0100](adr/0100-render-graph-seam.md) for the scheduler. Still open: the graph reasons about resource names but does not own, allocate, or alias render targets, and `Renderer3D`'s shadow/main/post sequence is not yet driven by it.
- [x] **Instanced 3D draws** — `Renderer3D` batches instances that share a mesh, both material textures, and the per-batch shading state into one instanced draw, mirroring the 2D path. The model and normal matrices travel in a per-instance vertex stream, so the shadow pass and the main pass draw the same batches from one upload; see [ADR-0101](adr/0101-instanced-3d-draws.md).

**Done when** a lit, shadowed 3D scene draws with culling, and the draw path is
extractable and measurable. *Met except render-target ownership / resource
tracking in the render graph.*

## M5 — Physics

Physics is the first gameplay system, and the place the component-and-query
boundary is proven: a hand-written core kept behind a seam so a third-party
solver can replace it later ([ADR-0031](adr/0031-physics3d-and-backend-seam.md)).

- [x] 2D physics: box and circle colliders, dynamic and static bodies, a sequential-impulse solver (gravity, restitution, friction), as a system (`Collider2D`, `RigidBody2D`, `Physics2DSystem`) — see [ADR-0009](adr/0009-physics2d-system.md)
- [x] Physics queries (raycast, overlap) and contact events — see [ADR-0013](adr/0013-physics2d-queries-events-angular.md)
- [x] 2D angular dynamics: angular velocity, torque, and shape-derived inertia, integrated by the solver — see [ADR-0013](adr/0013-physics2d-queries-events-angular.md)
- [x] 2D joints: distance and revolute constraints solved by the same sequential-impulse loop as contacts, with node-id body references that serialize — see [ADR-0021](adr/0021-physics2d-joints.md)
- [x] A uniform-grid (spatial hash) broad phase: candidate pairs are found once per step instead of once per iteration, so a step is O(candidates × iterations) rather than O(bodies² × iterations) — see [ADR-0023](adr/0023-physics2d-broadphase.md)
- [x] 2D island detection and body sleeping: bodies group into islands over the contact-and-joint graph and a settled island stops being solved — see [ADR-0026](adr/0026-physics2d-islands-sleeping.md). The graph itself was lifted into a shared `physics` module ([ADR-0111](adr/0111-shared-island-graph.md)) so both dimensions use one implementation.
- [~] 2D physics refinements: collider offsets and oriented-box colliders are done — a box collider carries the node's Z rotation and a local offset, and the narrow phase is a separating-axis test over the oriented shapes, so a rotated box is collided as its true shape rather than its bounding box — see [ADR-0037](adr/0037-physics2d-oriented-boxes-offsets.md). Continuous collision detection remains.
- [x] 3D physics (first cut): box and sphere colliders, dynamic and static bodies, a linear sequential-impulse solver (gravity, restitution, friction, damping), a uniform-grid broad phase, distance joints, scene queries, and contact events (`Collider3D`, `RigidBody3D`, `DistanceJoint3D`, `Physics3DSystem`) — see [ADR-0031](adr/0031-physics3d-and-backend-seam.md)
- [~] 3D physics refinements: angular dynamics are done — a body carries an angular velocity, torque, and a shape-derived (isotropic) inertia, the solver integrates the orientation as a quaternion and applies angular impulses from contacts and joints (`RigidBody3D` angular fields, `Quat` in `core/Math.h`) — see [ADR-0043](adr/0043-physics3d-angular-dynamics.md). Islands and sleeping are now done too, mirroring 2D on the shared graph: a settled 3D pile groups into an island and stops being solved, and `CharacterController3D` pins `can_sleep = false` on the body it drives every frame — see [ADR-0111](adr/0111-shared-island-graph.md). Oriented colliders, more joint types, and continuous collision detection remain, deferred behind the backend seam rather than chased to parity.
- [x] Integration with the scene and the editor: the play schedule runs `Physics3DSystem`, and the editor copies `Collider3D` / `RigidBody3D` into the play scene, so a 3D scene plays with physics through the same path a shipped game uses — see [ADR-0058](adr/0058-editor-physics3d-and-debug-draw.md)
- [x] Debug drawing: a `debugdraw` module builds collider wireframes for a 2D or a 3D scene (`DebugDrawList`) and a `DebugRenderer` draws them as a single line list over the world camera, built on a new RHI line primitive — see [ADR-0058](adr/0058-editor-physics3d-and-debug-draw.md)
- [x] Collision filtering and trigger colliders in both modules: a collider carries `layer` / `mask` bits and an `is_trigger` flag; a pair is considered only when both sides' masks agree, and a trigger reports its contacts but applies no impulse, so a body passes through it. The defaults are open, so a scene that sets nothing behaves exactly as before — see [ADR-0087](adr/0087-collision-filtering-and-triggers.md)

**Done when** a 2D and a 3D scene both simulate, report contacts, and answer
queries, and the behaviour is tested headlessly. *Met for the first cut; the
`[~]` refinements are deliberate cuts behind the backend seam, not gaps.*

## M6 — Animation and audio

- [x] 2D sprite-sheet animation (frame playback; the first system) — see the M2 prerequisite
- [x] Audio playback and mixing: `AudioDevice` with a real (miniaudio) and a null backend, `AudioClip` + `WavLoader`, `AudioSource`, and `AudioSystem` with a pure mixer — see [ADR-0016](adr/0016-audio-module.md)
- [~] Audio refinements: 3D positional attenuation and audio buses / mixing groups — see [ADR-0034](adr/0034-audio-attenuation-buses.md). The listener these depend on is now driven: an `AudioListenerSystem` in `runtime` copies the active camera's world position to the audio system, so a positional source is attenuated against the player's ear rather than the world origin, and the player and play-in-editor both adopt it ([ADR-0097](adr/0097-audio-listener-from-camera.md)). Stereo panning landed on top: a positional source is now panned across the output channels by the listener's orientation, so a source to the player's right is louder in the right ear, under a constant-power pan law ([ADR-0099](adr/0099-stereo-panning-from-listener-orientation.md)). Directivity and Doppler landed too: a spatial source carries a cone (inner/outer half-angles and an outer gain, with the axis taken from the node's forward `-Z`, so it is aimed by rotating the node), and a source with `doppler` set is pitch-shifted by its motion relative to the listener, its velocity derived from how far its node moved since the previous frame, against a settable speed of sound ([ADR-0112](adr/0112-audio-directivity-and-doppler.md)). Both are opt-in, so a scene authored before them mixes byte for byte. Panning is opt-in too — a 2D camera has no ear direction, so a 2D scene's mix is unchanged. A head-related filter for front/back cues remains.
- [x] Streaming / large-file playback and compressed codecs: mu-law, A-law, and IMA ADPCM decoding in `WavLoader`, and a pull-based `AudioStream` / `WavStream` a source opts into with `AudioSource::stream` — see [ADR-0060](adr/0060-compressed-audio-and-streaming.md)
- [x] Tilemaps — a `Tilemap` component holds a grid of tile indices over a tileset texture. Cell (0, 0) is the map's top-left and +Y goes down, the convention every tile editor uses; `kEmpty` draws nothing, so a sparse map is cheap. Extraction emits one `render::SpriteInstance` per non-empty cell into the *same* array the sprites fill, so a tile reuses the sprite draw path, the shared cull, and the `(layer, texture)` batcher — a 100×100 level is one component and one draw call per texture, not ten thousand nodes. The grid persists as the component's own compact JSON through the `grid` reflection property ([ADR-0062](adr/0062-tilemaps.md)). Tile collisions, mixed tile sizes, and a dedicated tile editor remain.
- [x] Animation state machines — a named-state graph over the sprite animator: states (a clip: first frame, frame count, fps, loop) and transitions (from, to, a require-finished gate, and trigger conditions) in an `AnimationStateMachine` component, advanced by `AnimationStateMachineSystem`, which drives the node's `SpriteAnimator` cell. Blend trees, continuous parameters, and skeletal states build on this shape and remain ([ADR-0059](adr/0059-animation-state-machines.md))
- [x] **Skeletal animation and skinning** — the `Mesh` is one indexed triangle list with a fixed position/normal/uv vertex, so skinning needed both a vertex format that carries joint indices and weights and a skeleton to drive it ([ADR-0040](adr/0040-gltf-importer.md)). The runtime landed first: `Skeleton` (a joint tree with a parent-first `order()` and inverse bind matrices), `AnimationClip` (vector and quaternion tracks with step/linear/cubic interpolation, a `LoopMode`, and `wrap_time`), `SkeletonPose` (a plain value type that computes world matrices and the `world * inverse_bind` skinning palette), `Animator` (playback plus a two-clip cross-fade and opt-in root motion), `SkeletonAnimationSystem` (advances every animator and publishes the palette in the frame step), `SkinnedMesh` + `skin_vertices` (CPU linear blend skinning, testable with no GPU), and `SkinnedMeshRenderer3D` with `extract_skinned_meshes`/`cull_skinned_meshes` at the extraction seam ([ADR-0104](adr/0104-skeletal-animation.md)). The asset half followed: `GltfLoader` reads `skins`, `JOINTS_0`/`WEIGHTS_0`, and `animations` into a bound `Skeleton` and `AnimationClip`s. The draw half closes it: `Renderer3D` binds the skinned vertex layout and the palette (a `mat4` array uniform, so a rig of any joint count stays inside the attribute limit), draws a character in its bind pose and its pose, and casts the shadow of that pose. A skinned mesh needs a fourth vertex component, so the joint indices travel as floats in `SkinnedVertex3D` to match the layout the device describes, which a `static_assert` and a CPU test pin together. Blend trees, additive layers, and skeletal state machines build on this shape and remain.

**Done when** a sprite animates, a character state machine drives it, and audio
plays, streams, and mixes. *Met: skeletal animation landed end to end — the rig,
the clip, the pose, the palette, the glTF import, and the skinned draw.*

## M7 — Scripting

Direction (decided, [ADR-0044](adr/0044-scripting-seam.md)): **C++ is the primary
language for now**, via native gameplay modules that build against the engine and
hot-reload, and the scripting layer is built as a **language-agnostic seam** so
another language can be added later without the engine core naming it. A
purpose-built engine scripting language (a GDScript-style option) is a *possible*
future, deliberately **not started** — it is a multi-year commitment and only
worth it if it is dramatically better for the engine's use case than embedding a
proven language, not merely different. The seam is the cheap, reversible
decision; the language is the expensive one, and it waits.

**Why this shape:** a language-agnostic seam means the engine never has to
un-learn a language choice. The cost of the seam is one interface; the cost of
the wrong language is a rewrite of everything that named it.

- [x] Choose and document the scripting strategy (ADR first): C++ native modules as the primary path, the seam's shape, and the criteria under which an embedded or custom language would be added ([ADR-0044](adr/0044-scripting-seam.md))
- [x] A language-agnostic scripting seam: the engine core talks to an interface (`Script`/`ScriptHost`/`ScriptRegistry`), never to a concrete language (`fe/script/`, driven by `ScriptSystem` through the ordinary schedule)
- [x] C++ native gameplay modules: compile against the engine, load, and hot-reload without restarting the editor — a module is a shared library that registers behaviours through a versioned entry point (`ScriptModuleApi.h`); `ScriptLibrary` opens it through the platform layer's `DynamicLibrary` seam and reloads it by destroying the live scripts first and rebinding them to the new code ([ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)). Automatic file-watching landed with editor integration ([ADR-0066](adr/0066-editor-driven-module-hot-reload.md))
- [x] Reflection-backed bindings, so a script's exposed properties flow through the existing reflection and serialization path ([ADR-0063](adr/0063-reflection-backed-script-properties.md))
- [x] Editor integration (attach, inspect, hot reload) — the editor owns a `ScriptCatalog` (a `ScriptHost` plus a `ScriptRegistry`) and the document persists its bindings through the serializer's `ScriptBinding` seam, so a scene saved from the editor comes back scripted; the inspector lists a node's behaviour and its reflected properties, and every reflected script field — scalar or resource reference — is an undoable `SetScriptProperty`/`SetScriptResource` operation ([ADR-0063](adr/0063-reflection-backed-script-properties.md)). File-watching hot reload has landed: `ScriptCatalog` owns a `ScriptLibrary`, loads modules named on the command line (`--module`), and polls them each frame, hot-reloading a rebuilt one and rebinding the nodes it scripted ([ADR-0066](adr/0066-editor-driven-module-hot-reload.md)). Debouncing a build has landed too: the poll reports a module only once its image's write time has held across a requested number of consecutive polls, and the frame loop asks for two ([ADR-0067](adr/0067-debouncing-a-module-build.md))
- [x] **Debugging support** — a script that misbehaves cannot be inspected: no breakpoints, no call stack, no watch. For C++ modules this means either a debugger integration story or an in-engine logging/assertion surface a game author can use without a debugger. The in-engine surface landed first ([ADR-0109](adr/0109-in-engine-debugging-surface.md)): the engine keeps its log in a bounded `core::LogBuffer` and records every contained script failure in `script::ScriptDiagnostics` (node, behaviour, message, frame), so a failure is inspectable after it is reported once, and the editor's console panel shows both. A debugger-integration story remains future work.
- [ ] *(Deferred)* An embedded language (Lua/Wren) or a custom engine language, only once the seam is proven and the criteria in the ADR are met.

**Done when** a game's rules live in a module the engine did not compile in, the
module hot-reloads, and its properties are editable and persisted. *Met, and the
first debugging surface is in; a debugger integration is open.*

## M8 — Ship anywhere (cross-platform targets)

The goal is that a game built in Fork Engine reaches every platform its players
use, not merely that the editor runs on several OSes. Plan and trade-offs: the
[strategy](STRATEGY.md) and [ADR-0018](adr/0018-multi-platform-shipping.md).

**Where this stands:** web is done end to end (the queue's Phase 1), desktop
install rules are done, and the packaging and export path is done. What remains
is per-OS artifacts and mobile.

- [x] Platform interface audit: the full OS contract (window/surface, input incl. touch and gamepad, filesystem, threads, time, dynamic libraries, lifecycle) is recorded concern by concern, with what is implemented now and what is deferred, and a **null/headless** window backend exists for CI and servers ([ADR-0041](adr/0041-platform-contract-and-null-backend.md)). The engine's frame loop and its systems run with no display and no GL context; the `fork_headless` example is the executable proof.
- [x] Headless rendering: a null RHI device so the render extraction, batching, and draw submission run with no context, selected by an explicit `Engine::Config::headless_render` opt-in ([ADR-0042](adr/0042-null-rhi-device.md)).
- [~] The gamepad and suspend/resume rows of the platform contract, each implemented behind the existing seam. Touch is done end to end: the web backend feeds `InputState` from DOM touch events, and gameplay reaches it through the action layer, so a phone plays the same code a keyboard does ([ADR-0075](adr/0075-gameplay-actions-input-seam.md)). The dynamic-library row landed with `platform::DynamicLibrary` ([ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)). Gamepad and suspend/resume remain.
- [x] Asset packaging: a versioned pack format (`PackFile`), a writer that turns a project's asset tree and startup scene into one file, and a `PackSource` the runtime mounts so the loaders and the scene reader read from a pack as transparently as from disk ([ADR-0030](adr/0030-asset-pack-format.md))
- [~] `fork_pack` tooling polish: compression and streaming a large pack. Reachability — packing only what the startup scene references — landed on `fork_pack` and `fork_export`: a scene's references are traced through reflection and only those paths are packed ([ADR-0065](adr/0065-pack-reachability-pruning.md)). Pruning is the default; `--no-prune` restores whole-tree packing. A packed asset's own references are followed too: a container's internal references (a glTF's external `.bin` and images) are expanded through a per-format reader the caller supplies ([ADR-0088](adr/0088-transitive-pack-references.md)). Compression and streaming remain.
- [x] The export path: `fork_player` mounts a pack and runs the game loop, and `fork_export` assembles a runnable build (the player, a fresh pack, and a manifest) from a project ([ADR-0064](adr/0064-export-runtime-and-player.md)). The runtime is generic and driven by the pack, so export has no compiler in it. The exported build is exercised end to end in CI.
- [x] Web target: Emscripten/WASM build with a WebGL2 backend (the cheapest proof that the abstraction holds). Verified by a headless-browser smoke test that runs the real frame loop. See the queue's Phase 1.
- [~] Per-desktop-target runtimes and app packaging: a prebuilt `fork_player` for each OS, cross-compilation, and platform app bundles (`.app`, MSI, AppImage) — the queue's Phase 3. A tag now produces per-OS release archives (a game archive and an installed SDK) plus `SHA256SUMS` ([ADR-0102](adr/0102-downloadable-release-builds.md)), and Linux gets a single-file AppImage a player can download, make executable, and run ([ADR-0103](adr/0103-linux-appimage.md)). The `.app` bundle, the Windows installer, and a prebuilt per-OS runtime for `fork_export --player` remain.
- [ ] Desktop parity: macOS and Windows backends brought to parity with Linux.
- [ ] Mobile: Android and iOS lifecycle, touch input, GLES/Vulkan/Metal backends, and app packaging — the queue's Phase 2.
- [ ] Additional graphics backends as targets require them (Vulkan/Metal/WebGPU)
- [ ] Consoles: architecture-ready; implementation only when SDK access permits

**Done when** the same game is exported and runs on desktop, web, and mobile from
one project. *Web and the export path are met; desktop artifacts and mobile are
open.*

## M9 — Performance (a product feature)

The engine holds a stable, low frame time on real scenes, and the team can prove
it and protect it. Plan and trade-offs: the [strategy](STRATEGY.md) and
[ADR-0017](adr/0017-performance-strategy.md).

**Where this stands:** measurement, extraction, the job system, parallel
extraction/culling, the frame arena, 2D and 3D instancing, and the render-graph
seam's scheduler are all done and guarded by CI. The remaining work is pass
culling and resource tracking in the graph.

- [x] Profiler: `Profiler` + `ProfileScope` in `core` ([ADR-0022](adr/0022-profiler-and-frame-timing.md)), recording named scopes and counters into a fixed frame ring
- [x] Profiler instrumentation and runtime integration ([ADR-0024](adr/0024-profiler-instrumentation.md)): `FE_PROFILE_SCOPE`/`FE_PROFILE_FUNCTION` macros with a free disabled build; `Application` brackets `frame`/`frame/update`/`frame/render` and `SceneSchedule` times each system as `system/<name>`; a by-value `FrameReport` (per-name totals, counters, summary/`to_string`) is the stats surface
- [x] Benchmark: a headless target reporting frame-time percentiles; CI records the numbers and fails on regression — `benchmarks/scene_bench.cpp` runs synthetic scenes through the real frame loop and reports p50/p95/p99; `benchmarks/check_regression.py` compares against the committed baseline and fails past a coarse tolerance, and a dedicated CI job runs it ([ADR-0033](adr/0033-performance-regression-gate.md))
- [x] Render extraction: split both renderers into "extract a flat instance list" and "issue draws" (the data-oriented seam) — a shared `render` module holds `render::SpriteInstance`/`MeshInstance` and the pure extract/cull steps; each renderer's `render()` is extract → cull → draw, and the draw step does no culling ([ADR-0052](adr/0052-render-extraction-seam.md))
- [x] Job system: a `jobs` module (thread pool, `parallel_for`, task graph); `FrameContext` exposes it to systems — `JobSystem` is a fixed `std::thread` pool with `parallel_for` and a dependency `TaskGraph`, and one API in two modes: a worker count of zero (the default) runs every job inline, so the module landed without changing any existing run and the serial-equivalence test compares the two. The submitting thread helps run the queue while it waits, so a nested call cannot deadlock. `FrameContext::jobs` is nullable like `resources`/`input`/`arena`, and `Application` owns the pool, sizes it from `set_worker_count`, and joins it when `run()` returns. The device stays single-threaded: a job extracts, culls, or computes, and only the frame thread submits. CI runs the pure-logic suite under ThreadSanitizer (`FE_ENABLE_TSAN`; [ADR-0068](adr/0068-job-system.md))
- [x] Parallel extraction and culling: chunk the instance array; output must be identical to the serial path — `render::extract_sprites`/`extract_tiles`/`extract_meshes` and `cull_sprites`/`cull_meshes` take an optional `JobSystem*`; each chunk appends to its own list (extraction) or fills a byte mask (culling) and the results are joined in order, so the output is byte-identical to serial. Two measured thresholds keep the fan-out off where it does not pay: extraction above 1024 nodes (break-even ~1200), culling above 16384 instances. `Renderer2D`/`Renderer3D` thread the pool through, `Application::render_frame` supplies its own, and every other caller keeps the serial path unchanged ([ADR-0069](adr/0069-parallel-extraction-and-culling.md))
- [x] Memory: a per-frame arena for transient data, with allocation counters — the `core` primitive (`FrameArena`: bump allocate / `reset` / `mark`+`restore` / `create<T>`) with allocation counters and a test that asserts the steady-state allocate/reset loop performs no heap allocation ([ADR-0051](adr/0051-frame-arena.md)); `Application` owns one arena, resets it once per frame before the systems run, and hands it to every system through `FrameContext::arena` (nullable, like `resources`/`input`), with headless tests pinning the per-frame reset and the disabled path ([ADR-0054](adr/0054-frame-loop-arena.md)). The extraction seam is arena-backed as well: `extract_sprites_into`/`extract_tiles_into`/`cull_sprites_in_place` write into a caller-sized span, take their parallel scratch from the arena, and `Application::render_frame` runs the 2D pass with no heap allocation, falling back to the heap path on overflow ([ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md))
- [x] Parallel physics broad phase: the uniform-grid insert is chunked across the job pool (count-then-write, so the entry buffer and the resulting pair set are byte-identical to the serial build), above a measured threshold ([ADR-0070](adr/0070-instancing-arena-seam-and-parallel-broadphase.md))
- [x] GPU efficiency: instancing — `Renderer2D` uploads the unit-square corners once and a per-instance transform/UV/colour record per sprite, and issues one indexed instanced draw per texture batch, with the per-vertex UV convention preserved so the image is unchanged. Material batching is part of the batch key already.
- [x] An in-editor profiler panel fed by the profiler data — `ProfilerPanel` + `ProfilerPanelViewModel` in `editor`; the editor owns an optional `core::Profiler`, brackets its own frame with it, and the panel draws the latest `FrameReport` ([ADR-0055](adr/0055-editor-profiler-panel.md))
- [~] **A render-graph seam** for passes and post-processing — the scheduler landed with M4: `render::RenderGraph` orders passes by declared dependency and profiles each as `pass/<name>` ([ADR-0100](adr/0100-render-graph-seam.md)), and pass culling followed: a pass declares the resources it reads and writes, a resource can be marked an output, and a pass whose declared writes nobody reads is dropped, iteratively, with a pass that declares no writes never culled ([ADR-0110](adr/0110-render-graph-pass-culling.md)). Render-target ownership remains; it needs the graph to own and alias targets, not just name them.
- [x] **3D instancing** — `Renderer3D` batches instances that share a mesh, both material textures, and the per-batch shading state into one instanced draw, mirroring the 2D path. The per-instance model and normal matrices travel in a per-instance vertex stream (`VertexLayout::mesh_instance()`), so the transform is no longer a uniform and one call covers every instance of a mesh; the shadow pass draws the same batches from the same upload ([ADR-0101](adr/0101-instanced-3d-draws.md)).

**Done when** a real scene holds its frame budget, the benchmark guards it, and
the sanitizers are green. *Met for 2D; render-graph render-target ownership
remains open.*

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
pursued further. See [ADR-0019](adr/0019-collaborative-editing.md) (Deprecated).

**Console targets.** Architecture-ready; no code lands without SDK access.

**A custom engine scripting language.** Deliberately not started; the criteria
under which it would be reconsidered are in
[ADR-0044](adr/0044-scripting-seam.md).
