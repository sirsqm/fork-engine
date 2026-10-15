// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioBus.h"
#include "fe/audio/AudioClip.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <vector>

namespace fe::audio {

// The "no bus" sentinel: a voice routed to no bus is scaled by the master gain
// alone, not by any group gain.
inline constexpr u32 kNoBus = 0xFFFFFFFFu;

// One currently playing instance of a clip.
//
// A voice is the runtime counterpart of an AudioSource: the source says "this
// node wants this clip at this gain and pitch", and the system creates a voice
// to track the read position as it plays. Separating them means the authored
// component stays serializable while the fast-changing playback state lives
// only for as long as the sound is audible.
struct Voice {
    const AudioClip* clip = nullptr;

    // Read position in the clip, in frames, as a fraction so a pitch that is not
    // a whole-number ratio still advances smoothly. Advanced by the mixer.
    f64 position = 0.0;

    f32 gain = 1.0f;
    f32 pitch = 1.0f;
    bool loop = false;
    bool playing = true;

    // Set by the mixer when a non-looping voice passes the end of its clip. A
    // finished voice contributes nothing on later calls.
    bool finished = false;

    // Total output frames this voice has produced. Exposed for diagnostics and
    // used by the system to decide when a paused source should be removed.
    u64 frames_played = 0;

    // --- Positional attenuation --------------------------------------------
    // When false the voice is non-positional and keeps full gain: `gain` is the
    // only level control, exactly as before this refinement. The remaining
    // fields in this block are ignored while `spatial` is false.
    bool spatial = false;

    // The source's world position, in the same space as MixParams::listener_position.
    // The AudioSystem fills this from the node's world transform; the mixer never
    // invents a transform of its own. Distinct from `position`, which is the
    // clip read position in frames.
    Vec3 world_position{};

    // Distance at which a spatial voice is at full gain, and the distance at
    // which it has fallen to silence. See distance_attenuation() for the curve.
    f32 min_distance = 1.0f;
    f32 max_distance = 20.0f;

    // The distance attenuation the mixer applied on the last call, for
    // diagnostics and metering. 1 when the voice is non-positional.
    f32 attenuation = 1.0f;

    // --- Streaming ---------------------------------------------------------
    // The channel count of the decoded samples for a streaming voice (a voice
    // whose frames come from an AudioStream rather than an AudioClip). Zero for
    // an ordinary clip voice, where the clip carries its own channel count.
    // Consulted only by add_streamed_voice, so a clip voice is unaffected.
    u32 stream_channels = 0;

    // --- Bus routing -------------------------------------------------------
    // The bus this voice routes to: an index into MixParams::buses, or kNoBus
    // when the voice is on no group bus (its contribution is scaled by the
    // master gain alone). The AudioSystem resolves a source's bus *name* to this
    // index, so the mixer does no string work.
    u32 bus = kNoBus;

    [[nodiscard]] bool active() const noexcept { return clip != nullptr && playing && !finished; }
};

// Distance attenuation for a positional voice.
//
// The model is **inverse distance with a cutoff at max_distance**: gain is 1
// within min_distance, falls monotonically to exactly 0 at max_distance, and is
// 0 beyond. Between the two it follows
//
//     gain(d) = (1/d - 1/max_distance) / (1/min_distance - 1/max_distance)
//
// which is the physical 1/d falloff rescaled so it reaches silence at the
// cutoff, rather than the constant floor a plain clamped 1/d curve leaves. It is
// the shape most engines call "inverse distance clamped", with the clamp turned
// into a true cutoff.
//
// Degenerate inputs disable the falloff instead of dividing by zero: when
// max_distance <= min_distance, or min_distance <= 0, there is no interior
// region and the curve becomes a step at min_distance - full gain inside it,
// silence at or beyond it. The function is total - it never returns NaN - and
// pure, so it is called directly by the tests.
[[nodiscard]] f32 distance_attenuation(f32 distance, f32 min_distance, f32 max_distance) noexcept;

// Everything mix_voices needs that is not on the voices themselves: the
// listener, the bus table, and the master gain. Grouped so the mixer signature
// stays readable and a caller can default-construct "no buses, origin listener".
struct MixParams {
    // Where the listener is, in the same world space as each voice's position.
    // Consulted only for positional voices.
    Vec3 listener_position{};

