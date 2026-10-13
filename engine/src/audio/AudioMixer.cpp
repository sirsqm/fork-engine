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

f32 mix_voices(Voice* voices, usize voice_count, f32* output, u32 frames, u32 out_channels, u32 output_sample_rate) {
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
                output[base + c] += clip_sample(clip, read_frame, c) * voice.gain;
            }
            voice.position += ratio;
            ++voice.frames_played;
        }
    }

    f32 peak = 0.0f;
    for (usize i = 0; i < total_samples; ++i) {
        peak = std::max(peak, std::abs(output[i]));
    }
    return peak;
}

} // namespace fe::audio
