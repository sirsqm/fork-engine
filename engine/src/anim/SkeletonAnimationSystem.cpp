// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/SkeletonAnimationSystem.h"

#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"

namespace fe::anim {
namespace {

// Samples a clip into `pose` at `time`, wrapped for the clip's own duration and
// loop mode. A clip with no duration (no keys) is skipped, so the pose keeps the
// rest values it already holds rather than sampling at a NaN time.
void sample_clip(const AnimationClip& clip, f32 time, LoopMode mode, SkeletonPose& pose) {
    const f32 duration = clip.duration();
    if (duration <= 0.0f) {
        return;
    }
    clip.sample(AnimationClip::wrap_time(time, duration, mode), pose);
}

// The index of the first root joint (a joint whose parent is out of range), or
// kNoParent when the skeleton has none.
[[nodiscard]] i32 find_root(const Skeleton& skeleton) noexcept {
    for (usize i = 0; i < skeleton.joint_count(); ++i) {
        const i32 parent = skeleton.joint(i).parent;
        if (parent < 0 || static_cast<usize>(parent) >= skeleton.joint_count()) {
            return static_cast<i32>(i);
        }
    }
    return Skeleton::kNoParent;
}

} // namespace

void SkeletonAnimationSystem::update(const FrameContext& context, Scene& scene) {
    advanced_ = 0;
    skipped_ = 0;
    animators_.clear();

    // One traversal to gather the animators, exactly as the sprite systems do.
    // The scene's node count is what a frame pays for this, not the animator
    // count.
    scene.for_each_node([&](Node& node) {
        if (Animator* animator = node.get_component<Animator>(); animator != nullptr) {
            animators_.push_back(animator);
        }
    });

    for (Animator* animator : animators_) {
        const Skeleton* skeleton = animator->skeleton_resource();
        if (skeleton == nullptr || skeleton->empty()) {
            // No rig: nothing to pose. Counted rather than treated as an error,
            // because a scene between "component added" and "asset assigned"
            // looks exactly like this.
            ++skipped_;
            continue;
        }

        // --- 1. Advance the clocks -------------------------------------------
        if (animator->playing) {
            animator->time += context.delta_time * animator->speed;
        }
        // The cross-fade advances on its own clock, so a paused clip still
        // finishes its fade rather than freezing mid-blend forever.
        animator->advance_blend(context.delta_time);

        // --- 2. Sample the primary clip over the rest pose -------------------
        // Starting from the rest pose is what lets a clip that animates only one
        // channel (a head turn, a bounce) leave every joint it does not mention in
        // its authored position.
        SkeletonPose& pose = animator->mutable_pose();
        pose.reset_to_rest(*skeleton);

        const AnimationClip* primary = animator->clip_resource();
        if (primary != nullptr) {
            sample_clip(*primary, animator->time, animator->loop, pose);
        }

        // --- 3. Cross-fade ----------------------------------------------------
        const AnimationClip* outgoing = animator->blend_clip_resource();
        if (outgoing != nullptr && animator->blend_weight > 0.0f) {
            SkeletonPose& blend_pose = scratch_blend_pose_;
            blend_pose.reset_to_rest(*skeleton);
            // The outgoing clip is sampled at its own clock, which has kept
            // advancing beside the primary's, so a fade between clips of
            // different lengths mixes each at the position it actually reached.
            sample_clip(*outgoing, animator->blend_time(), animator->loop, blend_pose);
            blend_poses(pose, blend_pose, animator->blend_weight, pose);
        }

        // --- 4. Root motion ---------------------------------------------------
        const i32 root = find_root(*skeleton);
        if (root >= 0 && static_cast<usize>(root) < pose.joint_count()) {
            JointPose& root_pose = pose.at(static_cast<usize>(root));
            animator->set_root_translation(root_pose.translation);
            animator->set_root_rotation(root_pose.rotation);

            if (animator->apply_root_motion) {
                const Vec3 delta = animator->root_translation() - animator->previous_root_translation();
                // The delta is the root joint's own motion in skeleton space. The
                // node applies it in its own space, which for the common case (a
                // character root that is not itself rotated) is the same basis. A
                // rotated root would need the node's basis applied here, which is
                // the character controller's job rather than the animation step's.
                if (Node* owner = animator->node(); owner != nullptr && delta.length_squared() > 0.0f) {
                    owner->transform().position += delta;
                }
                // Remove the consumed translation from the pose, so the geometry
                // does not move by it a second time through the palette.
                root_pose.translation = Vec3{};
            }
            animator->commit_root_translation();
            animator->commit_root_rotation();
        }

        // --- 5. Palette -------------------------------------------------------
        pose.skinning_matrices(*skeleton, animator->mutable_palette());
        animator->mark_palette_valid();
        ++advanced_;
    }
}

} // namespace fe::anim
