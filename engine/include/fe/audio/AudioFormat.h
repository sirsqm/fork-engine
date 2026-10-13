// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

namespace fe::audio {

// The PCM layout a stream is expressed in.
//
// The engine standardises on 32-bit float samples. Clips may be decoded from
// integer PCM of any width, but they are converted to float on load, so every
// stage above the loader - the mixer, the device, the system - deals with one
// sample type and never branches on the source bit depth. Channels are
// interleaved.
struct AudioFormat {
    u32 sample_rate = 44100;
    u32 channels = 2;

    [[nodiscard]] constexpr bool operator==(const AudioFormat&) const noexcept = default;
};

} // namespace fe::audio