    // The group-bus table. `buses[voice.bus].gain` scales the voice when its bus
    // index is in range. The master bus is *not* in this table: its gain travels
    // in `master_gain`, so there is a single source of truth for the master. An
    // empty table (the common case) means no groups, master only.
    const AudioBus* buses = nullptr;
    u32 bus_count = 0;

    // The gain of the implicit master bus. Every voice's output is multiplied by
    // it, on a group bus or not, so muting the master silences the whole mix in
    // one number. Because scaling is linear this is identical to summing the
    // buses and scaling the sum.
    f32 master_gain = 1.0f;

    [[nodiscard]] f32 bus_gain(u32 index) const noexcept {
        return index < bus_count && buses != nullptr ? buses[index].gain : 1.0f;
    }
};

// The output-channel sample for `frame` of `clip`, up-mixing mono to every
// channel. Channels beyond a multi-channel clip wrap, which is a well-defined
// answer for a layout mismatch rather than an out-of-range read.
[[nodiscard]] f32 clip_sample(const AudioClip& clip, u64 frame, u32 out_channel) noexcept;

// The output-channel sample for `frame` of an interleaved block of
// `source_channels`. The same rule clip_sample applies to a clip: mono is
// up-mixed to every output channel, and a multi-channel block maps each output
// channel onto the block's channels in order. Used by a streaming voice, whose
// samples come from an AudioStream rather than an AudioClip.
[[nodiscard]] f32 block_sample(const f32* samples, u32 source_channels, u64 frame, u32 out_channel) noexcept;

// The gain a voice's samples are scaled by before the master bus: its own gain,
// its distance attenuation (1 for a non-positional voice), and the gain of the
// bus it routes to. Factored out so a clip voice and a streaming voice are
// scaled by exactly the same rule, and a change to the rule cannot drift between
// them. As a side effect it records the attenuation it applied on `voice`, which
// is the value the diagnostics expose.
[[nodiscard]] f32 voice_level(Voice& voice, const MixParams& params) noexcept;

// Adds one streaming voice's already-decoded interleaved samples to `output`,
// which is *not* cleared and must already hold the clip voices' contribution:
// mix_voices applies the master gain over the whole sum, so a streaming voice is
// added after it and carries the master gain itself. `source_frames` frames were
// decoded from the stream at its own rate; `frames` output frames are added,
// reading source frame `(u64)(f * ratio)` for output frame `f`, where
// `ratio = stream_rate / output_rate` - the same nearest-sample resampling
// mix_voices applies to a clip. Samples are scaled by voice_level() and the
// master gain. The stream cursor is owned by the caller, so nothing is advanced
// here. Returns the peak absolute sample in `output` after the addition, for the
// caller's metering.
[[nodiscard]] f32 add_streamed_voice(Voice& voice, const f32* samples, u32 source_frames, u32 frames, f32* output,
                                     u32 out_channels, f64 ratio, const MixParams& params = {}) noexcept;

// Mixes `voice_count` voices into `output` and advances their read positions.
//
// This is the whole of the mixing maths, kept as a pure function: it touches no
// device, no scene, and no resource manager, so it can be called directly by a
// test with hand-built voices and asserted sample by sample. `AudioSystem` is a
// thin driver around it: it turns a delta time into a frame count, calls this,
// and submits the result to a device.
//
// `output` holds `frames * out_channels` interleaved samples and is *cleared*
// first, then each voice's contribution is added. Every voice reads its clip at
// a rate of
//
//     ratio = (clip.sample_rate / output_sample_rate) * pitch
//
// clip frames per output frame, which is what makes pitch a resampling ratio and
// not just a speed change. A looping voice wraps at the end of its clip; a
// one-shot voice is marked finished once past the end and contributes nothing
// afterwards.
//
// Each voice's contribution is scaled by
//
//     voice.gain * attenuation * params.master_gain * bus_gain
//
// where `attenuation` is distance_attenuation() for a positional voice and 1
// otherwise, and `bus_gain` is the gain of the voice's bus (1 when it is on no
// bus). A non-positional voice on no bus therefore keeps exactly its authored
// gain, which is the behaviour that existed before this refinement.
//
// Returns the peak absolute sample written, so a caller can report or meter the
// output without rescanning it.
[[nodiscard]] f32 mix_voices(Voice* voices, usize voice_count, f32* output, u32 frames, u32 out_channels,
                             u32 output_sample_rate, const MixParams& params = {});

} // namespace fe::audio
