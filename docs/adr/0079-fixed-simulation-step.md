# 0079 — A fixed simulation step

Date: 2026-10-25

Status: Accepted

## Context

`Engine::tick()` advanced the clock by the measured wall-clock delta between
frames, clamped to `kMaxDeltaTime` so a stall does not become a huge step. That
is what a game wants: the simulation follows real time, and a slow frame moves
things further than a fast one.

It is the wrong thing for a run that has to be *reproducible*. The web game check
(`tests/web/run_web_game.sh`) verifies that input reaches gameplay by turning on
the player's input probe: the runtime presses movement keys with real synthetic
DOM events and reports how far the watched node moved. Under a measured delta the
distance depends on how long the browser took over the two frames it ran, which
under headless Chromium on a loaded runner is wildly variable — the same run
reported `moved=1.000` and `moved=0.000` on different attempts. A check whose
result is a wall-clock measurement is flaky by construction, and the flakiness is
in the *verification*, not the engine.

The same need recurs beyond the check: a headless capture, a replay, a physics
step that must be stable, and any test that wants to assert "after N frames the
node is here" rather than "after N frames the node is somewhere plausible".

## Decision

Add `Engine::Config::fixed_delta_time` (seconds). Zero — the default — keeps the
existing behaviour exactly: `tick()` steps by the measured wall-clock delta. A
positive value makes every frame advance by exactly that much regardless of how
long the frame took, so the same scene and the same input produce the same state
on any machine, at any frame rate. `elapsed_time()` then measures simulated time
rather than wall-clock time, which is the point.

The player exposes it as `--fixed-step SECONDS`, and the input probe pins a step
of 1/60 s by default, so a probe run is reproducible without the caller having to
know the flag. A plain run leaves it at 0 and steps by the measured delta as a
game does.

This is deliberately a property of the engine's clock rather than of the probe:
the probe *measures* motion, and the fix belongs where the motion comes from. Any
other consumer that needs a stable simulation gets the same switch.

## Consequences

* A browser verification run is deterministic: the input probe reports the same
  distance on every attempt, so a non-zero `moved` is a property of the input and
  the behaviour rather than of the runner's load.
* A game can pin its simulation step for a stable, reproducible simulation — the
  usual reason a game wants a fixed timestep — without a new subsystem.
* The default is unchanged, so nothing that ran before behaves differently.
* This is a *fixed* step, not a *fixed-timestep accumulator*: it does not
  interpolate between steps, and it does not catch up after a stall (a stall is
  simply a normal frame as far as the simulation is concerned). That is enough
  for determinism and for the probe, and it is a smaller, clearer change than a
  full fixed-timestep loop with interpolation, which a game that needs smooth
  rendering under a low step rate would want later.

## Alternatives considered

* **Make the probe report frames rather than distance.** Rejected: distance is
  what proves the behaviour ran on the input; a frame count proves only that the
  loop ran, which the existing checks already cover.
* **Have the probe accumulate its own fixed delta and ignore the engine's.**
  Rejected: it would test a path the shipped game does not use, and the engine
  clock is where the nondeterminism actually lives.
* **A full fixed-timestep accumulator with interpolation.** Deferred: more
  machinery than this problem needs, and it changes how every game steps. A
  constant step is the smaller change that removes the flakiness and gives a game
  the stability knob.
