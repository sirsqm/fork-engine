# ADR-0097: Driving the audio listener from the active camera

Date: 2026-10-25

Status: Accepted

## Context

[ADR-0034](0034-audio-attenuation-buses.md) gave the mixer a distance-attenuation
curve: a positional voice's gain falls off with the distance from the listener to
the source's world position. It also gave the listener a home — a single `Vec3`
on `AudioSystem`, set with `set_listener_position`, defaulting to the origin —
and left the question of *who moves it* open.

Nothing did. A survey of the tree found `set_listener_position` called only from
`tests/src/test_audio.cpp`: the production path never set it, so the listener sat
at the world origin for the life of every game. Every positional source was
therefore attenuated against the origin rather than against the player's ear, and
a scene that placed sounds around a moving camera heard them all as though the
camera never moved. The mixer, the curve, and the buses were all correct; the
value they were fed was wrong, and no test caught it because each test set the
position itself.

Two constraints shape where the fix can live:

- **Module layering.** `audio` depends on `core`, `scene`, `systems`, and
  `resources` — deliberately not on `scene3d`. Choosing the active camera means
  knowing both `Camera3D` (`scene3d`) and `Camera2D` (`scene`), so a listener
  sync inside `audio` would invert the module graph for one `Vec3`. `runtime`
  already depends on everything and already owns the frame loop, so the knowledge
  belongs there.
- **CI has no sound card** (ADR-0034). The wiring must be provable with a null
  device, and the absence of a device must not be a failure.

A second, quieter gap sat next to it: the editor's play-in-editor copied
`Camera2D`, sprites, tilemaps, and the physics components into the play scene, but
not `AudioSource`. An audio node was reported through the unregistered-type path
and dropped, so play mode had no sound at all even though the engine had a mixer
and the authored scene had sources.

## Decision

**Add an `AudioListenerSystem` in `runtime` that copies the active camera's world
position to the audio system's listener, and adopt it — with the audio system and
a real device — in the player and in play-in-editor.**

### The camera is the ear, and its world position is what matters

The system reads the active camera from the renderers' own lookups
(`Renderer3D::find_active_camera`, `Renderer2D::find_active_camera`), so the ear
agrees with what was drawn rather than with a second rule that could drift from
it. A 3D camera is preferred when the scene has one — it is the camera the 3D
pass draws through — and a scene with only a 2D camera falls back to that, which
is every 2D game.

The position is the camera node's **world** position, not a local transform. That
is what makes a camera parented to a moving rig (a vehicle, a character) drag the
listener with it, with no second source of truth for where the player is. The 3D
case reuses `Camera3D::eye()`, which already computes exactly this; the 2D case
computes the same translation from the owner's world matrix, because `Camera2D`
exposes no equivalent accessor.

A camera detached from any node (a component built but never added to a scene)
has no transform to read. The system treats that as "no camera" and leaves the
listener where it was, rather than snapping it to the origin — a stale listener
is recoverable, a listener at the origin is a scene that suddenly hears every
sound from nowhere.

### `audio` stays free of the camera types

The system lives in `runtime` and holds a raw `audio::AudioSystem*`, forward
declared in the header so the header does not pull in `audio`. `audio` gains no
dependency. The null pointer is a supported value: the system is inert without
an audio system, so a caller may add it unconditionally without first proving one
exists.

### The listener is ordered before the mixer

The system is added to the schedule **before** the audio system. Systems run in
registration order, so the mixer reads the listener this frame rather than last
frame's — a fast-moving player otherwise hears every cue from one frame behind.

### A device that will not open is not an error

Adopting the audio system opens an output device where one exists, but a failure
to create, open, or start it is a logged warning, not an error. A CI machine has
no sound card, and a browser starts its audio context suspended until the first
user gesture ([ADR-0081](0081-web-audio-autoplay-gesture.md)); in both cases the
mixer runs silently rather than aborting a game that is otherwise fine. This is
the same contract `AudioSystem` already keeps.

### Play-in-editor adopts audio once, and copies audio sources

`PlaySession::enable_audio(Engine&)` is separate from `start()` and idempotent.
Separate, so a headless test that only wants the scene copy or the physics path
never opens a sound device; idempotent and called once by the host, so a
play/stop cycle does not reopen the device and the systems survive across it
(`stop()` does not clear the schedule). The adopted system is held until the
first `start()` moves it onto the schedule, after the motion systems, so the
listener reads a camera gameplay has already moved this frame.

`AudioSource` is added to the editor's component-copy factories, with the clip
shared by pointer exactly as a `Sprite2D` texture is (ADR-0004): the play scene
must not own a second decode of the same clip, and every field that controls
*how* the clip plays is copied because those are authored data.

## Consequences

**Easier:**

- A spatial scene sounds positional: a sound is loudest where the player is
  looking, and a camera on a moving rig carries its ear with it.
- The rule is in one place. The player and play-in-editor share
  `AudioListenerSystem` and adopt it the same way, so they cannot disagree about
  where the player hears from.
- The behaviour is testable with no hardware: a hand-built `AudioSystem(nullptr)`
  and a `Scene` are enough, and the player path is pinned end to end by a test
  that adopts the real engine's audio system into a schedule and reads the
  listener back.

**Harder / accepted costs:**

- A 2D camera has no ear direction (there is no up vector in a 2D scene); only
  its position is used. That is all the current mixer consults — attenuation is
  distance-only — so nothing is lost today, but a future panner that needs
  orientation will have to special-case the 2D camera.
- The camera lookup walks the scene for a camera each frame. It is the same walk
  the renderers already do, and it is bounded by the scene's camera count, so it
  is not worth caching; a scene with no camera is the common headless case and
  costs one pass.
- `PlaySession` now owns a device and a system, so it has an out-of-line
  constructor and destructor: the audio types are forward declared in the header
  and the `unique_ptr`s must be destroyed where those types are complete.

## Alternatives considered

- **Move the listener sync into `audio`.** Rejected: `audio` does not depend on
  `scene3d`, and giving it that dependency to move one `Vec3` inverts the module
  graph (ADR-0016, ADR-0034). `runtime` already depends on both camera types.
- **Have the renderers set the listener as they draw.** Rejected: rendering
  should not have a side effect on audio, and the 2D and 3D renderers would each
  need the coupling. A schedule system keeps the frame's ordering explicit and
  testable without a GPU.
- **Cache the camera at startup and re-read only its transform.** Rejected: a
  scene can activate a different camera, and the cached node would then be the
  wrong ear. The lookup is the renderers' own and is cheap.
- **Add the system unconditionally in `PlaySession::start()`.** Rejected: it
  would open a sound device in every headless test, and it would re-adopt across
  a play/stop cycle. A separate idempotent `enable_audio` the host calls once
  keeps the device out of tests that do not want it.
- **Snap a camera-less scene's listener to the origin.** Rejected: it makes a
  momentarily camera-less frame suddenly re-hear the whole scene from nowhere.
  Leaving the last position is the recoverable choice.

## Deferred

Still out of scope, tracked in the roadmap:

- Listener orientation for a directional panner. **Now shipped**:
  [ADR-0099](0099-stereo-panning-from-listener-orientation.md) adds a
  constant-power stereo pan driven by the listener's orientation, and a 3D
  camera supplies the facing this system already had in hand.
- Doppler shift.
- An editor inspector field for a camera's "audio listener" role, should a scene
  ever need more than one candidate camera.
