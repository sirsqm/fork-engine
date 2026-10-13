# ADR-0021: 2D joints (distance and revolute constraints)

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md),
  [ADR-0013](0013-physics2d-queries-events-angular.md)

## Context

ADR-0009 shipped the 2D solver and named "joints" as future work. ADR-0013 added
queries, contact events, and angular dynamics, and again left joints out. The M5
roadmap lists "joints" among the physics refinements. A game needs them for the
things a body-by-body solver cannot express: a rope, a spring, a hinge, a
pendulum, a wheel on an axle.

Two facts about the existing code shape the decision:

- The solver is a **sequential-impulse** loop over contacts (ADR-0009), extended
  with angular terms in ADR-0013. Any new constraint that wants to feel like the
  rest of physics should be solvable by the same loop, not by a second one.
- Components that point at other objects already exist and already serialize:
  `Sprite2D` stores a texture key (`Texture2DLoader::reference_key`) and
  `MeshRenderer3D` stores a mesh key (`ObjLoader::reference_key`). A joint points
  at two *bodies*, and the engine already has a stable way to name a node: its
  `Uuid` (ADR-0002).

The question is how to add joints without a second solver, a second reference
mechanism, or a new module.

## Decision

Add joints **inside the existing `physics2d` module**. No new module, no change
to `System`, `SceneSchedule`, or any other shared header.

### Two components, named by node id

`DistanceJoint2D` and `RevoluteJoint2D` are ordinary components. Each names its
two ends with `Uuid body_a` / `Uuid body_b`, exactly as a texture reference is a
stable key rather than a pointer. A joint lives on one node and points at two
others; it is gathered with the rest of the world each step.

- A **distance joint** holds the two body centres a fixed distance apart (a rigid
  rod), or pulls them back with a spring when `stiffness > 0`.
- A **revolute joint** pins a shared anchor point: translation is removed, relative
  rotation is left free (a hinge or pendulum). The shared point is authored as a
  local anchor on each body, so a pin offset from a body's centre needs no
  separate world anchor.

A nil, missing, or self-referential reference is **not** a crash. The joint is
skipped and a `JointDiagnostic` (`type`, `body_a`, `body_b`, `Error`) is recorded,
which `joint_diagnostics()` exposes after the step. This mirrors the contact
events of ADR-0013: a broken reference is a value the caller can read, not an
exception, and it keeps a partially-authored scene loadable.

### The maths is pure, in its own header

`JointSolver.h` holds the per-body state (`JointBody`) and a single row type
(`ConstraintRow`) with builders (`build_distance_row`, `build_revolute_rows`) and
appliers (`apply_row`, `apply_row_position`). It has no scene, no components, and
no allocation, for the same reason `Geometry.h` has none: the maths is the part
worth testing exhaustively and it must be callable without a world. The system
gathers the scene into these values, calls the builders, and applies the rows;
a test calls the same functions with hand-written inputs.

Each row carries two corrections, mirroring how a contact separates a velocity
impulse from a positional one:

- a **velocity bias** for a spring (a restoring velocity proportional to the
  stretch, damped by `damping`), and
- a **positional correction** for a rigid constraint: a fixed fraction
  (`0.2`) of the error is removed along the row each pass. Reaching the error at
  a fixed rate is frame-rate independent, where a velocity bias is not. The
  correction is scaled by the row's effective mass, so the amount removed does
  not depend on the two bodies' masses.

### One solve pass, after the contacts

`Physics2DSystem` gains `solve_joints`, called **once per step, after the contact
pass has written back**. It is a single pass rather than inside the contact
iteration loop: a pin needs its error removed at a fixed rate, not crushed to
zero, and running after the write-back means the joint reads each end's current
transform and velocity fresh. A joint correction is the last word on position,
which is what keeps a pinned body pinned even while it also rests on a collider.

A joint end may be **any node**, not only a collider. This required one small
extension to the gather: the system now simulates a node that has a `Collider2D`
*or* a `RigidBody2D` (previously only colliders), with the narrow phase skipping
any body that has no collider. That is what lets a pendulum hang from a bare
pivot node and a bob be a colliderless point mass.

## Consequences

**Easier:**

- Ropes, springs, hinges, and pendulums are expressible with no new machinery: a
  component, a row builder, and the existing loop.
- A joint body reference serializes and survives a save/load cycle, because it is
  a `Uuid` and the reflection layer already stores `ValueKind::Uuid` (ADR-0005).
- The pure maths is unit-testable with no device, no scene, and no allocation.
- The new component properties are reflected, so they serialize and appear in the
  inspector with no per-type editor code (ADR-0006).

**Harder / accepted costs:**

- **Fixed-timestep solve.** A joint is solved once per step and its correction
  factor is per-pass, so a wildly varying `delta_time` changes how fast a rigid
  joint converges. This matches the existing solver's assumptions and is
  acceptable for the frame rates this cut targets.
- **No joint limits or motors.** A revolute joint is a free hinge; there is no
  angle limit and no motor. Deferred, like contact manifolds in ADR-0013.
- **No joint breaking.** A joint never breaks under load. Deferred.
- **The scene id index is now rebuilt after load.** Deserialization assigns saved
  ids *after* attaching each node, so the scene's id index held the placeholder
  ids from creation. Joints are the first feature to *store* a node id in a
  component, so this latent gap became a real bug; `Scene::rebuild_index()` is
  now called once at the end of `from_json`. It is the minimal fix and helps any
  future node reference, not just joints.
- **Solve order is scene order.** Joints are solved in the order they are
  gathered, which is scene order. For independent joints this is irrelevant; a
  chain of joints converges more slowly than a Gauss-Seidel pass over several
  iterations would. A joint-aware iteration is deferred.

## Alternatives considered

- **A separate `physics2d_joints` module.** Rejected: a joint is a physics
  constraint and belongs beside the collider and body it constrains. A module
  boundary would add a dependency edge and a header for no isolation benefit.
- **Pointers to `RigidBody2D` instead of node ids.** Rejected: a pointer does not
  serialize, and it dangles the moment a node is removed. The node id is the
  engine's existing stable reference, and it is what `Sprite2D` and
  `MeshRenderer3D` already use for their references.
- **A world-space anchor for the revolute joint.** Rejected: it cannot follow a
  rotating body without a second transform, and it is redundant with the two
  local anchors. The local anchors are the minimal authoring model.
- **Solving joints inside the contact iteration loop.** Rejected: a rigid joint
  would then remove its whole error in one iteration and ring, and it would
  interleave with contact impulses in an order that is hard to reason about. One
  pass after the contacts is simpler and stable.
- **A velocity-bias-only formulation (no positional correction).** Rejected: it
  is frame-rate dependent and drifts under gravity. The fixed-rate positional
  correction is what holds a distance joint's length under load.
- **A generic constraint component with a type enum.** Deferred, not rejected. Two
  concrete components are clearer to author and to reflect now; a generic one can
  subsume them later if the count grows.
