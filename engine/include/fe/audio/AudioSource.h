// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>
#include <string>

namespace fe::audio {

// A sound attached to a node, played by the AudioSystem.
//
// Like Sprite2D holds a texture, this holds a clip. Holding the shared_ptr is
// what keeps the clip loaded: the ResourceManager caches weakly, so an
// AudioSource that owns the last reference is also the reason the clip is not
// released.
//
// The component carries only *state* (what to play and how); the AudioSystem
// carries the *behaviour* that advances and mixes it, exactly as SpriteAnimator
// and SpriteAnimationSystem are split. That is what makes the component
// serializable and inspectable while the update loop stays a testable system.
class AudioSource final : public Component {
public:
    static constexpr const char* kTypeName = "AudioSource";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The decoded clip to play. Null is allowed (the source is silent) so a
    // scene can be authored before its assets exist.
    std::shared_ptr<Resource> clip;

    // Linear gain. 0 is silent and 1 is the clip at its recorded level. Not
    // clamped here; the mixer applies it as-is so a scene can author an
    // intentional boost, and the output is not clipped by the engine.
    f32 gain = 1.0f;

    // Playback rate multiplier. 1 is the clip's own pitch, 2 is an octave up
    // (and twice as fast), 0.5 is an octave down. Implemented by advancing the
    // read position at `pitch` times the sample-rate ratio.
    f32 pitch = 1.0f;

    // When true the clip restarts from the beginning after it ends. When false
    // it is a one-shot: it stops and is marked finished when it reaches the end.
    bool loop = false;

    // When true the voice starts as soon as the source is seen by the system
    // (a node added after play started begins on the next update). When false
    // the voice is created stopped and starts only on an explicit play().
    bool play_on_start = true;

    // When true, distance attenuation is applied: the mixer scales the voice by
    // the falloff between `min_distance` and `max_distance` (see
    // distance_attenuation in AudioMixer.h). When false the source is
    // non-positional and keeps full gain, so a scene authored before this
    // refinement sounds exactly as it did. The position is always taken from the
    // node's world transform, never from a transform of its own; spatialisation
    // reuses the existing hierarchy rather than adding a second source of truth
    // for position.
    bool spatial = false;

    // Distance at which a spatial source is at full gain, and the distance at
    // which it has fallen to silence. Ignored while `spatial` is false.
    f32 min_distance = 1.0f;
    f32 max_distance = 20.0f;

    // The mixing group this source routes to, matched by name against the
    // AudioSystem's bus table (see AudioBus.h). Empty means the master bus: the
    // source is scaled by the master gain alone. The bus's gain scales every
    // source that names it, so a volume change on a bus affects them all at once.
    std::string bus_name{};

    [[nodiscard]] bool has_clip() const noexcept { return clip != nullptr; }

    // Restarts the source from the top of its clip on the next update. This is
    // the "play a cue now" entry point a gameplay system calls: a one-shot that
    // already finished is restarted, and a source authored with play_on_start
    // false begins playing. The request is transient - the AudioSystem consumes
    // it and clears the flag - so it survives exactly the frame it was set on.
    // Calling it does not touch `clip`, so a caller need only hold the source.
    void play() noexcept { play_requested = true; }

    // Set by play(), cleared by the AudioSystem when it is honoured. Exposed so
    // the behavior is observable rather than hidden in a private flag.
    bool play_requested = false;
};

} // namespace fe::audio
