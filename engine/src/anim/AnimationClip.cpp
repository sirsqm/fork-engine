// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/AnimationClip.h"

#include <algorithm>
#include <cmath>

namespace fe::anim {
namespace {

// The index of the last key whose time is <= `time`, or 0 when `time` is at or
// before the first key. Binary search, because a dense clip has hundreds of keys
// and this runs per joint per frame.
//
// Returns the index of the *left* key of the segment to interpolate, so a
// caller reads `keys[i]` and `keys[i + 1]`. The caller must have checked the
// list is non-empty.
[[nodiscard]] usize find_segment_start(const auto& keys, f32 time) noexcept {
    if (time <= keys.front().time) {
        return 0;
    }
    if (time >= keys.back().time) {
        return keys.size() - 1;
    }
    // upper_bound gives the first key strictly greater than `time`; the segment
    // starts one before it.
    const auto it =
        std::upper_bound(keys.begin(), keys.end(), time, [](f32 value, const auto& key) { return value < key.time; });
    const usize index = static_cast<usize>(std::distance(keys.begin(), it));
    return index == 0 ? 0 : index - 1;
}

// The normalised position of `time` inside the segment [keys[i], keys[i + 1]],
// in [0, 1]. Zero when the two keys share a time, so a zero-length segment holds
// the left key rather than dividing by zero.
//
// Clamped, which is what makes a sample before the first key hold it rather than
// extrapolating backwards: `find_segment_start` returns segment 0 for an earlier
// time, and the clamp turns the negative fraction into "at the left key".
[[nodiscard]] f32 segment_alpha(const auto& keys, usize i, f32 time) noexcept {
    if (i + 1 >= keys.size()) {
        return 0.0f;
    }
    const f32 span = keys[i + 1].time - keys[i].time;
    if (span <= 0.0f) {
        return 0.0f;
    }
    const f32 alpha = (time - keys[i].time) / span;
    return alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

// The cubic Hermite basis, evaluated at `t` in [0, 1], applied to the two values
// and the two tangents scaled by the segment's duration.
//
// This is glTF's CUBICSPLINE formula written out:
//   p(t) = (2t^3 - 3t^2 + 1) p0 + (t^3 - 2t^2 + t) dt m0
//        + (-2t^3 + 3t^2)    p1 + (t^3 - t^2)     dt m1
// The dt on the tangents is what makes a tangent a velocity in units per second
// rather than per segment, so an authored curve is independent of how far apart
// its keys are.
[[nodiscard]] f32 hermite(f32 t, f32 p0, f32 p1, f32 m0, f32 m1, f32 dt) noexcept {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const f32 h10 = t3 - 2.0f * t2 + t;
    const f32 h01 = -2.0f * t3 + 3.0f * t2;
    const f32 h11 = t3 - t2;
    return h00 * p0 + h10 * dt * m0 + h01 * p1 + h11 * dt * m1;
}

} // namespace

// ---------------------------------------------------------------------------
// VectorTrack
// ---------------------------------------------------------------------------

void VectorTrack::add_key(const VectorKey& key) {
    // Keep the list sorted. A loader reading a glTF accessor appends already
    // sorted keys and hits the fast path; a hand-authored clip that adds keys out
    // of order still ends up correct.
    if (keys.empty() || key.time >= keys.back().time) {
        keys.push_back(key);
        return;
    }
    const auto it = std::upper_bound(keys.begin(), keys.end(), key.time,
                                     [](f32 value, const VectorKey& existing) { return value < existing.time; });
    keys.insert(it, key);
}

Vec3 VectorTrack::sample(f32 time) const noexcept {
    if (keys.empty()) {
        return Vec3{};
    }
    if (keys.size() == 1) {
        return keys.front().value;
    }

    const usize i = find_segment_start(keys, time);
    // At or past the last key there is no segment to interpolate into; hold it.
    if (i + 1 >= keys.size()) {
        return keys[i].value;
    }
    const VectorKey& a = keys[i];
    const VectorKey& b = keys[i + 1];

    switch (interpolation) {
    case Interpolation::Step:
        return a.value;
    case Interpolation::Cubic: {
        const f32 t = segment_alpha(keys, i, time);
        const f32 dt = b.time - a.time;
        return Vec3{hermite(t, a.value.x, b.value.x, a.out_tangent.x, b.in_tangent.x, dt),
                    hermite(t, a.value.y, b.value.y, a.out_tangent.y, b.in_tangent.y, dt),
                    hermite(t, a.value.z, b.value.z, a.out_tangent.z, b.in_tangent.z, dt)};
    }
    case Interpolation::Linear:
    default: {
        const f32 t = segment_alpha(keys, i, time);
        return Vec3{a.value.x + (b.value.x - a.value.x) * t, a.value.y + (b.value.y - a.value.y) * t,
                    a.value.z + (b.value.z - a.value.z) * t};
    }
    }
}

// ---------------------------------------------------------------------------
// QuaternionTrack
// ---------------------------------------------------------------------------

void QuaternionTrack::add_key(const QuaternionKey& key) {
    if (keys.empty() || key.time >= keys.back().time) {
        keys.push_back(key);
        return;
    }
    const auto it = std::upper_bound(keys.begin(), keys.end(), key.time,
                                     [](f32 value, const QuaternionKey& existing) { return value < existing.time; });
    keys.insert(it, key);
}

Quat QuaternionTrack::sample(f32 time) const noexcept {
    if (keys.empty()) {
        return Quat::identity();
    }
    if (keys.size() == 1) {
        return keys.front().value;
    }

    const usize i = find_segment_start(keys, time);
    if (i + 1 >= keys.size()) {
        return keys[i].value;
    }
    const QuaternionKey& a = keys[i];
    const QuaternionKey& b = keys[i + 1];

    switch (interpolation) {
    case Interpolation::Step:
        return a.value;
    case Interpolation::Cubic: {
        // Component-wise Hermite over the four components, then renormalise. glTF
        // specifies exactly this: the spline is not a rotation in between, so the
        // normalise is what brings it back onto the unit sphere.
        const f32 t = segment_alpha(keys, i, time);
        const f32 dt = b.time - a.time;
        Quat result{hermite(t, a.value.x, b.value.x, a.out_tangent.x, b.in_tangent.x, dt),
                    hermite(t, a.value.y, b.value.y, a.out_tangent.y, b.in_tangent.y, dt),
                    hermite(t, a.value.z, b.value.z, a.out_tangent.z, b.in_tangent.z, dt),
                    hermite(t, a.value.w, b.value.w, a.out_tangent.w, b.in_tangent.w, dt)};
        return result.normalized();
    }
    case Interpolation::Linear:
    default: {
        const f32 t = segment_alpha(keys, i, time);
        return rotation_interpolation == RotationInterpolation::Nlerp ? Quat::nlerp(a.value, b.value, t)
                                                                      : Quat::slerp(a.value, b.value, t);
    }
    }
}

// ---------------------------------------------------------------------------
// AnimationClip
// ---------------------------------------------------------------------------

AnimationClip::AnimationClip(std::string path, std::string name, std::vector<JointTrack> tracks)
    : Resource(std::move(path)), name_(std::move(name)), tracks_(std::move(tracks)) {
    recompute_duration();
}

void AnimationClip::recompute_duration() {
    f32 longest = 0.0f;
    for (const JointTrack& track : tracks_) {
        if (track.translation) {
            longest = std::max(longest, track.translation->end_time());
        }
        if (track.rotation) {
            longest = std::max(longest, track.rotation->end_time());
        }
        if (track.scale) {
            longest = std::max(longest, track.scale->end_time());
        }
    }
    duration_ = longest;
}

usize AnimationClip::bind(const Skeleton& skeleton) {
    usize bound = 0;
    for (JointTrack& track : tracks_) {
        track.joint = skeleton.find_joint(track.joint_name);
        if (track.joint >= 0) {
            ++bound;
        }
    }
    return bound;
}

void AnimationClip::sample(f32 time, SkeletonPose& pose) const {
    for (const JointTrack& track : tracks_) {
        if (track.joint < 0) {
            continue; // Not bound to this rig.
        }
        const usize index = static_cast<usize>(track.joint);
        if (index >= pose.joint_count()) {
            continue; // The rig is shorter than the clip; skip rather than overrun.
        }
        JointPose& joint = pose.at(index);
        // Only the channels the clip carries are written, so a clip that animates
        // one channel leaves the others at whatever the pose already held.
        if (track.translation) {
            joint.translation = track.translation->sample(time);
        }
        if (track.rotation) {
            joint.rotation = track.rotation->sample(time);
        }
        if (track.scale) {
            joint.scale = track.scale->sample(time);
        }
    }
}

f32 AnimationClip::wrap_time(f32 time, f32 duration, LoopMode mode) noexcept {
    if (duration <= 0.0f) {
        return 0.0f;
    }
    if (time <= 0.0f) {
        // A negative time is meaningful only for PingPong, where it reflects to
        // the positive half rather than clamping, so a caller can drive the
        // parameter back and forth across zero.
        if (mode == LoopMode::PingPong) {
            const f32 doubled = std::fmod(-time, duration * 2.0f);
            return doubled <= duration ? doubled : duration * 2.0f - doubled;
        }
        return 0.0f;
    }

    switch (mode) {
    case LoopMode::Once:
        return time >= duration ? duration : time;
    case LoopMode::PingPong: {
        const f32 cycle = std::fmod(time, duration * 2.0f);
        return cycle <= duration ? cycle : duration * 2.0f - cycle;
    }
    case LoopMode::Loop:
    default:
        return std::fmod(time, duration);
    }
}

void blend_poses(const SkeletonPose& a, const SkeletonPose& b, f32 t, SkeletonPose& out) {
    // Clamp, so a caller that overshoots an ease does not extrapolate into a pose
    // neither clip contains.
    const f32 alpha = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const usize count = std::min(a.joint_count(), b.joint_count());
    out.resize(count);
    for (usize i = 0; i < count; ++i) {
        const JointPose& from = a.at(i);
        const JointPose& to = b.at(i);
        JointPose& result = out.at(i);
        result.translation = Vec3{from.translation.x + (to.translation.x - from.translation.x) * alpha,
                                  from.translation.y + (to.translation.y - from.translation.y) * alpha,
                                  from.translation.z + (to.translation.z - from.translation.z) * alpha};
        // Rotations interpolate on the sphere; a linear blend of two quaternions
        // is not a rotation and shows as a scale wobble at the midpoint.
        result.rotation = Quat::slerp(from.rotation, to.rotation, alpha);
        result.scale =
            Vec3{from.scale.x + (to.scale.x - from.scale.x) * alpha, from.scale.y + (to.scale.y - from.scale.y) * alpha,
                 from.scale.z + (to.scale.z - from.scale.z) * alpha};
    }
}

} // namespace fe::anim
