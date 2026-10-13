# ADR-0026: 2D island detection and body sleeping

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md),
  [ADR-0013](0013-physics2d-queries-events-angular.md),
  [ADR-0021](0021-physics2d-joints.md),
  [ADR-0023](0023-physics2d-broadphase.md)

## Context

ADR-0009 shipped the 2D solver, ADR-0013 added queries, contact events, and
angular dynamics, and ADR-0021 added joints. Each step left "sleeping" on the M5
refinement list. A settled scene is the common case: a level loads, a pile of
crates comes to rest, and from then on the solver re-resolves contacts that have
not changed and integrates bodies that have no velocity. That work is pure waste,
and it is the easiest large saving in the module.

Two facts about the existing solver shape the decision:

- The solver runs a **sequential-impulse loop over every pair, every step**
  (ADR-0009), with a per-body flat array gathered from the scene. Sleeping must
  skip work in that loop without changing the loop's shape.
- A body is a node with a `Collider2D` and/or a `RigidBody2D` (ADR-0021), and
  its motion is its `Node::transform()` position/rotation plus its `RigidBody2D`
  velocity. Sleeping has to leave both in a consistent state.

The question is how to decide when a body may stop, how to wake it, and where the
decision belongs, without a second solver or a change to any shared header.

## Decision

Add island detection and sleeping **inside the existing `physics2d` module**. No
new module, no change to `System`, `SceneSchedule`, or any other shared header.

### Sleeping is decided per island, not per body

A body alone is the wrong unit. A crate resting on another crate, or a pair held
by a joint, must stop and start together: if one slept while the other was still
integrating, the pair would drift apart or jitter. So bodies are grouped into
**islands** -- the connected components of the graph whose vertices are the
dynamic bodies and whose edges are contacts and joints.

The grouping is done by `IslandBuilder` in `IslandSolver.h`, a pure type with no
scene and no components. It is union-find with path compression and union by
size; `build(body_count, links)` groups `usize` body indices and returns the
islands sorted by their lowest member. Keeping it pure is the point: "which bodies
are grouped together" is a graph question, and it is answered and tested without a
world.

**A link to a static body is not an edge.** Static geometry is collided with but
does not conduct motion between the bodies resting on it, so a crate on the left
of a floor and a crate on the right are two islands even though both touch the
same floor. This is what lets one settle and sleep while the other is still
moving, and it is the reason the graph has only dynamic bodies as vertices.

### The step order: islands and wake checks before the solver

One step now runs, in order:

1. **gather** -- flatten the scene into bodies, joints, and the node index
2. **broad phase** -- find the candidate contact pairs once for the step
3. **islands** -- group the bodies by contacts and joints
4. **wake checks** -- wake an island touched by an awake dynamic body, then
   propagate that wake across the whole island
5. **integrate** -- gravity, torque, damping, position, rotation; awake bodies only
6. **solve** -- contacts and joints; awake bodies only
7. **sleep checks** -- advance each awake island's rest timers, then sleep a whole
   island once every body in it has been still long enough

Two orderings matter and are deliberate:

- **Wake checks run before integration.** The island graph links a pair whose
  bounds overlap, and that link exists *before* the solver runs, so a moving body
  wakes a sleeper it has just reached in the same step. The solver then resolves
  the pair, so an awake body can never pass through a sleeping one. Had islands
  been built from the exact contact set, they could only be built *after* the
  solve, and a fast body could overlap a sleeper for a frame before anything
  noticed.
- **Sleep checks run last.** The decision is made on the velocity the solver
  actually leaves behind, not the velocity entering the step, so a body that was
  pushed this step does not count as at rest.

### The link set is conservative

