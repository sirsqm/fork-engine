# ADR-0011: Conservative frustum culling in the 3D renderer

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

ADR-0010 shipped the first 3D pass deliberately without culling: `Renderer3D`
collected every `MeshRenderer3D` in the scene and issued a draw per instance.
That was the right first cut, but it means a scene pays for meshes that cannot
possibly appear on screen. The roadmap's M4 lists "spatial hierarchy and frustum
culling" as the next performance step.

The decision is narrower than it sounds. Three things had to be settled:

1. **What is the visibility primitive?** A mesh's local AABB already exists on
   `Mesh` (added for exactly this). It has to be tested in world space.
2. **Where does the maths live?** The renderer needs the six frustum planes.
   That is pure linear algebra with no RHI and no scene dependency.
3. **How aggressive is the test?** A per-instance tree or an oriented-box fit
   would cull more, and also risks hiding a mesh that is actually visible.

The engine's rules apply: modules depend only downwards, no speculative
frameworks, and a system is only done when tested.

## Decision

**A `Frustum` value type in `core`, and a conservative AABB test in
`Renderer3D` that rejects instances wholly outside it.**

### `core::Frustum`

- `Frustum::from_view_projection(const Mat4&)` extracts six inward-facing planes
  from a clip-space matrix by the standard row-sum method (left = w + x, right =
  w − x, and so on), then normalises each plane's normal. It is a plain struct
  with a six-plane array; it holds no reference to the matrix.
- Planes use the convention "inside is `dot(normal, p) + d >= 0`", so a point
  test is six dot products and the AABB test below is a max/min over corners.
- `contains_point` and `intersects_aabb` are the two operations. Both are
  allocation-free and inline.
- `Mat4` gained `transposed()` and free functions `perspective`, `look_at`, and
  `inverse` (the latter for the normal matrix) in the same change; `Vec4` gained
  `+`, `-`, and scalar `*` so plane combinations read as vector maths.

### `Renderer3D` culling

- Each instance's world AABB is computed by transforming the mesh's local AABB's
  eight corners and taking min/max. This is exact for an AABB (the extremes of a
  linear map over a box occur at vertices).
- An instance is drawn unless its world AABB lies wholly on the outer side of at
  least one plane. A box that straddles a plane is **kept**.
- `last_culled()` reports how many instances the test rejected, alongside the
  existing `last_draw_calls()`, so behaviour is observable and testable.
- The GPU buffer cache sweep now keys off every mesh seen this frame, culled or
  not. A mesh that is merely off screen keeps its buffers and does not
  re-upload when it returns.

## Consequences

- Culling is **conservative**: it can only save a draw, never hide a visible
  mesh. A false positive (an invisible mesh drawn) is a small waste; a false
  negative (a visible mesh hidden) would be a correctness bug, and the design
  makes it impossible.
- The test is a **flat pass over all nodes**, not a spatial hierarchy. That is
  intentional for this step: it is the leaf operation a BVH or octree would
  call, so adding a tree later changes where the loop lives, not what it does.
  Building a tree now, before there is a workload that needs it, would be
  speculative.
- The per-instance cost is a few dot products plus eight point transforms;
  meshes are the only thing transformed, not every vertex.
- `Frustum` is reusable: a future shadow pass, the editor's own viewport, and a
  scene-bounds helper all want the same six planes. It lives in `core` so they
  can use it without depending on a renderer.

## Alternatives considered

- **Per-vertex or per-triangle culling.** Rejected: far more work per frame for
  a test the GPU's clip stage already does correctly. Culling is about skipping
  *draw calls*, not individual triangles.
- **A BVH/octree now.** Deferred: real value only once static geometry and
  instance counts are large enough that the flat loop dominates. The interface
  here (world AABB vs. frustum) is exactly what a tree node would test.
- **Oriented bounding boxes (OBB).** Rejected: tighter fits buy little for
  roughly axis-aligned game meshes and require a much more involved
  separating-axis test.
- **Sphere bounds.** Rejected for now: an AABB is already cached on `Mesh` and
  gives a tighter fit for boxes and floors; a bounding sphere would be a second
  cached quantity for a marginal test-cost win.
- **Culling in the shader.** Rejected: by the time a vertex shader runs, the draw
  call has already been paid; the point is to avoid issuing it.
