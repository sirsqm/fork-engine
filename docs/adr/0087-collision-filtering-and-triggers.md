# 0087 — Collision filtering and trigger colliders

Date: 2026-10-25

Status: Accepted

## Context

Both physics modules resolve every pair of overlapping colliders the same way:
if two shapes overlap, an impulse is applied and the pair is reported as a
contact. There is no way to say "these two never touch", and no way to say
"this shape is a zone, not a solid". A grep for `layer`, `mask`, `filter`,
`trigger`, or `sensor` across `engine/` returns nothing in either solver, so
this is a hard capability wall rather than a missing convenience.

The wall is real for ordinary gameplay. A pickup is a zone the player passes
through, not a wall. A projectile must ignore the collider of the ship that
fired it, or the first shot collides with its own muzzle. Team-based
collision, one-way platforms, and "these two categories never interact" are
all the same missing idea. Without it a game can only express "everything
collides with everything", which is enough for a slice with a floor and a ball
and not much else.

Two constraints shape the design. First, the change must not alter the
behaviour of a scene that does not use it: every existing scene, test, and the
breakout slice must resolve exactly as they do today. Second, it must be
testable headlessly. The physics modules are pure logic with no GPU, and the
repo's test culture is to prove a behaviour in a headless test; a feature that
can only be verified by looking at a running game does not fit.

## Decision

A collider gains three fields, on both `Collider2D` and `Collider3D`:

* `u32 layer = 1;` — the single layer bit this collider belongs to.
* `u32 mask = 0xFFFFFFFF;` — the set of layers this collider collides with.
* `bool is_trigger = false;` — when true the collider is a sensor.

A pair is considered only when both sides agree:

```
(a.layer & b.mask) != 0 && (b.layer & a.mask) != 0
```

The rule is symmetric, so either side can opt out without the other knowing: a
projectile that ignores its owner only drops the owner's layer from its own
mask. The defaults preserve the old behaviour exactly — bit 0 is the default
layer and the mask is all-ones, so an unedited collider collides with
everything, and a scene that does not set these fields is unaffected.

The test lives on the component (`Collider2D::can_collide_with`) rather than in
the solver, so the rule has one definition that both physics modules and any
future query API share.

**Filtering is applied before the narrow phase, and is total.** A pair whose
layers disagree is skipped immediately after the existing bounds prefilter: it
never reaches the narrow phase, is never recorded in `active_pairs_`, and
therefore never produces a `ContactEvent`. "Ignored" has to mean ignored for
events too. A game that told the engine two things never touch must not receive
a Begin event for them.

**A trigger is applied after the pair is recorded, and only skips the
impulse.** For a pair that passes the filter, if either collider is a trigger,
the touching pair is still written to `active_pairs_` — so the existing
`reconcile_contacts` diff emits Begin and End events exactly as it does for a
solid pair — and the solver then `continue`s before the positional correction
and the impulse. A trigger therefore reports contacts and moves nothing. This
is what makes a pickup zone work: the game reads the Begin event and despawns
the pickup, while the player passes through it.

Both edits are a handful of lines in each solver, at the two sites each already
has: one place where a candidate pair is accepted, and one place where impulses
are applied.

## Consequences

* Gameplay can express pickups, checkpoints, projectiles that ignore their
  owner, team collision, and one-way platforms, in both 2D and 3D, with one
  field set.
* Existing scenes are unaffected by construction, not by luck: the defaults
  reproduce the pre-change pair test.
* The fields serialize and appear in the inspector for free, because the
  serializer and the inspector read the reflection registry. A saved scene
  keeps its filtering.
* The filter is checked per candidate per iteration. It is two bit tests on data
  already in hand, so the cost is negligible beside the narrow phase it
  short-circuits; in the common case it makes the step cheaper by rejecting a
  pair earlier.
* A filtered pair is invisible to the query API as well, once the queries learn
  to honour the filter. That is consistent — a pair the solver ignores should
  not be returned by a query — but it is a later change, not this one, so a
  query today still returns a filtered collider.
* The 32-bit layer set is a hard limit of 32 layers. That is far beyond what a
  game uses and is the standard choice, but it is a limit.

## Alternatives considered

* **A separate `CollisionFilter` component** instead of fields on the
  collider. Rejected: filtering is a property *of a collider*, exactly as
  `restitution` and `friction` already are. A second component would add a
  lookup per pair in the hot loop and a second thing to keep in sync in the
  inspector.
* **A category name string instead of bit layers.** Rejected: the pair test
  needs a bit set, is O(1) with one, and a string would need a project-level
  name registry the engine does not have and would have to resolve per pair.
* **Enforcing the filter in the broad phase**, by not inserting a filtered body
  into the grid. Rejected: the broad phase is deliberately pure and knows only
  bounds and indices (`BroadPhase.h`). The layer test needs component data, so
  it belongs in the solver where the collider is already in hand. It also could
  not express the symmetric rule, which depends on both colliders.
* **Making a trigger skip the pair record as well as the impulse.** Rejected:
  it would make a trigger invisible to gameplay, which defeats the point. A
  sensor that reported nothing would be indistinguishable from a filtered pair.
* **A `CollisionEvent` carrying the contact point and normal.** Out of scope.
  The existing `ContactEvent` is a signal to react to, and a listener that needs
  the geometry can run a query; adding geometry to the event is a separate
  change with its own cost.
