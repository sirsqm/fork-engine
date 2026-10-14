# Fork Engine Architecture

This document describes how Fork Engine is put together: the module graph, the
composition model, the subsystem boundaries, and the reasoning behind the
decisions that are not obvious. It describes both what exists today and the
direction the design is heading. Where something is not implemented yet, it is
marked as such.

Decisions that involve a real trade-off are recorded as ADRs in
[`docs/adr/`](adr/). This document explains the shape of the system; the ADRs
explain why it has that shape.

## 1. Goals and non-goals

The engine exists to make it practical for independent developers to build and
ship complete 2D and 3D games. That sets the priorities:

- Correct and coherent beats broad and shallow.
- A clean architecture that can absorb years of change beats short-term
  convenience.
- The editor is a first-class part of the product, not a bolted-on tool.

Three capabilities shape the architecture as much as they shape the roadmap, and
are planned in [`STRATEGY.md`](STRATEGY.md):

- **Performance is a product feature.** The engine is fast by construction and
  by measurement, not by a late optimization pass. The authoring model (the
  scene graph) and the processing model (flat, cache-friendly arrays) are
  deliberately different, and the conversion happens at one seam — see
  [ADR-0017](adr/0017-performance-strategy.md) and section 5.
- **Games ship anywhere.** The target is that a game reaches every platform its
  players use, not that the editor runs on several OSes — see
  [ADR-0018](adr/0018-multi-platform-shipping.md) and section 10.
- **Editing is collaborative.** Several people edit one project live, with
  presence and no manual merge — see
  [ADR-0019](adr/0019-collaborative-editing.md) and section 8.

Non-goals for now: matching a specific existing engine feature-for-feature, and
optimising before there is something worth optimising (which is why performance
work begins with measurement, not with a rewrite).

## 2. Module graph

The engine is one static library (`fork_engine`) built from modules. Modules may
depend only "downwards"; the dependency graph is acyclic by construction.

```
core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
core  <-  reflect  <-  scene, resources
core  <-  serialization  <-  scene, resources, reflect
core  <-  jobs  <-  systems
core, scene, resources  <-  systems
core, scene, systems, reflect  <-  physics2d
core, scene, systems, reflect  <-  physics3d
core, scene, resources  <-  mesh
core, scene, resources  <-  scene3d
core, rhi, resources, scene, mesh  <-  renderer3d
core, scene, systems, resources  <-  audio
core, resources  <-  pack
everything  <-  runtime
core  <-  net                     (planned: collaborative editing)
```

| Module | Responsibility | Depends on |
| --- | --- | --- |
| `core` | Fixed-width types, math, `Error`/`Result`, logging, assertions, UUID, filesystem helpers, frame profiler | — |
| `platform` | Window creation, input, and the graphics context | core |
| `rhi` | Graphics device abstraction and backend selection | core |
| `resources` | Asset loading, caching, lifetime | core, rhi |
| `scene` | Node/component hierarchy, transforms | core |
| `reflect` | Component property descriptions and the type registry | core, scene, resources |
| `serialization` | JSON persistence of scenes, driven by reflection | core, scene, resources, reflect |
| `systems` | Per-frame execution: `System`, `SceneSchedule`, `FrameContext`, and the concrete gameplay systems | core, scene, resources |
| `physics2d` | 2D collision shapes, a rigid-body simulation system (linear and angular) with a uniform-grid broad phase, joints (distance and revolute), queries, contact events, and island-based body sleeping | core, scene, systems, reflect |
| `physics3d` | 3D collision shapes (box, sphere), a rigid-body simulation system (gravity, restitution, friction, damping) with a uniform-grid broad phase, joints, queries, and contact events | core, scene, systems, reflect |
| `mesh` | CPU-side geometry: `Vertex3D`, `Mesh`, primitive generators, the OBJ loader | core, scene, resources |
| `scene3d` | 3D scene components (`Camera3D`, `MeshRenderer3D`, `DirectionalLight3D`, `PointLight3D`, `SpotLight3D`) and their reflection | core, scene, resources |
| `audio` | Audio output device, decoded clips, and a mixing system | core, scene, systems, resources |
| `pack` | Asset pack container, writer, and the runtime mount that reads a pack like a directory | core, resources |
| `jobs` *(planned)* | Thread pool, `parallel_for`, and a task graph; consumed by systems and by render extraction | core |
| `net` *(planned)* | Transport abstraction and the collaborative-editing session client | core |
| `renderer2d` | Batched 2D rendering | core, rhi, resources, scene |
| `renderer3d` | 3D mesh rendering with a lit pass and directional shadow map | core, rhi, resources, scene, mesh |
| `runtime` | `Engine` (subsystem lifetime), `Application` (frame loop) | all |

A new edge must point down this graph. Introducing an edge is an architectural
change and requires an ADR.

### Why these boundaries

- **`core` has no dependency on anything.** Types, math, and error handling are
  needed everywhere, so they must not drag in graphics or platform code.
- **`rhi` hides the graphics API.** Nothing above `rhi` includes a backend
  header (no `<glad/gl.h>` outside `src/rhi/opengl/`). This is what makes a
  second backend a contained change rather than a rewrite. The OpenGL 3.3 core
  backend is one implementation of the interface.
- **`scene` is pure data.** It does not render, simulate, or serialise itself.
  Rendering walks the graph to build a draw list; serialization writes it; future
  systems read it. Keeping the data model independent of its consumers is what
  lets the editor hold a scene without pulling in the runtime.
- **`platform` hides the windowing library.** GLFW appears in public headers
  only as a forward-declared handle (`GLFWwindow*`), never as a header include.

## 3. The composition model

