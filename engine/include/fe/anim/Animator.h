// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/anim/AnimationClip.h"
#include "fe/anim/Skeleton.h"
#include "fe/core/Math.h"
#include "fe/resources/Resource.h"
#include "fe/scene/Component.h"

#include <memory>
#include <vector>

namespace fe::anim {

// Drives a skeleton from one or two animation clips and publishes the skinning
// palette a skinned mesh renders with.
//
// This is the *state* half of animation, exactly as `SpriteAnimator` is for a
// sprite sheet: the component holds what the author set (which clip, how fast,
// whether it loops) and the per-frame result the renderer reads, while
// `SkeletonAnimationSystem` is the *behaviour* half that advances it. The split
// is the engine's rule (ADR-0002): a component is authored and serialized, a
// system owns the traversal and the step.
//
// The palette is an output, not a property. It is recomputed every frame by the
// system and is deliberately not serialized and not reflected: it is derived
// data, and persisting it would mean a saved scene carried a pose that could
// disagree with the clip that produced it. `pose()` and `palette()` are exposed
// for the renderer, for a test, and for a debug view — not for the inspector.
//
// Cross-fade. Two clips, not one. A character switching from an idle to a walk
// needs the two blended across a few frames or it snaps; a single-clip animator
// forces every transition to be authored as a clip of its own. `blend_clip` is
// the outgoing clip and `blend_weight` runs 0 (fully the primary) to 1 (fully
// the blend), so a caller drives a fade by ramping one number and the two clips
// are sampled and blended in between. The blend advances on its own clock, so
// the outgoing clip keeps playing rather than freezing on its first frame —
// which is the difference between a cross-fade and a pop.
//
// Root motion. A locomotion clip normally animates the root joint's translation,
// which under ordinary skinning moves the geometry but not the node, so a
// character walks in place. With `apply_root_motion` set, the system takes the
// root joint's *delta* between two frames and adds it to the owning node's
// transform, and then removes it from the pose so the geometry does not move
// twice. Opt-in rather than default because a game that drives its own character
// controller wants the in-place clip and would otherwise be fighting the
// animation for the same transform.
class Animator final : public Component {
public:
    static constexpr const char* kTypeName = "Animator";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The rig the palette is computed for. Null until an asset is bound; the
    // system skips an animator with no skeleton rather than producing an empty
    // palette.
    std::shared_ptr<Resource> skeleton;

    // The clip that is playing. Null is legal (the animator holds its pose) and
    // is the state a scene is in before a clip is assigned.
    std::shared_ptr<Resource> clip;

    // The outgoing clip of a cross-fade, and how much of it to mix in. See the
    // class comment. With `blend_clip` null or `blend_weight` zero the primary
    // clip is used alone and the blend costs nothing.
    std::shared_ptr<Resource> blend_clip;
    f32 blend_weight = 0.0f;

    // Playback position in the clip, in seconds. Serialized so a scene can be
    // saved mid-clip, which is what an editor scrubbing the timeline needs.
    f32 time = 0.0f;

    // Playback rate. 1 is real time, 2 is double speed, a negative value plays
    // backwards. Zero holds the pose without stopping the blend clock.
    f32 speed = 1.0f;

    LoopMode loop = LoopMode::Loop;

    // Whether the clock advances at all. Pausing keeps the pose and the blend
    // position, which is what a menu pause wants; clearing the clip would drop
    // the pose.
    bool playing = true;

    // See the class comment. Off by default.
    bool apply_root_motion = false;

    // --- Derived state (not serialized, not reflected) ----------------------

    // The resolved local pose of every joint for the frame just computed.
    [[nodiscard]] const SkeletonPose& pose() const noexcept { return pose_; }

    // The palette `world * inverse_bind` per joint, which is what the skinned
    // vertex shader weights a vertex by. Empty until the system has run once.
    [[nodiscard]] const std::vector<Mat4>& palette() const noexcept { return palette_; }
    [[nodiscard]] bool has_palette() const noexcept { return palette_valid_; }

    // The clip's duration, or 0 when no clip is bound. Resolved through the
    // resource so a caller does not have to dynamic_cast to ask.
    [[nodiscard]] f32 clip_duration() const noexcept;
    [[nodiscard]] f32 blend_duration() const noexcept;

    // The clip's authored name, or "" when none is bound. For a debug view and
    // for a test, which would otherwise repeat the cast.
    [[nodiscard]] const char* clip_name() const noexcept;

    // The bound skeleton as a typed pointer, or null. A convenience over the
    // `Resource` base for the common case; a resource of the wrong type reads as
    // null rather than as a bad cast.
    [[nodiscard]] const Skeleton* skeleton_resource() const noexcept;
    [[nodiscard]] const AnimationClip* clip_resource() const noexcept;
    [[nodiscard]] const AnimationClip* blend_clip_resource() const noexcept;

