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

Non-goals for now: matching a specific existing engine feature-for-feature,
supporting every platform on day one, and optimising before there is something
worth optimising.

## 2. Module graph

The engine is one static library (`fork_engine`) built from modules. Modules may
depend only "downwards"; the dependency graph is acyclic by construction.

```
core  <-  platform  <-  rhi  <-  resources  <-  scene  <-  renderer2d
core  <-  reflect  <-  scene, resources
core  <-  serialization  <-  scene, resources, reflect
core, scene, resources  <-  systems
core, scene, systems, reflect  <-  physics2d
core, scene, resources  <-  mesh
core, scene, resources  <-  scene3d
core, rhi, resources, scene, mesh  <-  renderer3d
core, scene, systems, resources  <-  audio
everything  <-  runtime
```

| Module | Responsibility | Depends on |
| --- | --- | --- |
| `core` | Fixed-width types, math, `Error`/`Result`, logging, assertions, UUID, filesystem helpers | — |
| `platform` | Window creation, input, and the graphics context | core |
| `rhi` | Graphics device abstraction and backend selection | core |
| `resources` | Asset loading, caching, lifetime | core, rhi |
| `scene` | Node/component hierarchy, transforms | core |
| `reflect` | Component property descriptions and the type registry | core, scene, resources |
| `serialization` | JSON persistence of scenes, driven by reflection | core, scene, resources, reflect |
| `systems` | Per-frame execution: `System`, `SceneSchedule`, `FrameContext`, and the concrete gameplay systems | core, scene, resources |
| `physics2d` | 2D collision shapes and a rigid-body simulation system | core, scene, systems, reflect |
| `mesh` | CPU-side geometry: `Vertex3D`, `Mesh`, primitive generators, the OBJ loader | core, scene, resources |
| `scene3d` | 3D scene components (`Camera3D`, `MeshRenderer3D`, `DirectionalLight3D`) and their reflection | core, scene, resources |
| `audio` | Audio output device, decoded clips, and a mixing system | core, scene, systems, resources |
| `renderer2d` | Batched 2D rendering | core, rhi, resources, scene |
| `renderer3d` | 3D mesh rendering with a lit pass | core, rhi, resources, scene, mesh |
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

Each update integrates velocity (gravity, damping) and position, then runs a
fixed number of sequential-impulse iterations over every pair: a bounding-box
broad phase, a narrow phase (`intersect` for box/box, circle/circle, circle/box),
a normal impulse with restitution, a Coulomb friction impulse, and Baumgarte
positional correction. Positions are written back to `Node::transform()`;
velocities back to the `RigidBody2D`.

The first cut deliberately has **no angular dynamics**, so a `Collider2D` is an
AABB or a circle and a shape is centred on its node. That limit is stated rather
than hidden: an offset that broke under rotation would be worse than no offset.
See [ADR-0009](adr/0009-physics2d-system.md) for the full scope and the
alternatives considered (a third-party library, node subclasses, a full ECS).

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
[ADR-0012](adr/0012-audio-module.md).

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
`color_texture()`. It is also where a future post-processing chain and shadow
passes will attach, which is why it lives in `rhi` rather than in `Renderer2D`.
`Device::begin_frame` always starts on the window's default framebuffer, so a
frame cannot leak into or out of a target.

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
each frame, so a shared mesh is uploaded once. The pass is a single
directional-light Blinn-Phong shader: albedo × (ambient + light colour ×
intensity × (diffuse + specular)). It is depth-tested and opaque, so draw order
does not change the image. The camera position is a uniform; the shader does not
assume the camera is at the origin. A 1×1 white texture stands in for meshes
without one, as in `Renderer2D`.

The 3D scene components (`Camera3D`, `MeshRenderer3D`, `DirectionalLight3D`)
live in `scene3d`, which does **not** depend on `renderer3d`: a camera or a light
is scene data the renderer reads. `Camera3D::view_projection` composes `look_at`
and `perspective`; a component never stores its own transform.

**Culling.** `Renderer3D` rejects instances whose world-space AABB lies wholly
outside the camera frustum before they reach the GPU (ADR-0011). `core::Frustum`
extracts the six planes from a view-projection matrix; the test is conservative
(a box straddling a plane is kept), so it can only save a draw, never hide a
visible mesh. It is a flat pass, not a spatial hierarchy — the leaf operation a
BVH would call.

Batching, shadows, multiple light types, a spatial tree, and a material graph
are later work behind these same interfaces.

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

This is the foundation of the asset pipeline. Import settings, GUID-based
references that survive file moves, and a content-addressed cache build on top of
it without changing the load/cache/release contract.

## 8. Editor (planned)

The editor is designed as a client of the engine, not a second copy of it:

- It embeds an `Engine` and drives frames itself, so the viewport and
  play-in-editor share the exact runtime the game uses.
- It reads and writes scenes through the `scene` and `serialization` modules,
  not through private engine internals.
- The property inspector is driven by the same type information the serializer
  uses. Reflection is the bridge; see [ADR-0005](adr/0005-reflection-and-serialization.md).

The editor is not implemented. Its requirements are being kept in mind so the
runtime interfaces stay editor-friendly, but no editor-facing abstraction is
built until there is an editor to need it.

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
and is not scattered through the engine. Desktop (Linux, macOS, Windows) is the
initial target; the abstraction exists so additional targets are a new backend,
not a rewrite.

## 11. Testing strategy

- **Pure-logic tests** (`fork_engine_tests`) cover math, filesystem, scene,
  components, resources, serialization, input, mesh geometry, 3D scene
  components (camera projection, reflection registration), and frustum culling
  (plane extraction, point/AABB visibility). They run anywhere.
- **Graphics tests** (`fork_graphics_tests`) create a hidden window and GL
  context, render a scene, and read the framebuffer back to assert what was
  actually drawn (pixel values, batching and draw counts, camera clear colour,
  depth ordering). They cover both the 2D and 3D renderers. They skip cleanly
  when no context is available, so CI stays green on machines without a GPU.
- Tests exercise real code paths. Mocks are avoided; where one is used, it is
  justified in the pull request.

Every milestone must build and pass its tests before the next one begins.

## 12. What is deliberately not here yet

- The parts of 3D rendering beyond the first lit pass: shadows, batching,
  point and spot lights, skeletal animation, a spatial hierarchy over static
  geometry, and a material graph (see [ADR-0010](adr/0010-3d-rendering-foundation.md)
  and [ADR-0011](adr/0011-frustum-culling.md)).
- 3D physics, and the parts of 2D physics not in the first cut: angular
  dynamics, collider offsets, joints, continuous collision detection, sleeping,
  and a spatial partition (see [ADR-0009](adr/0009-physics2d-system.md)).
- The parts of audio not in the first cut: 3D positional attenuation, streaming
  and large-file playback, compressed codecs, and audio buses / mixing groups
  (see [ADR-0012](adr/0012-audio-module.md)).
- Tilemaps, particles, animation clips and state machines.
- The editor.
- Scripting.
- Multi-threading in the runtime (the frame loop is single-threaded; the RHI
  contract already states that a device is owned by one thread).

These are sequenced in [`docs/ROADMAP.md`](ROADMAP.md).
