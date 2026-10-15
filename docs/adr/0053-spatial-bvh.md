# ADR-0053: A spatial BVH over static geometry in `core`

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0011](0011-frustum-culling.md),
  [ADR-0017](0017-performance-strategy.md),
  [ADR-0023](0023-physics2d-broadphase.md),
  [ADR-0051](0051-frame-arena.md)

## Context

The roadmap's M4 lists "a spatial hierarchy (BVH/octree) over static geometry",
and [ADR-0011](0011-frustum-culling.md) already framed it: frustum culling
landed as a *flat pass*, and both its record and the 3D renderer say the same
thing — "it is the leaf operation a BVH would call". The renderer still visits
every node every frame; culling only saves the draw, not the visit.

The forces are these:

1. **The leaf test already exists and is proven.** `Frustum::intersects_aabb` is
   conservative (a box straddling a plane is kept) and has its own tests. A tree
   must reuse it, not re-derive it, or the two tests can disagree and the tree
   will hide meshes the flat pass would have drawn.
2. **The input is boxes and indices, nothing else.** A tree over world-space
   AABBs needs no scene, no components, and no GPU. Like `Frustum` and the 2D
   `BroadPhase` ([ADR-0023](0023-physics2d-broadphase.md)), keeping it pure is
   what makes it exhaustively testable — the whole reason those two are separate
   files.
3. **A tree must be trustworthy before it is fast.** The failure that matters is
   not "slow" but "wrong": a query that drops a visible box is a rendering bug
   that no timing shows. So the queries are specified against a brute-force
   reference, and the tree's structural invariants are checked directly.

There is also a temptation to make this 2D, to sit behind the physics broad
phase. That is a different structure with a different consumer and its own
record (ADR-0023); conflating them would tie a renderer concern to a solver
concern.

## Decision

**Add `fe::SpatialBvh` and `fe::Aabb` to `core`: a static binary BVH built
top-down with a binned surface-area heuristic, queryable by frustum and by box,
reusing `Frustum::intersects_aabb` as its leaf test.**

### Where it lives

`core`, because it depends on nothing but `Frustum`, `Math`, and `Types` — the
same reasoning that put `Frustum` there. A renderer can hold one without a new
module edge, and a future consumer (shadow-caster selection, editor picking) can
too.

`fe::Aabb` is a new min/max box type. The physics modules each have their own
centre/half-extent box (`Aabb2D`, `Aabb3D`) tuned for their solvers; min/max is
what the frustum test and the union want directly, and a spatial index should not
reach into physics for a box. The two shapes are the same data; keeping each
where its consumer reads it best is worth the small duplication.

### Shape: binned SAH, leaves of a few primitives

The build is top-down. For each node it picks the axis with the widest centroid
spread, bins the primitives along it, and chooses the bin boundary with the
lowest SAH cost — the surface area of each side times its primitive count. A
split is taken only when it is cheaper than leaving the node together; otherwise
the node becomes a leaf. Leaves hold up to `kMaxLeafSize` primitives, and every
internal node stores the union of its children's bounds.

The SAH is the right objective because query cost tracks surface area: a split
that puts a large, empty-ish bound next to the ray (or frustum) is paid for on
every query, and the heuristic refuses it. The binned form (12 bins per axis) is
the standard approximation — within a few percent of the exact sweep for a build
that stays cheap and cache-friendly.

### The leaf cap is a hard rule, not a hint

A node with more than `kMaxLeafSize` primitives **must** split even when the SAH
finds no gain. Without that rule a scene whose centroids all coincide (a stack of
coincident meshes) never gains from any split and the build returns a single leaf
holding everything — correct but useless, and a depth cap alone would not fix it.
The cap forces the median split, which always halves, so the build terminates
with a real hierarchy on any input. A `kMaxDepth` cap backs it up: past that
depth the split is forced to the median, bounding recursion on adversarial input
where the SAH would otherwise peel one primitive off at a time.

### Queries are conservative, sorted, and allocation-free

