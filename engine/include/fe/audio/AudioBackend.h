// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioDevice.h"
#include "fe/core/Error.h"

#include <memory>

namespace fe::audio {

// Creates the platform's real output device, backed by miniaudio.
//
// This is the one entry point the engine uses to obtain a device, so no caller
// names a backend type directly and the miniaudio header never leaks past
// src/audio/. Returns an Error when no audio backend could be initialised (no
// sound card, no ALSA/PulseAudio/CoreAudio/WASAPI, a sandbox with audio
// disabled); the caller decides whether that is fatal. Tests never call this -
// they use NullAudioDevice, which needs no hardware.
[[nodiscard]] Result<std::unique_ptr<AudioDevice>> create_audio_device();

} // namespace fe::audio
