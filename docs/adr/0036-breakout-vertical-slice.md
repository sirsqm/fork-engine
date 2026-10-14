# ADR-0036: The breakout vertical slice, and the integration test it is

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md),
  [ADR-0004](0004-resource-ownership-weak-cache.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md),
  [ADR-0016](0016-audio-module.md),
  [ADR-0031](0031-physics3d-and-backend-seam.md)

## Context

By the time this ADR was written the engine had a scene graph, resources, a 2D
renderer, 2D and 3D physics, audio, serialization, an editor, and a systems
layer — each with its own unit tests. What it did not have was proof that any of
it worked *together*. Every subsystem had only ever been exercised in isolation;
the seams between them were assumed, not tested.

The roadmap calls this out (`M2.5 — A vertical slice`) and states the project's
guiding rule plainly: a few genuinely working systems beat a long list of
half-finished features. A small, finished game is the cheapest thing that tests
the seams, and it is what attracts contributors, because people contribute around
something finished.

The question this ADR answers is not "should there be an example game" — there
already was a `sandbox` — but *what role the game plays*: a demo that shows the
API, or the project's integration test. Those pull in opposite directions. A demo
is allowed to fake the hard parts; an integration test is not.

## Decision

Ship **one small, complete game — breakout — as the project's integration test**,
and treat its source as test code.

Specifically:

1. **The game is an external consumer.** `examples/breakout` is built against the
   engine's public headers only, with the engine linked as a library. If the
   public API is not self-sufficient to write a game, that is a defect the game
   exposes.

2. **The tests compile the game's own sources.** `fork_engine_tests` pulls
   `breakout_state.cpp`, `breakout_scene.cpp`, and `breakout_systems.cpp` in by
   path and steps them on an in-memory scene. The tests prove the shipped game,
   not a copy of it, and they need no window, GPU, or sound card — so they run in
   CI. The field layout has one definition (`build_breakout_scene`), shared by the
   application, the serialized scene file, and the tests, so the three cannot
   drift.

3. **The rules are pure.** `BreakoutState` has no engine dependency at all. Score,
   lives, bricks, and win/lose are a plain C++ class, unit-tested by hand. Rules
   that need a scene to test are rules that are hard to test.

4. **Gaps are fixed in the engine, not worked around in the game.** The slice is
   allowed to add small, general glue where the engine genuinely lacked it. Two
   such additions were made, and both are general rather than breakout-specific:
   `FrameContext` gained an `InputState` channel (a system must be able to read
   input without reaching back into `Engine`), and `AudioSystem` gained a
   cumulative `peak_max()` (a single-frame peak cannot prove audio was produced
   when the final frame is silent). Anything larger became a focused issue, not a
   new subsystem inside the game.

5. **The game is not a place to hide unfinished engine work.** Nothing in it is a
   placeholder presented as finished. Deferred features — 3D positional audio,
   streaming, compressed codecs, audio buses — are documented as deferred and the
   game uses the simple path deliberately.

## Consequences

**Easier.** The seams are exercised every CI run. A regression in physics contact
events, resource loading, serialization, audio mixing, or the systems order shows
up as a failing game test, not as a bug found by a player. New contributors have a
runnable, readable artifact to start from, and a walkthrough
(`examples/breakout/README.md`) that doubles as the first tutorial. The "one
definition of the field" rule makes the serialized scene a checked artifact rather
than a file that silently rots.

**Harder.** The example directory is now test infrastructure: a change to a shared
engine type can break the game tests, which is the point, but it means the example
cannot be casually deleted or rewritten. The tests reach across the tree with
`${CMAKE_SOURCE_DIR}` paths, which is a little unusual; it is deliberate, so the
tests depend on the game's source, not on `FE_BUILD_EXAMPLES` being enabled.

**Watch for.** The game is not allowed to become a second framework. If a feature
belongs in the engine, it goes in the engine (`examples/` is for using it, not for
inventing it). The breakout-specific systems are game code and stay in the game;
anything they reveal as general is promoted to the engine with its own tests.

## Alternatives considered

- **Test the seams with a synthetic test that builds a scene by hand.** Rejected:
  a hand-built test scene is exactly the copy that drifts from the real field, and
  it never finds the glue problems — the ones that only appear when a real game
  loads real assets, serializes, and runs. A game is a harder and more honest
  test.

- **Keep the sandbox and grow it.** Rejected: the sandbox is a milestone-1 API
  demo, not a game. Growing it would blur "a demonstration of the API" with "a
  finished thing that must ship", and it would accumulate exactly the kind of
  half-finished features the roadmap warns against.

- **Put the game in its own repository.** Deferred, not rejected: a separate repo
  is the eventual home for a flagship game, but while the engine is pre-1.0 the
  integration value of keeping it in-tree (same CI, same commit, no version skew)
  outweighs the separation.

- **Let the tests reimplement the field to avoid coupling.** Rejected for the
  reason above: the coupling is the test. Compiling the game's own sources is what
  makes a passing test mean the shipped game works.