Fork Engine uses a hybrid model. This is the single most consequential
architectural decision in the project, so it is worth stating plainly.

**A `Node` provides identity, a name, a transform, and hierarchy.** It is the
unit the editor manipulates and the serializer persists. Every node has exactly
one transform, so there is always a single, predictable answer to "where does
the transform live?".

**A `Component` provides a capability** (a sprite, a camera, a collider). It
carries data and, when needed, logic. It does not own hierarchy.

```
Scene
 └── Node "Camera"        [Camera2D]
 └── Node "Hero"          [Sprite2D]
      └── Node "Shadow"   [Sprite2D]
```

This is a scene-graph/entity-component hybrid: the hierarchy is the authoring
model, and composition happens through components. It gives up the "flat list of
entities" simplicity of a pure ECS in exchange for an editor model that maps
directly onto what users see, and for a transform hierarchy that is trivial to
reason about.

**The hierarchy organises data; it does not dictate execution.** A system that
needs a flat, contiguous layout for performance (physics broadphase, animation
blending, future 3D render extraction) reads the component data it cares about
into its own structure each frame. That is the data-oriented part of the hybrid:
systems choose their own memory layout instead of inheriting the graph's. See
[ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md).

### Systems and the frame schedule

Execution lives in **systems**, not on nodes. There is no `update()` virtual on
`Node` or `Component`; instead a `System` owns its own traversal and runs once
per frame from a `SceneSchedule`:

```cpp
class System {
public:
    virtual std::string_view name() const noexcept = 0;
    virtual void update(const FrameContext& context, Scene& scene) = 0;
};
```

`FrameContext` carries `delta_time`, `elapsed_time`, and a nullable
`ResourceManager*`, so a system reads everything it needs from the frame rather
than from globals -- which is what lets it run headlessly in a test.

The schedule is a plain ordered list. It is **not** a job graph: systems run in
registration order, on one thread, with no declared read/write sets. That is a
deliberate floor, not a ceiling -- parallelism and dependency ordering can be
added to `SceneSchedule` later without changing any system. The moment the
ordering becomes implicit instead of explicit at the call site, debugging a
frame becomes guesswork, so ordering is left visible. See
[ADR-0008](adr/0008-systems-and-frame-schedule.md).

The schedule lives with the scene, not in `Engine`. `Engine` owns no scene, so
`Application` creates a scene and a schedule and pairs them; the editor pairs
its own the same way, which is what makes play-in-editor run the same systems
through the same path.

`SpriteAnimationSystem` is the first system: it advances the `SpriteAnimator`
component and writes the current cell's UVs onto the node's `Sprite2D`. It shows
the shape -- gather the components you process into a flat array, advance them,
hand the result to the subsystem that consumes it -- without the renderer
knowing animation exists.

### Profiling and observability

The engine measures its own frame so performance work is grounded in numbers
rather than guesses ([ADR-0017](adr/0017-performance-strategy.md)). The
instrument is `fe::Profiler` in `core`: a fixed ring of frame samples holding
named CPU scopes and integer counters, plus an RAII `ProfileScope`
([ADR-0022](adr/0022-profiler-and-frame-timing.md),
[ADR-0024](adr/0024-profiler-instrumentation.md)).

Instrumentation is two macros:

```cpp
FE_PROFILE_SCOPE("physics/solve");  // times the enclosing block
FE_PROFILE_FUNCTION();              // times the enclosing function
```

Both are RAII, so the sample closes on every exit path. `FE_PROFILING` (default
1) gates them; when it is 0 they expand to nothing, so a shipping build pays
nothing and carries no name strings. The cost when on is a thread-local read and
a branch when idle, and one `now()` pair plus an interned-name lookup when a
frame is in progress -- no allocation, no lock.

The runtime feeds the profiler without any cooperation from gameplay code.
`Application` installs its profiler as the thread's current one and brackets
each frame with `frame`, `frame/update` (the `on_update` hook plus the system
run), and `frame/render`. `SceneSchedule` times each system as `system/<name>`.
A frame's cost therefore breaks down by phase and by system by default.

A caller reads the numbers as a **snapshot**, never a live pointer into the
moving ring: `Profiler::frame_report(offset)` and
`Application::last_frame_report()` return a `FrameReport` by value -- total,
update, render, per-name totals (sorted by cost), counters, and drop counts,
with owned name strings. `FrameReport::summary()` is a stable single line for
logs and CI; `to_string()` adds the per-scope detail. This is the read-only
surface the editor's future profiler panel renders; the editor lane consumes it
and the engine never includes an editor header.

The current profiler is thread-local and a profiler is recorded from one thread;
a worker thread records on its own profiler and cannot corrupt the main thread's
frame. Cross-thread aggregation waits for the `jobs` module (M9).

### Physics as a system

`physics2d` is the first system that *simulates* the scene rather than reading
it. It is the real test of the hybrid model: a physics solver wants a flat array
of bodies, while the scene is a tree of nodes, so `Physics2DSystem` flattens the
scene into its own layout, simulates, and writes back -- the scene graph is read
once and written once, and the inner loop never touches it.

Shape and motion are separate components, which is what makes static and dynamic
bodies the same code:

| Node has | Behaviour |
| --- | --- |
| `Collider2D` only | Static obstacle: collided with, never moved |
| `Collider2D` + `RigidBody2D` (mass > 0) | Dynamic body: pushed by gravity and collisions |

