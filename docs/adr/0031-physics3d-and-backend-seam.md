# ADR-0031: 3D physics, and a backend seam so the solver can be replaced

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md),
  [ADR-0013](0013-physics2d-queries-events-angular.md),
  [ADR-0021](0021-physics2d-joints.md),
  [ADR-0023](0023-physics2d-broadphase.md),
  [ADR-0026](0026-physics2d-islands-sleeping.md)

## Context

The 2D module proved the physics-as-a-system shape
([ADR-0009](0009-physics2d-system.md)): flatten the scene, simulate, write back,
with shape and motion as separate components. 3D gameplay needs the same
capability, and the natural move was to mirror the 2D module rather than invent
a new structure.

The uncomfortable question is scope. A production 3D solver is a multi-year
project: general convex collision (GJK/EPA), persistent contact manifolds with
warm starting, a proper constraint solver, continuous collision detection,
sleeping and islands, character controllers, vehicles, deterministic parallel
stepping, and years of bug-fixing against real games. Mature MIT-licensed
libraries already solve that problem — [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
is the obvious one — and reimplementing it would be a poor use of the project's
time. But a hand-written core is also genuinely useful: it validates the
architecture (component model, systems, reflection, serialization) against 3D
data, it is deterministic and headless for tests, and it teaches the domain.

The decision is therefore not "write 3D physics or not". It is *what the 3D
physics module commits to being*: a forever-solver, or a replaceable one.

## Decision

Ship a **hand-written 3D solver as the first cut**, deliberately narrower than
the 2D module, and treat it as a replaceable backend rather than the final
answer.

### Scope of the first cut

`physics3d` mirrors `physics2d` in structure and is honest about its limits:

- **Box and sphere shapes only.** The 3D analogues of box and circle. Capsules,
  convex hulls, and triangle meshes are later work.
- **Linear only.** There is no angular velocity, torque, or inertia; a body does
  not turn. `Transform` rotation is Euler angles and the engine has no
  quaternion yet, so angular integration is a separate cut, exactly as it was
  for 2D ([ADR-0013](0013-physics2d-queries-events-angular.md)). Joints are
  linear for the same reason.
- **One joint type.** `DistanceJoint3D` (a rigid rod or a spring), naming its
  ends by node id so the reference serializes. Revolute and other joint types
  follow with angular bodies.
- **No sleeping or islands yet.** Those were a later 2D cut
  ([ADR-0026](0026-physics2d-islands-sleeping.md)) and follow here too.
- **A uniform-grid broad phase**, shared in shape with the 2D one
  ([ADR-0023](0023-physics2d-broadphase.md)).

### Components and system

- `Collider3D` holds a shape (`Box` with a half-extent, or `Sphere` with a
  radius) and the surface material (`restitution`, `friction`).
- `RigidBody3D` holds linear velocity, mass, damping, and the `kinematic` and
  `affected_by_gravity` flags.
- `Physics3DSystem` is an ordinary `System`. Each update gathers the scene into a
  flat body array, integrates gravity and damping, finds candidate pairs once
  with the broad phase, runs a fixed number of sequential-impulse iterations
  (bounds prefilter, narrow phase, normal impulse with restitution, Coulomb
  friction, Baumgarte positional correction), solves joints, writes transforms
  and velocities back, and reconciles contact events. The scene graph is read
  once and written once; the inner loop never touches it.

Shape and motion stay separate components, so a static collider and a dynamic
body are one collision routine in two configurations — the composition model of
[ADR-0002](0002-hybrid-scene-ecs-architecture.md) doing the same work it does in
2D.

### The backend seam (the part that matters)

The module is designed so the hand-written solver can be replaced by a
third-party library without a rewrite of gameplay or the editor:

- **Components are the API.** Gameplay and the editor see `Collider3D`,
  `RigidBody3D`, `DistanceJoint3D`, and the query free functions — reflected,
  serialized data — never a solver type. Swapping the solver changes the
  *simulation*, not the *authored data*.
- **The system is one implementation of a step.** `Physics3DSystem` owns a
  `update(context, scene)` that flattens, simulates, and writes back. A Jolt
  backend would implement the same contract: read the same components, own its
  internal world, and write the same transforms and velocities. Nothing above
  the module names a solver.
- **Queries are free functions over a scene**, so a future backend answers them
  from its own world without the call sites changing.

This is the same seam the renderer uses for the RHI: an interface the engine
talks to, with the concrete implementation chosen below it. It is deliberately
*not* a full physics abstraction with virtual worlds and handles yet — that would
be speculative. The commitment is narrower and testable: **no gameplay or editor
code depends on the solver's internals**, so the replacement is a module-internal
change.

### When to replace it

The hand-written solver stays the default while it is sufficient: simple
gameplay, deterministic tests, headless builds, and scenes that box and sphere
shapes can describe. Integrate Jolt when a concrete need appears — convex
content, stable stacking at scale, character controllers, or a shipped game that
demands them — and do it behind the seam above. Do not extend the hand-written
solver toward production parity in the meantime; that is the multi-year path this
decision exists to avoid.

## Consequences

- 3D gameplay is possible now, with the same authoring model as 2D: components,
  reflection, serialization, and the systems schedule all work unchanged.
- The module is honest in its documentation about being a first cut, so its
  limits are not mistaken for a finished solver.
- Replacing the solver is a contained change because the components and the
  query functions are the API, not `Physics3DSystem`.
- The seam is not yet a formal interface. If a second backend actually lands, the
  step contract should be promoted to an explicit interface in the module header
  and recorded in a follow-up ADR; until then it is a convention the module
  keeps, not an abstraction it pays for.
- Until angular bodies arrive, 3D rotation is not simulated; a 3D object can be
  moved and collided but not spun by physics.

## Alternatives considered

- **Integrate Jolt immediately.** Rejected for now. It is the right long-term
  backend, but adopting a third-party solver before the engine's own 3D data
  path (components, reflection, serialization, the schedule) is exercised in 3D
  would front-load integration work ahead of the foundation it depends on. The
  seam makes this a later, reversible step rather than a now-or-never one.
- **Never write a solver; only wrap a library.** Rejected. The hand-written core
  is a small, deterministic reference that validates the architecture and serves
  tests and headless builds; it earns its place even if it is later demoted to a
  secondary backend.
- **A full physics abstraction now** (virtual `PhysicsWorld`, body handles,
  shape descriptors). Rejected as speculative. It is real cost paid before a
  second implementation exists; the component-and-query boundary already gives
  most of the reversibility for none of the abstraction.
- **Match 2D feature-for-feature** (angular dynamics, revolute joints, islands,
  sleeping). Rejected for the first cut. Each is a real subsystem; adding them
  before a game needs them is the breadth-without-depth trap the project's own
  guiding rule warns against.
