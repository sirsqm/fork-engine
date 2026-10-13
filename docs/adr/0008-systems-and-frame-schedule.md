# ADR-0008: Systems and the frame schedule

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0001](0001-engine-architecture-overview.md),
  [ADR-0002](0002-hybrid-scene-ecs-architecture.md)

## Context

ADR-0002 chose a hybrid model: a scene graph for authoring, and systems that
read component data into their own layout for execution. It stated the rule but
left the mechanism unspecified. Until now the only thing that ran per frame was
`Renderer2D`, which happens to follow the rule but is not a reusable execution
path: a game that wants to advance an animation, integrate a velocity, or emit
a particle had no place to put that code. The engine could display a scene; it
could not yet *run* one.

The obvious answer is the one most engines ship: give `Node` and `Component` a
virtual `update(dt)` and call it in a loop. That is also the answer ADR-0002
rejected, for concrete reasons. A per-node virtual call forces a fixed execution
shape on every subsystem, defeats data locality (one non-contiguous call per
object), and spreads the definition of "when does this run" across every class.
It is the Godot/Unity-instantiation model, and adopting it would quietly undo
the hybrid decision.

## Decision

Introduce a small **systems layer** and choose execution at the *system* level,
not the node level.

- A `System` is a unit of per-frame behaviour. It declares a name and one method:
  `update(const FrameContext&, Scene&)`. It owns its own traversal.
- `FrameContext` carries `delta_time`, `elapsed_time`, and a nullable
  `ResourceManager*`. A system reads what it needs from the context rather than
  from global state, which is what makes it headlessly testable.
- `SceneSchedule` is an ordered list of systems run once per frame. It is
  deliberately *not* a job graph: no parallelism, no read/write declarations, no
  dependency solver.
- The schedule is owned by whatever owns the scene, not by `Engine`. `Engine`
  holds no scene, so it cannot hold the scene's schedule either. `Application`
  creates a scene and a schedule and pairs them; the editor does the same.
- `Node` and `Component` gain no `update()`. There is no per-node behaviour
  callback anywhere in the engine.

The first system, `SpriteAnimationSystem`, advances the `SpriteAnimator`
component and writes the current cell's UVs onto the node's `Sprite2D`. It is
the proof that the model works end to end, not a headline feature.

## Consequences

**Easier:**

- Execution shape matches the data. `SpriteAnimationSystem` copies the animators
  it will advance into a flat array and iterates it; a future physics system can
  choose a completely different layout without asking permission from a base
  class.
- Systems are unit-testable with no window and no GPU: construct a scene, call
  `update` with a known delta, assert on the result.
- "What runs each frame, and in what order?" has a single answer: the schedule
  contents. There is no hidden traversal order to reason about.
- The editor runs a game headlessly by using the same `Scene + SceneSchedule`
  pair the standalone `Application` uses. Play-in-editor is not a second code
  path.

**Harder:**

- Some behaviour that "belongs" to a component has to be written as a system.
  This is the deliberate cost of the hybrid model and is documented; it is the
  same trade-off ADR-0002 already accepted.
- Systems share the scene by reference, so a system can observe another system's
  writes within a frame. Ordering is therefore semantically significant and must
  be explicit at the registration site.

## Alternatives considered

- **Per-node `virtual void update(dt)` on `Node`/`Component`.** Intuitive and
  familiar, but it is the model ADR-0002 rejected: fixed execution shape,
  scattered scheduling, poor locality. Rejected.
- **An ECS world with entities, archetypes, and systems on dense storage.**
  Excellent for the performance-critical core, but a large rewrite of the scene
  model and a worse fit for editor authoring. Not needed yet; ADR-0002 keeps the
  door open for an individual system to adopt a data-oriented layout *internally*,
  which is the cheap version of the same idea.
- **A parallel job graph with declared read/write sets.** The right end state
  for a large runtime, and premature now: with one real system, a dependency
  solver is complexity that cannot yet be validated. To be revisited when
  profiling shows a frame budget that needs it.
- **A global singleton "system registry".** Makes testing and multiple scenes in
  one process awkward. The schedule is a plain value owned next to the scene,
  so it can be constructed freely in a test.
