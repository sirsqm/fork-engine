# Fork Engine Plan

This is the project's plan in one document. It was three — a strategy, a
roadmap, and a shipping queue — and reading them apart meant the same reasoning
was stated twice and drifted. They are three views of one plan:

- **The pillars** (below) are the *why*: three capabilities that shape how every
  subsystem is designed. A subsystem that fights a pillar is wrong even if it
  works.
- **The milestones** (M1–M9) are the *sequence*: what each capability contains,
  what it depends on, where it stands, and how you know it is done.
- **The shipping queue** (Phases 1–4) is the *front line*: the concrete work, in
  order, that moves the engine toward a shipped game.

It is a plan, not a promise. Milestones may be reordered as the design becomes
clearer, and a change to a decision is recorded in an ADR.

## How to read this document

Each milestone is a coherent capability, not a feature list, and each is
independently testable: it must build and pass its tests before the next one
starts. The status of an item is one of:

- `[x]` — done and tested. The code is in the tree.
- `[~]` — partly done. The done part is named, and what remains is named with it.
- `[ ]` — not started.

A milestone's **Done when** line, and a queue item's **Done when** line, is the
acceptance test. It is written so a reader can check it without trusting the
prose: it names a test or an artifact. Where a milestone is large, the work is
broken into steps that each land separately.

**The queue is the automation's input.** It takes the first item whose `slug`
does not appear in an `fe-task:` commit subject and does exactly that; when the
queue is empty it makes no changes and exits. An item is finished when its slug
appears in an `fe-task:` subject — state is derived from history, never stored in
this file, so there is no "done" marker that can drift from the tree.

How an item is worked:

- The commit subject is `fe-task: <slug> - <short summary>`.
- "Done when" is the acceptance test. Satisfy it literally.
- Add or update tests. A change with no test is not finished.
- Build and test clean with `-DFE_WARNINGS_AS_ERRORS=ON` before committing.
- If an item is too large for one run, land a coherent tested first step and end
  the commit body with a `Remaining:` line naming what is left.
- Do not reorder or skip items. Take the first unfinished one.

## The guiding rule

**A few genuinely working systems beat a long list of half-finished features.**
This is why M2.5 (a vertical slice) sits before the breadth milestones, why the
queue is a single ordered line rather than parallel tracks, and why the
near-term order below finishes the slice before resuming the pillars. A
milestone that cannot be demonstrated end to end is not finished, no matter how
many of its items are checked.

---

## The three pillars

Each pillar below states its goal, the principles that follow, what it forces on
the architecture, how the work is verified, and the risks with their
mitigations. Where a pillar has landed work, the milestone that sequences it
(and its per-item status) is named rather than repeated. The decisions with real
trade-offs are recorded as ADRs: [0017](adr/0017-performance-strategy.md),
[0018](adr/0018-multi-platform-shipping.md), and the operation-model records
[0045](adr/0045-component-identity.md) /
[0047](adr/0047-local-operation-model.md) /
[0048](adr/0048-replicated-operation-log.md).

