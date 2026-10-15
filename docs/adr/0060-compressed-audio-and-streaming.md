# ADR-0060: Compressed audio codecs and streaming playback

- Status: Accepted
- Date: 2026-10-15
- Deciders: sqm

## Context

M6's open rows include "Streaming playback" and "Compressed formats". The audio
module today decodes a WAV once, whole, into an `AudioClip` and keeps every
decoded sample resident (`WavLoader` -> `AudioClip`). That is the right model for
a short sound - decode once, play it many times - and the wrong one for the two
sounds a real game is built from.

The first is a compressed sound. `WavLoader` decodes only linear PCM (8/16/24/32
bit) and IEEE float; a WAV carrying G.711 mu-law or A-law, or Microsoft IMA
ADPCM - the encodings a game's older or tool-generated assets actually use - is
rejected as unsupported. A game that cannot read the asset cannot ship it.

The second is a long sound. Music and ambience run for minutes; a ten-minute
stereo stream decoded to f32 is hundreds of megabytes held resident for the whole
time it plays, and a sound large enough should never be fully resident at all.
The whole-file model has no answer for that: by the time playback starts, the
cost is already paid.

Two properties constrain the fix. The audio module is deliberately headless and
deterministic - every test runs against `NullAudioDevice` with no sound card - so
a streaming API must be pull-based and synchronous, not a callback on another
thread. And the module already has one decode path that is trusted and tested; a
second path for streaming must not be allowed to drift from it, or a sound will
decode differently depending on whether it happened to be streamed.

## Decision

### 1. Compressed codecs live in one shared decoder

`WavLoader` decodes mu-law (tag 6), A-law (tag 7), and Microsoft IMA ADPCM (tag
0x11) in addition to the linear PCM and float it already handled. The codec -
the tag parsing, the G.711 reference decoders, the IMA step table and nibble
expansion, and the range decode - lives in `engine/src/audio/WavCodec.h`, an
internal header. `WavLoader.cpp` and the streaming path both include it, so a
loaded clip and a streamed sound decode through exactly the same code. The tests
assert that equality sample for sample; that is the property that keeps the two
from drifting.

mu-law and A-law are decoded by the ITU-T reference transform, not by a
round-trip against an encoder: the tests assert a known code (full-scale, and
zero) decodes to its known value. IMA ADPCM is decoded per block from the block
header, and the tests assert a hand-computed nibble sequence, that a block with
zero nibbles holds its predictor steady, and that two blocks with different
headers decode independently.

### 2. A streaming seam beside the whole-file loader

`AudioStream` (in `fe/audio/AudioStream.h`) is a pull-based, synchronous
interface: `read(n, out)` decodes up to `n` frames into a caller buffer and
advances a cursor, `seek(frame)` moves the cursor, and `format()`,
`frame_count()`, `position()`, and `at_end()` describe it. It is the audio
counterpart of `net::Transport`'s poll model - no callback runs on another
thread - so it is deterministic and testable with no sound card.

`WavStream` implements it over a WAV file. The file's bytes are read once (a
header has to be walked, and a compressed block codec needs its block intact),
but the decoded samples are not materialised: `read()` decodes only the requested
range, so the resident cost is the file bytes plus one block, not the decoded
PCM. A `read()` is served in bounded slices (`block_frames`, default 4096), so a
large request never decodes more than the block size at a time; a test asserts
that a stream never returns more frames than it was asked for. Seeking is exact
for every encoding: a fixed-width format indexes directly, and IMA ADPCM decodes
from the start of the block that contains the frame - which only works because a
block carries its own state.

### 3. A streaming voice in the mixer, not a second mixer

`AudioSource` gains a `stream` flag. When set, `AudioSystem` opens a `WavStream`
over the source's clip path and mixes it through `add_streamed_voice` on top of
the ordinary clip sum, applying the same level rule (`voice_level`: gain,
distance attenuation, bus gain, then the master gain) the clip path uses. Gain,
pitch, spatialisation and bus routing therefore behave identically for a streamed
sound and a loaded one; only where the samples come from differs. A stream that
cannot be opened (the flag set but the file missing) degrades to the source's
already-loaded clip rather than failing or going silent.

`voice_level` and the resampling rule are factored out of `mix_voices` and shared
with `add_streamed_voice`, so a change to either cannot apply to one path and not
the other. The `stream` flag defaults false, so every existing scene is
unchanged.

## Consequences

A game can now ship mu-law, A-law, and IMA ADPCM assets, and can play a long
sound without holding its decoded PCM resident. The two decode paths are one
codec, so a streamed sound and a loaded one are bit-identical for the frames a
stream returns, and that equality is a test rather than an intention.

The streaming path reads a file, so a source that streams is a source whose clip
has a path; a clip built in memory with no file streams as a loaded clip. This is
the seam that a pack-backed `ResourceSource` (ADR-0018, ADR-0030) will plug into
when assets ship in a pack: the stream would read its bytes from the source
instead of the filesystem, and nothing above it changes.

Streaming is pull-based and synchronous, so a source's decode runs on the frame
thread. That is the right trade for the module's determinism and the wrong one
for a decoder heavy enough to miss the frame budget; a future background-decode
seam would sit behind `AudioStream` without changing its interface.

## Alternatives considered

- **Decode compressed formats in the resource loader only, stream nothing.** The
  codec is needed either way, but this leaves the resident-memory problem - the
  reason M6 lists streaming - unaddressed, and a compressed stream is where
  streaming matters most (the compressed bytes are smaller, so the ratio of
  decoded to resident is larger).
- **A second, streaming-only decoder.** Rejected: two decoders for one format
  drift, and a sound would decode differently depending on how it was played.
  Sharing the codec makes the two paths equal by construction.
- **A background decode thread with a ring buffer.** Rejected for now: it breaks
  the module's deterministic, no-sound-card testability for a latency win that is
  not yet measured. The `AudioStream` interface leaves room for it later.
- **Wrap a third-party decoder (miniaudio's Vorbis/FLAC).** Deferred, not
  rejected: the vendored miniaudio carries decoders, but the WAV codecs here are
  small, reference-defined, and dependency-free, and they were the formats the
  module was rejecting. A codec that needs a real library (Vorbis, Opus) is a
  separate change with its own ADR.
