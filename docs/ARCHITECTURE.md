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

Two capabilities shape the architecture as much as they shape the roadmap, and
are planned in [`ROADMAP.md`](ROADMAP.md):

- **Performance is a product feature.** The engine is fast by construction and
  by measurement, not by a late optimization pass. The authoring model (the
  scene graph) and the processing model (flat, cache-friendly arrays) are
  deliberately different, and the conversion happens at one seam — see
  [ADR-0017](adr/0017-performance-strategy.md) and sections 5 and 6.
- **Games ship anywhere.** The target is that a game reaches every platform its
  players use, not that the editor runs on several OSes — see
  [ADR-0018](adr/0018-multi-platform-shipping.md) and section 11.

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
core  <-  jobs
core, scene, resources  <-  systems
core, scene, systems, reflect  <-  anim
core, scene, systems  <-  script
core, scene, systems, platform  <-  script (the module loader; ADR-0061)
core, scene, systems, reflect  <-  physics2d
core, scene, systems, reflect  <-  physics3d
core, scene, resources  <-  mesh
core, scene, resources  <-  scene3d
core, rhi, resources, scene, mesh  <-  renderer3d
core, scene, systems, resources  <-  audio
core, resources  <-  pack
core  <-  jobs
core, scene, serialization  <-  session
core, scene, serialization, session  <-  net
everything  <-  runtime
```

| Module | Responsibility | Depends on |
| --- | --- | --- |
| `core` | Fixed-width types, math, `Error`/`Result`, logging, assertions, UUID, filesystem helpers, frame profiler, frame arena, spatial BVH | — |
| `platform` | Window creation, input, the graphics context, and dynamic libraries (`DynamicLibrary`; `docs/adr/0061-cpp-gameplay-modules-hot-reload.md`) | core |
| `rhi` | Graphics device abstraction and backend selection | core |
| `resources` | Asset loading, caching, lifetime | core, rhi |
| `scene` | Node/component hierarchy, transforms, and the 2D authoring components (`Camera2D`, `Sprite2D`, `SpriteAnimator`, `Tilemap`) | core, resources |
| `reflect` | Component property descriptions and the type registry | core, scene, resources |
| `serialization` | JSON persistence of scenes, driven by reflection, and the serializable `Operation` model (apply/inverse, `docs/adr/0047-local-operation-model.md`) | core, scene, resources, reflect |
| `session` | The local, headless replicated log: `Session` holds a document and an ordered log of `Operation`s, two in-process replicas converge, and undo is log-based (`docs/adr/0048-replicated-operation-log.md`) | core, scene, serialization |
| `systems` | Per-frame execution: `System`, `SceneSchedule`, `FrameContext`, and the concrete gameplay systems | core, scene, resources |
| `anim` | Animation state machines: `AnimationStateMachine` (a named-state graph over sprite clips) and `AnimationStateMachineSystem`, which advances each machine and drives the node's `SpriteAnimator` (`docs/adr/0059-animation-state-machines.md`) | core, scene, systems, reflect |
| `script` | The language-agnostic gameplay seam: `Script`, `ScriptHost` (per-node instances keyed by stable node id), `ScriptRegistry`, and the `ScriptSystem` bridge; plus `ScriptLibrary`, the C++ gameplay-module loader and hot reload (`docs/adr/0061-cpp-gameplay-modules-hot-reload.md`) | core, scene, systems, platform |
| `physics2d` | 2D collision shapes, a rigid-body simulation system (linear and angular) with a uniform-grid broad phase, joints (distance and revolute), queries, contact events, and island-based body sleeping | core, scene, systems, reflect |
| `physics3d` | 3D collision shapes (box, sphere), a rigid-body simulation system (linear and angular: gravity, restitution, friction, damping, torque, inertia) with a uniform-grid broad phase, joints, queries, and contact events | core, scene, systems, reflect |
| `mesh` | CPU-side geometry: `Vertex3D`, `Mesh`, primitive generators, the OBJ loader | core, scene, resources |
| `scene3d` | 3D scene components (`Camera3D`, `MeshRenderer3D`, `DirectionalLight3D`, `PointLight3D`, `SpotLight3D`) and their reflection | core, scene, resources |
| `audio` | Audio output device, decoded clips, and a mixing system | core, scene, systems, resources |
| `pack` | Asset pack container, writer, and the runtime mount that reads a pack like a directory | core, resources |
| `jobs` | Thread pool, `parallel_for`, and a dependency task graph, in one API with a single-threaded mode; consumed by systems and by render extraction (`docs/adr/0068-job-system.md`) | core |
| `render` | The extraction seam: `render::SpriteInstance`/`MeshInstance`, the pure `extract`/`cull` steps both renderers share, the world-AABB expansion culling uses, and the parallel (chunked) extract/cull paths (`docs/adr/0052-render-extraction-seam.md`, `docs/adr/0069-parallel-extraction-and-culling.md`) | core, jobs, scene, scene3d, mesh, resources |
| `debugdraw` | Collider wireframes for a 2D or a 3D scene: a CPU-side `DebugDrawList` and a `DebugRenderer` that draws it as one line list (`docs/adr/0058-editor-physics3d-and-debug-draw.md`) | core, rhi, scene, physics2d, physics3d |
| `net` | The `Transport` seam (in-process loopback for tests, WebSocket planned), an authoritative `SessionServer` that orders every accepted edit, and the `SessionClient` a replica uses (`docs/adr/0049-net-transport-and-session-server.md`) | core, scene, serialization, session |
| `renderer2d` | Batched 2D rendering | core, rhi, resources, scene, render |
| `renderer3d` | 3D mesh rendering with a lit pass, directional shadow map, gradient environment, and an opt-in post-processing chain | core, rhi, resources, scene, mesh, render |
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
  The backend is chosen on `WindowDesc` (`WindowBackend::GLFW` or
  `WindowBackend::Null`), so a second backend is a case in the one factory, not
  a change above the module. The null backend (no window, no context) is what
  lets the engine run with no display; see
  [ADR-0041](adr/0041-platform-contract-and-null-backend.md).

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

`FrameContext` carries `delta_time`, `elapsed_time`, and nullable pointers to the
frame's `ResourceManager`, `InputState`, and `FrameArena`, so a system reads
everything it needs from the frame rather than from globals -- which is what lets
it run headlessly in a test. The arena is the frame's transient buffer: the loop
resets it once per frame before the systems run, so a system can build scratch
data (a flat list, an extraction array) with no per-frame heap churn
([ADR-0054](adr/0054-frame-loop-arena.md)).

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
frame. The `jobs` module is now in place ([ADR-0068](adr/0068-job-system.md)), but
cross-thread aggregation is not built yet: a job that wants its work in the frame
profile still has no way to contribute it, and that remains M9 work.

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
`DistanceJoint3D` constraints, and writes back. It is a **first cut** -- angular
dynamics are in (a body carries an angular velocity, torque, and a shape-derived
isotropic inertia, and the solver integrates its orientation as a quaternion), but
its collider is still axis-aligned, it has one joint type, and it has no islands
or sleeping yet. It is kept behind a component-and-query boundary so a
third-party solver (Jolt) can replace it without touching gameplay or the editor.
See [ADR-0031](adr/0031-physics3d-and-backend-seam.md) for the seam and
[ADR-0043](adr/0043-physics3d-angular-dynamics.md) for the angular cut.

**In the editor**, play-in-editor runs both physics systems: `PlaySession`'s
default schedule registers `Physics2DSystem` and `Physics3DSystem`, and
`EditorContext` registers copy factories for `Collider2D`, `RigidBody2D`,
`Collider3D`, and `RigidBody3D`, so an authored 2D or 3D scene simulates in play
while the authored scene is untouched. See
[ADR-0059](adr/0058-editor-physics3d-and-debug-draw.md).

**Debug drawing** is the `debugdraw` module: `DebugDrawList` collects world-space
lines (collider wireframes for a 2D or a 3D scene) and `DebugRenderer` draws them
as one line list over the world camera. It is a separate module, not a system,
because `physics3d` depends on `systems` and a system naming a `Collider3D` would
close a cycle. The pass uses a new RHI line primitive
(`Device::set_primitive_topology`), which is inert for every existing pass. See
[ADR-0059](adr/0058-editor-physics3d-and-debug-draw.md).

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
  runtime enforces this ordering (see section 7).

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

## 5. Parallelism

The `jobs` module is where the engine's CPU parallelism lives: a `JobSystem`
with a fixed pool of `std::thread` workers, `parallel_for`, and a dependency
`TaskGraph` ([ADR-0017](adr/0017-performance-strategy.md),
[ADR-0068](adr/0068-job-system.md)).

**One API, two modes.** `JobSystem(worker_count)` starts that many workers; the
default `JobSystem()` starts none, and every job then runs inline on the calling
thread. The API is identical in both, so a system that calls `parallel_for` reads
the same in a game, a headless tool, and a test, and a caller never branches on
whether threads exist. The single-threaded mode is what makes the serial
equivalence test possible (run the same call both ways and compare), what a
platform without threads selects, and what lets the module land without changing
any existing run: `Application` creates a pool only when a worker count is set.

**`parallel_for` is serial-equivalent.** `parallel_for(begin, end, body)` splits
the range into contiguous chunks and runs them on the pool, blocking until all are
done. The body runs once per index, exactly once, with the value a serial loop
would give it; only the *order* of calls is unspecified, so the body must not
assume one chunk ran before another, and two indices that write the same memory
are a race the caller avoids (chunk-local scratch, or an index-derived output
slot). The split is deterministic, so a parallel result is directly comparable to
the serial one.

**The submitting thread helps.** While it waits for its batch, the calling thread
runs queued tasks itself. That is what keeps a job that calls `parallel_for` again
from deadlocking against a pool whose workers are all busy waiting: the waiter
works too. The cost is that a nested call adds no parallelism - it runs its work
on the calling thread - which is the right trade for a module whose job is to not
deadlock.

**A graph is a builder, and a bad graph runs nothing.** `TaskGraph` records tasks
and their dependencies; `JobSystem::run` executes it. A task runs only after every
task it depends on has finished, and tasks with no unmet dependency may run in any
order. The graph is validated *before any body runs* - a dependency on an unknown
id, or a cycle, returns an `Error` and runs nothing - so a caller never sees a
partial run. A task body must not throw: an exception escaping a worker terminates
the process, so a body that can fail records the failure itself.

**The device stays single-threaded.** Threads may extract, cull, and compute, but
only the thread that owns the frame submits to the RHI. `FrameContext::jobs` is
nullable like `resources`/`input`/`arena`, and it carries this rule: a job must
not touch the device. The module cannot enforce what a body touches, so this is a
documented rule and review is the guard, as it is for the frame arena's lifetime
rule.

**Testing.** The pure-logic suite includes the jobs tests, which start real
threads, and CI runs them under ThreadSanitizer (`FE_ENABLE_TSAN`). Only
`fork_engine_tests` runs there: TSan instruments every thread, so the GL binaries
report races entirely inside Mesa/llvmpipe, GLFW, and X11, the same way the ASan
job excludes their leaks.

## 6. Rendering

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
also what the 3D renderer's post-processing chain attaches to
([ADR-0057](adr/0057-environment-and-post-processing.md)), which is why it lives
in `rhi` rather than in `Renderer2D`. `Device::begin_frame` always starts on the
window's default framebuffer, so a frame cannot leak into or out of a target.

**Primitive topology.** `Device::set_primitive_topology` chooses how the next
indexed draw assembles its indices: `Triangles` (the default, what every
existing pass uses) or `Lines`. It is device state rather than a parameter on
`draw_indexed`, so the draw signature is unchanged and no existing pass is
touched. A debug pass that draws a line list is the first user
([ADR-0059](adr/0058-editor-physics3d-and-debug-draw.md)); a `VertexLayout::position_color`
carries a per-vertex colour for it.

**`renderer2d::Renderer2D`** runs the extraction seam (section 6, below):
it extracts the scene's sprites into a flat array, culls it against the view
frustum, sorts the survivors into a stable draw order, builds a vertex/index
stream in CPU memory, uploads it in batches, and issues one indexed draw per
texture batch. Sprites that share a texture and layer collapse into a single draw
call. A 1×1 white texture stands in for sprites without one, so an untextured
sprite renders as a solid quad.

**Tilemaps** draw through that same path. `Tilemap` is a grid of tile indices
over a tileset texture; extraction emits one `SpriteInstance` per non-empty cell
into the same array the sprites fill, so a tile is a quad, a tile is culled by
the sprite quad test, and a map batches with the sprites by `(layer, texture)`. A
100×100 level is one component and one draw call per texture per layer, not ten
thousand nodes ([ADR-0062](adr/0062-tilemaps.md)).

**3D** is a separate renderer with its own pass structure, sharing the same
`rhi::Device` (see [ADR-0010](adr/0010-3d-rendering-foundation.md)). The 2D and
3D paths are not forced into one abstraction; they have different data and
different optima.

**`mesh::Mesh`** is a CPU-only `Resource`: interleaved `Vertex3D`
(position/normal/uv), 32-bit indices, and a cached local AABB. It knows nothing
about the GPU, exactly as `Image` knows nothing about textures, so geometry can
be built and tested with no context. `make_box`/`make_cube`/`make_plane` give a
scene something to render before an asset pipeline exists, and `ObjLoader`
(scheme `obj`) decodes Wavefront OBJ into a `Mesh`. `GltfLoader` (scheme `gltf`)
decodes glTF 2.0 — both the JSON `.gltf` form and the binary `.glb` container —
into one `Mesh` per primitive; it reads through `ResourceSource` like every other
loader, so a mounted pack is served unchanged
([ADR-0040](adr/0040-gltf-importer.md)).

**`renderer3d::Renderer3D`** runs the same seam: it extracts each
`MeshRenderer3D` with its node's world matrix and the mesh's world-space AABB
into a flat array, culls it, then issues one indexed draw per instance. It owns a
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

**Culling.** Both renderers reject instances whose world-space AABB lies wholly
outside the camera frustum, as the seam's culling step, before they reach the GPU
(ADR-0011, ADR-0052). `core::Frustum` extracts the six planes from a
view-projection matrix; the test is conservative (a box straddling a plane is
kept), so it can only save a draw, never hide a visible instance. `last_culled()`
reports how many instances the step dropped.

The 3D renderer culls through a spatial hierarchy: `Renderer3D` builds a
`core::SpatialBvh` (ADR-0053) over the extracted instances' world AABBs and
queries it, so a rejected subtree costs one box test instead of one per primitive
below it ([ADR-0056](adr/0056-renderer3d-spatial-cull.md)). The tree is rebuilt
only when an instance's world bounds change — the renderer caches a compact key
per instance and compares — so a moving camera over static geometry builds once
and queries every frame; `bvh_rebuilds()` reports the build count. The tree calls
the identical conservative leaf test and returns its hits in extraction order, so
the visible set and the rendered image are unchanged; the flat pass is kept as
`cull_flat` and is the reference the tree path is tested against. The 2D renderer
keeps the flat pass, which suits its small, highly dynamic quad set.

**Shadows.** When the scene has a directional light, `Renderer3D` runs a
depth-only pass before the main pass, rendering the culled instances into a
1024² sample-depth target (ADR-0014). The light-space matrix is an orthographic
projection fitted to the culled instances' world bounds, sized from their
bounding sphere so coverage is stable as the camera moves. The main pass samples
that depth with a 3×3 PCF filter; fragments outside the map are treated as lit,
and a slope-scaled bias removes self-shadowing acne. Only the directional light
casts; ambient stays unshadowed so occluded areas keep fill. `last_shadow_casters()`
reports the depth-pass draw count.

Batching, point/spot shadows, and a material graph are later work behind these
same interfaces.

**Environment and post-processing.** A camera can enable a full-screen gradient
background (`background_enabled` with a top/bottom colour pair), drawn on a
clip-space quad before the geometry with depth testing off, so it fills the frame
without occluding or tinting anything ([ADR-0057](adr/0057-environment-and-post-processing.md)).
It is a screen-space vertical gradient, the cheapest environment that reads as a
sky and needs no cubemap; a cubemap sky is a later step behind the same
`draw_background` call site.

`render()` takes an optional `renderer3d::PostProcessSettings`. When enabled, the
3D pass renders into a lazily created off-screen colour target and
`renderer3d::PostProcess` grades that image - exposure, an optional Reinhard
tonemap, and a vignette - into the caller's target. The window's default
framebuffer cannot be sampled as a texture, so the off-screen target is what makes
grading possible on the window at all. When the settings are disabled nothing
changes: the pass draws straight to the caller's target and no target is
allocated. `post_process_frames()` and `background_draws()` report that each pass
ran, so a test proves submission without reading back pixels.

### The extraction seam

Both renderers are structured as the extraction seam
([ADR-0017](adr/0017-performance-strategy.md),
[ADR-0052](adr/0052-render-extraction-seam.md)). `render()` is three steps, and
the first two are public so a caller or a test can run one in isolation:

- **Extract** — one walk of the hierarchy that fills a flat array of instances.
  `render::SpriteInstance` carries the texture, the resolved quad size, the UV
  sub-rectangle, the tint, the layer, and the world matrix — every value a draw
  reads, resolved here so the draw step never reaches back into the scene. Its
  `sprite` and `tilemap` pointers record which component it came from (exactly
  one is non-null), for provenance only. `Renderer2D::extract` fills one array
  from both sources: `extract_sprites`, then `extract_tiles`, which turns every
  non-empty cell of every `Tilemap` into one instance
  ([ADR-0062](adr/0062-tilemaps.md)). `render::MeshInstance` carries the mesh,
  its renderer component, the world matrix, and the mesh's world-space AABB.
  Extraction does no GPU work. `Renderer3D::extract` also records every mesh seen
  (culled or not), which its buffer-liveness sweep keys off.
- **Cull** — drops instances whose world AABB lies wholly outside the frustum.
  It is a pure function of the array: the world AABB is baked in at extraction, so
  culling needs neither the `Mesh` nor the `Scene`. A null frustum culls nothing.
  `Renderer3D::cull` queries its cached BVH (ADR-0056); `Renderer3D::cull_flat`
  and `Renderer2D::cull` are the flat, hierarchy-free passes.
- **Draw** — sort, build geometry, upload, and issue draws over the array it is
  handed. It does no culling; that is a separate, explicit step.

`render()` runs the steps in that order and builds the frustum from the same
view-projection the draw uses, so culling and drawing can never disagree about
what is on screen. `drawn + last_culled()` is the number of extracted instances.

The flat array is the unit a job system chunks and the unit instancing and a
render graph build on. The authoring model (the scene graph) and the processing
model (the array) are deliberately different, and this seam is the only place
they meet. It is the concrete form of the hybrid architecture
([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md)) in the hot path.

### Parallel extract and cull

The seam chunks across the job pool ([ADR-0068](adr/0068-job-system.md)) when a
pool is supplied and the array is large enough to pay for the fan-out
([ADR-0069](adr/0069-parallel-extraction-and-culling.md)). `render::extract_*`
and `render::cull_*` take a trailing `JobSystem* jobs = nullptr`; the renderers
thread it through and `Application::render_frame` supplies its own, so an
interactive run is parallel and every other caller is unchanged.

Output is identical to the serial path by construction, not by luck. Extraction
flattens the hierarchy into a node list, splits it into contiguous chunks, and
has each chunk append to its own vector; the vectors are concatenated in chunk
order, so the fan-out order cannot change the result. Culling tests visibility in
parallel into a position-indexed byte mask, then compacts in one serial pass, so
the survivors keep their original relative order.

Two measured thresholds keep the fan-out off where it does not pay
(`kParallelExtractThreshold` = 1024, `kParallelCullThreshold` = 16384). Extraction
crosses over around 1200 nodes and is a clean win above that; culling is a *loss*
until roughly 16000 instances, because the per-instance test is cheap and the
compaction — not the visibility test — is the bottleneck. Below its threshold a
step stays serial even when a pool is present. `Renderer3D::cull` keeps its BVH,
which is already sub-linear, so a pool does not change it.

### Camera selection

"Which camera is active for this scene?" has exactly one implementation per
renderer, shared by that renderer and the runtime's clear-colour logic.
`Renderer2D::find_active_camera` and `Renderer3D::find_active_camera` each encode
the rule for their scene (first enabled camera, else the first camera). Two
independent answers to that question is precisely the kind of divergence that
produces subtle bugs, so the runtime calls the renderer's helper rather than
reimplementing the rule.

## 7. Subsystem lifetime

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

Creation is conditional on there being a surface. A headless engine
(`WindowDesc::backend = WindowBackend::Null`) has no graphics context, so
`Engine::initialize` creates the window, the resource manager, the loaders and
the audio system, but **skips the device and both renderers**. `has_graphics()`
reports which path was taken; everything that renders checks it and does nothing
when it is false, so a null window is never asked to draw. The rest of the loop —
the clock, input edges, the systems schedule — runs unchanged. See
[ADR-0041](adr/0041-platform-contract-and-null-backend.md).

Because scene components can own GPU-backed resources (a `Sprite2D` holds a
`Texture2D`), `Application` releases the scene **before** shutting the engine
down. Releasing a texture after the GL context is destroyed is undefined
behaviour; the ordering rule is enforced in `Application::run()` and documented
at the call site. A headless run has no GPU-backed resources to release, so the
same ordering is simply harmless there.

## 8. Resources

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

Change detection and reload reuse that contract rather than bending it. Each
cache entry remembers the file its resource was loaded from and that file's write
time; `poll_changed()` returns the references whose file changed, and `reload()`
re-loads a named one and returns the fresh instance for the caller to adopt and
re-wire. Detection and adoption are separate because the cache is weak: an
instance swapped in with no caller to hold it would be released at once. See
[ADR-0046](adr/0046-resource-change-detection-and-reload.md).

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

## 9. Editor

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
| `fork_editor_core` | Document (`EditorContext`), scene hierarchy view model, asset browser view model (`AssetBrowserViewModel`), play-in-editor (`PlaySession`), the script catalogue (`ScriptCatalog`), the headless property helpers (`PropertyEditor`), and the headless viewport maths and picking (`Viewport`). No toolkit, no window. | `fork_engine` |
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

### Stable identity and the local operation model

Networked collaborative editing — several people editing one project live over a
shared server — was once planned as a milestone and is no longer: it needs a
hosted authoritative service, and that running cost is not one the project will
carry. The local steps taken toward it are shipped and kept, because they stand
on their own merit:

- Every edit goes through the command stack rather than mutating the scene
  ([ADR-0015](adr/0015-editor-undo-redo.md)), so an edit is already a discrete,
  invertible step, and every local edit now builds a serializable `Operation`
  ([ADR-0047](adr/0047-local-operation-model.md)) that names its objects by
  stable id.
- Serialization and the inspector are reflection-driven
  ([ADR-0005](adr/0005-reflection-and-serialization.md)), so a property edit is
  already addressable as `(component, property path, value)` with no per-type
  code.
- Nodes already carry a stable `Uuid`, and components carry one too
  ([ADR-0045](adr/0045-component-identity.md)).

Together these give the editor durable object identity and a serializable,
invertible edit — which is what makes undo/redo robust across a save and load,
independent of any network. A `Session` holding the document and an ordered log
is present too ([ADR-0048](adr/0048-replicated-operation-log.md)), as is the
`net` module's transport seam and in-process session server
([ADR-0049](adr/0049-net-transport-and-session-server.md)); they are exercised by
their tests and are available to anyone who wants to build a networked editor,
but the project does not pursue that further.

## 10. Scripting

Gameplay behaviour attaches to a node as a `Script`, owned and driven by a
`ScriptHost`, behind a seam that never names a concrete language. C++ is the
first language, built directly against the engine; the seam exists so a second
language can be added later without the engine core naming it. See
[ADR-0044](adr/0044-scripting-seam.md).

**`Script` is a sibling of `System`, not a `Component`.** The distinction is the
point:

- A `System` ([ADR-0008](adr/0008-systems-and-frame-schedule.md)) is
  engine-shipped, data-oriented behaviour that owns its own traversal of the
  scene. Animation, physics, and audio are systems. It is deliberately not a
  per-node callback.
- A `Script` is game-specific behaviour attached to one node. It is how a game
  adds "this door opens when the player has the key" without editing the engine.
  It is per-node (so the editor can attach it), but it is *driven* by a system,
  so execution still belongs to the schedule.

A script is not a component: components are engine data
([ADR-0002](adr/0002-hybrid-scene-ecs-architecture.md),
[ADR-0005](adr/0005-reflection-and-serialization.md)); scripts are game code.
Keeping them separate is what stops the per-node `update()` model ADR-0002
rejected from returning through the component system. A script's *fields* are
still reflected — a registered `ScriptDesc` lists them exactly as a component's
`TypeDesc` does — so they persist and appear in the inspector, but the script
instance itself is owned by a `ScriptHost`, not stored in the scene
([ADR-0063](adr/0063-reflection-backed-script-properties.md)).

The module is `script` (`engine/include/fe/script/`). Its seam headers are
header-only and depend only on `core`, `scene`, and `systems`; the module loader
(`ScriptLibrary.cpp`) adds one edge, to `platform`, for `DynamicLibrary`:

- **`Script`** — a per-node unit of behaviour: `attach(Node&)`,
  `detach(Node&)`, `update(const FrameContext&)`, a `name()`, and an `enabled`
  flag. It reads the frame's context the way a system does, so it is testable
  headlessly.
- **`ScriptHost`** — owns the live instances, keyed by the node's stable `Uuid`
  — the same identity the serializer persists and the operation model
  ([ADR-0047](adr/0047-local-operation-model.md)) names its objects by. One script
  per node; attaching replaces the previous one. The host is the boundary where
  a failing script is contained: an error returned from `update()`, or an
  exception thrown from it, is logged with the node's id and the script's name,
  the script is disabled, and the rest keep running and the frame keeps going
  ([ADR-0003](adr/0003-error-handling.md)).
- **`ScriptRegistry`** — maps a behaviour name to a factory, so a script can be
  created by name. This is what the editor and a future loader need; it mirrors
  the shape of `reflect::Registry` ([ADR-0006](adr/0006-component-reflection.md))
  but is a value the caller owns, not a process global.
- **`ScriptSystem`** — an ordinary `System` that drives a `ScriptHost` from the
  `SceneSchedule`. This is the whole bridge to the frame loop: scripts run in
  the sandbox, in play-in-editor, and in a headless test through the identical
  path, with no new execution model and no editor or RHI dependency.

A script's `update()` must not attach or detach scripts; those are structural
changes made between frames.

**C++ gameplay modules.** A module is a shared library that registers behaviours
through a versioned entry point (`fe/script/ScriptModuleApi.h`: the exported ABI
version and `fe_script_module_register`). `ScriptLibrary` opens it through
`platform::DynamicLibrary`, checks the ABI version before calling into it, and
records which behaviours it added; `attach()` builds a script from the registry
and binds it through the ordinary `ScriptHost`. Hot reload destroys the module's
live scripts first — detaching while its code is still loaded — then unloads the
image, loads the replacement, and rebinds a fresh script to each node that had
one, so no instance ever outlives the code it runs
([ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)). The library also
detects a rebuild: it records each image's write time at load and
`poll_changed()` reports the ids whose image changed, the same poll-driven
"detect, then adopt" shape as the resource manager
([ADR-0046](adr/0046-resource-change-detection-and-reload.md)). There is no OS
watch; a caller decides how often to ask. The poll also debounces a build
([ADR-0067](adr/0067-debouncing-a-module-build.md)): a module is reported only
once its image's write time has held across a requested number of consecutive
polls (`settle_polls`), so a linker still writing the file is not read mid-write.
That matters because a failed reload drops the module (ADR-0061), so reading a
partial image would lose the working code for a change that was never real.

**Landed since**: reflection-backed script bindings. A registered `ScriptDesc`
exposes a script's fields to the inspector and the serializer, and
`serialization::ScriptBinding` — implemented by `fe::script::ScriptBinding` —
persists which behaviour runs on which node and with what property values
([ADR-0063](adr/0063-reflection-backed-script-properties.md)). A saved scene now
comes back scripted, and every reflected script field — a scalar or a resource
reference — is an undoable `SetScriptProperty`/`SetScriptResource` operation.

**In the editor**, the document owns a `ScriptCatalog`
(`editor/ScriptCatalog.h`) — a `ScriptLibrary` (which owns the loaded images and
the `ScriptRegistry` describing them) paired with the `ScriptHost` that holds the
live instances — so the inspector can attach, replace, and detach a behaviour and
`EditorContext` can persist the bindings by threading a `script::ScriptBinding`
over the catalog through save and load. Loading a different scene clears the
catalog, so no script outlives the nodes it was bound to. The catalog is headless
and lives in `fork_editor_core`, so script editing is covered by the ordinary
editor tests.

**Hot reload, driven from the editor** ([ADR-0066](adr/0066-editor-driven-module-hot-reload.md)):
`EditorHost` loads the modules named on the command line (`--module`) before any
scene opens, then polls them once per frame, between frames and never from inside
a script's `update()`. A module whose image was rebuilt is reloaded and the nodes
it scripted are rebound. Because a scene opened in the editor attaches its scripts
through the serializer rather than through the library, the document hands the
library the bindings a load produced (`adopt_bindings()`), so a reload rebinds
those nodes too. The editor's `ScriptCatalog` declares its library before its host
so the host — holding scripts whose code lives in the images — is destroyed first.

**Not in this cut** (named so they are planned, not forgotten): preserving a
script's state across a reload, and debugging support. These are the remaining M7
rows; the seam they attach to is the one above and does not change to accommodate
them.


## 11. Platform layer

Cross-platform support is designed in from the start, not retrofitted. All
platform-specific functionality — window creation, input, filesystem access,
threading, graphics context initialisation — lives behind the `platform` module
and is not scattered through the engine.

The goal is not that the editor runs on several operating systems; it is that a
game built here reaches every platform its players use — see
[ADR-0018](adr/0018-multi-platform-shipping.md) and
[`ROADMAP.md`](ROADMAP.md). That raises the bar on the layer:

- **The contract is complete.** Beyond window and input, it covers touch and
  gamepad input, threads, time, dynamic libraries, and application lifecycle
  (suspend, resume, resize, orientation). A target is a new backend, not a
  scattering of `#ifdef`s. [ADR-0041](adr/0041-platform-contract-and-null-backend.md)
  audits the contract and records, concern by concern, what is implemented and
  where it lives; the deferred rows (gamepad, suspend/resume) are named there so
  they are planned rather than forgotten. Touch and gamepad attach to the
  existing `InputState` seam — a backend translates native events into
  `on_key`-style calls — so they need no new interface. Touch has landed as
  `InputState::on_touch` plus id-keyed `Began`/`Moved`/`Ended` queries, with the
  same per-frame `begin_frame()` edge semantics as the mouse path; no backend
  feeds it yet. Dynamic libraries are now implemented: `platform::DynamicLibrary`
  opens an image, resolves a symbol by name, and closes it, with `dlopen`/`dlsym`
  and `LoadLibrary`/`GetProcAddress` the only native APIs the engine names. It is
  what loads a C++ gameplay module ([ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)).
