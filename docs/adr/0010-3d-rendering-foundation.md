# ADR-0010: A 3D rendering foundation on the shared RHI

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

The engine renders 2D through `renderer2d::Renderer2D`, built on the
`rhi::Device` abstraction (ADR-0001, ADR-0003). The roadmap's M4 is 3D: meshes,
materials, cameras, and lights. The first cut has to answer four questions:

1. **Where does 3D live in the module graph?** The 2D renderer sits above
   `rhi`; 3D has more inputs (a mesh resource, a lit material, a camera, a
   light) and must not force the 2D path to grow.
2. **What is the CPU-side mesh, and who owns the GPU buffers?** `Sprite2D` holds
   a `Texture2D` (a GPU-backed `Resource`). A mesh has no single obvious owner.
3. **How does a 3D scene component compose?** Components are capabilities
   attached to a `Node` (ADR-0002). A renderer component needs a mesh, a
   material, and the node's world transform, without inventing a second
   transform.
4. **What does the first lit pass actually do,** given that shadows, batching,
   and culling are explicitly later work?

The engine's rules constrain the answers: modules depend only downwards; an
unnecessary new edge needs an ADR; a system is only "done" when it is tested;
and nothing may be presented as finished that is not.

## Decision

**A new `mesh` module and a new `renderer3d` module, both above `rhi`, beside
`renderer2d`. 3D scene components live in a `scene3d` module that depends only
on `scene` and `resources`.**

### `mesh` — CPU-side geometry

- `Vertex3D` is the interleaved vertex the 3D shader consumes: `vec3 position`,
  `vec3 normal`, `vec2 uv`. It is deliberately fixed; tangents, colour, or bone
  weights will be a second layout and a second shader, not a redesign.
- `Mesh` is a `Resource` holding interleaved vertices, 32-bit indices, and a
  cached local AABB. It is **CPU-only**: it does not know about the GPU. This
  mirrors `Image` (CPU pixels) versus `Texture2D` (GPU texture), and it means a
  mesh can be generated, loaded, and unit-tested with no graphics context.
- Built-in primitive generators (`make_box`, `make_cube`, `make_plane`) let a
  scene be authored and rendered before any asset pipeline exists, and keep
  tests independent of files on disk.
- `ObjLoader` (scheme `obj`) decodes Wavefront OBJ into a `Mesh`. The parser is
  exposed separately from `load()` so it is testable without a file, exactly as
  `ImageLoader` exposes its decoder.

### `renderer3d` — one lit pass

- `Renderer3D` walks the scene, pairs each `MeshRenderer3D` with its node's
  world matrix, and issues one indexed draw per instance. It owns a buffer cache
  keyed by the mesh, uploading on first use and sweeping dead entries at the end
  of each frame. This is the same consumer-of-scene-data shape as `Renderer2D`.
- The vertex layout `position_normal_uv` was added to `rhi::VertexLayout`
  alongside the existing sprite layout. The RHI gained `Shader::set_mat3` and
  `Shader::set_vec3` so the renderer can set a normal matrix and vector
  uniforms; both are thin `glUniform*` wrappers.
- The lit shader is a single directional-light Blinn-Phong pass: albedo ×
  (ambient + light colour × intensity × (diffuse + specular)). It is
  depth-tested and opaque (blending disabled), so draw order does not affect
  the result. The camera position is a uniform, not assumed to be the origin.
- Depth testing is enabled for the pass and the default framebuffer provides a
  depth buffer; an off-screen target used for 3D must be created with
  `depth = true`.

### `scene3d` — composition

- `Camera3D`, `MeshRenderer3D`, and `DirectionalLight3D` are ordinary
  `Component`s. Position and orientation come from the owning `Node`'s
  transform; no component stores its own transform.
- `Camera3D::view_projection` composes `look_at` and `perspective` from
  `fe::Mat4` and guards a degenerate near/far pair.
- Reflection registration lives in `engine/src/scene3d/Scene3DTypes.cpp` behind
  `reflect::register_scene3d_types()`, mirroring `register_physics_types()`. The
  reflection layer stays free of the module; `Engine::initialize` calls it.
  Resource references serialize as stable manager keys
  (`ObjLoader::reference_key`, `Texture2DLoader::reference_key`).

### Module graph

```
core, scene, resources  <-  mesh
core, rhi, mesh, resources, scene  <-  renderer3d
core, scene, resources  <-  scene3d   (components only; no renderer edge)
```

`scene3d` does **not** depend on `renderer3d`. A camera or a light is scene
data; the renderer reads it. This keeps the renderer swappable and lets the
editor hold a 3D scene without a renderer.

## Consequences

- 2D and 3D are independent renderers over one `rhi::Device`. Neither is forced
  into the other's abstraction; they have different data and different optima.
- The mesh is unit-testable with no GPU, and the renderer is tested end-to-end
  by reading pixels back from a real (software) GL context.
- The first cut is deliberately small: one directional light, no shadows, no
  batching, no culling, no materials beyond a base colour + optional texture.
  The interfaces (`Mesh`, `Renderer3D::render`, the components) do not change to
  add those; they are additions behind them.
- `MeshRenderer3D` reuses `ObjLoader::reference_key`, so a mesh reference
  survives serialization and reloads through the ordinary `ResourceManager`
  path, like a sprite's texture key.
- One draw call per instance is slower than a batched path. The mesh buffer
  cache removes the per-frame upload cost, which is the real win at this stage;
  batching is a later revision behind the same interface.

## Alternatives considered

- **Put 3D components in `renderer3d`.** Rejected: it would make scene data
  depend on a renderer, so the editor and a headless tool could not describe a
  3D scene without a graphics device. Components belong in `scene3d`.
- **A GPU-backed `Mesh` that uploads in `Resource::load`.** Rejected: it would
  make meshes untestable without a context and couple asset loading to the RHI.
  The renderer's cache is the seam, matching `Image`/`Texture2D`.
- **One unified renderer for 2D and 3D.** Rejected: the sprite batcher and a
  depth-tested lit pass share almost nothing; a shared abstraction would be
  complexity without benefit (ADR-0001's non-goal).
- **Vulkan-first, or a retained command list.** Rejected for this milestone:
  OpenGL 3.3 core already satisfies the RHI contract, and the RHI is shaped so a
  second backend can implement it later without leaking upward.
- **A full material system now.** Deferred: a base colour, an optional texture,
  and a specular scalar cover the first lit pass; a material graph is a larger
  design that deserves its own ADR.
