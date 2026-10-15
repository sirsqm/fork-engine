# ADR-0052: The render extraction seam

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0010](0010-3d-rendering-foundation.md),
  [ADR-0042](0042-null-rhi-device.md),
  [ADR-0033](0033-performance-regression-gate.md)

## Context

[ADR-0017](0017-performance-strategy.md) names three capabilities that shape the
engine, and the first is that performance is a product feature. Its concrete
first step is the **extraction seam**:

> Both renderers gain an **extraction** step: one walk of the hierarchy that
> emits a flat array of instances (mesh/sprite, world matrix, material handle,
> visibility). Issuing draws becomes a separate step over that array.

Until now the two renderers walked the scene, decided what was visible, sorted
it, and issued draws in one function each. `Renderer2D::render` and
`Renderer3D::render` both began with a `for_each_node` walk that built a local
`DrawItem`/`Instance` vector and then went straight on to cull and draw. The
scene graph and the flat array existed, but the array was a private local: no
caller could hold it, no test could assert on it, and the culling step could not
be exercised without a GPU.

That shape blocks the work ADR-0017 lists as *following* the seam. Sorting,
batching, instancing, and parallel culling are all operations on the flat array;
if the array only exists inside a `render()` body, none of them has an interface
to attach to. It also made culling untestable without a device, which is exactly
the kind of coupling the null device ([ADR-0042](0042-null-rhi-device.md)) was
added to remove from the draw path.

Constraints that shaped the decision:

- **The scene graph stays the authoring model.** The seam is
  [ADR-0002](0002-hybrid-scene-ecs-architecture.md) applied to the hot path: the
  authoring model and the processing model are deliberately different, and the
  conversion happens in one place. This is not an ECS migration.
- **The RHI device stays single-threaded.** Extraction and culling are CPU work;
  only draw submission touches the device, and it must stay on one thread.
- **Behaviour must not change.** The seam is a refactor of *how* the renderers
  are structured, not what they draw. Every existing renderer test must pass
  unchanged, including the draw-call batching and the shadow pass.

## Decision

Introduce a shared `fe::render` module holding the seam's data types and its
pure steps, and restructure both renderers so `render()` is exactly those steps
in sequence.

**The data types.** `render::SpriteInstance` and `render::MeshInstance` are plain
values: the resource handle to bind, the component the draw reads its material
from, the node's world matrix, and (for a mesh) the mesh's **world-space AABB**.
Baking the world AABB at extraction is what makes culling a pure function of the
array — it needs neither the `Mesh` nor the `Scene`.

**The steps**, exposed as public members so a caller and a test can run one in
isolation:

1. `extract(scene, out)` — one `for_each_node` walk that fills `out`. No GPU
   work. `Renderer3D::extract` also appends every referenced mesh to a
   `meshes_seen` list, which is what its buffer-liveness sweep keys off.
2. `cull(out, frustum)` — a `static` function that erases instances whose world
   AABB lies wholly outside the frustum. A null frustum culls nothing.
3. `draw(device, …)` — sort, build geometry, upload, and issue draws over the
   array it is handed. It does no culling: culling is a separate, explicit step.

`render()` is `extract` → build a frustum from the same view-projection the draw
uses → `cull` → `draw`, so the culling step and the draw step can never disagree
about what is on screen. `last_culled()` is reported alongside `last_draw_calls()`
so `drawn + last_culled()` is the number of extracted instances.

**Why a new module rather than a header in each renderer.** Both renderers share
the same seam and the same two shapes. Putting the types and the pure steps in
`fe::render` gives the follow-on parallel-culling step one array shape to chunk
regardless of which renderer produced it, and it keeps the AABB expansion in one
place instead of a copy in each renderer. `fe::render` sits beside `renderer2d`
and `renderer3d` and depends on `scene` and `scene3d`; the renderers depend on
it, so the arrow points one way.

## Consequences

- Sorting, batching, instancing, and parallel culling now have an interface: each
  becomes a change to one step over one array, not a rewrite of a `render()`
  body. The array is also the unit a job system can chunk.
- Culling is testable with no device and no renderer. The tests hold an extracted
  array, build a frustum, and assert which instances survive — including the
  conservative cases (a straddling box is kept) that a GPU test could not reach.
- `render()` gained a step and the renderers gained a dependency on `fe::render`.
  The two renderers still do not share a draw path: 2D batches by texture, 3D
  draws per instance, and neither is forced into the other's shape.
- A `MeshInstance` carries a world AABB it would not otherwise need, and a
  `SpriteInstance` carries a resolved size. That is the cost of making culling a
  pure function; it is a handful of floats per instance and it removes a
  per-instance expansion from the cull pass.
- The seam is a prerequisite, not a win on its own. It lands with no measured
  frame-time change; the wins are the follow-ons it unblocks, each of which must
  still show a before/after number (ADR-0017, ADR-0033).

## Alternatives considered

- **Keep the array local to each `render()` and add a `cull` flag.** Rejected. It
  leaves the array unobservable, which is the problem being solved: a test could
  not assert on what was extracted, and parallel culling would have nothing to
  chunk.
- **One shared `Renderer` with a 2D and a 3D pass.** Rejected for the reason
  [ADR-0010](0010-3d-rendering-foundation.md) gives: 2D and 3D have different
  data and different optima, and forcing them into one abstraction costs more
  than the seam saves. The seam shares the *array and its steps*, not the draw
  path.
- **Store the mesh's local bounds in the instance and expand during culling.**
  Rejected. It would make culling depend on the `Mesh` and the model matrix, so
  the pass could not run over a bare array — precisely the property that makes it
  unit-testable and chunkable. Expanding once at extraction is cheaper and keeps
  the cull pass pure.
- **Make culling a method on `Scene`.** Rejected. Culling is a view-dependent
  operation over extracted instances, not scene data; putting it on `Scene` would
  tie it to the authoring model the seam is deliberately separating from.
- **Migrate the runtime to a data-oriented ECS now.** Rejected for the reason
  ADR-0017 already gives: it is a large migration for a win the seam captures at
  a fraction of the cost. The hybrid model permits a data-oriented path where it
  pays, and the seam is that path's entry point.