- **A null backend exists.** A headless implementation of the window and input
  part of the contract — no OS window, no graphics context, no-op events — for
  CI, dedicated servers, and deterministic tests. It is selected with
  `WindowDesc::backend = WindowBackend::Null` and is the proof that nothing
  leaks past the abstraction: the engine's frame loop, the systems schedule, and
  the tests run on a machine with no display. The `fork_headless` example is the
  executable form of that claim. A no-op RHI device (so the render extraction
  itself can run headless) is the recorded next step, not part of this one. See
  [ADR-0041](adr/0041-platform-contract-and-null-backend.md).
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

## 12. Testing strategy

- **Pure-logic tests** (`fork_engine_tests`) cover math, filesystem, scene,
  components, resources, serialization, input, mesh geometry, 3D scene
  components (camera projection, reflection registration), frustum culling
  (plane extraction, point/AABB visibility), and the job system (serial
  equivalence, reentrancy, graph ordering). They run anywhere. This suite is
  also what runs under ThreadSanitizer (`FE_ENABLE_TSAN`, ADR-0068), because it
  starts real threads and needs no display.
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

## 13. What is deliberately not here yet

- The parts of 3D rendering beyond the forward lit pass and directional shadows:
  batching, point/spot shadows, cascaded shadow maps, skeletal animation, a
  cubemap/atmospheric sky (the gradient environment is in place), a multi-pass
  effect stack (exposure/tonemap/vignette is in place), and a material graph (see
  [ADR-0010](adr/0010-3d-rendering-foundation.md),
  [ADR-0011](adr/0011-frustum-culling.md),
  [ADR-0012](adr/0012-forward-lighting-model.md),
  [ADR-0014](adr/0014-directional-shadow-mapping.md), and
  [ADR-0057](adr/0057-environment-and-post-processing.md)). The spatial hierarchy
  over static geometry is in place and wired: `SpatialBvh` (ADR-0053) backs
  `Renderer3D`'s cull pass (ADR-0056).
