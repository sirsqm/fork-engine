// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioBackend.h"

#include "MiniAudioDevice.h"

namespace fe::audio {

Result<std::unique_ptr<AudioDevice>> create_audio_device() {
    // The only backend the engine ships today. Returning it through the factory
    // rather than naming the type at the call site is what keeps miniaudio out
    // of every header above this translation unit.
    return std::unique_ptr<AudioDevice>(new MiniAudioDevice());
}

} // namespace fe::audio
