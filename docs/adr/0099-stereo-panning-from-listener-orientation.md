# ADR-0099: Stereo panning from the listener's orientation

Date: 2026-10-28

Status: Accepted

## Context

[ADR-0034](0034-audio-attenuation-buses.md) gave the mixer distance attenuation
and buses; [ADR-0097](0097-audio-listener-from-camera.md) moved the listener to
the active camera so a positional source is attenuated against the player's ear
rather than the world origin. Both left the mixer's spatial model **distance
only**: it measured how *far* a source was and said nothing about which *side* of
the listener it was on. A source five metres to the player's right and one five
metres to the left were mixed identically — same gain, same two output channels.
That is the audible half of spatialisation missing: a player cannot tell which
way a sound is, only how close.

ADR-0097 recorded the follow-up explicitly in its "Deferred" section — *"Listener
orientation for a directional panner (the current curve is distance-only)"* — and
its own consequences note that a 2D camera has no ear direction to pan against.
This record is that follow-up.

The forces at play:

- **Panning needs a facing, and only a 3D camera has one.** A pan is the
  direction to a source projected onto the listener's left/right axis. That axis
  comes from the ear's forward and up vectors. `Camera3D` has a world rotation and
  therefore a forward and an up; `Camera2D` has a position and no up vector in the
  plane, so it cannot supply a left/right axis at all.
- **The change must be additive.** Distance attenuation, buses, the master gain,
  streaming, and every existing audio test were already correct and asserted
  sample by sample. A pan law that scales a *centred* voice would change those
  expected samples and quietly alter every 2D game's mix, which is a regression
  dressed as a feature.
- **CI has no sound card** (ADR-0034, ADR-0097). The maths must be provable as a
  pure function with hand-built voices, and the plumbing with a null device.

## Decision

**Add a constant-power stereo pan to the mixer, driven by the listener's
orientation, and set that orientation from a 3D camera in `AudioListenerSystem`.**

### The pan law is constant power, and it lives in the pure mixer

`pan_gain(pan, channel)` returns the gain for one ear from a pan in `[-1, 1]`:
`-1` is hard left, `0` centred, `+1` hard right. The two gains are the cosine and
sine of a quarter turn, so `left² + right² == 1` at every pan. This is the
standard constant-power law rather than a linear fade, because a linear law dips
about 3 dB in the middle: a sound crossing the listener would audibly duck as it
passed the centre. Constant power keeps its loudness even across the sweep, and at
hard left the right gain is exactly 0, so a hard-panned sound is truly in one ear.

The function is total and pure: a pan outside `[-1, 1]` is clamped (a rounding
error in a normalised direction hard-pans rather than running `cos`/`sin` outside
the first quadrant), a channel other than 0 or 1 is centred rather than silenced,
and it never returns NaN. Being pure, it is asserted directly by the tests rather
than only through a mix.

### The pan is the direction to the source on the listener's right axis

`voice_pan(voice, params)` returns the pan for a voice and records it on
`voice.pan` for diagnostics, exactly as `voice_level` records `attenuation`. The
pan is the unit direction from the listener to the voice dotted with the
listener's right axis (`forward × up`). A source to the right is positive, one
ahead or behind centres (its right component is zero), and one directly overhead
centres too. This is the standard first-order model: it gives left/right
separation, the audible half, and deliberately leaves front/back and elevation to
a future head-related filter.

Distance is not part of the pan — it is a pure direction, so a source does not
become "more panned" as it recedes; distance is `attenuation`'s job and the two
stay orthogonal.

### The listener's facing is opt-in, so the change is additive

`MixParams` gains `listener_forward` and `listener_up`, defaulting to a **zero**
forward vector. A zero forward means "no facing", and `voice_pan` returns 0 for
it, so the mixer applies no per-ear gain at all. This is the default for every
existing caller: every 2D scene, every hand-built test, and every scene that sets
only a position. They mix byte for byte as they did before, which is what keeps
the refinement additive rather than a silent change to every 2D game's mix.

`has_orientation()` is the one predicate for "is there an ear direction", and it
is the same degeneracy `voice_pan` already centres on, so there is one rule
rather than a separate "was it set" flag.