Each update integrates linear velocity (gravity, damping) and position, and
angular velocity (torque, damping) and rotation, then runs a fixed number of
sequential-impulse iterations over the *candidate* pairs: a uniform-grid
(spatial hash) broad phase finds the candidates once per step, a narrow phase
(`intersect` for box/box, circle/circle, circle/box) tests each candidate, and
the solver applies a normal impulse with restitution, a Coulomb friction
impulse, and Baumgarte positional correction. The candidate set is computed once,
before the iteration loop, so a step costs O(candidates * iterations) rather than
O(bodies² * iterations). Positions and rotation are written back to
`Node::transform()`; linear and angular velocities back to the `RigidBody2D`.

**Queries** are free functions over a scene (`Physics2DQueries.h`): `raycast`,
`overlap_circle`, `overlap_box`, and `overlap_point`. They are not a system, so
the editor can pick without running a schedule, and they reuse the same geometry
the solver uses. A raycast returns a single value; the overlap queries append to
a caller-owned buffer, so a hot loop does not allocate.

**Contact events** are collected on the system and read after the step:
`Physics2DSystem::contact_events()` returns the contacts that began or ended
during the last update, keyed by node pair and ordered deterministically. A
gameplay system that runs after physics reads them; no event bus is introduced,
because the systems layer (ADR-0008) deliberately has none.

Angular dynamics was added in a later cut, extending the same impulse equations
rather than replacing the solver. A box collider is an *oriented* box: it
carries the node's Z rotation and a local offset, so a rotated or offset shape is
collided as its true shape rather than its bounding box. See
[ADR-0009](adr/0009-physics2d-system.md) for the first cut,
[ADR-0013](adr/0013-physics2d-queries-events-angular.md) for queries, contact
events, and angular dynamics,
[ADR-0021](adr/0021-physics2d-joints.md) for joints, and
[ADR-0037](adr/0037-physics2d-oriented-boxes-offsets.md) for oriented-box
colliders and collider offsets.

**Joints** are two components, `DistanceJoint2D` (a rigid rod or a spring) and
`RevoluteJoint2D` (a hinge or pendulum), each naming its two ends by node id so
the reference serializes. The constraint maths is pure and lives in
`JointSolver.h` (`JointBody`, `ConstraintRow`, `build_*`, `apply_*`), so it is
unit-testable with no scene. `Physics2DSystem::solve_joints` gathers the scene
into those values and applies each row once per step, after the contact pass has
written back, using the same sequential-impulse shape as a contact. A joint end
may be any node, not only a collider, so a pendulum can hang from a bare pivot;
a broken reference (nil, missing, or self-referential) is skipped and reported
through `joint_diagnostics()` rather than crashing.

**Sleeping** stops a settled scene from doing solver work. Bodies are grouped
into *islands* -- the connected components of the contact-and-joint graph over
the dynamic bodies, with static geometry deliberately not joining islands, so two
crates on one floor are separate. `IslandBuilder` (`IslandSolver.h`) is a pure
union-find over body indices with no scene and no components; `Physics2DSystem`
builds its links from overlapping bounds and from joints and calls it. A body
that has been slow for `sleep_time` (default 0.5 s) lets its whole island sleep,
and the solver and integrator then skip it; a contact or joint to an *awake*
dynamic body wakes the island before the solver runs, so nothing ever passes
through a sleeping body. The island pass runs before the solver and the sleep
pass after it, both by design. The state is observable (`is_sleeping`,
`sleeping_body_count()`, `island_count()`, `same_island()`) and controllable
(`can_sleep`, `sleep()`, `wake()`), and the authored knobs serialize while the
live state does not. See [ADR-0026](adr/0026-physics2d-islands-sleeping.md).

**3D physics** is a sibling module (`physics3d`), not a mode of the 2D one, and
shares its shape: `Collider3D` (box or sphere) and `RigidBody3D` are separate
components, `Physics3DSystem` flattens the scene, integrates, runs a uniform-grid
broad phase, solves contacts with the same sequential-impulse form, solves
`DistanceJoint3D` constraints, and writes back. It is deliberately a **first
cut** -- linear only (no angular dynamics), one joint type, and no islands or
sleeping yet -- and it is kept behind a component-and-query boundary so a
third-party solver (Jolt) can replace it without touching gameplay or the editor.
See [ADR-0031](adr/0031-physics3d-and-backend-seam.md).

### Audio as a module

`audio` sits beside `physics2d` in the graph, not inside a renderer: sound is a
sibling capability, not part of drawing. It is split along three seams.