- The parts of physics not in the first cuts: continuous collision detection, a
  rotated broad-phase cell, oriented 3D colliders, and 3D islands / sleeping.
  3D physics is in place and integrated with the editor, and collider debug
  drawing exists for both 2D and 3D scenes (see
  [ADR-0009](adr/0009-physics2d-system.md),
  [ADR-0013](adr/0013-physics2d-queries-events-angular.md),
  [ADR-0021](adr/0021-physics2d-joints.md),
  [ADR-0023](adr/0023-physics2d-broadphase.md),
  [ADR-0037](adr/0037-physics2d-oriented-boxes-offsets.md),
  [ADR-0031](adr/0031-physics3d-and-backend-seam.md),
  [ADR-0043](adr/0043-physics3d-angular-dynamics.md), and
  [ADR-0059](adr/0058-editor-physics3d-and-debug-draw.md)).
- The parts of audio not in the first cut: 3D positional attenuation, streaming
  and large-file playback, compressed codecs, and audio buses / mixing groups
  (see [ADR-0016](adr/0016-audio-module.md)).
- The editor's asset browser, 3D viewport, scripting workflow, and project
  settings (the editor shell, hierarchy, inspector, 2D viewport,
  play-in-editor, and undo/redo exist; see section 9).
- The parts of 2D authoring beyond tilemaps: particles, tile collisions,
  mixed tile sizes and tile flags (flip/rotate), and multiple tilemap layers
  per map (see [ADR-0062](adr/0062-tilemaps.md)).
