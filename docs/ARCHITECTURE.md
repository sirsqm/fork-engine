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
core  <-  serialization  <-  scene
everything  <-  runtime
```

| Module | Responsibility | Depends on |
| --- | --- | --- |
| `core` | Fixed-width types, math, `Error`/`Result`, logging, assertions, UUID, filesystem helpers | — |
| `platform` | Window creation, input, and the graphics context | core |
| `rhi` | Graphics device abstraction and backend selection | core |
| `resources` | Asset loading, caching, lifetime | core, rhi |
| `scene` | Node/component hierarchy, transforms | core |
| `serialization` | JSON persistence of scenes | core, scene, resources |
| `renderer2d` | Batched 2D rendering | core, rhi, resources, scene |
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

**`renderer2d::Renderer2D`** walks a scene, collects sprites, sorts them into a
stable draw order, builds a vertex/index stream in CPU memory, uploads it in
batches, and issues one indexed draw per texture batch. Sprites that share a
texture and layer collapse into a single draw call. A 1×1 white texture stands in
for sprites without one, so an untextured sprite renders as a solid quad.

**3D** (not implemented) will be a separate renderer with its own pass structure
(shadow, depth prepass, opaque, transparent, post-process) sharing the same
`rhi::Device`. The 2D and 3D paths will not be forced into one abstraction; they
have different data and different optima.

### Camera selection

"Which camera is active for this scene?" has exactly one implementation,
`Renderer2D::find_active_camera`, shared by the renderer and the runtime's
clear-colour logic. Two independent answers to that question is precisely the
kind of divergence that produces subtle bugs, so the runtime calls the renderer's
helper rather than reimplementing the rule.

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
  components, resources, serialization, and input. They run anywhere.
- **Graphics tests** (`fork_graphics_tests`) create a hidden window and GL
  context, render a scene, and read the framebuffer back to assert what was
  actually drawn (pixel values, batching counts, camera clear colour). They skip
  cleanly when no context is available, so CI stays green on machines without a
  GPU.
- Tests exercise real code paths. Mocks are avoided; where one is used, it is
  justified in the pull request.

Every milestone must build and pass its tests before the next one begins.

## 12. What is deliberately not here yet

- 3D rendering, lighting, shadows, skeletal animation.
- Physics, audio, animation, tilemaps, particles.
- The editor.
- Scripting.
- Reflection (the serializer currently uses a small per-component switch; a
  registry replaces it once the component set grows, without changing the file
  format).
- Multi-threading in the runtime (the frame loop is single-threaded; the RHI
  contract already states that a device is owned by one thread).

These are sequenced in [`docs/ROADMAP.md`](ROADMAP.md).
