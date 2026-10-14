# ADR-0043: 3D angular dynamics, and a quaternion orientation

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md),
  [ADR-0013](0013-physics2d-queries-events-angular.md),
  [ADR-0021](0021-physics2d-joints.md),
  [ADR-0031](0031-physics3d-and-backend-seam.md),
  [ADR-0037](0037-physics2d-oriented-boxes-offsets.md)

## Context

ADR-0031 shipped the first 3D physics cut and was explicit about its biggest
limit: **linear only**. A `RigidBody3D` had a linear velocity and mass, no
angular velocity, torque, or inertia, so a body could be moved and collided but
never turned. The reason it gave was concrete and worth restating: `Transform`
stores rotation as Euler angles, and integrating an angular velocity through
Euler rates is the gimbal-lock problem. A body that tumbles needs an
*orientation*, and the engine had no orientation type — only the Euler triple
that `Mat4::rotation_euler` composes.

2D did not have this problem because it has exactly one rotational degree of
freedom: a scalar angle about Z integrates cleanly, so ADR-0013 could add angular
dynamics without a new type. 3D has three coupled rotational degrees of freedom,
and a scalar angle per axis is exactly what gimbal lock is made of.

The question this ADR answers is not "how do we add torque", but "how do we carry
an orientation in 3D without either a gimbal-locked Euler path or a second,
competing representation that every other subsystem has to learn".

## Decision

Add 3D angular dynamics to the existing `physics3d` module, and add a `Quat` type
to `core/Math.h` to carry the orientation. No new module, no change to
`System`, `SceneSchedule`, or any other shared header beyond the `Quat` addition.

### A `Quat` in core, with `Transform` unchanged

`Quat` lives in `core/Math.h` beside `Vec2`, `Vec3`, and `Mat4`: it is a value
type with no dependency on physics, exactly as those are. It is the smallest type
that can represent a 3D rotation and integrate an angular velocity without
gimbal lock.

`Transform` is deliberately **not** changed. It keeps its Euler-angle `rotation`
and its existing composition API, because changing the storage would ripple
through the renderer, the serializer, the editor, and every test that authors a
rotation as `Vec3{0, 0, angle}`. Instead the solver converts at the boundary:
`Quat::from_euler` reads the authored rotation when a body is gathered, and
`Quat::to_euler` writes the integrated orientation back when the body is written
out. `from_euler` uses the same `Rz * Ry * Rx` (X first) composition as
`Mat4::rotation_euler`, so a body that is never spun keeps exactly the rotation
its author set, and a round-trip through the boundary is the identity.

This keeps the *representation* concern inside the module that needs it. The rest
of the engine sees `Transform` exactly as before.

### Angular state on the body, derived inertia, the same solver

`RigidBody3D` gains `angular_velocity`, `torque`, `inertia`, `angular_damping`,
and `fixed_rotation`, mirroring `RigidBody2D` (ADR-0013) field for field. The
solver integrates them in the same sequential-impulse structure, not a new one:

- **Inertia is derived from the shape** when the author does not set it
  (`Collider3D::inertia_about_center`): `(2/5) m r²` for a solid sphere, and
  `(1/18) m (w² + h² + d²)` for a box. The box formula is the mean of the box's
  three principal moments, so a cube reports the exact `(1/6) m w²` about any
  axis. The solver carries a *single scalar* moment — an isotropic approximation
  of the inertia tensor — exactly as the 2D solver carries its one in-plane axis.
  A non-cube therefore spins the same about every axis; a full diagonal tensor is
  later work.
- **The orientation is integrated as a quaternion**: `q' = q + ½ ω q`, then
  renormalised, the standard first-order update. It is stable at game step sizes
  and is what lets a body turn without Euler angles.
- **The contact response gains angular terms.** The contact point is taken on A's
  surface along the normal, giving each body a lever arm `r`. The relative
  velocity includes `ω × r`; the effective mass along the normal and the tangent
  includes the `(r × n)² · inv_inertia` terms; and the impulse produces torque
  `r × J · inv_inertia`. This is what couples rotation into the linear response
  and makes a box tumble and a ball roll rather than slide.
