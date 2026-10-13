# ADR-0012: Audio as a module with a device seam and a pure mixer

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0004](0004-resource-ownership-weak-cache.md),
  [ADR-0008](0008-systems-and-frame-schedule.md),
  [ADR-0009](0009-physics2d-system.md)

## Context

The engine can render and simulate but is silent. Audio is the next independent
capability, and like physics it is a good test of the composition model: it is a
per-frame system that consumes scene data, produces output for a device, and
must be testable with no hardware.

Three forces shape the decision:

- **CI has no sound card.** Whatever the design is, the parts that matter must be
  provable without an audio device, or the module is untested on every build.
- **Audio output is inherently real-time and lives on another thread.** The
  game's frame rate and the audio callback's period are independent. The design
  has to keep the game clock authoritative while never blocking the audio thread.
- **A third-party backend is the pragmatic choice.** Writing ALSA/CoreAudio/WASAPI
  backends by hand is weeks of platform work with no architectural payoff.

## Decision

Add an **`audio` module** that sits beside `physics2d` in the graph, not inside a
renderer, and split it along three seams: the device, the mixer, and the system.

### The device is an interface; the null backend is the contract under test

`AudioDevice` is a small push-oriented interface: `open`, `start`, `stop`,
`submit`, and `format`. The consumer above the seam pushes interleaved float
frames; each backend translates.

Two implementations ship:

- **`MiniAudioDevice`** is the real backend. It opens the platform's output
  through [miniaudio](https://github.com/mackron/miniaudio) and drains a
  lock-free ring buffer on the audio thread. `AudioSystem::submit` fills the
  ring; the audio callback empties it, zero-filling on underrun. No locks are
  taken on the audio thread and nothing allocates there after open.
- **`NullAudioDevice`** is a deterministic sink with no hardware. It records
  submitted frames, counts them, and tracks the peak sample. It is a **test
  double**, not a shipping backend, and it is what the unit tests and CI run
  against. It is documented as such in its header.

Push (rather than a pull callback the game fills) is deliberate: it makes the
game clock the authority on how much audio exists, keeps the real-time callback
tiny, and makes the null device a trivial recorder. Back-pressure is expressed
as a partial accept from `submit`, which is the correct behaviour for audio - a
brief underrun is better than a stall or a growing delay.

### The mixer is a pure function

`mix_voices` takes voices, an output buffer, a frame count, and the output
format, and does all of the mixing maths. It touches no device, no scene, and no
resource manager. Each voice reads its clip at

```
ratio = (clip.sample_rate / output_sample_rate) * pitch
```

clip frames per output frame, which is what makes pitch a resampling ratio and
not merely a speed change. A looping voice wraps at the end; a one-shot voice is
marked `finished` once past the end and contributes nothing afterwards. Mono is
up-mixed to every output channel.

Because it is pure, the tests call it directly with hand-built voices and assert
exact sample values - the expected numbers are written by hand, not re-derived
with the code under test.

### The system is thin plumbing

`AudioSystem` is an ordinary `System`, like `Physics2DSystem`. Each update it
reconciles the scene's `AudioSource`s with its playing voices (gather and carry
state, the `SpriteAnimationSystem` shape), converts `delta_time` into a frame
count at the device's sample rate, calls `mix_voices`, and submits the block.
It holds no mixing maths of its own. A test drives it with a `NullAudioDevice`.

### The clip is a resource; the loader is split from the parser

`AudioClip` is a `Resource` holding decoded interleaved float PCM, the audio
counterpart of `Image` for textures. `WavLoader` decodes 16- and 24-bit PCM (and
32-bit float) under the `wav` scheme. The parser is exposed separately from
`load()`, so a test generates a RIFF buffer in memory and checks the decode
without writing a file.

`AudioSource` stores the clip's **manager key** (`wav:path.wav`), resolved
through the `ResourceManager` on load, exactly as `Sprite2D` stores a texture
key. The source also carries `gain`, `pitch`, `loop`, `play_on_start`, and a
`spatial` toggle with min/max distance. Position, when spatialisation lands,
comes from the node's **world transform**; no separate transform is introduced.

### The backend library

miniaudio is a single-header, dependency-free C library released under
**public domain (Unlicense) or MIT-0, at the user's choice**. Both are
permissive and compatible with this project's MIT license: they impose no
copyleft, no attribution requirement, and no conditions that conflict with
MIT. The license is recorded here as the dependency review the roadmap
requires. It is pulled in through `FetchContent` and linked as the `miniaudio`
target, whose C implementation is compiled once; the engine includes only its
declarations, so the header never leaks above `src/audio/` and the third-party
code keeps its own warning settings.

## Consequences

**Easier:**

- The mix path is unit-tested with no hardware, so CI proves the module. This is
  the single most important property of the design.
- Pitch, gain, looping, one-shot termination, resampling, and mono up-mixing are
  each a handful of numbers in a test, because the mixer is pure.
- A second backend (or a different library) is a new `AudioDevice`, not a
  rewrite. The engine names no backend type: it goes through
  `create_audio_device()`.
- Audio is opt-in. `Engine::initialize` registers the loader and creates the
  system with no device; a caller opens a device and attaches it. A machine with
  no sound card runs everything else unchanged.

**Harder / accepted costs:**

- The ring buffer is single-producer/single-consumer and drops frames when full.
  That is correct for audio but means a stalled game thread loses samples rather
  than applying back-pressure to the frame loop.
- Voices are copied into a contiguous array each frame. With a handful of voices
  this is not worth avoiding; a thousand voices would want a different layout.
- The real backend is verified by hand in the sandbox, not asserted in CI. The
  null backend is the contract CI checks; the real device is proven to open,
  mix, and submit in a manual run (see the sandbox's `audio: mixed ... peak ...`
  line).

## Alternatives considered

- **No device abstraction; call the library directly from the system.**
  Rejected: it would make the mix path untestable without hardware and put a
  third-party header above the module boundary.
- **A pull/callback mixer** (the game provides a fill callback the backend
  calls). Rejected: it hands the audio thread control of the game clock and
  makes the null backend awkward. Push keeps one authority for time.
- **A lock-protected buffer instead of a lock-free ring.** Rejected: taking a
  mutex on the audio thread is exactly the thing that causes dropouts. A
  single-producer/single-consumer ring is the standard, bounded-cost answer.
- **Vendoring miniaudio as a committed header** instead of `FetchContent`.
  Rejected for consistency: the other third-party libraries (glfw, json,
  doctest) come through `FetchContent`, and a pinned tag keeps the build
  reproducible. A vendored copy would also add a 4 MB file to the repository.
- **Writing our own platform backends.** Rejected as weeks of work with no
  architectural payoff; the seam already isolates the choice.

## Deferred

Explicitly out of scope for this cut, and tracked in the roadmap:

- 3D positional attenuation (the `spatial` flag and distances are stored and
  serialized, but the mixer does not attenuate yet).
- Streaming / large-file playback (clips are fully decoded into memory).
- Compressed codecs (only PCM and float WAV are decoded).
- Audio buses and mixing groups.
