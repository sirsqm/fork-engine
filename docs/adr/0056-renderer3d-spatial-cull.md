# ADR-0056: A spatial-hierarchy cull in the 3D renderer

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0011](0011-frustum-culling.md),
  [ADR-0017](0017-performance-strategy.md),
  [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0053](0053-spatial-bvh.md),
  [ADR-0003](0003-error-handling.md)

## Context

[ADR-0011](0011-frustum-culling.md) added frustum culling to the 3D renderer: each
instance's world AABB is tested against the six view planes, and an instance is
dropped only when its box lies wholly outside the view. That test is
conservative — a box that straddles a plane is kept — so culling can only ever
save a draw, never hide a visible mesh. It was correct and it was flat: the
renderer visited **every** instance's box, one `intersects_aabb` call each.

[ADR-0052](0052-render-extraction-seam.md) split rendering into extract (a flat
`render::MeshInstance` array, each carrying its precomputed world AABB) and cull
(a pure pass over that array) and draw. That seam is what made culling a pure
function of an array, and it is exactly the shape a spatial index wants.

[ADR-0053](0053-spatial-bvh.md) landed `core::SpatialBvh`: a binned-SAH binary
BVH over world-space AABBs that answers frustum queries and reuses
`Frustum::intersects_aabb` verbatim as its leaf test. It landed as a tested
primitive and said so plainly — *"Wiring `Renderer3D` to build and query a tree
in place of its flat cull pass is the follow-on."* The M4 roadmap row
("a spatial hierarchy over static geometry") was left open on exactly that note.

The forces: the flat pass is `O(n)` per frame in the number of drawables, and a
scene with thousands of static meshes pays that whether or not most are off
screen. A BVH turns a rejected *subtree* into one box test instead of one test
per primitive below it. But a tree has to be built, and a build is `O(n log n)`;
if it is rebuilt every frame it can cost more than the flat pass it replaces. The
question this record settles is not *whether* to cull through a tree, but **when
the tree is (re)built, and how the tree path is kept provably identical to the
flat one.**

## Decision

**`Renderer3D` culls through a cached `SpatialBvh` built from the instances'
world AABBs, rebuilt only when those AABBs change; the flat pass is kept as
`cull_flat` and is the reference the tree path is tested against.**

### The tree culls the same set; only the work changes

The BVH's frustum query calls the identical conservative
`Frustum::intersects_aabb` leaf test the flat pass used, and its output is sorted
by primitive index — the extraction order. So the visible set, its order, and
therefore the rendered image are bit-for-bit what they were. The tree changes how
the set is *found*, not which set it is. `cull_flat` is retained (and still
static) precisely so this claim is testable: a test builds a grid of boxes, culls
it both ways, and asserts the same count dropped and the same survivors in the
same order.

### Rebuild on a change in geometry, not per frame

A BVH over *static* geometry is worth building once. The renderer caches the
tree plus a compact per-instance key (`MeshInstanceKey`: the mesh pointer and the
world AABB). Each cull compares the live keys to the cached ones; if they are
equal the tree is reused, and only a change triggers a rebuild. The key is exact,
not approximate — a rebuild is cheap relative to a wrong cull, so a float that
moved by one ulp counts as a change. A moving camera over static geometry
therefore builds the tree once and queries it every frame; a scene where geometry
moves rebuilds when it moves. This is the property that makes the tree a win
rather than a cost, and it is asserted directly: five frames over an unchanged
scene report one build; moving one instance reports a second.

### The decision is a pure function, so it is testable with no scene

The rebuild decision depends only on the key list, not on the scene, the frame,
or the GPU. A test drives `cull` with hand-built instance arrays and asserts on a
`bvh_rebuilds()` counter, with no window and no GL context — the same headless
discipline ADR-0052 established for the seam and ADR-0053 for the tree.

### Buffers are reused; the steady state does not grow

The key, bound, hit, and visibility buffers are members reused across frames, and
`SpatialBvh::build` reuses its own node and scratch arrays (ADR-0053). A
steady-state frame — same geometry, new camera — does not reallocate. This is the
same "no per-frame churn" goal ADR-0017 names, and the reason the cache is a
member rather than a local.

### A null frustum still culls nothing, and does not touch the cache

`cull(instances, nullptr)` is the explicit "draw everything" case; it returns 0
and leaves the cache untouched, so a caller that skips culling does not force a
rebuild it will not use.

## Consequences

**Easier:**

- A scene with many static meshes culls in `O(log n)` node tests plus the leaves
  that survive, instead of `O(n)` box tests, with no change to the visible set.
- The tree is built once for static geometry. The cost the flat pass paid every
  frame is now paid on change.
- The equivalence is pinned by a test, not argued. `cull_flat` is a permanent
  reference, so a future change to the tree path that altered the visible set
  fails a test rather than a screenshot.
- The decision surface is a counter. `bvh_rebuilds()` lets a test prove the cache
  works without a timing or a profiler.

**Harder:**

- `Renderer3D` now holds tree state. It is not a pure function of the array any
  more — the same input can take the cached or the rebuild path depending on what
  came before. That is the price of caching, and the counter plus the flat
  reference keep it observable.
- The key is `O(n)` to compare each frame. That is strictly cheaper than the
  flat cull it replaces, but it is not free; a scene with tens of thousands of
  static instances could make the compare itself the cost. A hash or a
  scene-change signal would remove it, and is the natural next step if a profile
  asks for it.
- Static only. There is no insert/remove (ADR-0053); a scene with many *moving*
  instances rebuilds often and would prefer a refit or a dynamic structure. The
  key already detects that case; it does not yet optimise it.
- The tree adds memory proportional to the instance count. For a scene of a few
  meshes this is a net cost over the flat pass; it pays off as the drawable count
  grows.

## Alternatives considered

- **Keep the flat pass and rely on the extraction seam alone.** Rejected for M4.
  The flat pass is `O(n)` per frame regardless of how much is off screen; that is
  the exact cost the roadmap row exists to remove. It is kept as `cull_flat`, but
  not as the primary path.
- **Rebuild the BVH every frame.** Rejected. A build is `O(n log n)`; paying it
  every frame can exceed the flat pass it replaces for a scene that barely moves.
  The key-compare cache is what makes the tree worth having.
- **Rebuild on a scene-wide "changed" flag.** Rejected for this change. The
  engine has no such flag on `Scene` today, and adding one is a wider change than
  this row. The per-instance key is local to the renderer, needs no engine API,
  and detects the same condition (geometry moved) that a flag would.
- **Cull sprites through a tree too.** Rejected. Sprites are a flat, screen-space
  workload that the 2D renderer already batches; a tree over 2D quads adds build
  cost for a set that is usually small and highly dynamic. The 3D static case is
  the one the roadmap names.
- **Parallelise the query in this change.** Rejected as scope creep, and blocked
  on the `jobs` module. The tree query is the natural unit to chunk later
  (ADR-0017), and this record leaves it a pure function of the array so that
  remains a drop-in.
- **Store the tree in the scene, not the renderer.** Rejected. The tree is built
  from *world* bounds under the current transforms, which the renderer computes at
  extraction; the scene does not track world-space AABBs, and putting the tree
  there would couple the authoring model to a render optimisation. The renderer is
  the consumer, so the renderer owns the cache.