**The pillars are ordered by risk.** Performance is the lowest-risk and improves
everything else; shipping is medium; the shareable-edit model was the
highest-risk because it reaches into the editor's core, which is why only its
local, headless first steps were taken and the network was left out (see
[Not planned](#not-planned)).

### Pillar 1 — Performance is a product feature

**Goal.** The engine holds a stable, low frame time on real scenes, and the team
can *prove* it and *protect* it. A performance claim without a number attached is
not a claim.

**Principles.**

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

**What it implies.** A frame profiler (named scopes into a fixed ring, plus the
engine's counters) feeding the editor panel; a render-extraction seam that emits
a flat instance array separately from issuing draws; a job system
(`parallel_for` and a task graph) used by systems and extraction, never by the
RHI; a per-frame arena for transient data; and a benchmark harness in CI that
fails on regression.

**Where it stands.** Sequenced as **[M9](#m9--performance-a-product-feature)**,
which carries the per-item status. Nothing is restated here.

**Verification.** The benchmark is the gate, not a dev tool: CI records the
numbers and fails on a large regression. The sanitizer jobs (ASan/UBSan, and TSan
for the pure-logic suite) are the second gate — a performance change that
introduces a race is a defect, not a win.

**Risks.**

- **Premature optimization.** Mitigated by the measure-first rule: nothing is
  parallelised or cached until the profiler can show it matters.
- **Parallelism bugs (data races).** Mitigated by keeping the device
  single-threaded, testing parallel paths against serial ones, and running the
  suite under ThreadSanitizer.
- **Benchmark noise.** Mitigated by coarse tolerances, a baseline recorded on the
  CI runner rather than a dev machine, and comparing against the gate job's own
  artifact (see `benchmarks/README.md`).

### Pillar 2 — Games ship anywhere

**Goal.** A developer can choose a target platform and get a runnable build of
their game without hand-editing the engine. Desktop first, then web and mobile,
then consoles where the platform holders allow it. This is not "the editor runs
on every OS"; it is "**games made here reach every platform their players use**".

**Near-term scope, stated plainly.** The target *today* is desktop: Linux and
Windows through the OpenGL 3.3 core backend. macOS is buildable but not yet
supported (GL 3.3 is deprecated there), and web, mobile, and consoles are
planned, not promised. The README and the status table state this plainly, and
[ADR-0032](adr/0032-near-term-platform-scope.md) records why the claim is
sequenced rather than asserted. A portability claim the engine cannot
demonstrate costs more than it buys.

**Principles.**

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

**What it implies.** A complete platform layer with everything OS-specific behind
it; a versioned asset-pack format, a writer, and a source the loaders read from
as transparently as from disk; export templates (a prebuilt runtime per target
plus a build step that turns a project into a runnable game, with no compiler in
the runtime); and renderer backends as targets demand them.

**Where it stands.** Sequenced as **[M8](#m8--ship-anywhere-cross-platform-targets)**,
which carries the per-item status. Nothing is restated here.

**Verification.** Every target is proven by running the *same* game: the breakout
slice is the portability test, and the web smoke test (headless Chromium) is the
acceptance check for the web target. A backend that compiles but has never run a
frame is not done.

**Risks.**

- **Scope explosion.** Mitigated by proving portability with the cheapest target
  (web) before mobile, and by only adding a backend when a target needs it.
- **Platform-specific bugs hidden in shared code.** Mitigated by the platform
  audit, the null backend, and CI across the three desktop platforms.
- **A portability claim outrunning the code.** Mitigated by the near-term scope
  above and [ADR-0032](adr/0032-near-term-platform-scope.md).

### Pillar 3 — Edits are durable and shareable

**Goal.** An edit is a discrete, serializable operation over a stably identified
object, not a direct mutation. Undo, redo, and replay all fall out of the same
model, which is what makes a local edit trustworthy.

**Principles.**

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

**What it implies.** Stable component identity, an operation model, and an
ordered log every replica applies in the same order; and coalescing, so a
continuous gesture (a drag) becomes *one* undo step, which requires a stable
coalescing key and an inverse that survives merging.

**What landed** (this pillar has no milestone of its own; it lands in the editor,
so its status is recorded here):

- Stable component identity ([ADR-0045](adr/0045-component-identity.md)).
- The serializable operation model
  ([ADR-0047](adr/0047-local-operation-model.md)).
- The ordered log ([ADR-0048](adr/0048-replicated-operation-log.md)).
- Transform editing as the first non-component operation kind, which established
  the shape for the next one
  ([ADR-0089](adr/0089-editor-transform-editing.md)).

They are shipped and tested, and they make undo/redo more robust on their own.
The network half — transport, presence, and an authoritative session — is
[not planned](#not-planned).

### How the pillars fit the milestones

- **Performance** adds a measurement milestone and a data-oriented extraction
  seam to both renderers, then a job system — sequenced as **M9**. It is the
  lowest-risk pillar and improves everything else, so it runs in parallel with
  feature work rather than after it.
- **Ship anywhere** makes the platform layer and asset packaging first-class,
  then export templates, then web and mobile — sequenced as **M8**, expanded from
  a single "cross-platform" line into concrete targets.
- **Edits are durable and shareable** lands after the editor is mature, as the
  local steps that also strengthen undo. Its network half is not planned.

The pillars are the *direction*, not the next step. The next step is the
**vertical slice** — one small game built end to end (M2.5) — and then making
that slice a game a stranger can play (the queue's Phase 4). Nothing in the
pillars is worth building before the engine has proven, in one finished artifact,
that the parts work together; the near-term order below is written in that
spirit.

---

## Milestones

### Milestone map

| # | Milestone | Depends on | Status |
| --- | --- | --- | --- |
| M1 | Foundation | — | done |
| M2 | Editor foundation | M1 | mostly done (export moved to M8) |
| M2.5 | A vertical slice | M2 | in progress |
| M3 | Reflection and a real asset pipeline | M2 | done |
| M4 | 3D rendering | M1, M3 | mostly done (skeletal animation in M6) |
| M5 | Physics | M1, M3 | mostly done (3D parity open) |
| M6 | Animation and audio | M4 | mostly done (skeletal open) |
| M7 | Scripting | M2, M5 | core done (debugging open) |
| M8 | Ship anywhere | M2.5, M3 | partly done (web done, mobile open) |
| M9 | Performance | M1 | mostly done (render graph open) |

The dependencies are real: M4 needs the asset pipeline from M3 to load a mesh,
and M7 needs a scene with gameplay systems (M5) before a script has anything to
drive.

### Near-term priorities

The order the next work is actually taken in, chosen so that a small number of
things get *finished* rather than many get started. This is the front line;
the milestones below hold the detail.

1. **Make the slice a game worth shipping** — the breakout slice becomes a game
   a stranger can play: a title, a score, lives, levels, persistence, and
   settings (M2.5 and the [shipping queue](#the-shipping-queue)'s Phase 4).
   Everything else is secondary to having one thing that is genuinely finished.
2. **Honest scope** — the "ship anywhere" claim is scoped to desktop for now, in
   [ADR-0032](adr/0032-near-term-platform-scope.md), so the README does not
   promise more than the engine delivers.
3. **Scripting depth** — the language-agnostic seam and C++ native modules are
   in place (M7); what remains is debugging support, which is what a game author
   needs the first time a script misbehaves.
4. **Guard the performance claims** — the benchmark as a CI regression gate
   (M9) and sanitizer builds (ASan/UBSan, TSan), while the codebase is still
   small enough to keep them cheaply green.
5. **Editor and rendering depth** — a 3D authoring view (M2) and the render
   graph (M4, M9), which together unblock skeletal animation and 3D instancing.
6. **More platforms** — desktop artifacts and mobile (M8), after the above.

The intent: finish the small things. The engines that last are the ones whose
authors ruthlessly complete them.

---

### M1 — Foundation (done)

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

### M2 — Editor foundation

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

### M2.5 — A vertical slice: one small game, end to end

**In progress.** The first complete game (`examples/breakout`) ships and is the
integration test. What remains is not engine work but game work: making the slice
a game a stranger can play (the [shipping queue](#the-shipping-queue)'s Phase 4).

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
is missing than ten plan items, and it is the precondition for a community.

### M3 — Reflection and a real asset pipeline

A game is mostly content. Without an asset pipeline that survives a rename, a
re-import, and a rebuild, every later milestone is slower to build.

- [x] Property inspector integration with editor hints (Range, Color, Resource) — typed hints on `Property` (`RangeHint`/`ColorHint`/`ResourceHint`) drive a bounded slider, a colour picker, and an asset drop target; resource edits are undoable via `SetResourceCommand` ([ADR-0028](adr/0028-editor-hint-metadata.md))
- [x] GUID-based resource references that survive file moves — a per-asset GUID sidecar and an asset database; see [ADR-0020](adr/0020-resource-identity-guid-sidecar.md)
- [x] Import settings per asset type — typed, stored in the sidecar, applied at load ([ADR-0027](adr/0027-asset-import-settings.md))
- [x] An import cache — a content-addressed store so a re-import and a rebuild are cheap ([ADR-0029](adr/0029-import-cache.md))
- [x] Hot reload of resources — the manager records each cached resource's file and write time, `poll_changed()` reports the ones that changed, and `reload()` re-loads a named one and returns the fresh instance for the caller to adopt ([ADR-0046](adr/0046-resource-change-detection-and-reload.md))

**Done when** an asset can be imported, referenced by GUID, re-imported cheaply,
and hot-reloaded without restarting the editor. *Met.*

### M4 — 3D rendering

- [x] Meshes, materials, cameras, and a 3D renderer — the first lit pass; see [ADR-0010](adr/0010-3d-rendering-foundation.md)
- [x] A directional light
- [x] Frustum culling (world-AABB, conservative) — see [ADR-0011](adr/0011-frustum-culling.md)
- [x] Directional, point, and spot lights — see [ADR-0012](adr/0012-forward-lighting-model.md)
- [x] Shadow mapping (single-cascade directional) — see [ADR-0014](adr/0014-directional-shadow-mapping.md)
- [x] A spatial hierarchy (BVH) over static geometry — `SpatialBvh` in `core` builds a binned-SAH binary BVH over world-space AABBs and answers frustum and box queries, reusing `Frustum::intersects_aabb` as its leaf test, with the query checked against a brute-force reference ([ADR-0053](adr/0053-spatial-bvh.md)); `Renderer3D` now culls through a cached tree, rebuilt only when an instance's world bounds change, and the tree path is pinned to the flat reference `cull_flat` by a test that asserts an identical visible set and order ([ADR-0056](adr/0056-renderer3d-spatial-cull.md))
- [x] Environment rendering and a post-processing chain — a camera-driven full-screen gradient background, and an opt-in exposure/tonemap/vignette chain over an off-screen colour target; see [ADR-0057](adr/0057-environment-and-post-processing.md)
- [x] A glTF importer — `GltfLoader` decodes `.gltf` (JSON) and `.glb` (binary) into a `Mesh` per primitive, reading through `ResourceSource` so a mounted pack works unchanged; metallic-roughness materials resolve relative to the file; see [ADR-0040](adr/0040-gltf-importer.md)
- [ ] **A render-graph seam** for passes and post-processing — today the 3D pass order is fixed in code. The seam is what lets passes be reordered, culled, and profiled as a unit, and it is shared with M9's remaining work.
- [ ] **Instanced 3D draws** — `Renderer2D` instances; `Renderer3D` still draws per mesh. Needed before a scene with many repeated meshes holds a frame budget.

**Done when** a lit, shadowed 3D scene draws with culling, and the draw path is
extractable and measurable. *Met except the render graph and 3D instancing.*

### M5 — Physics

Physics is the first gameplay system, and the place the component-and-query
boundary is proven: a hand-written core kept behind a seam so a third-party
solver can replace it later ([ADR-0031](adr/0031-physics3d-and-backend-seam.md)).

- [x] 2D physics: box and circle colliders, dynamic and static bodies, a sequential-impulse solver (gravity, restitution, friction), as a system (`Collider2D`, `RigidBody2D`, `Physics2DSystem`) — see [ADR-0009](adr/0009-physics2d-system.md)
- [x] Physics queries (raycast, overlap) and contact events — see [ADR-0013](adr/0013-physics2d-queries-events-angular.md)
- [x] 2D angular dynamics: angular velocity, torque, and shape-derived inertia, integrated by the solver — see [ADR-0013](adr/0013-physics2d-queries-events-angular.md)
- [x] 2D joints: distance and revolute constraints solved by the same sequential-impulse loop as contacts, with node-id body references that serialize — see [ADR-0021](adr/0021-physics2d-joints.md)
- [x] A uniform-grid (spatial hash) broad phase: candidate pairs are found once per step instead of once per iteration, so a step is O(candidates × iterations) rather than O(bodies² × iterations) — see [ADR-0023](adr/0023-physics2d-broadphase.md)
- [~] 2D island detection and body sleeping: bodies group into islands over the contact-and-joint graph and a settled island stops being solved — see [ADR-0026](adr/0026-physics2d-islands-sleeping.md)
- [~] 2D physics refinements: collider offsets and oriented-box colliders are done — a box collider carries the node's Z rotation and a local offset, and the narrow phase is a separating-axis test over the oriented shapes, so a rotated box is collided as its true shape rather than its bounding box — see [ADR-0037](adr/0037-physics2d-oriented-boxes-offsets.md). Continuous collision detection remains.
- [x] 3D physics (first cut): box and sphere colliders, dynamic and static bodies, a linear sequential-impulse solver (gravity, restitution, friction, damping), a uniform-grid broad phase, distance joints, scene queries, and contact events (`Collider3D`, `RigidBody3D`, `DistanceJoint3D`, `Physics3DSystem`) — see [ADR-0031](adr/0031-physics3d-and-backend-seam.md)
- [~] 3D physics refinements: angular dynamics are done — a body carries an angular velocity, torque, and a shape-derived (isotropic) inertia, the solver integrates the orientation as a quaternion and applies angular impulses from contacts and joints (`RigidBody3D` angular fields, `Quat` in `core/Math.h`) — see [ADR-0043](adr/0043-physics3d-angular-dynamics.md). Oriented colliders, more joint types, and islands / sleeping remain, mirroring the 2D cuts; deferred behind the backend seam rather than chased to parity.
- [x] Integration with the scene and the editor: the play schedule runs `Physics3DSystem`, and the editor copies `Collider3D` / `RigidBody3D` into the play scene, so a 3D scene plays with physics through the same path a shipped game uses — see [ADR-0058](adr/0058-editor-physics3d-and-debug-draw.md)
- [x] Debug drawing: a `debugdraw` module builds collider wireframes for a 2D or a 3D scene (`DebugDrawList`) and a `DebugRenderer` draws them as a single line list over the world camera, built on a new RHI line primitive — see [ADR-0058](adr/0058-editor-physics3d-and-debug-draw.md)
- [x] Collision filtering and trigger colliders in both modules: a collider carries `layer` / `mask` bits and an `is_trigger` flag; a pair is considered only when both sides' masks agree, and a trigger reports its contacts but applies no impulse, so a body passes through it. The defaults are open, so a scene that sets nothing behaves exactly as before — see [ADR-0087](adr/0087-collision-filtering-and-triggers.md)

**Done when** a 2D and a 3D scene both simulate, report contacts, and answer
queries, and the behaviour is tested headlessly. *Met for the first cut; the
`[~]` refinements are deliberate cuts behind the backend seam, not gaps.*

### M6 — Animation and audio

- [x] 2D sprite-sheet animation (frame playback; the first system) — see the M2 prerequisite
- [x] Audio playback and mixing: `AudioDevice` with a real (miniaudio) and a null backend, `AudioClip` + `WavLoader`, `AudioSource`, and `AudioSystem` with a pure mixer — see [ADR-0016](adr/0016-audio-module.md)
- [~] Audio refinements: 3D positional attenuation and audio buses / mixing groups — see [ADR-0034](adr/0034-audio-attenuation-buses.md). The listener these depend on is now driven: an `AudioListenerSystem` in `runtime` copies the active camera's world position to the audio system, so a positional source is attenuated against the player's ear rather than the world origin, and the player and play-in-editor both adopt it ([ADR-0097](adr/0097-audio-listener-from-camera.md))
- [x] Streaming / large-file playback and compressed codecs: mu-law, A-law, and IMA ADPCM decoding in `WavLoader`, and a pull-based `AudioStream` / `WavStream` a source opts into with `AudioSource::stream` — see [ADR-0060](adr/0060-compressed-audio-and-streaming.md)
- [x] Tilemaps — a `Tilemap` component holds a grid of tile indices over a tileset texture. Cell (0, 0) is the map's top-left and +Y goes down, the convention every tile editor uses; `kEmpty` draws nothing, so a sparse map is cheap. Extraction emits one `render::SpriteInstance` per non-empty cell into the *same* array the sprites fill, so a tile reuses the sprite draw path, the shared cull, and the `(layer, texture)` batcher — a 100×100 level is one component and one draw call per texture, not ten thousand nodes. The grid persists as the component's own compact JSON through the `grid` reflection property ([ADR-0062](adr/0062-tilemaps.md)). Tile collisions, mixed tile sizes, and a dedicated tile editor remain.
- [x] Animation state machines — a named-state graph over the sprite animator: states (a clip: first frame, frame count, fps, loop) and transitions (from, to, a require-finished gate, and trigger conditions) in an `AnimationStateMachine` component, advanced by `AnimationStateMachineSystem`, which drives the node's `SpriteAnimator` cell. Blend trees, continuous parameters, and skeletal states build on this shape and remain ([ADR-0059](adr/0059-animation-state-machines.md))
- [ ] **Skeletal animation and skinning** — the one open item in this milestone and the last thing a 3D game with animated characters needs. The `Mesh` is one indexed triangle list with a fixed position/normal/uv vertex, so skinning needs both a vertex format that carries joint indices and weights and a skeleton to drive it; `GltfLoader` deliberately reads neither today ([ADR-0040](adr/0040-gltf-importer.md)). Sequenced after the render graph (M4) because skinning is a vertex-shader concern the render graph should own.

**Done when** a sprite animates, a character state machine drives it, and audio
plays, streams, and mixes. *Met except skeletal animation.*

### M7 — Scripting

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
- [ ] **Debugging support** — a script that misbehaves cannot be inspected: no breakpoints, no call stack, no watch. For C++ modules this means either a debugger integration story or an in-engine logging/assertion surface a game author can use without a debugger.
- [ ] *(Deferred)* An embedded language (Lua/Wren) or a custom engine language, only once the seam is proven and the criteria in the ADR are met.

**Done when** a game's rules live in a module the engine did not compile in, the
module hot-reloads, and its properties are editable and persisted. *Met; debugging
is open.*

### M8 — Ship anywhere (cross-platform targets)

The goal and trade-offs are [Pillar 2](#pillar-2--games-ship-anywhere); this
milestone sequences them. Plan and ADR:
[ADR-0018](adr/0018-multi-platform-shipping.md).

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
- [ ] Per-desktop-target runtimes and app packaging: a prebuilt `fork_player` for each OS, cross-compilation, and platform app bundles (`.app`, MSI, AppImage) — the queue's Phase 3.
- [ ] Desktop parity: macOS and Windows backends brought to parity with Linux.
- [ ] Mobile: Android and iOS lifecycle, touch input, GLES/Vulkan/Metal backends, and app packaging — the queue's Phase 2.
- [ ] Additional graphics backends as targets require them (Vulkan/Metal/WebGPU)
- [ ] Consoles: architecture-ready; implementation only when SDK access permits

**Done when** the same game is exported and runs on desktop, web, and mobile from
one project. *Web and the export path are met; desktop artifacts and mobile are
open.*

### M9 — Performance (a product feature)

The goal and trade-offs are [Pillar 1](#pillar-1--performance-is-a-product-feature);
this milestone sequences them. Plan and ADR:
[ADR-0017](adr/0017-performance-strategy.md).

**Where this stands:** measurement, extraction, the job system, parallel
extraction/culling, the frame arena, and 2D instancing are all done and guarded
by CI. The remaining work is the render graph and 3D instancing.

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
- [ ] **A render-graph seam** for passes and post-processing — the last open item here and the one M4 also needs. Passes are currently ordered in code; the seam lets them be reordered, culled, and profiled as a unit.
- [ ] **3D instancing** — `Renderer3D` still issues per-mesh draws, so a scene with repeated meshes pays a draw call each. The 2D path already instances; this is the 3D half.

**Done when** a real scene holds its frame budget, the benchmark guards it, and
the sanitizers are green. *Met for 2D; the render graph and 3D instancing are
open.*

### Not planned

**Networked collaborative editing.** Several people editing one project live over
a shared server was once planned as M10. It is not planned any more: it needs a
hosted authoritative service, and that running cost is not one the project will
carry. The local steps that landed on the way stay — they are listed under
[Pillar 3](#pillar-3--edits-are-durable-and-shareable), which records their
status — and the transport and session-server work that only existed to serve the
network is likewise not pursued further. See
[ADR-0019](adr/0019-collaborative-editing.md) (Deprecated).

**Console targets.** Architecture-ready; no code lands without SDK access.

**A custom engine scripting language.** Deliberately not started; the criteria
under which it would be reconsidered are in
[ADR-0044](adr/0044-scripting-seam.md).


---

## The shipping queue

The ordered backlog the daily automation works through, one item per run.

### Phases at a glance

| Phase | Ship bar |
| --- | --- |
| 1 — a 2D game renders in a browser | A game runs in a headless Chromium and is verified to draw |
| 2 — the same game on a phone | The same game accepts touch and runs on a mobile browser |
| 3 — a downloadable desktop build | A per-OS artifact a player can download and run |
| 4 — the game is worth shipping | The slice is a real game, not a demo |

Each phase's bar is deliberately a *runnable* artifact, not a merge. A phase that
ends with code that compiles but has never run on its target is not finished.

---

### Phase 1 — a 2D game renders in a browser

The bar: `fork_export --target web` produces a directory, and a headless
browser loads it, runs the frame loop, and draws a non-blank frame.

#### `glsl-dialect-seam`

Translate engine shader sources to a GLSL ES dialect behind the RHI, so WebGL2
can compile them, with desktop translation the identity.

**Done when** a unit test proves desktop passes through byte for byte and the ES
translation rewrites the version and adds a fragment precision exactly once.

#### `shader-dialect-wiring`

Select the dialect where the device is created, not per shader.

**Done when** the graphics tests pass with the desktop dialect and a test proves
the dialect is applied at creation.

#### `emscripten-toolchain`

An Emscripten CMake toolchain file that leaves the desktop build untouched.

**Done when** a documented, reproducible configure command exists and CI can run
it.

#### `web-window-backend`

A canvas-backed window backend with a WebGL2 context and no GLFW.

**Done when** the web target builds and a headless smoke test constructs the
window.

#### `frame-loop-stepping`

Expose `initialize()` / `step_frame()` / `shutdown()` so a browser's
`requestAnimationFrame` can drive the engine without blocking.

**Done when** a test drives N frames via `step_frame()` and observes the same
per-frame invariants as `run()`.

#### `pack-fetch-source`

A `ResourceSource` that reads the pack over HTTP, because a browser has no
filesystem.

**Done when** a test serves a pack from an in-process HTTP server and reads an
asset through the fetch source, matching the disk source's bytes.

#### `web-export-template`

`fork_export --target web` writes the wasm runtime, the pack, a manifest, and an
HTML/JS shell.

**Done when** the export produces that directory and the shell's expected file
names match what the export writes.

#### `web-smoke-test-ci`

A CI job that configures and builds the web target so the web path cannot rot.

**Done when** the job exists, is green on a push, and runs on every change.

#### `headless-browser-verification`

Load the exported build in headless Chromium and assert the game renders.

**Done when** the check runs in CI against a real export and fails when the
renderer is deliberately broken.

#### `3d-in-the-browser`

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

Both hold. The check runs the 2D demo, the 3D game, a 3D touch run and a
phone-sized 2D run, and asserts a live canvas, a live loop, a non-blank frame
and a pruned export that carries its mesh and texture. The "no GL error" half
was the last to be checked: a non-blank frame is blind to a *rejected* draw, and
the two defects this item fixed — a depth-texture format GLSL ES 3.00 rejects,
and a colour-attached depth pass — both left a non-blank frame behind. The
device now has a `drain_errors()` seam (the null backend reports none), the
player reports the count as `glerr=N` and logs each error, and the web game
check fails on a non-zero count ([ADR-0095](adr/0095-web-gl-error-verification.md)).
All four runs report `glerr=0`.

#### `web-gameplay-logic`

A 3D scene rendered in a browser but nothing responded to input: gameplay logic
lived only in C++ modules, and a browser has no dynamic loader, so a web export
had no logic to consume the actions the input path was already delivering.
Compile a small, useful set of behaviours into the engine and register them
in-process, so every target — including wasm — has gameplay to run.

**Done when** a web build runs a scene whose bound behaviour reads the movement
actions and moves its node, verified by the browser input probe. See
[ADR-0078](adr/0078-engine-shipped-gameplay-behaviours.md).

#### `playable-3d-web-game`

A 3D test game that is playable in a browser, not merely rendered: a fly camera,
a movable player, and something to look at, driven by the engine-shipped
behaviours with a keyboard and a touchscreen.

**Done when** the project exports for the web, runs in headless Chromium, and the
input probe observes the player node move in response to a synthetic key.

#### `fixed-simulation-step`

The browser input probe measured wall-clock distance, so the same run reported
different values on a loaded runner and the check was flaky by construction. Let
the engine pin a fixed simulation step so a run is reproducible.

**Done when** a test drives the frame loop with a fixed step and observes the
same delta every frame, and the web input probe reports the same distance across
repeated runs. See [ADR-0079](adr/0079-fixed-simulation-step.md).

#### `web-renderer3d-frame-allocations`

The 3D renderer rebuilt its per-frame working set — the extracted instances, the
mesh-liveness list, and the light lists — on the heap every frame, and swept
cached mesh buffers with a scan that was quadratic in the number of on-screen
meshes. Reuse the buffers across frames and make the sweep linear, so a 3D game's
frame time is not dominated by allocator noise.

**Done when** a steady-state frame reuses capacity rather than allocating and the
mesh-buffer sweep visits each cached mesh once.

#### `web-window-buffer-size-cache`

Every `framebuffer_width()`/`framebuffer_height()` call crossed into JavaScript —
`emscripten_webgl_get_drawing_buffer_size` and `emscripten_get_canvas_element_size`
are not free — and the frame loop asks for the size several times a frame. The
buffer only changes when the page resizes the canvas, which is exactly when the
resize callback already runs, so cache it there.

**Done when** a steady-state frame reads the drawing-buffer size from the cache
rather than querying the browser, and the resize path keeps the cache honest.

#### `web-resize-and-dpr`

A browser canvas is resized by the page, not by the game, and a phone reports a
device pixel ratio above 1. Make the engine follow the canvas size and the DPR.

**Done when** a test changes the canvas size and observes the renderer's
viewport and the input coordinate space both follow.

#### `web-audio-context`

A browser will not start an audio context until a user gesture. Make the web
audio device resume on the first gesture and stay silent rather than crash
before one.

**Done when** a headless test proves the device tolerates a context that starts
suspended and plays after a resume.

#### `web-visibility-pause`

A backgrounded tab stops receiving animation frames, so the engine's clock
jumps on return. Clamp the frame delta when a page becomes visible again.

**Done when** a test feeds a large time step after a visibility change and
observes the simulation advance by a bounded amount.

#### `web-build-in-ci-artifacts`

The web build is compiled in CI but thrown away. Publish the exported game as a
CI artifact so a run can hand a reviewer a link.

**Done when** the CI job uploads the export directory and the artifact downloads
and runs.

---

### Phase 2 — the same game on a phone

The bar: the breakout slice accepts touch and runs in a mobile browser, then
builds for Android.

#### `webgl2-mobile-parity`

GLES3 on Android and iOS is close to WebGL2 but not identical. Confirm the ES
dialect and the RHI work under a GLES3 context and fix any divergence.

**Done when** the ES dialect sources compile under a GLES3 context (a software
GLES implementation is acceptable) and any divergence found is fixed with a
test.

#### `touch-input-actions`

Express input as abstract actions so touch maps onto the same gameplay code as
keyboard and mouse. This item carried the web input plumbing it depends on: DOM
keyboard, pointer and touch events feed `InputState` from the web backend, and
browser `KeyboardEvent.code` maps onto the engine's `Key` enum so bindings are
layout-independent. Touch is therefore usable, not merely present.

**Done when** a test drives a touch sequence and observes the same gameplay
action a keyboard press produces.

#### `touch-gesture-regions`

Bind screen regions to actions declaratively, so a game declares "the left third
steers" once instead of reading raw touch points. Shipped as
`fe::player::ActionBindings`: a `fork_input` document lists actions with their
keys (by UI Events code), mouse buttons, and normalised touch regions, and
`fork_export` packs it and records its key so the player applies it after the
built-ins. `"replace": true` lets a game override the engine's default layout
rather than stack on it ([ADR-0090](adr/0090-declarative-input-bindings.md)).

**Done when** a game declares regions and a test proves a touch in each region
fires the bound action and a touch outside fires none. Both hold: a unit test
drives touches in, between, and outside the declared regions, and the web game
check dispatches a real synthetic touch at a point only the game's own document
covers, with no keys pressed, so the node moving means the document was packed,
parsed and applied end to end in a browser.

#### `mobile-viewport-meta`

A mobile browser needs a viewport meta tag and a full-bleed canvas or the game
renders at desktop scale with scrollbars. Shipped as a full-bleed, non-scrolling
page: the canvas is `position: fixed; inset: 0` over the whole window, the
viewport meta carries `width=device-width` and `viewport-fit=cover`, and the page
suppresses scroll, overscroll and browser touch gestures so a drag reaches the
game ([ADR-0091](adr/0091-mobile-viewport-and-full-bleed-canvas.md)). The shell
also measures its own layout into a hidden `#layout` element, because a headless
browser can read a number but cannot judge "looks right".

**Done when** the export shell carries the viewport meta and a test asserts the
canvas fills the window under a simulated phone size. Both hold: a unit test
asserts the meta's contents and the canvas rules, and the web game check runs the
2D slice at a phone's CSS size (844x390) and fails unless the canvas box equals
the window box.

#### `touch-fullscreen-prompt`

Offer a fullscreen request on the first touch, so a phone game is not stuck in a
browser chrome frame. Shipped as a best-effort request on the first touch or
pointer press on the canvas: `{ once: true }`, and every refusal — a missing API,
a rejected promise, a synchronous throw — is swallowed, because the game is
already playable in the frame it has
([ADR-0092](adr/0092-fullscreen-from-first-touch.md)).

**Done when** the shell requests fullscreen from a gesture and degrades silently
where the browser refuses. Both hold: the request is bound to the first
`touchstart`/`pointerdown` on the canvas and its failure paths are all caught;
the unit test asserts the request and the gesture binding, since a headless
Chromium cannot grant fullscreen to a synthetic event.

#### `mobile-safe-area`

Notches and home indicators cover a full-bleed canvas. Keep interactive regions
inside the safe area. Shipped as a normalised seam on `ActionMap`:
`set_safe_area_insets` takes four fractions of the viewport, and the insets
*shrink the space a region is measured in* rather than being subtracted from
each region, so one declared layout stays valid on a notched and an unnotched
phone alike. The shell reads CSS `env(safe-area-inset-*)`, converts the pixels
to fractions at the boundary that measured them, and passes them as the
`--safe-area` flag; the player forwards them
([ADR-0093](adr/0093-safe-area-insets-seam.md)).

**Done when** the shell reads the safe-area insets and a test proves a declared
region is offset by them. Both hold: unit tests drive a touch into a reserved
band (fires nothing) and into the usable area's own edge (fires), and a nonsense
report is asserted to fall back to the identity mapping; the web game check
prints the value the runtime was given. The insets are always zero in a desktop
headless browser, so the behaviour is proved by the unit tests and the plumbing
by the browser.

#### `android-window-backend`

Implement the Android window backend: native activity, EGL surface, lifecycle
(suspend/resume), and touch.

**Done when** the Android target builds and a documented command produces a
runnable APK.

#### `android-export-template`

Extend `fork_export` to produce an Android build.

**Done when** `fork_export --target android` produces a directory that installs
and launches on a device or emulator.

#### `ios-window-backend`

Implement the iOS window backend: a Metal or GLES surface, lifecycle, and touch.

**Done when** the iOS target builds and produces a runnable app bundle. Note:
this needs a macOS host; record the constraint rather than stubbing it.

#### `ios-export-template`

Extend `fork_export` to produce an iOS build.

**Done when** `fork_export --target ios` produces an Xcode project or archive
that a macOS host can sign and run.

---

### Phase 3 — a downloadable desktop build

The bar: a player downloads one file per OS, runs it, and plays the game.

#### `install-rules`

Every packaging step in this phase starts from an installed engine, and there
was none: `cmake --install` did nothing, so the only way to use the tools or
build against the public headers was from the source checkout. Add install rules
for the engine library, its public headers, the editor shell, and the three
shipping tools, with the layout decided in one place (GNUInstallDirs, Windows
libraries in `bin`, a relative RPATH on Linux), and cover the installed tree with
a smoke test that runs every installed tool from the prefix. See
[ADR-0084](adr/0084-install-rules.md).

**Done when** `cmake --install` produces a tree with `bin`, `lib`, `include`, and
`share`; every installed tool runs from the prefix; and the smoke test asserts
it in CI.

#### `desktop-export-templates`

The export path copies the player built beside it. Shipping needs a *prebuilt*
runtime per desktop OS so a developer without the engine's toolchain can export.

**Done when** a documented, reproducible release build produces a per-OS runtime
artifact that `fork_export` accepts via `--player`, and the smoke test runs it.

#### `linux-appimage`

Produce a single-file Linux artifact that runs on a clean machine without the
engine's shared libraries.

**Done when** the AppImage builds reproducibly in CI and launches the game on a
clean container.

#### `windows-zip`

Produce a Windows artifact a player can unzip and run.

**Done when** the zip builds in CI and the game launches from it on a clean
machine.

#### `macos-app-bundle`

Produce a `.app` bundle, then a DMG, with the runtime's dependencies bundled.

**Done when** the DMG builds in CI and the game launches from it on a clean
machine.

#### `desktop-packaging`

Script the packaging for all three desktop targets behind one command.

**Done when** each packaging step is scripted, reproducible in CI, and produces
an artifact that runs on a clean machine.

#### `release-version-stamping`

A downloadable build must say which version it is. Stamp the engine version into
the exported manifest and show it in the game. Shipped as an optional
`engine_version` field on the export manifest, stamped by `fork_export` from
`fe::version::kEngineVersionString` so it comes from the one source of truth
(the root `CMakeLists.txt`, through the generated `Version.h`) and cannot be
typed in wrong. The web page's `<title>` carries it as `Name 0.1.0`, and the
player logs one line when a manifest recorded one. The field is optional and
`format_version` is not bumped: an added optional field a reader may ignore does
not change the document's shape ([ADR-0094](adr/0094-export-version-stamping.md)).

**Done when** the export manifest carries the version from
`CMakeLists.txt` and a test proves it matches `fe::version::kEngineVersion`.
Both hold: a unit test round-trips the field and rejects a wrong type, and an
end-to-end export of `examples/demo_project` writes `"engine_version": "0.1.0"`
and a `<title>Demo 0.1.0</title>`. A manifest written before the field existed
still loads, reading no version rather than a guess.

---

### Phase 4 — the game is worth shipping

The bar: the slice is a game a stranger can play, not a demo.

#### `text-label`

The engine cannot draw a character: text lives only in the editor's ImGui
panels and the web shell's HTML. Nothing in Phase 4 can be built without it —
a title, a score, a menu, and a settings screen are all strings. Add a
`TextLabel` component that emits one glyph quad per character through the
existing 2D extraction seam, and a generated bitmap-font atlas behind it
([ADR-0083](adr/0083-in-engine-text-rendering.md)).

**Done when** a test lays out a string, extracts it, and observes one instance
per glyph with the expected advance, and the breakout slice draws its score and
lives on screen in a headless browser run.

#### `in-game-ui`

Text alone cannot make a menu. A `TextLabel` is a world-space object: its
glyphs are placed by the node transform and drawn through the world camera, so
a HUD built from labels drifts out of its corner when the window's aspect ratio
changes, and nothing can be pointed at, so no button and no menu is possible.
Add screen-space UI as scene components — a `UiCanvas` and a `UiElement` — laid
out by one pure solver that drawing and hit testing both consume, drawn as quads
through the existing 2D seam, and driven by one input system
([ADR-0086](adr/0086-in-game-ui-layout-and-extraction.md)).

**Done when** a test asserts that the rectangle the renderer draws a button in
equals the rectangle the input system hit tests, and the breakout HUD is a
screen-space canvas whose score and lives stay in the window's corners at any
aspect ratio.

#### `collision-filtering-and-triggers`

Every collider collides with every other collider, so a game cannot say "these
two never touch" or "this shape is a zone, not a solid". A pickup the player
passes through, a projectile that ignores its owner, and team collision are all
the same missing idea. Add `layer`/`mask` filtering and a trigger flag to
`Collider2D` and `Collider3D`, applied in one place in each solver
([ADR-0087](adr/0087-collision-filtering-and-triggers.md)).

**Done when** a test proves two colliders with non-overlapping masks pass
through each other and report no contact, and a trigger reports a contact
without applying an impulse, in both 2D and 3D.

#### `slice-title-and-menus`

A title screen, a pause, and a game-over screen, so the slice has a shape.

**Done when** a test drives the state machine through title → play → game over →
title.

#### `slice-scoring-and-progression`

Score, lives, and levels that get harder, so there is a reason to keep playing.

**Done when** a test proves a level advances on a clear and the difficulty
increases.

#### `slice-persistence`

Save the best score locally, so progress survives a reload.

**Done when** a test writes and reads a score through the storage seam on both
desktop and web.

#### `slice-settings`

Volume and control settings, persisted, with a touch-friendly menu.

**Done when** a test changes a setting, reloads, and observes it retained.

#### `scripting-native-modules`

The scripting seam plus C++ native modules as the primary language (M7), so game
logic can live outside the engine.

**Done when** a game's rules load from a module the engine did not compile in,
with a test covering load and reload.

---
### Backlog — not yet ordered

- Additional graphics backends (Vulkan, Metal, WebGPU) as targets demand.
- Skeletal animation/skinning (the one open M6 item), needed for animated 3D.
- More example games beyond the breakout slice, once the first one ships.

### Ongoing

- Keep the build reproducible and warning-clean.
- Keep the tests green; every fix gets a test that would have caught it.
- Keep the quality gates from [Pillar 1](#pillar-1--performance-is-a-product-feature)
  green rather than merely present: the sanitizer job
  ([ADR-0035](adr/0035-ci-quality-gates.md)) and the benchmark regression gate
  (see `benchmarks/` and [ADR-0033](adr/0033-performance-regression-gate.md)).
- Keep the architecture documentation and ADRs current.
- Review dependency licenses for compatibility with MIT.