- The scripting seam and the C++ path exist (section 10,
  [ADR-0044](adr/0044-scripting-seam.md)): C++ gameplay modules load and
  hot-reload through `ScriptLibrary`
  ([ADR-0061](adr/0061-cpp-gameplay-modules-hot-reload.md)), a script's
  reflected properties persist through the scene serializer, and the editor
  attaches, inspects, and persists script bindings through its `ScriptCatalog`,
  polling the loaded modules each frame and hot-reloading a rebuilt one
  ([ADR-0063](adr/0063-reflection-backed-script-properties.md),
  [ADR-0066](adr/0066-editor-driven-module-hot-reload.md)).
  What is not here yet is debouncing a build before a reload, preserving a
  script's state across a reload, debugging, and any embedded or custom
  language. These are the remaining M7 rows, and none of them changes the seam.
- Multi-threading in the runtime (the frame loop is single-threaded; the RHI
  contract already states that a device is owned by one thread). The `jobs`
  module is in place (section 5, [ADR-0068](adr/0068-job-system.md)); parallel
  extraction is the next M9 row.
- The performance work itself: the extraction seam is in place (section 6,
  [ADR-0052](adr/0052-render-extraction-seam.md)); the profiler, the benchmark
  gate, and the frame arena are in place -- the `core` primitive
  ([ADR-0051](adr/0051-frame-arena.md)) and its wiring into the frame loop, which
  owns one arena, resets it each frame, and hands it to systems through
  `FrameContext` ([ADR-0054](adr/0054-frame-loop-arena.md)); the job system is
  in place too (section 5, [ADR-0068](adr/0068-job-system.md)), so parallel
  culling, instancing, and moving the extraction seam onto the arena are what
  remain in M9.
- Shipping to platforms beyond desktop: export templates, per-target runtimes,
  and the web/mobile backends are planned in M8 (section 11). The asset-pack
  format and the runtime mount are in place ([ADR-0030](adr/0030-asset-pack-format.md)).
- Stable component identity and the local operation model are in place, which
  is what makes undo/redo survive a save and load (section 9). Networked
  collaborative editing is not planned.

These are sequenced in [`docs/MILESTONES.md`](MILESTONES.md).
