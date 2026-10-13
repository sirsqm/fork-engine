# ADR-0009: 2D physics as the first gameplay system

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0008](0008-systems-and-frame-schedule.md)

## Context

ADR-0008 added the systems layer and one system (`SpriteAnimationSystem`) that
only reads and writes data already on the nodes it touches. It proved the
mechanism, but not the harder claim: that a system can *simulate* the scene -
change positions over time, resolve interactions between objects - without the
scene graph dictating how it executes. Physics is the natural first test of that
claim, and it is also the first gameplay capability the engine actually needs.

The question is not only "how do we do physics", but "what does the first
simulation system teach us about the composition model". A physics engine is the
classic case where the authoring representation (a tree of nodes with
transforms) is a poor match for the execution representation (a flat array of
bodies). If the hybrid model is right, a physics system should be able to
flatten the scene into its own layout, simulate, and write back, with no
per-node callback anywhere.

## Decision

Add a **`physics2d` module** with a rigid-body simulation system, and model
bodies as components rather than node subclasses.

### Shapes and bodies are separate components

- `Collider2D` holds a shape (`Box` with a half-extent, or `Circle` with a
  radius) and the surface material (`restitution`, `friction`).
- `RigidBody2D` holds linear velocity, mass, damping, and two flags
  (`kinematic`, `affected_by_gravity`).

A node with a `Collider2D` and no `RigidBody2D` is a **static obstacle**: it is
collided with but never moves. A node with both, with positive mass, is a
**dynamic body**. "Floor" and "falling crate" are the same two components in
different configurations, not two classes. This is the composition model of
ADR-0002 doing real work: the split between "what shape am I" and "how do I
move" is exactly the split that lets a static collider and a dynamic body share
one collision routine.

### The solver is a system, and it flattens the scene

`Physics2DSystem` is an ordinary `System`. Each `update`:

1. Walks the scene once and copies every collider (with its node's world
   position and material data) into a flat `std::vector<Body>`.
2. Integrates velocity (gravity, damping) and then position.
3. Runs a fixed number of **sequential-impulse** iterations over every pair:
   broad-phase bounding-box rejection, then narrow phase, then a normal impulse
   with restitution and a Coulomb friction impulse, plus Baumgarte positional
   correction.
4. Writes positions back to the node transforms and velocities back to the
   rigid bodies.

The scene graph is read once and written once. The inner loop never touches it.

### Deliberate scope limits

This is the *first* cut, and it is honest about what it does not do:

- **No angular dynamics.** No rotation, no torque, no angular velocity, no
  inertia tensor. A body keeps whatever rotation its transform has; the solver
  does not change it.
- **No collider offset.** A shape is centred on the node's world position. An
  offset would have to be rotated to stay correct, and without angular dynamics
  it could not be. Centring the shape is honest; shipping an offset that breaks
  under rotation would not be.
- **No sleeping, no joints, no continuous collision detection, no spatial
  partition.** The broad phase is a bounding-box test inside an O(n²) pair
  loop. A spatial hash belongs there when profiling shows it matters.
- **No event/callback API.** A system or a game reads positions after the
  update. Contact callbacks are a later addition, and they need a story for
  when in the frame they fire before they can be designed well.

## Consequences

**Easier:**

- The composition model is validated by a real consumer. `Physics2DSystem`
  demonstrates the full hybrid claim: author in a tree, execute on a flat array,
  write back. No base class was changed to make it possible.
- Static and dynamic behaviour are one component apart, so a level is authored
  by adding or omitting a `RigidBody2D`. The editor will reflect that directly.
- Both components are reflected, so they serialize and will appear in the
  inspector with no per-type editor code.
- The geometry (`intersect` for box/box, circle/circle, circle/box) is pure and
  allocation-free, so it is exhaustively testable without a scene or a GPU.

**Harder / accepted costs:**

- Sequential impulse with a fixed iteration count is not a general-purpose
  solver. Stacks are handled approximately; a tall stack will jitter. That is
  the standard trade-off for a first implementation and is the reason the
  iteration count is a tunable field rather than a constant.
- Because there is no angular dynamics, a box collider is an AABB and cannot
  rotate. A rotating box collider is a known limitation, not a bug.
- Writing positions back onto `Node::transform()` keeps the transform the single
  source of truth for position, but it means a physics body and a script that
  both write the transform can fight. The resolution (physics is authoritative
  for a dynamic body) is deferred until scripting exists.

## Alternatives considered

- **A full third-party physics library (Box2D, Chipmunk).** Rejected for now.
  Both are excellent and would be the right choice for production 3D. For the
  first 2D cut, the value of a physics system here is as much architectural as
  functional: writing it against our own component model proves the model. The
  RHI is already abstracted for exactly this reason, and a future ADR can swap
  the solver for a library behind the same `Physics2DSystem` surface once the
  feature set (rotation, joints, CCD) justifies the dependency.
- **A node subclass per body (`StaticBody2D`, `RigidBody2D`).** Rejected: it
  hard-codes the composition into the type hierarchy and is the Godot model that
  ADR-0002 moved away from. Components keep "static" and "dynamic" a data
  difference.
- **A full ECS with archetype storage for physics.** Rejected as premature. The
  flatten-into-a-vector step gets the locality benefit for the shapes this cut
  supports without committing the whole engine to ECS storage. If physics grows
  into the dominant cost, that is the point to revisit.
