# ADR-0033: Oriented-box colliders and collider offsets

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0009](0009-physics2d-system.md),
  [ADR-0013](0013-physics2d-queries-events-angular.md),
  [ADR-0023](0023-physics2d-broadphase.md)

## Context

ADR-0013 added angular dynamics: a body integrates rotation and the solver
applies angular impulses. It was explicit about the one thing it left undone,
and why:

- **A collider is still an AABB.** Rotation reaches the node's transform and the
  contact equations, but a box collider is not re-oriented. A rotated box is
  *drawn* rotated and *collided* as its unrotated axis-aligned box. This was the
  honest limit named in ADR-0009 and repeated in ADR-0013.
- **A collider has no offset.** An offset would have to be transformed by the
  node's rotation to stay attached, and until rotation was integrated there was
  no rotation to transform by. ADR-0013 deferred the offset with the same
  reasoning.

Both limits are now the blocker. A tilted ramp, a hinged door, a hammer head on
the end of a shaft, and a car with an offset hitbox are all ordinary gameplay
shapes, and each is either collided wrong (a rotated box as its larger bounding
box) or not expressible at all (a shape away from the node origin). They are
grouped here because they share one cause -- the collider's placement ignores the
node's rotation -- and because fixing the placement once fixes both.

The question is not "how do we rotate a box", but "how does the collider's
placement join the orientation the node already carries, without a second
placement mechanism that competes with it". The transform already has a Z
rotation; the broad phase already has a conservative bound; the narrow phase
already has a shape test. The placement must extend those, not sit beside them.

## Decision

Extend the `physics2d` module in place. No new module, no change to `Scene`,
`Transform`, or any shared header.

### A box collider is an oriented box, `Obb2D`

`Geometry.h` gains `Obb2D`: a centre, a half-extent, and a rotation about the Z
axis. The narrow phase gains `intersect(const Obb2D&, const Obb2D&)`,
`intersect(const Circle2D&, const Obb2D&)`, and the flipped
`intersect(const Obb2D&, const Circle2D&)`.

Box vs box is a separating-axis test over the four face normals (two from each
box). A box is convex with only two face normals, so four axes are exact: SAT
reports a collision if and only if the true shapes overlap. The axis with the
least overlap is the contact normal and its overlap is the penetration, matching
the axis-aligned test it generalises.

Circle vs box is resolved in the box's local frame: the circle centre is rotated
by `-rotation` so the box is axis-aligned, the existing closest-point test runs
there, and the normal is rotated back. This keeps the face, edge, corner, and
centre-inside cases byte-for-byte identical to the axis-aligned version -- the
only difference is the frame it is computed in.

`Aabb2D` is kept. It is still the right type for the broad phase and for the
ray/containment helpers, and it is the zero-rotation special case of `Obb2D`.
The axis-aligned `intersect` overloads are thin wrappers that build the matching
zero-rotation `Obb2D` and call the oriented test, so there is one narrow phase
rather than two that can drift apart.

### The broad phase stays axis-aligned and conservative

`bounds_of(const Obb2D&)` returns the smallest axis-aligned box containing the
four rotated corners -- the box's extent along each world axis. This is exact for
a box, and it is a superset of the true shape, so the uniform-grid broad phase
(ADR-0023) keeps its invariant: a pair that truly touches always shares a cell. A
pair that shares a cell but does not truly touch is rejected by the bounds
prefilter and then by the oriented narrow phase, so a rotated box no longer
generates the false positives its bounding box would.

### A collider carries a local `offset`

`Collider2D` gains `offset`, a `Vec2` displacement in the node's local frame. It
is rotated by the node's Z rotation before it is added to the node's world
position, so an offset stays attached to the shape as the node turns: an offset
of `(1, 0)` on a node rotated 90 degrees places the shape one unit along the
node's local `+x`, which is world `+y`. A zero offset is the default and the old
behaviour.

The placement helpers live on the collider and take the node's world centre and
Z rotation:

- `world_center(center, rotation)` -- the offset rotated into world space.
- `oriented_box(center, rotation)` -- the box as an `Obb2D` at that placement.
- `oriented_circle(center, rotation)` -- the circle at that placement.
- `bounds(center, rotation)` -- the world AABB of the placed shape, for the grid.

The solver, the queries, and the island builder all place a collider through
these helpers, so the shape the solver collides is the shape a query sees and the
shape the island graph reasons about.

### The contact point is taken at the placed surface

The single-point contact estimate now uses the collider's world centre and its
oriented support radius, not a shape assumed centred on the node. This matters
only when the offset or rotation is non-zero -- with both zero it reduces to the
old lever arm exactly -- and it is what keeps a body's centre of mass at the node
origin while its surface is where the offset puts it.

## Consequences

**Easier:**

- A tilted ramp, a hinged door, and an offset hitbox are expressible and are
  collided as their true shapes. A rotated box stops colliding as the larger
  bounding box that would falsely stop a body near its corner.
- The offset and the orientation are the same transform the renderer uses, so a
  shape is drawn and collided with one placement and no per-shape editor code.
- `offset` is a reflected property, so it serializes and appears in the
  inspector with no new editor code (ADR-0006).
- Queries and the solver agree by construction: both place a collider through
  the same helpers, so a raycast and a collision cannot disagree about where a
  shape is.

**Harder / accepted costs:**

- **The broad phase is looser for a rotated box.** Its cell coverage is the
  bounding box of the rotated shape, which is larger than the shape, so a rotated
  box yields more candidates than an axis-aligned one. This is the same
  conservative trade ADR-0023 already makes; the narrow phase rejects the extra
  candidates. A tighter grid (or a rotated-cell partition) is deferred.
- **The box narrow phase is O(1) but heavier than the axis-aligned test** (four
  projections instead of two interval tests). It is still constant per pair, and
  the axis-aligned case pays it too because it routes through the oriented test.
  The extra cost is a handful of multiplies per candidate pair.
- **A rotated box is not swept.** Fast rotation can still tunnel; continuous
  collision detection remains the next refinement and is not in this cut.
- **A circle's shape does not depend on rotation**, so a rotated circle only
  moves by its offset. There is no oriented ellipse or capsule; those are not
  requested and are not added speculatively.
- **The offset is not a full local transform.** There is no local scale and no
  rotation of the shape *relative to the node* -- the shape's orientation is the
  node's. A child node is the way to give a shape an orientation of its own.

## Alternatives considered

- **Keep the AABB and rotate the box into it each step.** Rejected: it is the
  false-positive behaviour this change removes, and it would still collide a
  rotated box as its larger bound.
- **Add an `Obb2D` narrow phase but keep the AABB `intersect` overloads
  separate.** Rejected: two narrow phases can drift. The axis-aligned overloads
  are wrappers over the oriented test, so the zero-rotation case cannot diverge
  from the general one.
- **Make `Aabb2D` the only box type and rotate its corners at each call.**
  Rejected: it pushes the rotation into every call site and re-derives the axes
  per test. `Obb2D` carries the rotation once and exposes its axes.
- **Store the collider's placement as a `Mat4` or a quaternion.** Rejected: 2D
  rotation is one scalar. A matrix would be a second representation of the
  transform the node already has, and a source of truth that can disagree with
  it.
- **Put the offset in the `Transform` (move the node) instead of the collider.**
  Rejected: the offset is a property of the *shape*, not of the node. A node's
  position is its centre of mass; moving it to offset a shape would move the
  mass and the draw transform too. The collider owns its local placement.
- **A full oriented-box collider with a rotated broad phase cell.** Deferred,
  not rejected: the conservative axis-aligned cell is correct and simple, and a
  tighter partition is an optimisation to make only when a profile asks for it
  (ADR-0017).
- **Continuous collision detection in the same change.** Deferred: it is an
  independent refinement with its own tests and its own trade-offs, and this
  change should be reviewable on its own.
