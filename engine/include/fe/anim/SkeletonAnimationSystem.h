// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/anim/Animator.h"
#include "fe/systems/System.h"

#include <string_view>
#include <vector>

namespace fe::anim {

// Advances every `Animator` in the scene once per frame and publishes the
// skinning palette its skinned mesh renders with.
//
// The behaviour half of animation, matching `SpriteAnimationSystem` and
// `AnimationStateMachineSystem`: the component is authored and serialized, the
// system owns the traversal and the step. It gathers the animators into a flat
// list once and walks that, exactly as the execution model (ADR-0002) prescribes,
// rather than paying for a virtual dispatch per node per frame.
//
// What one frame does per animator:
//
//   1. Advance the clip clock by `delta_time * speed`, unless `playing` is false.
//   2. Wrap it for the loop mode.
//   3. Start from the skeleton's rest pose, sample the primary clip over it, and
//      (when a cross-fade is in flight) sample the outgoing clip into a second
//      pose and blend the two by `blend_weight`.
//   4. Compute the palette `world * inverse_bind` for the blended pose.
//   5. Apply root motion to the owning node's transform, when asked.
//   6. Publish the pose and palette back onto the animator.
//
// Why the system and not the renderer. The palette is a property of the animator,
// not of the draw: a game asks "where is the hand joint" for a hitbox, an editor
// draws the rig as a skeleton overlay, and a tool bakes a pose to a static mesh —
// none of which is a draw. Computing it in the frame step means all three read one
// answer, computed once, rather than each recomputing it and disagreeing about
// ordering.
//
// Ordering. Add this system *before* the renderer extracts, so the palette the
// renderer reads is this frame's. It is independent of the sprite systems: a
// scene with both is a scene with both, and neither reads the other's data.
//
// Root motion writes the node's transform. A game that also moves the character
// from a controller must leave `apply_root_motion` off, or the two will fight;
// the flag is per animator precisely so one character can do either.
class SkeletonAnimationSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "SkeletonAnimationSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    // Animators advanced during the most recent update. Exposed for tests and the
    // editor's debug view; not a gameplay-facing value.
    [[nodiscard]] usize last_advanced_count() const noexcept { return advanced_; }

    // Animators skipped because they had no skeleton or no clip bound. A non-zero
    // count is not an error — a scene mid-load looks like this — but it is the
    // number a test asserts on to prove a missing binding is skipped rather than
    // producing a garbage pose.
    [[nodiscard]] usize last_skipped_count() const noexcept { return skipped_; }

private:
    // Reused across frames so a steady-state scene does not allocate. Only the
    // outgoing pose of a cross-fade is a scratch: the primary pose and the
    // palette are written straight into the animator that owns them, so a frame
    // with no fade allocates nothing at all and a fade reuses one pose buffer.
    std::vector<Animator*> animators_;
    SkeletonPose scratch_blend_pose_;

    usize advanced_ = 0;
    usize skipped_ = 0;
};

} // namespace fe::anim
