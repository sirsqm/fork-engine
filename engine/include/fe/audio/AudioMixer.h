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

    // The pan the mixer applied on the last call, in [-1, 1]: -1 is hard left,
    // 0 is centred, +1 is hard right. 0 when the voice is non-positional or the
    // listener has no orientation (a 2D scene), so the mix is unchanged. Exposed
    // for the same reason `attenuation` is: a test can assert the pan without
    // re-deriving the vector maths.
    f32 pan = 0.0f;

    // --- Directivity cone --------------------------------------------------
    // The axis the source projects along, in world space. A source that faces a
    // listener is louder than one that faces away. The zero vector (the default)
    // is "no cone": the source is omnidirectional, exactly as before this
    // refinement, so a scene that sets nothing sounds the same. A caller that has
    // a facing (a speaker, a megaphone) sets a real vector - the AudioSystem
    // derives it from the node's forward - and opts the source into directivity.
    Vec3 cone_direction{};

    // The cone's half-angles in degrees: full gain inside `cone_inner_degrees`,
    // falling linearly in angle to `cone_outer_gain` at `cone_outer_degrees` and
    // staying there beyond. An inner angle of 180 or more (the default 360) makes
    // the cone omnidirectional, so the refinement is opt-in. See
    // cone_attenuation() for the curve.
    f32 cone_inner_degrees = 360.0f;
    f32 cone_outer_degrees = 360.0f;

    // Gain applied beyond the outer cone, in [0, 1]. 1 is "the cone does
    // nothing" and 0 is "silent behind the source".
    f32 cone_outer_gain = 1.0f;

    // The source's linear velocity, in world units per second, for the Doppler
    // shift. The AudioSystem fills this from the node's motion; the mixer never
    // invents one. Zero (the default) means "not moving", so the Doppler factor
    // is 1 and the mix is unchanged.
    Vec3 velocity{};

    // The cone gain the mixer applied on the last call, in [0, 1]. 1 when the
    // voice is non-positional, has no cone direction, or is omnidirectional.
    // Exposed for diagnostics and tests, like `attenuation`.
    f32 cone_gain = 1.0f;

    // The Doppler factor the mixer applied on the last call. 1 when the voice is
    // non-positional, Doppler is disabled, or nothing is moving. It scales the
    // playback rate (a pitch shift), not the level, because that is what the
    // effect is. Exposed for diagnostics and tests.
    f32 doppler = 1.0f;

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

// Directivity attenuation for a cone source.
//
// A source that faces the listener is heard at full gain; one that faces away is
// heard at `outer_gain`. `angle_degrees` is the angle between the source's cone
// axis and the direction from the source to the listener, in degrees, in
// [0, 180].
//
// The curve is a piecewise-linear ramp: full gain for an angle at or inside
// `inner_degrees`, falling linearly to `outer_gain` at `outer_degrees`, and
// `outer_gain` beyond. A degenerate cone - `inner_degrees >= 180`, or
// `outer_degrees <= inner_degrees` - is omnidirectional and returns 1, so a
// source that never sets a cone is unaffected. `outer_gain` is clamped to [0, 1]
// so the function can only attenuate, never boost.
//
// The function is total and pure, so it is called directly by the tests.
[[nodiscard]] f32 cone_attenuation(f32 angle_degrees, f32 inner_degrees, f32 outer_degrees, f32 outer_gain) noexcept;

// The Doppler frequency factor for one voice.
//
// The factor is the ratio of the sound's apparent frequency to its emitted one,
// which is what the playback rate is multiplied by: a source approaching the
// listener has a factor above 1 (higher pitch), one receding below 1. The model
// is the classic one
//
//     factor = (c + v_listener_towards) / (c + v_source_towards)
//
// where `c` is `speed_of_sound`, and each velocity is the component along the
// line from the source to the listener, signed so that motion *towards* the
// other raises the numerator (listener) or lowers the denominator (source).
//
// Total and pure: a zero or negative speed of sound, a source at the ear, or a
// denominator that would cross zero each return 1 (no shift) rather than
// dividing by zero or inverting. The factor is clamped to a sane band
// ([0.25, 4]) so a runaway velocity cannot produce an absurd pitch.
[[nodiscard]] f32 doppler_factor(f32 speed_of_sound, const Vec3& source_position, const Vec3& source_velocity,
                                 const Vec3& listener_position, const Vec3& listener_velocity) noexcept;

// Stereo pan gain for one output channel, from a pan in [-1, 1].
//
// A positional voice is panned by where it sits relative to the listener's
// facing: a sound to the listener's right is louder in the right ear, one to the
// left louder in the left ear, and one straight ahead equal in both. `channel`
// selects which ear: 0 is left, 1 is right. Any other channel (a surround
// layout, or a mono output) is centred, so this cannot silence a channel the
// pan model has no opinion about.
//
// The law is the "constant power" one -3 dB pan law that every mixer uses rather
// than a linear fade: the two gains are cos and sin of a quarter turn, so
// `left^2 + right^2 == 1` at every pan. A linear law would dip ~3 dB in the
// middle, so a sound crossing the listener would audibly duck as it passed the
// centre; constant power keeps its loudness even across the sweep. At hard left
// the right gain is exactly 0 and vice versa, so a hard-panned sound is truly in
// one ear.
//
// Total and pure: a pan outside [-1, 1] is clamped, and the function never
// returns NaN.
[[nodiscard]] f32 pan_gain(f32 pan, u32 channel) noexcept;

