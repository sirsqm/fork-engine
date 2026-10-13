# ADR-0013: Physics queries, contact events, and angular dynamics

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md)

## Context

ADR-0009 shipped the first 2D physics cut: a sequential-impulse solver with
linear motion, gravity, restitution, and friction. It was explicit about three
things it did not do, each for a reason:

- **No queries.** There was no way to ask "what is under this point" or "what
  does this ray hit". Gameplay (aiming, line of sight, ground checks) and the
  editor (picking) both need this.
- **No contact events.** A game could read positions after a step but could not
  react to a contact *beginning* or *ending* ("I just landed", "I left the
  ground"). ADR-0009 deferred this because it needed "a story for when in the
  frame they fire".
- **No angular dynamics.** A body had linear velocity only, so a box could not
  spin and a rolling ball could not roll. The absence of rotation was also why a
  collider had no offset: an offset would have to be rotated to stay correct.

This ADR covers all three as one milestone (M5). They are grouped because they
share a cause — the first cut was deliberately minimal — and because they are
independent of the 3D work happening in parallel, so they can land without
touching any shared subsystem.

The question this ADR answers is not "how do we add rotation", but "how do the
new capabilities attach to the existing model without a second mechanism that
competes with the first". The systems layer already owns per-frame execution and
the reflection layer already owns serialization; a query API and an event
channel must live inside those, or they become parallel machinery.

## Decision

Extend the `physics2d` module in place. No new module, no change to `System`,
`SceneSchedule`, or any other shared header.

### Queries are free functions over the scene, not a system

`Physics2DQueries.h` provides `raycast`, `overlap_circle`, `overlap_box`, and
`overlap_point`. They take a `const Scene&` and read the colliders directly.

A query is not a per-frame behaviour, so it is not a `System`. Making it one
would force a caller to run a schedule to ask a question, and would make the
editor's picking depend on the runtime. Instead the query layer reuses the same
`Geometry` helpers the solver uses, so a query and a collision agree by
construction.

The result types are plain structs (`RaycastHit`, `OverlapHit`) with a `hit` or
a count as the primary answer. A miss is a value, not an `Error`: a ray that
hits nothing is an ordinary outcome a caller branches on, not an exceptional
one, and the engine's error convention (ADR-0003) reserves `Error` for failures
a caller must handle.

**Allocation.** A raycast returns one value and never allocates. The overlap
queries append to a caller-owned `std::vector`, so a hot loop reuses one buffer
and does not allocate once it has capacity. This is the same "caller owns the
scratch" choice the solver makes with its body array. A query that returned a
fresh vector would allocate per call, which is exactly what the milestone asked
to avoid.

### Contact events are collected on the system and read after the step

`Physics2DSystem` exposes `contact_events()`: a buffer of `ContactEvent`
(`phase`, `a`, `b`) rebuilt every step. A contact is keyed by the *pair of node
identities*, ordered, so it is stable across frames regardless of scene order.
Each step the system sorts the pairs that touch now and the pairs that touched
last frame, diffs them, and emits `Begin` for a new pair and `End` for a
disappeared one.

This was chosen over dispatching through the `System` interface for three
reasons:

1. **No second event bus.** The systems layer (ADR-0008) deliberately has no
   pub/sub and no job graph. Adding one for contacts would be the speculative
   framework ADR-0008 rejected, and it would be a second mechanism competing
   with the first.
2. **Read after the step, not mid-solve.** A listener that ran during the solver
   could mutate the scene while it iterates. Collecting and reading after means
   a listener always sees a settled world. It also matches how physics already
   works: it is a system, and a game reads its outputs after `update`.
3. **A home already exists.** A gameplay system that runs *after* physics in the
   schedule reads the buffer in its own `update`. That is the composition model
   doing the work: no callback registration, no ordering puzzle, just "physics
   runs, then my system runs and reads what changed".

The buffer is cleared at the start of every step, so it always describes exactly
one step. Order within a step is deterministic (all `Begin`, then all `End`,
each sorted by the pair key), because a game reacting to two events in one frame
must get the same order every run.

### Angular dynamics extend the existing solver

`RigidBody2D` gains `angular_velocity`, `torque`, `inertia`, `angular_damping`,
and `fixed_rotation`. The solver integrates rotation and applies angular
impulses using the *same* sequential-impulse structure, not a new solver:

- Inertia is derived from the shape when the author does not set it
  (`Collider2D::inertia_about_center`: `(1/12) m (w²+h²)` for a box, `½ m r²`
  for a disc), so a body rotates sensibly with no authoring.
- The contact point is taken on A's surface along the normal, giving each body a
  lever arm. The effective mass along the normal and the tangent includes the
  `r × n` / `r × t` terms, which is what couples rotation into the linear
  response.
- `fixed_rotation` zeroes the inverse inertia and freezes the integrated
  rotation, so a body that must stay upright is a flag rather than a special
  case in the solver.

`torque` is a single authored value applied every step, the rotational
counterpart of `gravity`. A general force/torque accumulator a script can add to
during the frame is later work, consistent with how forces are handled now.

## Consequences

**Easier:**

- Gameplay and the editor get queries with no dependency on the runtime: the
  editor can pick with a raycast without running a schedule.
- Contact reactions need no new API to learn. A system reads a vector after
  physics, exactly as it reads any other system output.
- A rolling ball, a spinning crate, and a lever all fall out of the same solver.
  The angular terms are additions to the impulse equations that were already
  there.
- The new component properties are reflected, so they serialize and appear in
  the inspector with no per-type editor code (ADR-0006).

**Harder / accepted costs:**

- **A collider is still an AABB.** Rotation is integrated into the node's
  transform and applied by the solver to contacts, but a box collider is not
  re-oriented: a rotated box is drawn rotated and collided as its unrotated
  AABB. This is the same honest limit ADR-0009 named. Oriented boxes are the
  next step and are not in this cut.
- **The contact event carries no geometry.** It reports the pair and the phase,
  not the point or normal. A listener that needs those runs a query. This keeps
  the event small and the diff cheap; adding a manifold is deferred until a real
  consumer needs it.
- **The event diff is O(pairs log pairs) per step.** Sorting the pair lists is
  cheap at the scale this cut targets (the broad phase is still O(n²)); a
  spatial partition would change both together and is deferred with it.
- **Torque is not a persistent impulse.** A contact's angular impulse lasts the
  frame it is applied, like the linear impulse; `torque` is a constant
  acceleration. There is no accumulated force/torque buffer yet.

## Alternatives considered

- **A physics query as a `System`.** Rejected: it forces a schedule to ask a
  question and couples editor picking to the runtime. Queries are reads, not
  behaviours.
- **An event bus or a listener registry for contacts.** Rejected: it duplicates
  the systems layer. ADR-0008 chose an ordered list of systems with no pub/sub
  on purpose; a contact channel is better served by "read the buffer after the
  step" than by a second dispatch mechanism.
- **Callbacks fired during the solve.** Rejected: a listener could mutate the
  world mid-iteration, and the firing point would be ambiguous (per pair? per
  iteration? per step?). Collecting and reading after removes both problems.
- **A return-by-value `std::vector` for overlaps.** Rejected: it allocates per
  call, which the milestone explicitly ruled out for a hot loop. The caller-owned
  buffer is the same choice the solver makes.
- **A full oriented-box collider (OBB) now.** Deferred, not rejected. It is the
  natural follow-on to rotation, but it changes the narrow phase and the broad
  phase, so it belongs in its own change with its own tests.
- **A force/torque accumulator on the body.** Deferred. Without scripting there
  is no caller to accumulate across a frame, so a constant `torque` (like
  `gravity`) is the honest minimal form.
