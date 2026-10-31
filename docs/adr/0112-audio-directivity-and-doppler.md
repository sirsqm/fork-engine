# ADR-0112: Audio directivity and Doppler

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0034](0034-audio-attenuation-buses.md),
  [ADR-0097](0097-audio-listener-from-camera.md),
  [ADR-0099](0099-stereo-panning-from-listener-orientation.md),
  [ADR-0016](0016-audio-module.md)

## Context

[ADR-0034](0034-audio-attenuation-buses.md) added distance attenuation and buses;
[ADR-0097](0097-audio-listener-from-camera.md) drove the listener from the active
camera; [ADR-0099](0099-stereo-panning-from-listener-orientation.md) panned a
source across the output by the listener's orientation. The M6 audio row names
what is left of the refinement set: **Doppler, cone attenuation, and a
head-related filter for front/back cues.**

Two of those three are physical, and share a shape: each needs a *velocity* or a
*direction* that the scene already has but the mixer does not. A source aimed
like a loudspeaker needs a cone axis; a source moving relative to the ear needs
both velocities. The head-related filter is different in kind -- it is a
per-listener transfer function, a filter bank rather than a gain -- and is left
for its own record.

The constraints are the ones ADR-0034 set, unchanged:

- **CI has no sound card.** Every new term must be provable through the null
  device, so the maths belongs in the pure mixer and `AudioSystem` stays thin
  plumbing that resolves scene data into plain values.
- **A scene authored before this sounds exactly as it did.** A source with no
  cone and no Doppler must mix byte for byte as before. Both features are opt-in
  and both default to "off".

## Decision

**Extend the pure mixer with a directivity cone and a Doppler shift, and have
`AudioSystem` derive the axis and the velocities from the scene.**

### Directivity cone

`AudioSource` gains `cone_inner_degrees`, `cone_outer_degrees`, and
`cone_outer_gain`. They are **half-angles**: full gain inside the inner angle,
falling linearly to `cone_outer_gain` at the outer angle, and that gain beyond.
The axis is the **node's own forward** -- the `-Z` of its world transform, the
convention `Camera3D::forward()` uses -- so a source is aimed by rotating its
node, with no second transform to keep in sync. The defaults (`360/360/1`) make
the source omnidirectional, so a source that never sets a cone is unchanged.
`AudioSystem` computes the axis only when the cone is non-omnidirectional and
spatial, and leaves it zero otherwise, which the mixer reads as "no cone" and
skips the vector maths.

### Doppler shift

`AudioSource` gains a `doppler` flag. `AudioSystem` gains a listener velocity, a
speed of sound (default 343, the speed of sound in air for a metre-unit scene),
and a global enable. The source's velocity is **derived from how far its node
moved since the previous frame** -- `(position - previous_position) / delta_time`
-- so a moving node shifts pitch with no extra authoring. The first frame an
entry exists has no previous sample, so its velocity is zero; a non-positive
delta (a paused step) leaves the previous position in place and reports no
velocity, so a pause is not a huge jump. The shift is off by default at both the
system and the source, so an existing scene is unchanged.

The two physical facts the classic Doppler relation needs are the **closing
speed** (the component of each velocity along the source-to-listener line, taken
positive *towards* the other) and the **speed of sound**:

```
f_observed = f_emitted * (c - v_listener) / (c - v_source)
```

so a listener closing on the source raises the numerator (a pitch-up) and a
source closing on the listener lowers the denominator (also a pitch-up). The
factor is clamped to `[0.25, 4]` so a runaway velocity cannot pitch a voice to an
absurd resampling rate, and a supersonic closing speed (a non-positive numerator
or denominator) is clamped to "no shift" rather than producing a nonsense pitch.
Because the source velocity is a finite difference of positions, the whole effect
is a pure function of two frames of transforms, which is what makes it testable
headlessly.

## Consequences

**Easier:**

- An NPC calls out in the direction they face, a megaphone is loud in front and
  quiet behind, and an engine pitches up as it passes -- all by setting fields on
  the source and (for Doppler) one system flag.
- Both effects are pure and headless: cone gain at a range of angles, the
  inner/outer interpolation, the omnidirectional default, the closing-speed
  sign, and the pause case are all asserted with the null device.
- The listener gains a velocity and a speed of sound as ordinary settable state,
  so a game with a fast camera and a custom unit scale can match the effect to
  its world.

**Harder / not yet done:**

- A head-related filter for front/back cues remains. It is a per-listener
  transfer function, not a gain, so it needs a filter stage the mixer does not
  have and is left for its own record.
- The source velocity is a per-frame finite difference, so a source that
  teleports registers one frame of a very large velocity. That is the same
  limitation every position-derived velocity has; a game that moves a source by
  teleport can set `doppler = false` on it for that frame.
- The cone axis is the node's `-Z`, so a source that wants a different axis
  needs a child node or a future axis field. The forward convention was chosen
  because it matches the camera and needs no second transform.

## Alternatives considered

- **A cone axis stored on the source as a vector.** Rejected: it is a second
  transform to keep in sync with the node, and it duplicates what the node's
  rotation already says. The `-Z` forward convention is the one the rest of the
  engine already uses.
- **Deriving the source velocity from a stored `velocity` field instead of
  position.** Rejected for the scene path: the node is what moves, so a stored
  velocity would have to be written by hand and kept in step with the transform.
  Differencing the position the mixer already receives makes a moving node shift
  pitch with no authoring.
- **On by default.** Rejected: it would change every existing mix, and the
  compatibility rule ADR-0034 set is that a scene authored before a refinement
  sounds exactly as it did.
- **Putting the maths in `AudioSystem`.** Rejected: CI has no sound card, so
  anything in the system is untested on every build. The mixer is the single
  source of mixing truth, as ADR-0034 established.
