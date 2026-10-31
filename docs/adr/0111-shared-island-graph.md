# ADR-0111: A shared island graph for 2D and 3D physics

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0026](0026-physics2d-islands-sleeping.md),
  [ADR-0031](0031-physics3d-and-backend-seam.md),
  [ADR-0043](0043-physics3d-angular-dynamics.md)

## Context

[ADR-0026](0026-physics2d-islands-sleeping.md) gave 2D physics islands and body
sleeping: bodies group into islands over the contact-and-joint graph, and a
settled island stops being solved. The graph itself is a small, pure type,
`physics2d::IslandBuilder`, that knows only indices -- no scene, no components,
no physics -- so it is tested exhaustively on hand-written links.

[ADR-0043](0043-physics3d-angular-dynamics.md) and the 3D roadmap row list
"islands / sleeping" as a 3D refinement that mirrors the 2D cut, deliberately
deferred behind the backend seam. Implementing it raises one design question the
2D work did not face: **where does the graph live?**

Two options present themselves. Copy the builder into `physics3d` (a near-exact
duplicate of a type that has no dimension in it), or lift it into a shared
module both systems depend on. The graph has no 2D or 3D content: an island is a
connected component of a graph whose vertices are dynamic bodies and whose edges
are contacts and joints, and none of those are dimensional. A copy would be the
same code maintained twice, and the two copies would drift.

Constraints that shaped the decision:

- **Keep the module graph acyclic and honest.** `physics2d` and `physics3d` are
  siblings that must not depend on each other, so the shared code cannot live in
  either one.
- **Keep the 2D names working.** `physics2d::IslandBuilder` and
  `physics2d::IslandLink` are used by the 2D system and by `test_sleeping.cpp`;
  moving the type must not rewrite those call sites.
- **The graph stays pure.** Whatever is shared is the index-only graph, not the
  system logic around it.

## Decision

Lift the pure island graph into a new shared **`physics`** module
(`fe/physics/IslandSolver.h`, `engine/src/physics/IslandSolver.cpp`), depending
only on `core`. Both `physics2d` and `physics3d` depend on it.

`fe/physics2d/IslandSolver.h` becomes a thin header that includes the shared one
and re-exports the names into `fe::physics2d`, so the 2D system and its tests are
unchanged:

```cpp
namespace fe::physics2d {
using physics::IslandBuilder;
using physics::IslandLink;
}
```

`Physics3DSystem` gains the same island and sleeping machinery the 2D system has,
built on the shared builder: sleep fields on `RigidBody3D` (`can_sleep`,
`is_sleeping`, `sleep_time`, the two thresholds, `sleep_timer`, and `wake()` /
`sleep()` / `is_resting()`), an island graph built before integration from
pre-step bounds and joints, a wake pass that propagates a wake across an island
before the solver runs, and a sleep pass that advances each awake island's rest
timers and sleeps a whole island at once. The step order mirrors 2D: gather,
islands, wake, integrate, broad phase, solve, joints, write back, sleep, and
reconcile. The 3D graph is built over `bounds_overlap` of the colliders and the
distance joints, exactly as 2D, so a moving body wakes a sleeper in the step it
reaches it.

## Consequences

**Easier:**

- One implementation of the graph, one set of graph tests, and no drift between
  the dimensions. The 2D behaviour is unchanged by construction: its system calls
  the same code through the same names.
- The 3D system gains sleeping, so a settled 3D pile stops costing solver time,
  closing the roadmap gap ADR-0043 named.
- The new `physics` module has no dimension and no system logic, so it stays
  trivially testable and cannot grow a scene dependency.

**Harder / not yet done:**

- A body that a gameplay system drives every frame must set `can_sleep = false`,
  or it will sleep and ignore the driver's writes. `CharacterController3D` does
  this for its body, because it writes the body's velocity every frame. This is
  the same contract 2D already documents; it is now a live concern on the 3D
  player path.
- 3D islands are built by an O(n^2) bounds scan separate from the solver's
  uniform-grid broad phase, mirroring 2D. Folding the two passes together is the
  same follow-on in both dimensions and remains open.
- Oriented 3D colliders, more joint types, and continuous collision detection
  remain the other 3D refinements behind the backend seam.

## Alternatives considered

- **Copy the builder into `physics3d`.** Rejected: the graph has no dimensional
  content, so a copy is the same code twice, and the two copies drift. The whole
  point of a pure graph is that it is shareable.
- **Put the shared graph in `physics2d` and have `physics3d` depend on it.**
  Rejected: it makes the sibling 3D module depend on the 2D one, which is a
  false statement about the design -- 3D physics does not use 2D physics.
- **Put the graph in `core`.** Rejected: it is a physics concept, not a core
  primitive; `core` should not carry domain types. A small `physics` module says
  exactly what the code is.
- **Leave 3D without sleeping.** Rejected: ADR-0043 named it as the mirror of the
  2D cut, the shared graph makes it cheap, and a settled pile that keeps being
  solved is real wasted work.
