// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioMixer.h"

#include <algorithm>
#include <cmath>

namespace fe::audio {

f32 clip_sample(const AudioClip& clip, u64 frame, u32 out_channel) noexcept {
    const u32 channels = clip.channels();
    if (channels == 0) {
        return 0.0f;
    }
    // Mono is up-mixed to every output channel; a multi-channel clip maps each
    // output channel onto the clip's channels in order.
    const u32 source_channel = channels == 1 ? 0 : out_channel % channels;
    const u64 index = frame * channels + source_channel;
    const std::vector<f32>& samples = clip.samples();
    if (index >= samples.size()) {
        return 0.0f;
    }
    return samples[static_cast<usize>(index)];
}

f32 pan_gain(f32 pan, u32 channel) noexcept {
    // Only the two ears have a pan; a third or later channel (a surround layout)
    // and a mono output (channel 0 of one channel) are centred. The caller
    // guards the mono case by only panning when out_channels >= 2, but the
    // function is total on its own so a direct test cannot reach a surprising
    // value.
    if (channel > 1) {
        return 1.0f;
    }

    // Clamp so a caller that computed a pan slightly outside [-1, 1] (a rounding
    // error in a normalised direction) is centred/hard-panned rather than fed to
    // cos/sin outside the first quadrant.
    const f32 clamped = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);

    // Constant-power pan: the pan angle sweeps a quarter turn, and the two gains
    // are its cosine and sine. `pan = -1` is angle 0 (left = cos 0 = 1, right =
    // sin 0 = 0); `pan = +1` is angle pi/2 (left = 0, right = 1); `pan = 0` is
    // pi/4, where both are sqrt(2)/2 and the powers sum to 1.
    const f32 angle = (clamped + 1.0f) * 0.5f * (kPi * 0.5f);
    return channel == 0 ? std::cos(angle) : std::sin(angle);
}

f32 voice_pan(const Voice& voice, const MixParams& params) noexcept {
    // A non-positional voice has no place in space, so it is never panned. A
    // listener with no facing (the default, and every 2D scene) has no right
    // axis, so panning is off entirely rather than relative to an invented ear.
    if (!voice.spatial || !params.has_orientation()) {
        return 0.0f;
    }

    const Vec3 forward = params.listener_forward.normalized();
    const Vec3 up = params.listener_up.normalized();
    const Vec3 right = forward.cross(up);
    const f32 right_len = right.length();
    if (right_len <= 0.0f) {
        // forward is parallel to up (or up is zero): no right axis exists.
        return 0.0f;
    }

    // Direction from the ear to the sound, normalised so the pan is a pure
    // direction and does not grow with distance (distance is attenuation's job).
    const Vec3 offset = voice.world_position - params.listener_position;
    const f32 distance = offset.length();
    if (distance <= 0.0f) {
        // At the ear there is no direction to pan by.
        return 0.0f;
    }

    // The pan is the component of the direction along the right axis, which is
    // already in [-1, 1] because both vectors are unit length. Positive is to the
    // right.
    return (offset / distance).dot(right / right_len);
}

f32 distance_attenuation(f32 distance, f32 min_distance, f32 max_distance) noexcept {
    // Full gain at or inside min_distance, silence at or past max_distance. The
    // curve in between is the inverse-distance falloff rescaled so it reaches
    // exactly zero at the cutoff.
    if (distance <= min_distance) {
        return 1.0f;
    }
    if (distance >= max_distance) {
        return 0.0f;
    }
    // A degenerate span (min <= 0, or max <= min) has no interior: the two
    // branches above already returned, so reach here only with a usable span.
    const f32 span = (1.0f / min_distance) - (1.0f / max_distance);
    if (span <= 0.0f) {
        return 0.0f;
    }
    return ((1.0f / distance) - (1.0f / max_distance)) / span;
}

f32 block_sample(const f32* samples, u32 source_channels, u64 frame, u32 out_channel) noexcept {
    if (samples == nullptr || source_channels == 0) {
        return 0.0f;
    }
    const u32 source_channel = source_channels == 1 ? 0 : out_channel % source_channels;
    return samples[frame * source_channels + source_channel];
}

f32 voice_level(Voice& voice, const MixParams& params) noexcept {
    voice.attenuation = 1.0f;
    voice.pan = 0.0f;
    if (voice.spatial) {
        const Vec3 offset = voice.world_position - params.listener_position;
        voice.attenuation = distance_attenuation(offset.length(), voice.min_distance, voice.max_distance);
        voice.pan = voice_pan(voice, params);
    }
    return voice.gain * voice.attenuation * params.bus_gain(voice.bus);
}