- **`fixed_rotation`** zeroes the inverse inertia and freezes the integrated
  orientation, so a body that must stay upright is a flag rather than a special
  case in the solver. It is the 3D counterpart of `RigidBody2D::fixed_rotation`.

### The joint row carries angular terms

`ConstraintRow3D` gains `angular_a`/`angular_b` Jacobians and `JointBody3D` gains
an orientation, an angular velocity, and an inverse inertia, mirroring the 2D row
(ADR-0021). A distance joint measures between the two centres, so its angular
Jacobians stay zero and it remains linear in practice; the terms exist so the row
type is complete and a future anchor-based joint (a revolute or point-to-point
constraint) needs no new machinery.

### Reflection and serialization

The new properties are registered in `Physics3DTypes.cpp`, so they serialize and
appear in the inspector with no per-type editor code. `Transform` rotation is
already serialized as a `Vec3` Euler triple; nothing about the document format
changes.

## Consequences

**Easier:**

- A tumbling crate, a rolling ball, and a spinning wheel all fall out of the same
  solver. The angular terms are additions to the impulse equations that were
  already there, not a second solver.
- `Quat` is available to any future subsystem that needs an orientation — the
  renderer's scene graph, an animation system, a camera — without each inventing
  its own.
- The 3D module is now feature-comparable with 2D on dynamics: linear and angular
  motion, contacts, and distance joints.
- The new component properties are reflected, so they round-trip through the
  serializer and the inspector for free (ADR-0006).

**Harder / accepted costs:**

- **A collider is still axis-aligned.** Rotation is integrated into the node's
  orientation and applied by the solver to contacts, but a box collider is not
  re-oriented: a rotated box is drawn rotated and collided as its unrotated AABB.
  This is the same honest limit 2D had before oriented boxes (ADR-0037), and
  oriented 3D shapes are the natural next cut.
- **Inertia is isotropic.** A single scalar moment cannot represent a
  non-uniform body's true tensor, so a long box spins the same about every axis.
  This is a deliberate first step; the scalar keeps the solver and the serialized
  data small, and a diagonal tensor is later work.
- **`to_euler` has the usual extraction limit.** The recovered Y rotation lies in
  `[-π/2, π/2]`. This only matters when a body's orientation is read back out as
  Euler angles (for serialization or a debug view); the simulation itself is in
  quaternion space and is unaffected.
- **Torque is not a persistent impulse.** A contact's angular impulse lasts the
  frame it is applied, like the linear impulse; `torque` is a constant
  acceleration, the rotational counterpart of `gravity`. There is no accumulated
  force/torque buffer yet, consistent with how forces are handled now.

## Alternatives considered

- **Keep `Transform` as the orientation and integrate Euler rates.** Rejected.
  Euler rates do not compose the way an angular velocity does; a body would
  gimbal-lock, and the error would be subtle and content-dependent. This is the
  exact reason ADR-0031 deferred angular dynamics in the first place.
- **Store the orientation as a quaternion in `Transform`.** Rejected for now.
  It is the "more correct" long-term representation, but it changes a type every
  subsystem reads, and the document format and the inspector with it. The
  boundary conversion gets the same simulation with none of that churn; promoting
  `Transform` to a quaternion is a separate, later decision.
- **A full diagonal inertia tensor on the body.** Deferred, not rejected. It is
  the right model for non-uniform bodies, but it needs an orientation to rotate
  the tensor into world space each step and a vector inverse-inertia path through
  the solver. A scalar is the honest minimal form and is what the tests pin.
- **Oriented 3D colliders now.** Deferred, not rejected. It is the natural
  follow-on to a real orientation, but it changes the narrow phase and the broad
  phase, so it belongs in its own change with its own tests, exactly as 2D split
  it out (ADR-0037).
- **A revolute / point-to-point 3D joint now.** Deferred. The row type carries
  the angular terms so it is a small, additive step when a game needs a hinge,
  but shipping it here would be breadth without a consumer.