// Everything mix_voices needs that is not on the voices themselves: the
// listener, the bus table, and the master gain. Grouped so the mixer signature
// stays readable and a caller can default-construct "no buses, origin listener".
struct MixParams {
    // Where the listener is, in the same world space as each voice's position.
    // Consulted only for positional voices.
    Vec3 listener_position{};

    // Which way the listener faces, as a unit vector, in the same space as
    // `listener_position`. Panning is the component of a voice's direction along
    // the listener's *right* axis, which is derived from this and `listener_up`.
    //
    // The default is the zero vector, which means "no facing, so no panning": a
    // voice is centred and its level is exactly what it was before panning
    // existed. That keeps the refinement purely additive - every 2D scene and
    // every existing test, none of which has a facing, mixes byte for byte as it
    // did. A caller that has an ear direction (the `AudioListenerSystem` reading
    // a 3D camera) sets a real vector and opts the scene into panning.
    //
    // A zero vector is the same degenerate facing `voice_pan` already centres
    // on, so there is one rule rather than a separate "was it set" flag.
    Vec3 listener_forward{};

    // The listener's up axis, used with `listener_forward` to build the right
    // axis. Kept separate from forward because a listener may roll (a camera on a
    // banking aircraft); deriving right from forward and world-up alone would
    // make the pan ignore the roll. Ignored while `listener_forward` is zero.
    Vec3 listener_up{0.0f, 1.0f, 0.0f};

    // The listener's linear velocity, in world units per second, for the Doppler
    // shift. The zero vector (the default) means "the ear is still", so a moving
    // source is the only thing that shifts pitch and a scene that sets nothing
    // keeps its mix. Consulted only for positional voices with a real cone or a
    // non-zero source velocity.
    Vec3 listener_velocity{};

    // The speed of sound in the scene's world units per second, for the Doppler
    // shift. The default is the speed of sound in air (343 m/s) for a scene whose
    // unit is a metre, which is the convention the physics gravity default uses.
    // A scene that authors its own scale sets this to match, and the shift scales
    // with it: a larger speed of sound means a smaller, subtler shift.
    f32 speed_of_sound = 343.0f;

    // Whether the Doppler shift is applied at all. False (the default) keeps
    // every existing scene mixing byte for byte: the factor is 1 and pitch is
    // unchanged, so this refinement is opt-in like panning and directivity.
    bool doppler_enabled = false;

    // Whether panning is active. False while `listener_forward` is zero, which
    // is the default and every 2D scene: the mixer then applies no per-ear gain,
    // so a centred voice keeps exactly its authored level rather than the -3 dB
    // a constant-power law puts at the centre. Only a listener with a real facing
    // (a 3D camera) opts in, and only then does the pan law apply - which is why
    // enabling it does not retroactively change a mix that never had an ear
    // direction.
    [[nodiscard]] bool has_orientation() const noexcept { return listener_forward.length_squared() > 0.0f; }

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
//
// This is the voice's *level*, not its per-ear gain: panning is applied per
// output channel on top of it (see pan_gain), because a stereo pan is a different
// gain for the left and right ears rather than one number for the voice. The
// level folds in the directivity cone as well as distance attenuation: a source
// facing away from the listener is quieter than one facing it.
[[nodiscard]] f32 voice_level(Voice& voice, const MixParams& params) noexcept;

// The pan the mixer computes for a voice, in [-1, 1], and records on
// `voice.pan`. 0 for a non-positional voice, and 0 when the listener has no
// facing (`params.has_orientation()` is false, or the facing is degenerate: a
// zero forward vector, or one parallel to `listener_up`), so the voice is
// centred rather than panned by a right axis that does not exist.
//
// The pan is the direction from the listener to the voice projected onto the
// listener's right axis: a voice to the right is positive, one behind is centred
// (its right component is zero), and one directly overhead is centred too. This
// is the standard first-order model and is deliberately not a full HRTF - it
// gives left/right separation, which is the audible half of spatialisation, and
// leaves front/back and elevation to a future head-related filter.
[[nodiscard]] f32 voice_pan(const Voice& voice, const MixParams& params) noexcept;

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
//     voice.gain * attenuation * cone_gain * params.master_gain * bus_gain
//
// where `attenuation` is distance_attenuation() for a positional voice and 1
// otherwise, `cone_gain` is cone_attenuation() for a positional voice with a
// cone axis and 1 otherwise, and `bus_gain` is the gain of the voice's bus (1
// when it is on no bus). A non-positional voice on no bus therefore keeps
// exactly its authored gain, which is the behaviour that existed before this
// refinement.
//
// A positional voice is additionally panned across the output channels by
// voice_pan() and pan_gain(): its level is multiplied by a per-ear gain so a
// sound to the listener's right is louder in the right ear. Panning applies only
// when the listener has a facing (`params.has_orientation()`), so a 2D scene - a
// position but no ear direction - mixes exactly as it did before, and only when
// the output is stereo (`out_channels >= 2`); a mono output is centred. The pan
// touches channels 0 and 1 only, so a third or later channel (a surround layout)
// is centred, because the pan model has no opinion about it. A non-positional
// voice is never panned.
//
// Returns the peak absolute sample written, so a caller can report or meter the
// output without rescanning it.
[[nodiscard]] f32 mix_voices(Voice* voices, usize voice_count, f32* output, u32 frames, u32 out_channels,
                             u32 output_sample_rate, const MixParams& params = {});

} // namespace fe::audio
