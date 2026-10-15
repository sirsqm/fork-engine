// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/anim/AnimationStateMachine.h"
#include "fe/systems/System.h"

#include <string_view>

namespace fe {

// Advances every AnimationStateMachine in the scene once per frame and, when a
// machine asks to drive playback, writes the active state's clip onto the node's
// SpriteAnimator.
//
// This is the behaviour half of the machine, in the same split SpriteAnimator
// (state) and SpriteAnimationSystem (behaviour) already use: the component is
// authored and serialized, the system is the per-frame step. It owns its own
// traversal — it gathers the machine-bearing nodes into a flat list and walks
// that — exactly as the execution model (ADR-0002) prescribes.
//
// Ordering: add this system *after* SpriteAnimationSystem. The machine computes
// the animator's cell from its own clock and overwrites `current_frame`, so the
// last writer in the frame wins and the rendered UVs are the machine's. When a
// machine has `drive_animator` false it writes nothing, and the animator plays
// its own loop as before.
class AnimationStateMachineSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "AnimationStateMachineSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    // Machines advanced during the most recent update. Exposed for tests and the
    // editor's debug view; not a gameplay-facing value.
    [[nodiscard]] usize last_advanced_count() const noexcept { return advanced_; }

private:
    usize advanced_ = 0;
};

} // namespace fe
