// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/Animator.h"

#include <utility>

namespace fe::anim {

const Skeleton* Animator::skeleton_resource() const noexcept {
    return dynamic_cast<const Skeleton*>(skeleton.get());
}

const AnimationClip* Animator::clip_resource() const noexcept {
    return dynamic_cast<const AnimationClip*>(clip.get());
}

const AnimationClip* Animator::blend_clip_resource() const noexcept {
    return dynamic_cast<const AnimationClip*>(blend_clip.get());
}

f32 Animator::clip_duration() const noexcept {
    const AnimationClip* resolved = clip_resource();
    return resolved != nullptr ? resolved->duration() : 0.0f;
}

f32 Animator::blend_duration() const noexcept {
    const AnimationClip* resolved = blend_clip_resource();
    return resolved != nullptr ? resolved->duration() : 0.0f;
}

const char* Animator::clip_name() const noexcept {
    const AnimationClip* resolved = clip_resource();
    return resolved != nullptr ? resolved->name().c_str() : "";
}

void Animator::play(std::shared_ptr<Resource> new_clip, LoopMode new_loop) {
    clip = std::move(new_clip);
    loop = new_loop;
    time = 0.0f;
    // A new clip invalidates any fade in flight: the outgoing clip belonged to
    // the previous state, and mixing it into the new one is not what the caller
    // asked for.
    blend_clip.reset();
    blend_weight = 0.0f;
    blend_duration_ = 0.0f;
    blend_remaining_ = 0.0f;
}

void Animator::cross_fade_to(std::shared_ptr<Resource> next, f32 duration_seconds) {
    if (duration_seconds <= 0.0f) {
        // A zero-length fade is a cut: no outgoing clip at all, so the system
        // does not pay for a blend it would finish in the same frame.
        blend_clip.reset();
        blend_weight = 0.0f;
        blend_duration_ = 0.0f;
        blend_remaining_ = 0.0f;
        blend_time_ = 0.0f;
        clip = std::move(next);
        time = 0.0f;
        return;
    }

    // The clip that was playing becomes the outgoing one and keeps its own
    // clock, so the fade mixes the two at their respective positions rather than
    // freezing the outgoing clip on frame zero.
    blend_clip = clip;
    blend_time_ = time;
    blend_duration_ = duration_seconds;
    blend_remaining_ = duration_seconds;
    blend_weight = 1.0f;
    clip = std::move(next);
    time = 0.0f;
}

void Animator::advance_blend(f32 delta_time) {
    if (blend_remaining_ <= 0.0f || blend_duration_ <= 0.0f) {
        return;
    }
    // The outgoing clip keeps playing during the fade, so the two clips are
    // mixed at their own positions rather than the outgoing one being frozen.
    blend_time_ += delta_time;
    blend_remaining_ -= delta_time;
    if (blend_remaining_ <= 0.0f) {
        // Retire the fade: the primary clip is now the only one, and holding a
        // finished blend would keep sampling a clip whose contribution is zero.
        blend_remaining_ = 0.0f;
        blend_duration_ = 0.0f;
        blend_weight = 0.0f;
        blend_time_ = 0.0f;
        blend_clip.reset();
        return;
    }
    blend_weight = blend_remaining_ / blend_duration_;
}

} // namespace fe::anim
