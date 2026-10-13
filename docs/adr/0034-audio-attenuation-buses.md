# ADR-0034: Audio attenuation and mixing buses

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0016](0016-audio-module.md)

## Context

[ADR-0016](0016-audio-module.md) shipped the audio module with a device seam, a
pure `mix_voices`, and an `AudioSource` that already carried a `spatial` flag and
min/max distances - stored and serialized, but ignored by the mixer. It also
deferred "audio buses and mixing groups" outright. This record covers the first
two M6 audio refinements: **3D positional attenuation** and **a bus / mixing
group layer**.

Two constraints carry over unchanged from ADR-0016 and shape every decision
here:

- **CI has no sound card.** Whatever is added must be provable with the null
  device, or it is untested on every build. So the new maths belongs in the pure
  mixer, not in the device.
- **The mixer is the single source of mixing truth.** `AudioSystem` is thin
  plumbing around it. Adding attenuation and buses must not move maths into the
  system or make the mixer depend on the scene.

A scene authored before this refinement must also sound exactly as it did: a
source that is not positional and is on no bus keeps full gain. That is the
compatibility line the design has to hold.

## Decision

**Extend the pure mixer with a documented distance-attenuation curve and a flat
bus table, and let `AudioSystem` resolve the scene data (listener position, node
world transform, bus names) into the plain values the mixer consumes.**

### Distance attenuation: inverse distance with a cutoff

A positional voice's gain falls off by distance from the listener to the source's
**world position**. Within `min_distance` it is full gain; at `max_distance` it
reaches exactly silence; beyond it stays silent. Between the two it follows an
**inverse-distance** curve, rescaled so it terminates at zero at the cutoff:

```
gain(d) = (1/d - 1/max_distance) / (1/min_distance - 1/max_distance)
```

This is the shape most engines call "inverse distance clamped", with the clamp
promoted to a true cutoff. Inverse distance was chosen over linear because it is
the physically motivated falloff (sound intensity falls with 1/d) and it keeps a
source close to the listener noticeably louder than a linear ramp would, which is
what makes a positional cue read as positional. The rescale is what makes the
falloff reach zero rather than leaving the constant floor a plain clamped `1/d`
curve leaves at the cutoff - a source does not "stop" if it is still audible at
`max_distance`.

`distance_attenuation()` is a free, pure function in `AudioMixer.h`, so the curve
is unit-tested directly and the mixer and the tests share one definition.
Degenerate inputs (`max_distance <= min_distance`, or `min_distance <= 0`) do not
divide by zero: there is no interior region and the curve becomes a step at
`min_distance`. The function is total and never returns NaN.

The position is taken from the **node's world transform** by `AudioSystem`, never
from a transform of the source's own - ADR-0016 already fixed this rule, and it
is what keeps a hierarchy of moving parents correct with no second source of
truth. The listener is a single `Vec3` on the system (`set_listener_position`),
defaulting to the origin.

### Buses: a flat table, resolved to an index before the mix

A **bus** is a named group with its own gain. A source routes to a bus by name
(`AudioSource::bus_name`); every source that names a bus is scaled by that bus's
gain, so pulling a group down is one number rather than an edit to each source.
Buses sum into the **master** bus, whose gain scales the whole mix.

The mixer sees only a **flat array of `AudioBus` values** and a per-voice **bus
index**; it does no string work and allocates nothing. `AudioSystem` owns the
table, matches `bus_name` to a bus index when it reconciles voices, and passes
the table (plus the master gain and the listener) in a `MixParams` value. This
keeps the mixer deterministic and device-free: the same voices and params always
produce the same samples.

A voice's contribution is scaled by

```
voice.gain * attenuation * params.master_gain * bus_gain(voice.bus)
```

The master is applied once to the summed output; because scaling is linear this
is identical to folding it into every voice, and it matches the "buses sum into
the master" mental model. A voice on no bus (`kNoBus`) is scaled by the master
gain alone. A **non-positional source on no bus therefore keeps exactly its
authored gain** - the pre-refinement behaviour, asserted as a regression test.

The master bus is deliberately **not** an entry in the table: its gain travels in
`MixParams::master_gain` so there is a single owner of the value. `set_bus_gain`
with the reserved empty name is a no-op rather than a second place to set it.

A muted voice (bus gain 0, source gain 0, or past its cutoff) still **advances
its read position**. Unmuting a group resumes where the group was muted rather
than replaying from the start, which is what a player expects from a volume
slider.

### What stays out of the mixer

The mixer takes no `Scene`, no `Node`, and no `ResourceManager`. Turning a node's
world transform and a bus name into a `Voice` is `AudioSystem`'s job; the mixer's
contract is unchanged in spirit from ADR-0016, only wider.

## Consequences

**Easier:**

- Attenuation and buses are unit-tested with no hardware, like the rest of the
  mixer: the curve is asserted at its endpoints and for monotonicity, a bus gain
  is asserted to scale its sources, and the master is asserted to scale the sum.
- A new level control (a mute/solo, a duck, a future effect chain) is a value in
  the bus table, not a new branch in the mix loop.
- The bus table is `std::vector<AudioBus>` reused across frames; the mix path
  stays allocation-free in steady state and deterministic.

**Harder / accepted costs:**

- Bus routing is **flat**: a bus cannot itself feed another bus. The master is
  the only implicit parent. This is enough for the M6 goal (independent groups
  into a master) and avoids a graph, a cycle check, and a topological order; a
  hierarchical graph is a later change if a real need appears.
- Matching a source's `bus_name` to an index is an O(buses) string compare per
  source per frame during reconciliation. With a handful of buses and sources
  this is not worth avoiding; a large bus set would want the index cached on the
  source.
- Distance attenuation is computed once per voice per `mix_voices` call, not
  per output frame. A voice that moves within a single mix block would want a
  finer curve; at a game's frame size the difference is inaudible.

## Alternatives considered

- **A linear rolloff.** Rejected: it is simpler to reason about but has no
  physical basis and makes near-field sources too quiet relative to far ones,
  which weakens the positional cue. The curve is a one-function change if a
  project ever wants it.
- **Plain clamped `1/d` (a constant floor at `max_distance`).** Rejected: it
  never reaches silence, so a source at the cutoff is still audible and there is
  no natural "cull" distance. The rescale gives a true cutoff for free.
- **A pull-callback attenuation hook** (the game provides a distance function).
  Rejected: it moves a deterministic, testable curve into user code and makes the
  mixer depend on a callback. A documented built-in curve is provable in CI.
- **Hierarchical buses (a graph).** Rejected for this slice as noted above: flat
  groups into a master cover the goal with no cycle handling.
- **Per-voice master scaling instead of post-sum.** Rejected: equivalent
  numerically, but post-sum matches "buses sum into the master" and keeps the
  master in one place.

## Deferred

Still out of scope, and tracked in the roadmap:

- Streaming / large-file playback (clips are fully decoded into memory).
- Compressed codecs (only PCM and float WAV are decoded).
- Hierarchical buses, mute/solo state, and per-bus effect chains.
- Doppler shift and directional (cone) attenuation.
