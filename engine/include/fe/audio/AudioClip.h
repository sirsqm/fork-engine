// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioFormat.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"

#include <vector>

namespace fe::audio {

// Decoded PCM audio held in main memory.
//
// This is the CPU-side asset, the audio counterpart of `Image` for textures.
// Decoding is separated from playback on purpose: the same decoded clip can feed
// the mixer, a waveform display in the editor, or a future importer step, and it
// can be unit-tested with no sound card. Samples are 32-bit float, interleaved,
// in the range [-1, 1].
class AudioClip final : public Resource {
public:
    static constexpr const char* kTypeName = "AudioClip";

    AudioClip(std::string path, AudioFormat format, std::vector<f32> samples)
        : Resource(std::move(path)), format_(format), samples_(std::move(samples)) {}

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    [[nodiscard]] AudioFormat format() const noexcept { return format_; }
    [[nodiscard]] u32 sample_rate() const noexcept { return format_.sample_rate; }
    [[nodiscard]] u32 channels() const noexcept { return format_.channels; }

    // Interleaved float samples. Size is always frame_count() * channels().
    [[nodiscard]] const std::vector<f32>& samples() const noexcept { return samples_; }

    // Number of frames (one sample per channel per frame).
    [[nodiscard]] u64 frame_count() const noexcept {
        return format_.channels == 0 ? 0 : static_cast<u64>(samples_.size()) / format_.channels;
    }

    // Duration in seconds at the clip's own sample rate.
    [[nodiscard]] f64 duration_seconds() const noexcept {
        return format_.sample_rate == 0 ? 0.0 : static_cast<f64>(frame_count()) / format_.sample_rate;
    }

    [[nodiscard]] usize memory_usage() const noexcept override { return samples_.size() * sizeof(f32); }

private:
    AudioFormat format_;
    std::vector<f32> samples_;
};

} // namespace fe::audio