f32 add_streamed_voice(Voice& voice, const f32* samples, u32 source_frames, u32 frames, f32* output, u32 out_channels,
                       f64 ratio, const MixParams& params) noexcept {
    if (samples == nullptr || output == nullptr || source_frames == 0 || frames == 0 || out_channels == 0) {
        return 0.0f;
    }
    // The level is resolved exactly as a clip voice's is, then the master gain is
    // applied here because mix_voices already folded it into the sum these
    // samples are added to.
    const f32 level = voice_level(voice, params) * params.master_gain;
    const u32 source_channels = voice.stream_channels == 0 ? out_channels : voice.stream_channels;
    if (ratio <= 0.0) {
        ratio = 1.0;
    }

    // Panning, exactly as mix_voices applies it: the same per-ear gains on the
    // same two channels, so a streamed spatial voice and a clip spatial voice
    // pan identically.
    const bool pan_active = voice.spatial && out_channels >= 2 && params.has_orientation();
    const f32 left_gain = pan_active ? pan_gain(voice.pan, 0) : 1.0f;
    const f32 right_gain = pan_active ? pan_gain(voice.pan, 1) : 1.0f;

    for (u32 f = 0; f < frames; ++f) {
        const u64 source_frame = static_cast<u64>(static_cast<f64>(f) * ratio);
        if (source_frame >= source_frames) {
            // The decoded block ran out before the output request did; the rest
            // of this voice's contribution is silence.
            break;
        }
        const usize base = static_cast<usize>(f) * out_channels;
        for (u32 c = 0; c < out_channels; ++c) {
            const f32 channel_gain = c == 0 ? left_gain : (c == 1 ? right_gain : 1.0f);
            output[base + c] += block_sample(samples, source_channels, source_frame, c) * level * channel_gain;
        }
    }

    f32 peak = 0.0f;
    for (usize i = 0; i < static_cast<usize>(frames) * out_channels; ++i) {
        peak = std::max(peak, std::abs(output[i]));
    }
    return peak;
}

f32 mix_voices(Voice* voices, usize voice_count, f32* output, u32 frames, u32 out_channels, u32 output_sample_rate,
               const MixParams& params) {
    if (output == nullptr || frames == 0 || out_channels == 0) {
        return 0.0f;
    }

    const usize total_samples = static_cast<usize>(frames) * out_channels;
    std::fill(output, output + total_samples, 0.0f);

    if (output_sample_rate == 0) {
        return 0.0f;
    }

    const f64 output_rate = static_cast<f64>(output_sample_rate);

    for (usize v = 0; v < voice_count; ++v) {
        Voice& voice = voices[v];
        if (!voice.active()) {
            continue;
        }

        // Resolve the voice's level once per call: its own gain, the distance
        // attenuation (1 for a non-positional voice), and the gain of the bus it
        // routes to. The master gain is applied to the sum at the end, which is
        // equivalent to folding it in here because scaling is linear. A level of
        // zero (a muted bus, a silent source, a voice past its cutoff) still
        // advances the read position through the normal path below, so unmuting
        // resumes at the right place rather than replaying from where it stopped.
        const f32 level = voice_level(voice, params);

        // Panning: a per-ear gain applied on top of the level, computed once per
        // voice because it depends only on the voice and the listener, not on the
        // frame. `pan_active` is false for a mono output or a listener with no
        // facing, in which case both gains are 1 and the loop below multiplies by
        // 1 - the pre-panning path, exactly.
        const bool pan_active = voice.spatial && out_channels >= 2 && params.has_orientation();
        const f32 left_gain = pan_active ? pan_gain(voice.pan, 0) : 1.0f;
        const f32 right_gain = pan_active ? pan_gain(voice.pan, 1) : 1.0f;

        const AudioClip& clip = *voice.clip;
        const u64 clip_frames = clip.frame_count();
        if (clip_frames == 0) {
            // An empty clip is silent; a one-shot is done immediately.
            if (!voice.loop) {
                voice.finished = true;
            }
            continue;
        }

        // Clip frames consumed per output frame. The first factor is the
        // resampling ratio between the clip's rate and the output rate; the
        // second is the authored pitch. Combining them is what keeps a clip at
        // the right speed when the device runs at a different rate than it was
        // recorded at.
        const f64 ratio = (static_cast<f64>(clip.sample_rate()) / output_rate) * static_cast<f64>(voice.pitch);

        for (u32 f = 0; f < frames; ++f) {
            if (voice.position >= static_cast<f64>(clip_frames)) {
                if (voice.loop) {
                    // Wrap; fmod rather than a subtraction so a large step (a
                    // long frame) still lands inside the clip.
                    voice.position = std::fmod(voice.position, static_cast<f64>(clip_frames));
                } else {
                    voice.finished = true;
                    break;
                }
            }

            const u64 read_frame = static_cast<u64>(voice.position);
            const usize base = static_cast<usize>(f) * out_channels;
            for (u32 c = 0; c < out_channels; ++c) {
                // Channels 0 and 1 carry the pan; a third or later channel is
                // centred (pan_gain returns 1 for it), and a mono output never
                // reaches here with pan_active true.
                const f32 channel_gain = c == 0 ? left_gain : (c == 1 ? right_gain : 1.0f);
                output[base + c] += clip_sample(clip, read_frame, c) * level * channel_gain;
            }
            voice.position += ratio;
            ++voice.frames_played;
        }
    }

    // The master bus: one scale over the summed output. Applying it after the
    // sum is what makes it "the buses sum into the master", and it is the same
    // value as folding it into every voice's level.
    f32 peak = 0.0f;
    for (usize i = 0; i < total_samples; ++i) {
        output[i] *= params.master_gain;
        peak = std::max(peak, std::abs(output[i]));
    }
    return peak;
}

} // namespace fe::audio
