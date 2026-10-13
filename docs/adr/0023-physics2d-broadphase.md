# ADR-0023: A uniform-grid broad phase for 2D physics

- **Status:** Proposed
- **Date:** 2026-10
- **Related:** [ADR-0009](0009-physics2d-system.md),
  [ADR-0013](0013-physics2d-queries-events-angular.md),
  [ADR-0017](0017-performance-strategy.md)

## Context

ADR-0009's first cut resolved collisions with a double loop over every body pair,
and it ran that loop once per sequential-impulse iteration. Its own note said so:
"This is O(pairs * iterations); a spatial hash belongs here once profiling shows
it matters, not before."

It now matters. Two things changed the cost profile:

1. **The pair set was recomputed `iterations` times per step.** Even with a
   perfect pair enumeration, four iterations meant four enumerations. The
   candidate set is a property of the positions at the top of the step, not of
   the iteration, so recomputing it inside the loop was pure waste.
2. **Scenes got denser.** A joint chain of many bodies (ADR-0021) and a pile of
   resting crates both make the O(n²) term visible: the work grows with the
   square of the body count while the useful work grows with the number of pairs
   that actually touch.

The solver's own shape suggested the fix. It already flattens the scene into a
flat `std::vector<Body>` before it runs, so a broad phase that operates on a flat
array of bounds fits the existing design instead of fighting it.

## Decision

Add a **uniform-grid (spatial hash) broad phase** in `physics2d`, and compute its
candidate set **once per step**, before the iteration loop.

### The grid is pure and separate

`BroadPhase` (`BroadPhase.h`/`.cpp`) knows only `std::span<const Aabb2D>` and
returns index pairs. It has no scene, no components, and no dependency on the
solver, so it is tested directly without a world -- the same reason `Geometry.h`
and `JointSolver.h` are separate files.

`build()`:

1. Picks a **cell size** (below).
2. Inserts each body's AABB into **every cell it overlaps**.
3. Groups the `(cell, body)` entries by sorting them, then emits a pair for every
   two bodies in the same cell.
4. **Sorts and de-duplicates** the pairs into a canonical order.

### Once per step, not once per iteration

`Physics2DSystem::update` builds the grid after integration and before the
iteration loop, then each iteration walks only the candidate list. The
`bounds_overlap` prefilter and the narrow phase are unchanged and still run on
every candidate: sharing a cell is necessary but not sufficient, so the grid is
free to be conservative.

### The cell size is derived from the bodies

`cell_size = max(2 * mean_body_extent, 0.1)`, where a body's extent is the larger
side of its AABB.

- **Derived, not configured.** A scene of tiny bodies gets a tiny cell and a
  scene of large bodies a large cell, so the number of cells a body spans stays
  roughly constant without the caller tuning anything. The mean is over the
  bodies actually present, so it tracks the scene as it changes.
- **2x is the scale.** Two body-widths per cell keeps a body to a handful of
  cells (bounded span) while keeping the per-cell body count low, which is the
  balance that makes the pair emission cheap. A cell much smaller than a body
  makes every body span many cells; a cell much larger degenerates toward the
  O(n²) double loop it replaces.
- **The floor is a divide-by-zero guard**, not a tuning knob. A scene of
  point-sized bodies would otherwise pick a zero cell size.

### The result is sorted for determinism

Pairs are emitted in sorted, de-duplicated `(first, second)` order. Contact
events are derived by diffing this frame's pairs against last frame's, so the
pair set's *order* is what makes the Begin/End diff stable: an order that
depended on hash iteration or memory addresses would change which events fire and
in what order across runs. The grid therefore emits the same set in the same
order for the same input, regardless of the iteration strategy inside it.

### Bodies with no collider never enter

A joint-only mass (a node with a `RigidBody2D` but no `Collider2D`, from
ADR-0021) has no bounds, so it is never inserted and never paired. The solver
also keeps an explicit null-collider guard on each candidate, so the invariant
holds even if a caller ever routes a colliderless body through.

Because the flattened body array holds those colliderless masses while the grid
holds only bodies with a collider, the grid's indices are **not** indices into
the body array. The system keeps a small owner map from grid index back to body
index. Without it, a point mass ahead of the colliders in scene order would shift
every index and the solver would resolve the wrong bodies -- a silent, wrong
answer rather than a crash, which is exactly the kind of defect a superset test
is meant to catch.

## Consequences

**Easier:**

- A step is O(candidates * iterations) instead of O(bodies² * iterations). The
  `iterations`-times recomputation is gone entirely, which is a win even for
  scenes where the grid prunes nothing.
- The candidate set is deterministic and order-stable, so contact events are
  reproducible: two identical scenes stepped identically emit identical events,
  frame for frame.
- `BroadPhase` is pure, so its safety property (a superset of the true overlaps)
  is tested directly, including the boundary-straddle case that is the classic
  spatial-hash bug.
- The grid is a reusable primitive: physics queries (ADR-0013) can later consult
  the same structure instead of scanning every body.

**Harder / accepted costs:**

- **A conservative set.** The grid returns extra candidates for pairs in the same
  cell that do not overlap. They are rejected by the prefilter, so correctness is
  unaffected; only a little narrow-phase-adjacent work is spent. The sort and
  de-duplication also cost O(c log c) per step, which is small next to the solve.
- **A uniform grid is poor for wildly mixed scales.** One enormous body makes the
  cell size large and the grid coarse for everything else. A hierarchical
  structure (a BVH) handles that better; this is not it, and the cell sizing is
  the honest compromise for the scenes this engine has today.
- **Not yet measured against a profiler.** The grid is chosen because the
  structure of the work demanded it (a known O(n²·iterations) shape), not because
  a profile number is in the PR. ADR-0017 says measure before optimising; the
  measurement harness (the profiler, ADR-0022) is landing in parallel, and this
  change should be re-checked against it. That is why this record is **Proposed**,
  not Accepted.

## Alternatives considered

- **Keep the double loop, add only the once-per-step hoist.** Rejected as
  incomplete: hoisting removes the `iterations` factor but leaves the O(n²) term,
  which is the one that grows with scene size. The two fixes are independent and
  the grid is the one that matters at scale.
- **A bounding-volume hierarchy (BVH).** The right answer for large, static,
  wildly mixed-scale scenes: O(log n) queries and adaptive cell sizes. Rejected
  *for now* because it is more code, needs rebuilding as bodies move, and the
  scenes this engine has are small and roughly uniform. The `BroadPhase` seam is
  kept narrow precisely so a BVH can replace the grid behind it later.
- **Sweep-and-prune (sort along one axis).** Simple and fast for mostly-sorted,
  sparse layouts, and a good fit for a scene that changes little per frame.
  Rejected because it degrades on the 2D-dense clusters (a pile of crates) this
  change is partly aimed at, where many bodies share an x-interval, and it needs
  a sort of all bodies every step anyway. The grid handles dense 2D overlap
  better and has no global ordering to maintain.
- **Cell size as a caller-set constant.** Rejected: it makes every caller guess,
  and the guess is wrong the moment the scene's scale changes. Deriving it from
  the bodies keeps the common case correct with no input.