**The device is an interface.** `AudioDevice` opens a stream at a sample rate and
accepts mixed interleaved float frames (`submit`). Two implementations exist:
`MiniAudioDevice`, the real backend built on
[miniaudio](https://github.com/mackron/miniaudio), which drains a lock-free ring
buffer on the audio thread; and `NullAudioDevice`, a deterministic sink with no
hardware. The null backend is a **test double**, not a shipping path: it records
the frames it is given, so the mix pipeline is provable with no sound card, and
it is what the unit tests and CI run against. No header above `src/audio/`
includes miniaudio.

**The mixer is a pure function.** `mix_voices` takes voices, an output buffer,
and a format, and does all of the maths - gain, pitch (a clip/output
sample-rate ratio), looping, one-shot termination, and mono up-mixing. It
touches no device and no scene, so the tests call it directly and assert exact
samples.

**The system is thin.** `AudioSystem` reconciles the scene's `AudioSource`s with
its voices, turns `delta_time` into a frame count, calls `mix_voices`, and
submits the result. It holds no maths of its own.

`AudioClip` is the decoded CPU-side asset (the `Image` of audio); `WavLoader`
decodes PCM WAV under the `wav` scheme, with the parser exposed separately from
`load()` so it is testable without a file. `AudioSource` stores the clip's
manager key, plus gain, pitch, loop, play-on-start, and a spatial toggle whose
position would come from the node's world transform - no separate transform is
introduced. Audio is opt-in: `Engine` registers the loader and creates the
system with no device, and a caller opens a device and attaches it, so a machine
with no sound card runs everything else unchanged. See
[ADR-0016](adr/0016-audio-module.md).

### Identity

Every node has a stable `Uuid`. The scene keeps an index from `Uuid` to `Node`,
so a reference to a node survives renames and reorders. Serialization restores
the saved id rather than minting a new one, so references remain valid across a
save/load cycle. Future cross-scene and resource references will use the same
`Uuid` scheme.

### Ownership and lifetime

- A `Node` is owned by its parent, or by the `Scene` when it is a root. Nodes are
  created through `Scene::create_node()` or `Node::create_child()`.
- A `Component` is owned by its `Node` and is move-only.
- A `Resource` is owned by whoever holds a `std::shared_ptr` to it. The
  `ResourceManager` caches **weakly**, so a resource lives exactly as long as
  something references it (see [ADR-0004](adr/0004-resource-ownership-weak-cache.md)).
- GPU objects are owned by the `rhi::Device` and released when their
  `shared_ptr` count reaches zero. Because their destructors call into the
  graphics API, they must be released **before** the device is shut down; the
  runtime enforces this ordering (see section 6).

## 4. Error handling

Recoverable failures are values, not exceptions. Functions that can fail return
`fe::Error` or `fe::Result<T>`:

```cpp
Result<std::shared_ptr<Resource>> load(std::string_view path);
```

This keeps failure modes visible in signatures, avoids the cost of unwinding on
hot paths, and makes it hard to ignore an error by accident. Exceptions are not
used at all. Assertions are reserved for programming errors:

- `FE_ASSERT(expr)` — a debug contract, compiled out in release.
- `FE_CHECK(expr)` — an always-on contract, for cheap preconditions on public
  API boundaries and for data the engine does not control.

See [ADR-0003](adr/0003-error-handling.md).

## 5. Rendering

Rendering is split into a device abstraction and renderers built on top of it.

**`rhi::Device`** is a command-oriented interface: create resources, begin and
end a frame, set state, bind, and draw. The OpenGL 3.3 core backend implements
it. The interface is intentionally small — enough to express what the engine
draws today, and shaped so that a Vulkan or Metal backend can implement it later
without leaking those APIs upward.

**Off-screen render targets.** `rhi::RenderTarget` is a framebuffer with a color
texture (and optional depth) that can be bound for drawing and then sampled like
any texture. This is the seam the editor viewport uses to render a scene into a
panel independent of the window: bind the target, draw, unbind, and blit
`color_texture()`. A depth attachment is a renderbuffer by default; setting
`RenderTargetDesc::sample_depth` makes it a texture instead, exposed through
`depth_texture()`, which is what the directional shadow map needs. The seam is
also where a future post-processing chain will attach, which is why it lives in
`rhi` rather than in `Renderer2D`. `Device::begin_frame` always starts on the
window's default framebuffer, so a frame cannot leak into or out of a target.

**`renderer2d::Renderer2D`** walks a scene, collects sprites, sorts them into a
stable draw order, builds a vertex/index stream in CPU memory, uploads it in
batches, and issues one indexed draw per texture batch. Sprites that share a
texture and layer collapse into a single draw call. A 1×1 white texture stands in
for sprites without one, so an untextured sprite renders as a solid quad.

**3D** is a separate renderer with its own pass structure, sharing the same
`rhi::Device` (see [ADR-0010](adr/0010-3d-rendering-foundation.md)). The 2D and
3D paths are not forced into one abstraction; they have different data and
different optima.

**`mesh::Mesh`** is a CPU-only `Resource`: interleaved `Vertex3D`
(position/normal/uv), 32-bit indices, and a cached local AABB. It knows nothing
about the GPU, exactly as `Image` knows nothing about textures, so geometry can
be built and tested with no context. `make_box`/`make_cube`/`make_plane` give a
scene something to render before an asset pipeline exists, and `ObjLoader`
(scheme `obj`) decodes Wavefront OBJ into a `Mesh`.

**`renderer3d::Renderer3D`** walks the scene, pairs each `MeshRenderer3D` with
its node's world matrix, and issues one indexed draw per instance. It owns a
buffer cache keyed by the mesh, uploading on first use and sweeping dead entries
each frame, so a shared mesh is uploaded once. The pass is a single forward
Blinn-Phong shader: albedo × (ambient + the sum of the light contributions),
each light contributing colour × intensity × (diffuse + specular) with its own
falloff. It is depth-tested and opaque, so draw order does not change the image.
The camera position is a uniform; the shader does not assume the camera is at
the origin. A 1×1 white texture stands in for meshes without one, as in
`Renderer2D`.

The 3D scene components (`Camera3D`, `MeshRenderer3D`, `DirectionalLight3D`,
`PointLight3D`, `SpotLight3D`) live in `scene3d`, which does **not** depend on
`renderer3d`: a camera or a light is scene data the renderer reads.
`Camera3D::view_projection` composes `look_at` and `perspective`; a component
never stores its own transform. Point and spot lights read their node's world
position, and a spot light its node's forward axis, the same way the camera
reads its eye.

**Lighting.** The renderer supports a directional light plus bounded arrays of
point and spot lights (ADR-0012). Lights are gathered once per frame — where the
owning node is still in hand — and the brightest are kept when a scene exceeds
`kMaxPointLights`/`kMaxSpotLights`. The packed arrays are uploaded through
`rhi::Shader::set_vec4_array`, and the shader's loops are bounded by count
uniforms. Point and spot falloff reaches zero at the light's `range`, so a
light's influence is finite.

**Culling.** `Renderer3D` rejects instances whose world-space AABB lies wholly
outside the camera frustum before they reach the GPU (ADR-0011). `core::Frustum`
extracts the six planes from a view-projection matrix; the test is conservative
(a box straddling a plane is kept), so it can only save a draw, never hide a
visible mesh. It is a flat pass, not a spatial hierarchy — the leaf operation a
BVH would call.

**Shadows.** When the scene has a directional light, `Renderer3D` runs a
depth-only pass before the main pass, rendering the culled instances into a
1024² sample-depth target (ADR-0014). The light-space matrix is an orthographic
projection fitted to the culled instances' world bounds, sized from their
bounding sphere so coverage is stable as the camera moves. The main pass samples
that depth with a 3×3 PCF filter; fragments outside the map are treated as lit,
and a slope-scaled bias removes self-shadowing acne. Only the directional light
casts; ambient stays unshadowed so occluded areas keep fill. `last_shadow_casters()`
reports the depth-pass draw count.

Batching, point/spot shadows, a spatial tree, and a material graph are later work
behind these same interfaces.

### The extraction seam (planned)

The renderers today walk the hierarchy and issue draws in the same pass. As the
scene grows that is both cache-hostile and hard to parallelise, so the planned
structure separates the two steps — see
[ADR-0017](adr/0017-performance-strategy.md):

- **Extract** — one walk of the hierarchy that emits a flat array of instances
  (mesh or sprite, world matrix, material handle, visibility).
- **Issue** — draws the flat array, after sorting, batching, culling, and
  instancing have operated on it.

The flat array is the unit a job system chunks and the unit instancing and a
render graph build on. The authoring model (the scene graph) and the processing
model (the array) are deliberately different, and this seam is the only place
they meet. It is the concrete form of the hybrid architecture
([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md)) in the hot path.

### Camera selection

"Which camera is active for this scene?" has exactly one implementation per
renderer, shared by that renderer and the runtime's clear-colour logic.
`Renderer2D::find_active_camera` and `Renderer3D::find_active_camera` each encode
the rule for their scene (first enabled camera, else the first camera). Two
independent answers to that question is precisely the kind of divergence that
produces subtle bugs, so the runtime calls the renderer's helper rather than
reimplementing the rule.

## 6. Subsystem lifetime

`Engine` is the single place that knows the order in which subsystems are
created and destroyed:

```
window  ->  device  ->  resource manager (with loaders)  ->  renderers
```

Creation order matters: the resource manager needs a device to create GPU
textures, and the device needs a current context from the window. Destruction
happens in reverse.

`Engine` does not run a frame loop. `Application` does. Keeping the two apart is
what lets the editor embed an `Engine` and drive frames itself for
play-in-editor, without inheriting the standalone loop or forking the runtime.

Because scene components can own GPU-backed resources (a `Sprite2D` holds a
`Texture2D`), `Application` releases the scene **before** shutting the engine
down. Releasing a texture after the GL context is destroyed is undefined
behaviour; the ordering rule is enforced in `Application::run()` and documented
at the call site.

## 7. Resources

The `ResourceManager` maps a path to a single shared instance, dispatches to a
`ResourceLoader` by scheme and file extension, and caches weakly so assets are
released when nothing references them.

Schemes disambiguate loaders that handle the same file but produce different
resources. `assets/hero.png` loads a CPU `Image`; `tex:assets/hero.png` loads a
GPU `Texture2D` from the same file, reusing the decoded image so the decode is
not repeated. A component that references a texture stores the **cache key**
(including the scheme), not the raw file path, so the reference resolves to the
same GPU resource after a scene reload. See
[ADR-0004](adr/0004-resource-ownership-weak-cache.md).

This is the foundation of the asset pipeline. Import settings, a
content-addressed cache, and hot reload build on top of it without changing the
load/cache/release contract.

### Import settings

How an asset is turned into a resource -- how a texture is sampled, and later how
a mesh is scaled or a sprite sheet is sliced -- is a property of the asset, not
of every component that references it. Those choices live in the asset's sidecar,
in a `settings` object whose `kind` names the importer's settings type, and are
applied when the asset loads. `ImportSettings` is a tagged value type: the
texture kind carries a sampling filter, an addressing mode, and a mipmap flag.
An asset with no settings loads with the importer's defaults, which reproduce the
behaviour from before the mechanism existed, so **a sidecar written earlier still
loads unchanged**; a sidecar whose settings are malformed is rejected rather than
silently imported differently. See
[ADR-0027](adr/0027-asset-import-settings.md).

The settings reach the loader without changing `ResourceLoader::load`: the
`ResourceManager` scopes the current asset's settings for the duration of the
load, and a loader that cares reads them through `current_import_settings()`.
`Texture2DLoader` already loads the CPU `Image` through the manager, so it
honours the texture settings with no new dependency. Both a `guid:` reference and
a path reference to a known asset resolve the same settings; a path to an
unindexed file simply has none. `AssetDatabase::import(path, scheme, settings)`
is the single write point, and re-importing with new settings keeps the asset's
GUID (ADR-0020), so changing how an asset is imported never breaks references to
it.

### Import cache

An import is a pure function of its inputs, so its result can be stored and
reused. The `ImportCache` is a **content-addressed store of imported artifacts**:
it keys an artifact on `(source bytes, effective import settings, importer
identity)` and writes it under a cache root, so a re-import — in the same run or
a later one — is a cache read instead of a re-decode. It holds no identity and
no sidecar; the `AssetDatabase` stays the identity owner and the `ResourceManager`
stays the weak-cache loader, so the cache adds no second index.

The key is an FNV-1a 64-bit hash of a length-prefixed encoding of those three
inputs, rendered as 16 hex characters. **Invalidation is a consequence of the
key**: a change to the source bytes, the settings, or the importer yields a
different key, so the stale entry is simply never asked for. Eviction is "delete
the disposable root" — a cache root is derived data, and a bounded-growth GC is a
later concern. A malformed or mismatched artifact reads as a miss, never a wrong
result: the artifact stores its own key and is validated on read.

The first artifact cached is the decoded `Image`. The cache is checked inside the
loader (`ImageLoader` holds an `ImportCache*`, null by default), which is where
the artifact format already lives and which adds no module edge. The GPU
`Texture2D` is not cached directly: it decodes the CPU `Image` through the
manager, so a warm decode cache lets it skip the decode and pay only the upload —
the dominant half of the win. Caching the uploaded texture would need a serialized
`rhi::TextureDesc` and a device at read time, and is a follow-up. A cache with no
root is inert, exactly like an `AssetDatabase` with no root; the editor roots it
at `<project>/.fork/cache` (opt out with `--no-cache`), and the sandbox opts in
with `--cache DIR`. See [ADR-0029](adr/0029-import-cache.md).

### Asset identity

A reference that is a file path breaks when the file moves. To fix that, an
asset can carry a **stable GUID** in a sidecar metadata file next to it
(`assets/hero.png` and `assets/hero.png.meta`). The `AssetDatabase` owns the
sidecars, indexes them by GUID and by path, and is the only component that turns
a `guid:<uuid>` reference into the asset's **current** path. A scene therefore
stores `guid:` references, and a moved asset still resolves.

The sidecars are the source of truth; the in-memory index is a cache rebuilt by
scanning (`AssetDatabase::rescan`), so losing the index is never a data loss. The
serializer writes a GUID when its `ResourceManager` has an asset database that
knows the referenced asset, and falls back to the path key otherwise, so
path-based references keep working in headless tests and tools. A version-1 scene
document (path keys) is migrated to GUIDs at load time. See
[ADR-0020](adr/0020-resource-identity-guid-sidecar.md).

## 8. Editor

The editor is a client of the engine, not a second copy of it:

- It embeds an `Engine` and drives frames itself, so the viewport and
  play-in-editor share the exact runtime the game uses.
- It reads and writes scenes through the `scene` and `serialization` modules,
  not through private engine internals.
- The property inspector is driven by the same type information the serializer
  uses. Reflection is the bridge; see [ADR-0005](adr/0005-reflection-and-serialization.md).

The editor lives outside the engine library, under `editor/`, and adds no edge
to the module graph above: the dependency points from the editor to the engine,
never back. It is built as three targets:

| Target | Responsibility | Depends on |
| --- | --- | --- |
| `fork_editor_core` | Document (`EditorContext`), scene hierarchy view model, asset browser view model (`AssetBrowserViewModel`), play-in-editor (`PlaySession`), the headless property helpers (`PropertyEditor`), and the headless viewport maths and picking (`Viewport`). No toolkit, no window. | `fork_engine` |
| `fork_editor` | The Dear ImGui panels and the dock space (`EditorUi`), the 2D `ViewportPanel` (renders into an off-screen target and shows it as an image), the `AssetBrowserPanel`, and `EditorHost`, which embeds an `Engine` and runs the editor loop. | `fork_editor_core`, ImGui |
| `fork_editor_shell` | The executable entry point. | `fork_editor` |

The split exists so the editable, testable logic has no UI dependency: the
document, the hierarchy view model, the play-scene copy, and the property
helpers are exercised headlessly. The panels are a thin projection of that
logic. The UI toolkit choice and its consequences are recorded in
[ADR-0007](adr/0007-editor-ui-toolkit.md).

Play-in-editor copies the authored scene into a play scene, drives frames of the
copy through the embedded `Engine`, and discards the copy when play stops, so
playing can never mutate what the user is editing. Each tick the session runs a
`SceneSchedule` over the play scene - the same `Scene + SceneSchedule` pairing
`Application` uses (ADR-0008) - so play exercises real gameplay (at least
`SpriteAnimationSystem`) through the identical execution path. Panel-to-host
actions are a one-shot request (`PlayRequest`) rather than a direct call, which
keeps the panels free of the engine.

The viewport renders the scene into an off-screen `rhi::RenderTarget` sized to
the panel, then shows the target's colour texture as an `ImGui::Image`. This
reuses the runtime's own `Renderer2D` and `Camera2D` rather than a second render
path, and is why the RHI grew render targets. Showing a backend texture inside
the toolkit needs an interop identifier, which `rhi::Texture::native_handle()`
provides as a deliberately narrow escape hatch. Camera pan/zoom and node picking
live in the headless `Viewport` (screen/world transforms and a smallest-quad
hit-test), so the interaction is unit-testable without a window; `ViewportPanel`
is its thin toolkit projection.

The inspector is driven entirely by the reflection registry
([ADR-0006](adr/0006-component-reflection.md)): for the selected node it looks
up each component's `TypeDesc` and draws one widget per `Property`, reading
through the property's getter and writing through its setter. It never names a
component type or field, so a newly reflected component appears without an
editor change. A type that is not registered is shown read-only rather than
crashing, and resource references are resolved through the document's
`ResourceManager` (null in a headless document, where the field is shown but not
editable). Text widgets need a buffer that outlives the frame, so the panel
holds that scratch state.

The widget for a property is chosen from typed *editor hints* carried on the
`Property` ([ADR-0030](adr/0028-editor-hint-metadata.md)): a `RangeHint` turns a
Float or Int into a bounded slider or drag, a `ColorHint` turns a float group
into a colour picker (RGB or RGBA, from the group's member count), and a
`ResourceHint` turns a resource reference into an asset drop target labelled
with the expected type. A property with no hint falls back to the generic widget
for its `ValueKind`. The hints are data, not strings: bounds are numbers and the
resource type is a name, so there is no hint grammar to parse and a
mis-registration is visible at the registration call site. The hint helpers
(`resolve_range_hint`, `clamp_to_range`, `color_channel_count`) are pure and
live in `fork_editor_core`, so they are covered headlessly while `InspectorPanel`
stays a thin projection.

Editing is reversible through a command stack in `fork_editor_core`
([ADR-0015](adr/0015-editor-undo-redo.md)), not by snapshotting the scene. Each
edit is a `Command` with an exact `execute()`/`undo()` pair; the panels build
commands and hand them to `EditorContext`, which owns the stack and never lets a
panel mutate the scene directly. Commands address their objects by `Uuid` and
hold whatever they detach (a node's subtree, a component), so undo restores the
identical object - ids, sibling order, and component order intact. Property
edits go through one generic `SetPropertyCommand` that resolves its setter
through the reflection registry, so a newly reflected field is undoable with no
new code. A resource reference is edited through `SetResourceCommand`, which
carries the `ResourceManager` needed to resolve the reference and is otherwise
the same undoable, coalescing edit ([ADR-0030](adr/0028-editor-hint-metadata.md)).
A command may report a coalescing key: consecutive edits of the same property
(the frames of a drag) merge into one undo step, and the editor ends the group
when the widget's gesture finishes. Editing is refused while play-in-editor is
running, because play shows a copy and an edit to the hidden authored scene
would be invisible.

The asset browser lists what the resource system has indexed. Its logic lives in
the headless `AssetBrowserViewModel`, which reads the `AssetDatabase` the
`ResourceManager` already maintains, so the browser shows exactly the assets the
engine can load rather than a second, parallel view of the filesystem. It
sorts by path, filters by a case-insensitive substring, and holds the selection
by `AssetId`, so a rescan (a file move, an import) is reflected without the
browser knowing about paths. The one thing it produces that outlives the session
is an asset's stable `guid:<uuid>` reference, which is what a drag out of the
browser hands to a drop target such as a sprite's texture field. Listing is
read-only; importing and moving assets are later work. Each entry also reports its
import settings kind (ADR-0027), so the browser shows what an asset will be
imported as; editing those settings in the editor is a follow-up.

The remaining M2 groundwork item is done: per-asset import settings live in the
sidecar and are applied at load ([ADR-0027](adr/0027-asset-import-settings.md)).

### The project file

A project is a directory with a `fork.project.json` file in it, and that file is
what the editor opens and saves
([ADR-0025](adr/0025-project-file.md)). It carries the settings that change
behaviour today - the project name, the asset root, and the startup scene - as
versioned UTF-8 JSON, the same shape and for the same reasons as a scene
([ADR-0005](adr/0005-reflection-and-serialization.md)). Paths inside it are
stored relative to the project directory, so a project is portable.

`ProjectSettings` is a headless value type in `fork_editor_core`; it parses,
writes, and resolves paths, and is fully testable without a display.
`EditorContext::load_project` reads the settings and applies them through the
engine's existing public API, pointing the `AssetDatabase` at the resolved asset
root. The host does this **before the first scene load**, so `guid:<uuid>`
references inside the startup scene resolve during deserialization. The engine
never learns what a project is: it stays reusable and project-agnostic, and the
editor - the thing that opens a project - is the owner of the concept.

`fork_editor_shell --project DIR` (or a project file, or `--project-file FILE`)
opens one. With no project the editor still runs: the browser is empty and only
path references resolve.

The "build/export path" is deliberately not here. Settings are a small,
editor-local format; assembling a runnable build from a project is the packaging
and export-template machinery already planned for
[ADR-0018](adr/0018-multi-platform-shipping.md) (M8). The milestone line split
into two: settings land here, the build/export path lands with M8. The packaging
half of that path has since landed in the `pack` module
([ADR-0030](adr/0030-asset-pack-format.md)): the `fork_pack` tool reads
`fork.project.json`, and the `pack` module turns the project's asset tree and
startup scene into one file the runtime mounts. The editor still owns the
project concept; the pack writer only takes resolved paths. Export templates and
the `fork export` step remain open.

### Collaborative editing (planned)

The goal is that several people edit one project at once, see each other's
changes and presence live, and never run a manual merge — see
[ADR-0019](adr/0019-collaborative-editing.md) and
[`STRATEGY.md`](STRATEGY.md). The editor is already shaped for it:

- Every edit goes through the command stack rather than mutating the scene
  ([ADR-0015](adr/0015-editor-undo-redo.md)), so an edit is already a discrete,
  invertible step.
- Serialization and the inspector are reflection-driven
  ([ADR-0005](adr/0005-reflection-and-serialization.md)), so a property edit is
  already addressable as `(component, property path, value)` with no per-type
  code.
- Nodes already carry a stable `Uuid`.

Collaboration generalises this. Components gain stable ids; the command stack is
generalised into a serializable `Operation`; a `Session` holds the document and
an ordered, replicated log that every replica applies to converge. An
authoritative session server orders structural operations and resolves
concurrent property edits last-writer-wins per property; presence (cursors,
selections, viewport cameras) rides alongside as ephemeral messages. The first
steps are local and headless — two in-process replicas, no network — which also
makes undo/redo more robust, so they earn their keep before any server exists.
The `net` module is where the transport lives; nothing above it knows whether
the peer is in-process or remote.

## 9. Scripting (planned)

Scripting will be integrated through reflection and the component system:
scripts implement behaviour, access engine objects and components, respond to
events, and expose configurable properties to the inspector. The language and
embedding strategy have not been chosen; the decision and its trade-offs will be
recorded in an ADR before any integration code is written. Native C++ extensions
and custom components are the supported path today.

## 10. Platform layer

Cross-platform support is designed in from the start, not retrofitted. All
platform-specific functionality — window creation, input, filesystem access,
threading, graphics context initialisation — lives behind the `platform` module
and is not scattered through the engine.

The goal is not that the editor runs on several operating systems; it is that a
game built here reaches every platform its players use — see
[ADR-0018](adr/0018-multi-platform-shipping.md) and
[`STRATEGY.md`](STRATEGY.md). That raises the bar on the layer:

- **The contract is complete.** Beyond window and input, it covers touch and
  gamepad input, threads, time, dynamic libraries, and application lifecycle
  (suspend, resume, resize, orientation). A target is a new backend, not a
  scattering of `#ifdef`s.
- **A null backend exists.** A headless implementation of the whole contract —
  no window, no-op input, an in-memory filesystem view — for CI, dedicated
  servers, and deterministic tests. It is the proof that nothing leaks past the
  abstraction, and it builds and passes the suite on any machine.
- **Assets ship in a pack.** Browsers and mobile do not give a game a normal
  filesystem, so the runtime reads assets from a pack through the same resource
  paths it uses on disk. The resource system never assumes loose files. The
  `pack` module holds the container (`PackFile`), the writer that turns a
  project's asset tree and startup scene into one file (`PackWriter`), and the
  mount (`PackSource`) that a `ResourceManager` reads through; the `fork_pack`
  tool reads the project file and drives the writer, so the engine stays
  project-agnostic. See [ADR-0030](adr/0030-asset-pack-format.md).
- **Export templates.** A prebuilt runtime per target plus an export step turns
  a project into a runnable build. This is release infrastructure, separate from
  the library build.
- **Backends on demand.** Desktop is OpenGL 3.3 core; web wants WebGL2/WebGPU,
  mobile wants GLES/Vulkan/Metal, and consoles want their own APIs. A backend is
  written when a target needs it, and never requires touching code above the RHI.

Desktop (Linux, macOS, Windows) is the initial target; web and mobile follow,
and consoles are an architecture goal implemented only when SDK access permits.

## 11. Testing strategy

- **Pure-logic tests** (`fork_engine_tests`) cover math, filesystem, scene,
  components, resources, serialization, input, mesh geometry, 3D scene
  components (camera projection, reflection registration), and frustum culling
  (plane extraction, point/AABB visibility). They run anywhere.
- **Graphics tests** (`fork_graphics_tests`) create a hidden window and GL
  context, render a scene, and read the framebuffer back to assert what was
  actually drawn (pixel values, batching and draw counts, camera clear colour,
  depth ordering, the point/spot lighting paths, and shadow casting). They cover
  both the 2D and 3D renderers. They skip cleanly when no context is available,
  so CI stays green on machines without a GPU. Play-in-editor and the full
  editor host (panels and all) live here too, because both need a context.
- **Editor tests** (`fork_editor_tests`) cover the headless editor logic: the
  hierarchy view model, the document, the play-scene copy, the command stack,
  the property helpers, and the viewport's camera maths and picking. They link
  `fork_editor_core` only, never the UI toolkit.
- Tests exercise real code paths. Mocks are avoided; where one is used, it is
  justified in the pull request.

Every milestone must build and pass its tests before the next one begins.

## 12. What is deliberately not here yet

- The parts of 3D rendering beyond the forward lit pass and directional shadows:
  batching, point/spot shadows, cascaded shadow maps, skeletal animation, a
  spatial hierarchy over static geometry, environment rendering and
  post-processing, and a material graph (see
  [ADR-0010](adr/0010-3d-rendering-foundation.md),
  [ADR-0011](adr/0011-frustum-culling.md),
  [ADR-0012](adr/0012-forward-lighting-model.md), and
  [ADR-0014](adr/0014-directional-shadow-mapping.md)).
- 3D physics, and the parts of 2D physics not in the first cuts: continuous
  collision detection and a rotated broad-phase cell (see
  [ADR-0009](adr/0009-physics2d-system.md),
  [ADR-0013](adr/0013-physics2d-queries-events-angular.md),
  [ADR-0021](adr/0021-physics2d-joints.md),
  [ADR-0023](adr/0023-physics2d-broadphase.md), and
  [ADR-0037](adr/0037-physics2d-oriented-boxes-offsets.md)).
- The parts of audio not in the first cut: 3D positional attenuation, streaming
  and large-file playback, compressed codecs, and audio buses / mixing groups
  (see [ADR-0016](adr/0016-audio-module.md)).
- The editor's asset browser, 3D viewport, scripting workflow, and project
  settings (the editor shell, hierarchy, inspector, 2D viewport,
  play-in-editor, and undo/redo exist; see section 8).
- Tilemaps, particles, animation clips and state machines.
- Scripting.
- Multi-threading in the runtime (the frame loop is single-threaded; the RHI
  contract already states that a device is owned by one thread). The `jobs`
  module and parallel extraction are planned in M9.
- The performance work itself: the profiler, the extraction seam, the job
  system, the frame arena, and instancing are planned in M9 (section 5 describes
  the extraction seam the rest depends on).
- Shipping to platforms beyond desktop: export templates, per-target runtimes,
  and the web/mobile backends are planned in M8 (section 10). The asset-pack
  format and the runtime mount are in place ([ADR-0030](adr/0030-asset-pack-format.md)).
- Collaborative editing: component identity, the operation model, the
  replicated log, the session server, and presence are planned in M10
  (section 8).

These are sequenced in [`docs/ROADMAP.md`](ROADMAP.md).
