// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/AnimationStateMachineSystem.h"

#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/SpriteAnimator.h"

namespace fe {

void AnimationStateMachineSystem::update(const FrameContext& context, Scene& scene) {
    advanced_ = 0;

    scene.for_each_node([&](Node& node) {
        auto* machine = node.get_component<AnimationStateMachine>();
        if (machine == nullptr) {
            return;
        }

        machine->advance(context.delta_time);
        ++advanced_;

        if (!machine->drive_animator) {
            return;
        }
        auto* animator = node.get_component<SpriteAnimator>();
        if (animator == nullptr) {
            return;
        }

        // The machine's active state selects a clip: the absolute cell is the
        // state's first frame plus the frame within the clip. The animator's own
        // grid (frames_x/frames_y) is left untouched, so a state machine and a
        // plain animator share the same sheet layout; the machine only chooses
        // which cell is showing.
        const i32 active = machine->active_state();
        if (active < 0 || static_cast<usize>(active) >= machine->states.size()) {
            return;
        }
        const AnimationState& state = machine->states[static_cast<usize>(active)];
        const u32 total = animator->count();
        if (total == 0) {
            return;
        }
        animator->current_frame = (state.first_frame + machine->clip_frame()) % total;
    });
}

} // namespace fe