`query(const Frustum&, std::vector<u32>& out)` and its box overload clear `out`,
descend the tree skipping any node whose bounds miss the query, and test each
leaf primitive with the same predicate the flat pass would use. The result is
sorted by primitive index with no duplicates. `out` is a caller buffer so a query
does not allocate; `build` reuses its node and scratch arrays across calls.

### Determinism

Node order, split choice, and query output depend only on the input order and the
geometry. Ties break by bin index during the build, and — because `std::partition`
does not preserve the order of equal elements — each leaf's primitive range is
sorted by index before the tree is exposed, so the stored order (and therefore
the tree) does not depend on how the partition shuffled equal-bin primitives. Two
builds of the same input on the same build produce byte-identical node arrays,
and the query output is sorted regardless. This is what lets a test assert
"identical tree" rather than "equivalent tree", and it is what the benchmark and
the editor's debug view need to be comparable across runs.

### What this does *not* do yet

This lands the structure and its queries. The 3D renderer still does its flat
pass and does not hold a `SpatialBvh`; wiring the renderer to build a tree over
its static instances and query it in place of the flat loop is the follow-on. The
tree is also static: there is no insert or remove, so a changed scene is rebuilt.
That matches "over static geometry" and avoids the refit/rotation machinery a
dynamic tree needs.

## Consequences

**Easier:**

- A scene with many static instances can be culled by descending a tree instead
  of visiting every instance. A rejected subtree costs one box test, not one per
  primitive below it.
- The query reuses `Frustum::intersects_aabb` verbatim, so the tree and the flat
  pass agree on what is visible by construction, not by two tests happening to
  match.
- The structure is pure and headless, so its correctness is pinned against a
  brute-force reference and its invariants (every primitive in exactly one leaf,
  a parent covers its subtree) are checked directly.
- A second query shape (box overlap) comes almost free and serves editor picking
  and shadow-caster selection later.

**Harder:**

- The tree is static. Any change to the geometry means a rebuild; a scene that
  moves many instances per frame would not want this. That is the documented
  scope ("static geometry"), and a dynamic structure is a separate decision.
- Build cost is real and paid up front. It is amortized only when the tree is
  queried more than once or over more instances than a flat pass would visit; a
  scene of a dozen meshes is faster without it.
- SAH builds are O(n log n) on average but the binned approximation can pick a
  slightly suboptimal split; the tree is still correct, just marginally less
  tight. Correctness never depends on the split quality.
- The `core` `Aabb` duplicates the shape of the physics boxes. They are separate
  types with separate conventions, which is a small amount of repetition in
  exchange for each consumer keeping its natural representation.

## Alternatives considered

- **An octree.** Rejected as the primary structure. An octree splits space on a
  fixed grid rather than on the data, so it is simpler to build but adapts worse
  to non-uniform scenes (a scene of tall thin objects, or a few large meshes
  among many small ones) and typically needs a deeper tree for the same
  separation. The SAH BVH costs more to build and culls better; for static
  geometry built once, build cost is the cheaper thing to spend.
- **A BVH in `renderer3d`, beside the culling pass.** Rejected. The structure
  needs nothing from the renderer, and putting it in `core` lets other consumers
  (editor picking, shadow casters) reuse it without depending on the renderer.
- **Reuse the physics `BroadPhase`.** Rejected. That is a 2D uniform grid keyed
  for the solver's pair enumeration, over `Aabb2D`; it answers "which pairs might
  overlap", not "which boxes meet a region". Sharing it would couple a renderer
  concern to a solver concern for no reuse.
- **A dynamic BVH with insert/remove.** Rejected for now. It brings refit and
  rotation machinery that the "static geometry" scope does not need, and a
  changed scene can rebuild. Adding it before a moving-geometry consumer exists
  is speculative.
- **An exact SAH sweep instead of binning.** Rejected. The exact sweep is
  O(n log n) per axis with a full sort, and the binned approximation is within a
  few percent of its cost for a fraction of the build time. Query correctness is
  identical either way.
- **Wiring the renderer to the tree in this change.** Rejected as scope creep.
  The renderer change is a separate step with its own testing; landing the
  structure, its queries, and their correctness first is the reviewable unit.
