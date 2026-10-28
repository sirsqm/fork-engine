# ADR-0101: Instanced 3D draws

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0014](0014-directional-shadow-mapping.md),
  [ADR-0100](0100-render-graph-seam.md),
  [ADR-0042](0042-null-rhi-device.md)

## Context

[ADR-0017](0017-performance-strategy.md) makes performance a product feature and
[ADR-0052](0052-render-extraction-seam.md) split both renderers at an extraction
seam, so the *set* of drawables is a flat array that culling and batching operate
on. That paid off for 2D first: `Renderer2D` uploads a unit quad once and a
per-instance record per sprite, and issues one instanced draw per texture batch,
so a scene of a thousand sprites is a handful of draw calls.

`Renderer3D` did not follow. Every visible instance was its own draw call, with
the model matrix and normal matrix set as uniforms and one `draw_indexed` per
instance. The mesh-buffer cache and the frustum cull (with its BVH,
[ADR-0053](0053-spatial-bvh.md)) removed the per-frame upload and the off-screen
work, but the per-instance draw call remained, and the roadmap recorded it as the
last open item of M9:

> **3D instancing** — `Renderer3D` still issues per-mesh draws, so a scene with
> repeated meshes pays a draw call each. The 2D path already instances; this is
> the 3D half.

A scene built from repeated geometry — a floor of tiles, a forest of one tree, a
crowd, a wall of crates — is the common shape a real 3D game takes, and it is
exactly the shape that pays a draw call per instance. The RHI already had the
mechanism: `bind_instance_buffer` and `draw_indexed_instanced` landed with the 2D
instanced path, and `VertexAttribute::divisor` lets one layout span a per-vertex
and a per-instance stream.

Constraints that shaped the decision:

- **The draw must not change what is on screen.** Instancing is a submission
  change, not a shading change. The lit result of a scene must be identical to
  the per-instance path it replaces, or the tests that assert a pixel's colour
  stop meaning what they meant.
- **The shadow pass draws the same geometry.** `Renderer3D` renders the visible
  instances twice — once into the shadow map
  ([ADR-0014](0014-directional-shadow-mapping.md)), once into the main pass. Both
  walks must keep seeing the same instance set, or a shadow detaches from its
  caster.
- **The null device must still prove submission.** The headless backend
  ([ADR-0042](0042-null-rhi-device.md)) asserts draw-call counts, so the counts
  it reports must be the counts the instanced path really issues.

## Decision

Batch the 3D draw by **(mesh, base-colour texture, emissive texture, per-batch
shading state)** and issue one instanced draw per batch. The per-instance model
matrix and normal matrix travel in a per-instance vertex stream rather than in
uniforms, so one call can cover every instance of a mesh.

Specifically:

- **A per-instance vertex layout.** `VertexLayout::mesh_instance()` adds
  locations 3-9: the model matrix's four columns and the normal matrix's three,
  each a `vec4` with divisor 1. It pairs with `position_normal_uv()` (locations
  0-2) on the same draw, which is what `bind_vertex_buffer`-then-
  `bind_instance_buffer` exists for. Unlike the sprite layout no column is
  dropped: a sprite transform never writes a world z, but a mesh transform moves
  on all three axes.
- **The normal matrix is computed on the CPU.** `transpose(inverse(model))` per
  vertex is real vertex work; the CPU already computes it once per instance, so it
  is packed into the stream. The 4th component of each normal column is padding
  the shader ignores; it keeps every attribute a `vec4` and the stride 16-byte
  aligned.
- **Both vertex shaders read the stream.** The lit shader builds `mat4`/`mat3`
  from the attributes; the shadow shader builds only the `mat4` it needs, so it
  still reads one stream and both passes draw the same batches from the same
  upload.
- **The renderer batches by run, not by sort.** `build_batches` resolves each
  instance's surface and textures once, groups a new instance into the open batch
  when every per-batch field matches and it is contiguous in the instance stream,
  and otherwise opens a new batch. The draw-call count is therefore bounded by the
  number of distinct `(mesh, material)` pairs a scene shows, not by its instance
  count. Runs rather than a global sort, because depth testing makes draw order
  free within the opaque pass, so a sort would buy nothing and cost a reorder.
- **One upload serves both passes.** The instance stream is built and uploaded
  once, before the shadow pass; the shadow pass and the main pass then read the
  same buffer at each batch's offset.
- **The batch key is exact, not approximate.** Two instances share a draw only
  when their resolved base colour, specular, shininess, and emissive are exactly
  equal. A near-miss would shade one instance with another's colour, which is a
  visible defect; a missed batch is only a draw call, which is not.

## Consequences

Easier:

- A scene of repeated meshes is a handful of draw calls. Three cubes of one mesh
  went from three calls to one; the count now tracks distinct meshes and
  materials, which is what a level's content dictates rather than how many copies
  the artist placed.
- The shadow pass inherits the same batching, so the depth pass gets the win too.
- The RHI, the sprite path, and the 3D path now share one instancing story:
  `VertexAttribute::divisor`, `bind_instance_buffer`, and
  `draw_indexed_instanced` are exercised by both renderers, so the backend seam
  has two consumers instead of one.

Harder / to watch:

- **A per-instance attribute the layout forgets is a silent wrong transform.**
  The failure mode instancing invites is every instance drawn with one matrix, so
  the second copy vanishes or overlaps the first. A draw-call count cannot catch
  it, so a test samples two instances at opposite sides of the frame and asserts
  both landed where they belong.
- The normal matrix is CPU work per instance per frame. It is one 3x3 inverse,
  against the per-vertex inverse it replaces; a static scene could cache it, which
  is left until the benchmark asks.
- The instance stream is re-uploaded whole each frame. A scene whose instances
  mostly stand still could upload only what moved; the 2D path re-uploads whole
  too, and both are bounded by the visible instance count.

Not changed:

- The on-screen result. The lit output, the shadow map, and the counters a test
  reads (`last_draw_calls` is the batch count now, the drawn-instance return
  value is unchanged) are the assertions that hold the two paths equivalent.
- Culling, the BVH, and the extraction seam. Instancing is a change to the draw
  step alone.

## Alternatives considered

- **A global sort by material, then instanced draws.** Rejected: the opaque pass
  depth-tests, so draw order is already free, and a sort would reorder the
  instance stream (and so the draw order of equal-depth geometry) for no
  submission benefit over grouping contiguous runs.
- **Per-instance attributes carrying the full material.** Rejected for now: a
  material is a struct of several fields and two texture handles, which does not
  fit the vertex-attribute budget and would make the shader sample per-instance
  uniforms anyway. The batch key is the material, which is what the 2D path does.
- **Keep the model matrix a uniform and instance only the transform.** Rejected:
  a uniform is per-draw, not per-instance, which is the whole reason the current
  code cannot batch. The transform has to be per-instance data or there is no
  batching.
- **A storage buffer of instance records.** Rejected: the engine's targets are
  WebGL2 and GL 3.3, where instanced vertex attributes are the portable
  mechanism and storage buffers are not universally available. The attribute
  stream is what both backends already implement.
- **A compute-generated instance stream.** Rejected: it is a different renderer,
  not a batching change, and WebGL2 has no compute. The CPU-side batch is the
  portable cut.
