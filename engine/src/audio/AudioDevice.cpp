// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioDevice.h"

#include <algorithm>
#include <cmath>

namespace fe::audio {

AudioDevice::~AudioDevice() = default;

Result<AudioFormat> NullAudioDevice::open(u32 sample_rate, u32 channels) {
    if (open_) {
        return Error("null audio device is already open");
    }
    if (sample_rate == 0 || channels == 0) {
        return Error("null audio device requires a non-zero sample rate and channel count");
    }
    format_ = AudioFormat{sample_rate, channels};
    open_ = true;
    playing_ = false;
    frames_submitted_ = 0;
    peak_sample_ = 0.0f;
    last_block_.clear();
    return format_;
}

Error NullAudioDevice::start() {
    if (!open_) {
        return Error("null audio device cannot start before open()");
    }
    playing_ = true;
    return Error::none();
}

void NullAudioDevice::stop() noexcept {
    playing_ = false;
}

u32 NullAudioDevice::submit(const f32* frames, u32 frame_count) {
    if (!open_ || !playing_ || frames == nullptr || frame_count == 0) {
        return 0;
    }

    const usize sample_count = static_cast<usize>(frame_count) * format_.channels;
    last_block_.assign(frames, frames + sample_count);

    for (usize i = 0; i < sample_count; ++i) {
        peak_sample_ = std::max(peak_sample_, std::abs(frames[i]));
    }
    frames_submitted_ += frame_count;
    return frame_count;
}

void NullAudioDevice::reset() noexcept {
    frames_submitted_ = 0;
    peak_sample_ = 0.0f;
    last_block_.clear();
}

} // namespace fe::audio