An edge is added for a pair of dynamic bodies whose **bounds overlap** (the broad
phase's notion of "may be touching") or a joint between two dynamic bodies.
Using bounds rather than the exact contact is deliberate: it can join two bodies
that are near but not quite touching, which only makes sleeping *more*
conservative -- the group wakes and sleeps together -- never less safe. It is also
what lets the graph be built before the solver runs. The bounds test here is a
simple O(n^2) loop over the gathered bodies. It is separate from the solver's
uniform-grid broad phase (ADR-0023): the island graph is built *before*
integration, over the pre-step bounds, while that broad phase is built *after*
it, over the post-step bounds. The two passes are the next thing to unify if they
are ever brought to the same point in the step.

### The sleep state is observable and controllable

`RigidBody2D` gains the sleep state and its knobs:

| Field | Meaning |
| --- | --- |
| `can_sleep` | when false, the body never sleeps and keeps its island awake |
| `is_sleeping` | live state: the solver and integrator skip the body |
| `sleep_time` | how long motion must stay below the thresholds (default 0.5 s) |
| `sleep_linear_threshold` | linear speed below which the body counts as at rest |
| `sleep_angular_threshold` | angular speed below which the body counts as at rest |
| `sleep_timer` | how long the body has been below the thresholds (runtime) |

`is_resting()` reports the raw threshold test; `sleep()` and `wake()` are the
explicit controls. The authored knobs serialize; the live state (`is_sleeping`,
`sleep_timer`) deliberately does not, so a saved scene does not reload with
bodies frozen mid-settle. This is registered in `PhysicsTypes.cpp` with the rest
of the component's properties, so it appears in the inspector with no editor code
(ADR-0006).

A body is **woken** by any of: a contact with an awake dynamic body, a joint to
an awake dynamic body, or an explicit `wake()`. Waking is propagated across the
whole island before the solver runs, so waking one body of a jointed pair moves
both.

## Consequences

**Easier:**

- A settled scene stops doing solver and integration work: a sleeping body is
  skipped in both the contact loop and the joint pass.
- The island grouping is reusable: it is the natural unit for any future
  per-group pass (a joint-aware iteration, a sleeping hierarchy), and it is
  already exposed through `island_count()` and `same_island()`.
- The pure `IslandBuilder` is exhaustively unit-testable with hand-written links,
  and the system behaviour is deterministic, so the whole feature is testable
  headless with no timing thresholds.

**Harder / accepted costs:**

- **Thresholds are heuristics.** A body is asleep when it has been slow for
  `sleep_time`, not when it is provably at equilibrium. A body that is genuinely
  creeping below the threshold will sleep and stop creeping. That is the intended
  trade: the thresholds are exposed so a game can retune them.
- **The bounds test over-joins.** Two dynamic bodies whose bounds overlap but
  which never actually touch are one island. They wake and sleep together, which
  costs a little work and is never wrong, only conservative.
- **An O(n^2) link pass.** The island link enumeration is quadratic in the number
  of bodies. It is separate from the solver's uniform-grid broad phase (ADR-0023)
  because the island graph is built before integration and the broad phase after
  it; folding them into one pass is deferred, and the sleeping feature does not
  add a second partition.
- **A sleeping body's contacts are still recorded.** The event pass still diffs
  the touching pairs each step so a contact that ends because a node left the
  scene is reported cleanly; a sleeping body's position does not change, so its
  contacts neither begin nor end and the event stream stays quiet.
- **Wake is conservative across an island.** Waking one body wakes every dynamic
  body it is connected to, even if some were about to sleep. Correct and simple;
  a finer per-body wake that still preserves the group invariant is deferred.

## Alternatives considered

- **Per-body sleeping.** Rejected: it is the classic bug source. A body resting
  on another that sleeps alone freezes while its neighbour keeps integrating, so
  the pair separates or jitters. The island is the smallest unit that is correct.
- **Islands from the exact contact set, built after the solve.** Rejected: the
  graph would only be available after the step, so a fast body could overlap a
  sleeper for a frame before waking it. Building from overlapping bounds before
  the solve removes the window.
- **A separate `physics2d_sleep` module.** Rejected: sleeping is a physics
  decision about physics bodies; it belongs beside the solver that skips them. A
  module boundary would add a dependency edge for no isolation benefit.
- **A wake-on-impulse hook in the solver.** Rejected: it would scatter the wake
  decision through the impulse code and miss a wake from a joint or from an
  explicit call. One wake pass, before the solver, is the single place the
  decision is made.
- **Sleeping driven by the editor or a gameplay system.** Rejected: whether a
  body is at rest is a property of the simulation, and putting it in the editor
  would leave play-in-editor and headless runs without it. It belongs in the
  system.
- **A single global `sleep_time` on the system.** Rejected as the primary knob: a
  platform that must keep integrating needs `can_sleep = false` on the body, and
  a heavy object may want a longer settle than a light one. Per-body knobs are the
  flexible choice; a system default can be layered on later.
