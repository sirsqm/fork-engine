// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/systems/System.h"

#include <string_view>

namespace fe {

namespace audio {
class AudioSystem;
} // namespace audio

// Keeps the audio listener at the active camera.
//
// The listener is where a positional sound is heard *from*: the mixer measures
// each spatial source's distance to it and attenuates accordingly
// (docs/adr/0034-audio-attenuation-buses.md). Until now nothing in the engine
// ever moved it -- it sat at the origin, so every spatial source was attenuated
// against the world origin rather than against the player's ear, and a scene
// that placed sounds around a moving camera heard them all as if the camera
// never moved.
//
// The camera is the right ear by construction: it is the node the player is
// looking through, so a sound is loudest when it is where the player is looking.
// The camera's *world position* is what matters, which is why this reads the
// owning node's world matrix rather than a local transform -- a camera parented
// to a moving rig (a vehicle, a character) then drags the listener with it, with
// no second source of truth for position.
//
// This lives in `runtime`, not `audio`, because choosing the active camera means
// knowing both camera types: `Camera3D` (scene3d) and `Camera2D` (scene). The
// `audio` module depends on neither (docs/ARCHITECTURE.md), and giving it that
// dependency to move one Vec3 would invert the module graph for a convenience.
// `runtime` already depends on everything, so the knowledge lands where the
// frame loop that owns the pairing lives.
//
// A 2D camera has no ear direction (there is no up vector in a 2D scene) but it
// does have a position, and position is what attenuation consults: attenuation
// is distance-only. A 3D camera additionally sets the listener's orientation,
// which drives stereo panning: a source to the player's right is heard in the
// right ear and one to the left in the left ear. Panning needs a facing a 2D
// scene does not have, so it stays off there rather than being panned against an
// invented axis.
class AudioListenerSystem final : public System {
public:
    // `audio` may be null, in which case the system is inert. That is the same
    // "no device, no output, but the scene still runs" contract AudioSystem
    // keeps, and it means a caller can add this system unconditionally without
    // first proving an audio system exists.
    explicit AudioListenerSystem(audio::AudioSystem* audio = nullptr) noexcept : audio_(audio) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "AudioListenerSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    void set_audio_system(audio::AudioSystem* audio) noexcept { audio_ = audio; }
    [[nodiscard]] audio::AudioSystem* audio_system() const noexcept { return audio_; }

private:
    audio::AudioSystem* audio_ = nullptr;
};

} // namespace fe