`AudioSystem` carries the facing (`set_listener_orientation`, plus accessors) and
forwards it into `MixParams`, mirroring how it already carries the position.

### A 3D camera supplies the facing; a 2D camera clears it

`Camera3D` gains an `up()` accessor beside its existing `forward()`, both derived
from the owning node's world matrix so a camera on a banking rig reports a rolled
up rather than world up. `AudioListenerSystem` sets the position *and* the
orientation from a 3D camera. A 2D camera sets the position only and **clears**
the orientation, so a scene that switches renderers does not keep panning against
an axis it no longer has.

### Panning applies only to a stereo output, on channels 0 and 1

`mix_voices` and `add_streamed_voice` apply the per-ear gains only when
`out_channels >= 2`, and only to channels 0 and 1; a mono output is centred and a
third or later channel (a surround layout) is centred, because the pan model has
no opinion about them. A non-positional voice is never panned — it has no place
in space. Both the clip path and the streaming path apply the identical rule, so a
streamed spatial voice pans the same as a clip one.

## Consequences

**Easier:**

- A 3D game sounds spatial: a source to the player's right is heard in the right
  ear and one to the left in the left ear, and the separation tracks the camera's
  rotation as the player turns.
- The rule is one pure function, `pan_gain`, plus one direction computation,
  `voice_pan`, so a test can pin the law and the geometry separately without a
  device or a mix.
- The change is additive: a 2D scene, or any caller that sets only a position,
  mixes exactly as before, because a zero facing turns panning off.

**Harder / accepted costs:**

- A scene that opts into panning hears a *centred* source at the constant-power
  centre (about -3 dB), not at its un-panned level. That is the price of an
  even-loudness sweep and is the one behaviour that changes when a 3D scene starts
  panning; it is pinned by a test rather than left implicit.
- Front/back and elevation are not distinguished: a source behind the listener
  centres, exactly like one ahead. Resolving them needs a head-related filter,
  which is a much larger piece of work and is deferred.
- The pan is recomputed each frame from the listener's facing and the voice's
  world position, so it tracks both without any smoothing. A hard pan that
  crosses the listener in one frame can therefore jump rather than glide; a
  smoothed pan is a future refinement, not a correctness issue.
- `MixParams` grows two fields and `Voice` one, all defaulted, so a caller that
  ignores panning is unaffected.

## Alternatives considered

- **A linear pan law (`left = 1 - p`, `right = p`).** Rejected: it sums to 1 but
  dips about 3 dB in the middle, so a source crossing the listener audibly ducks
  as it passes the centre. Constant power is the law every mixer uses and the one
  that keeps loudness even.
- **Default the facing to the identity (looking down -Z, up +Y) instead of
  zero.** Rejected: it would pan every existing scene, including every 2D game and
  every existing test, against an ear direction nobody asked for, and change their
  expected samples. A zero default keeps the refinement additive; a caller that
  has an ear direction opts in.
- **Put the panning in the renderer or the scene, not the mixer.** Rejected: a
  pan is a per-output-channel gain, which is exactly what the mixer produces;
  putting it anywhere else means two places computing the same gain.
- **Apply the pan in `voice_level` as a single number.** Rejected: a pan is a
  *different* gain per ear, not one number for the voice. `voice_level` stays the
  voice's level and the pan multiplies it per channel, which is also why the
  streamed and clip paths can share the per-ear gains.
- **Derive the right axis from `forward` and world up alone.** Rejected: it would
  ignore a rolled listener (a camera on a banking aircraft). Taking the ear's own
  up is no more work and is correct under roll.
- **Special-case the 2D camera with a synthetic facing.** Rejected: a 2D scene has
  no left/right axis in the world — there is no up vector in the plane — so any
  facing would be invented. Leaving panning off for 2D is the honest answer and
  keeps 2D mixes unchanged.

## Deferred

Still out of scope, tracked in the roadmap (ADR-0034's remaining refinements):

- A head-related filter for front/back and elevation cues.
- Doppler shift and directional (cone) attenuation.
- Smoothing a pan that crosses the listener in a single frame.
- A listener "distance model" other than inverse-distance-clamped.