    // Binds a clip and restarts playback at its first key, with `loop` mode.
    // The shape a gameplay call wants ("play Walk on a loop"); assigning `clip`
    // directly keeps the current time, which is the shape a scrub wants.
    void play(std::shared_ptr<Resource> new_clip, LoopMode new_loop = LoopMode::Loop);

    // Starts a cross-fade from the current primary clip to `next`, over
    // `duration_seconds`, by promoting the primary to the blend slot and making
    // `next` primary with the blend weight starting at 1. The system ramps the
    // weight down to 0 as the fade runs. A zero or negative duration snaps
    // instead of fading.
    void cross_fade_to(std::shared_ptr<Resource> next, f32 duration_seconds);

    // The remaining cross-fade time, in seconds. Zero when no fade is running.
    [[nodiscard]] f32 blend_remaining() const noexcept { return blend_remaining_; }

    // --- Used by SkeletonAnimationSystem ------------------------------------

    // The pose the system samples into, and the palette it fills, exposed
    // mutably so the system writes the frame's result in place.
    //
    // In place rather than "compute, then hand over": a `set_derived(SkeletonPose)`
    // taking the pose by value would copy or move a per-joint vector every frame
    // for every character, and the buffers would be reallocated each time. The
    // animator owning them means the capacity is allocated once and reused, which
    // is the same reasoning the renderer's per-frame instance buffer uses.
    //
    // The system is the only caller. A caller outside the frame step reads the
    // const accessors above.
    [[nodiscard]] SkeletonPose& mutable_pose() noexcept { return pose_; }
    [[nodiscard]] std::vector<Mat4>& mutable_palette() noexcept { return palette_; }
    void mark_palette_valid() noexcept { palette_valid_ = true; }

    // Advances the cross-fade clock by `delta_time`, retiring the blend when it
    // reaches the end. Called by the system once per frame. Advances the outgoing
    // clip's own clock beside it, so a fade between clips of different lengths
    // samples each at the position it actually reached.
    void advance_blend(f32 delta_time);

    // The clock the outgoing clip of a cross-fade is sampled at, in seconds. It
    // advances with the primary clip's clock but is kept separately so a fade
    // between two clips of different lengths samples each at its own position
    // rather than forcing the outgoing clip onto the incoming one's timeline.
    [[nodiscard]] f32 blend_time() const noexcept { return blend_time_; }
    void set_blend_time(f32 clock) noexcept { blend_time_ = clock; }

    // The root joint's local translation in the pose just computed, and the
    // value it held the previous frame, which is what a root-motion delta is
    // measured between. Both are zero when there is no skeleton or no root.
    //
    // The system calls `set_root_translation` with this frame's value, consumes
    // the delta between it and `previous_root_translation()`, and then calls
    // `commit_root_translation()` to make this frame's value the new previous.
    // Keeping "set" and "commit" separate is what lets the delta be read more
    // than once in a frame without the second read seeing zero.
    [[nodiscard]] Vec3 root_translation() const noexcept { return root_translation_; }
    [[nodiscard]] Vec3 previous_root_translation() const noexcept { return previous_root_translation_; }
    void set_root_translation(Vec3 translation) noexcept { root_translation_ = translation; }
    void commit_root_translation() noexcept { previous_root_translation_ = root_translation_; }

    // The root joint's animated rotation and the value it held last frame, for
    // the rotational half of root motion. Same set/commit shape as the
    // translation above.
    [[nodiscard]] Quat root_rotation() const noexcept { return root_rotation_; }
    [[nodiscard]] Quat previous_root_rotation() const noexcept { return previous_root_rotation_; }
    void set_root_rotation(Quat rotation) noexcept { root_rotation_ = rotation; }
    void commit_root_rotation() noexcept { previous_root_rotation_ = root_rotation_; }

private:
    SkeletonPose pose_;
    std::vector<Mat4> palette_;
    bool palette_valid_ = false;

    // The cross-fade clock. `blend_duration_` is how long the fade runs and
    // `blend_remaining_` counts down; the weight is derived from the two, so a
    // caller cannot leave them inconsistent. `blend_time_` is the outgoing clip's
    // own playback position, advanced beside the primary's.
    f32 blend_duration_ = 0.0f;
    f32 blend_remaining_ = 0.0f;
    f32 blend_time_ = 0.0f;

    Vec3 root_translation_{};
    Vec3 previous_root_translation_{};
    Quat root_rotation_ = Quat::identity();
    Quat previous_root_rotation_ = Quat::identity();
};

} // namespace fe::anim
