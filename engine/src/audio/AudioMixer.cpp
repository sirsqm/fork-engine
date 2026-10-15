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
    if (voice.spatial) {
        const Vec3 offset = voice.world_position - params.listener_position;
        voice.attenuation = distance_attenuation(offset.length(), voice.min_distance, voice.max_distance);
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

    for (u32 f = 0; f < frames; ++f) {
        const u64 source_frame = static_cast<u64>(static_cast<f64>(f) * ratio);
        if (source_frame >= source_frames) {
            // The decoded block ran out before the output request did; the rest
            // of this voice's contribution is silence.
            break;
        }
        const usize base = static_cast<usize>(f) * out_channels;
        for (u32 c = 0; c < out_channels; ++c) {
            output[base + c] += block_sample(samples, source_channels, source_frame, c) * level;
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
                output[base + c] += clip_sample(clip, read_frame, c) * level;
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
