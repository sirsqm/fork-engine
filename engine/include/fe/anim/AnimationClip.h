// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/anim/Skeleton.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fe::anim {

// How the value between two keys is produced.
//
// The three modes are the three glTF defines, and they are the three an animator
// actually reaches for:
//
//   * Step   - hold the left key until the right one is reached. A snap, used
//              for a sprite-style flip or a discrete state change.
//   * Linear - interpolate. For a translation or a scale this is a lerp; for a
//              rotation it is a slerp (see `RotationInterpolation`), because a
//              linear blend of two quaternions is not a rotation.
//   * Cubic  - a cubic Hermite segment per key, with a tangent on each side. What
//              an exporter writes when the author set a curve handle in the DCC
//              tool, and what gives an ease-in/ease-out without extra keys.
enum class Interpolation : u8 {
    Step,
    Linear,
    Cubic,
};

// Which interpolation a *rotation* track uses between linear keys.
//
// A separate choice because the two are both correct and trade differently:
// `Slerp` sweeps at a constant angular rate, `Nlerp` is a chord and is cheaper
// but does not. glTF says "LINEAR" and leaves the choice open, which is exactly
// why it is named here rather than assumed.
enum class RotationInterpolation : u8 {
    Slerp,
    Nlerp,
};

// How a clip's playback time maps into its key range when it runs past the end.
enum class LoopMode : u8 {
    Once,     // Hold the final pose. The shape a one-shot attack or a death uses.
    Loop,     // Wrap to the start. The shape a walk cycle uses.
    PingPong, // Reverse at each end. The shape a pendulum uses.
};

// One key of a vector channel (translation or scale).
//
// The tangents are only read by a Cubic track; a Step or Linear track leaves
// them zero and they cost nothing but the space. They are stored per key rather
// than derived from the neighbours because glTF's CUBICSPLINE carries an in and
// an out tangent per key, and a curve the author shaped is not reproducible from
// the values alone.
struct VectorKey {
    f32 time = 0.0f;
    Vec3 value{};
    Vec3 in_tangent{};
    Vec3 out_tangent{};

    [[nodiscard]] constexpr bool operator==(const VectorKey&) const noexcept = default;
};

// One key of a rotation channel.
struct QuaternionKey {
    f32 time = 0.0f;
    Quat value = Quat::identity();
    // Cubic tangents, as quaternions. glTF writes them that way and requires the
    // result to be renormalised after the spline, which `sample` does.
    Quat in_tangent{};
    Quat out_tangent{};

    [[nodiscard]] constexpr bool operator==(const QuaternionKey&) const noexcept = default;
};

// A translation or scale channel: a sorted list of vector keys.
//
// The keys are kept sorted by time and the sampler assumes it. `add_key` inserts
// in order, so a loader that reads a glTF accessor (already sorted) appends and a
// hand-authored clip is sorted as it is built.
class VectorTrack {
public:
    Interpolation interpolation = Interpolation::Linear;
    std::vector<VectorKey> keys;

    void add_key(const VectorKey& key);
    [[nodiscard]] bool empty() const noexcept { return keys.empty(); }
    [[nodiscard]] f32 start_time() const noexcept { return keys.empty() ? 0.0f : keys.front().time; }
    [[nodiscard]] f32 end_time() const noexcept { return keys.empty() ? 0.0f : keys.back().time; }

    // The value at `time`. A time before the first key or after the last holds
    // that key's value, so a clip sampled outside its range does not extrapolate
    // into a pose the author never made.
    [[nodiscard]] Vec3 sample(f32 time) const noexcept;
};

// A rotation channel: a sorted list of quaternion keys.
class QuaternionTrack {
public:
    Interpolation interpolation = Interpolation::Linear;
    RotationInterpolation rotation_interpolation = RotationInterpolation::Slerp;
    std::vector<QuaternionKey> keys;

    void add_key(const QuaternionKey& key);
    [[nodiscard]] bool empty() const noexcept { return keys.empty(); }
    [[nodiscard]] f32 start_time() const noexcept { return keys.empty() ? 0.0f : keys.front().time; }
    [[nodiscard]] f32 end_time() const noexcept { return keys.empty() ? 0.0f : keys.back().time; }

    [[nodiscard]] Quat sample(f32 time) const noexcept;
};

// Everything a clip animates about one joint.
//
// The three channels are separate tracks rather than one track of a combined
// "joint transform" because a format writes them separately and a clip commonly
// animates only one: a look-at rig rotates a head with no translation, and a
// bounce animates translation with no rotation. A channel left absent means
// "this clip says nothing about it", so the joint keeps its rest value for that
// channel and the two compose. Collapsing them into one track would force every
// clip to author all three and would overwrite an untouched channel with a
// default.
struct JointTrack {
    // The joint this track drives. Set by `AnimationClip::bind` from the name;
    // -1 while unresolved, and a track with -1 is skipped by `sample` rather
    // than silently writing joint 0.
    i32 joint = -1;

    // The joint's authored name, and how the track finds its joint. A name rather
    // than an index because it is what a glTF animation channel carries (a
    // channel names a node, not a position in a skin's joint array) and what a
    // human-authored clip can be written against.
    std::string joint_name;

    std::optional<VectorTrack> translation;
    std::optional<QuaternionTrack> rotation;
    std::optional<VectorTrack> scale;

    [[nodiscard]] bool empty() const noexcept { return !translation && !rotation && !scale; }
};

// A named animation: a set of joint tracks over a time range.
//
// A resource, because a clip is loaded, cached, shared by every instance of a
// character, and referenced by a component by path — exactly like a mesh. It
// holds CPU data only and knows nothing about a GPU, a scene, or a node, so it
// can be sampled and asserted on in a test with nothing else constructed.
//
// The clip is bound to a skeleton separately from loading, because a glTF's
// animations and its skins are independent top-level arrays and one clip is
// frequently usable by several rigs. `bind` resolves each track's joint name to
// an index once; until then a track is inert.
class AnimationClip final : public Resource {
public:
    static constexpr const char* kTypeName = "AnimationClip";

    AnimationClip(std::string path, std::string name, std::vector<JointTrack> tracks);

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The clip's authored name ("Walk", "Idle"). Not the file path: a glTF holds
    // several clips and the name is what a component references.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    [[nodiscard]] const std::vector<JointTrack>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] usize track_count() const noexcept { return tracks_.size(); }

    // The clip's length: the latest key time over every track. A clip with no
    // keys is zero-length. Computed from the tracks so it can never disagree with
    // them; a caller that needs a longer hold adds a key.
    [[nodiscard]] f32 duration() const noexcept { return duration_; }

    // Resolves every track's `joint_name` against `skeleton`, returning how many
    // tracks found a joint. A track that does not resolve keeps -1 and is skipped
    // by `sample`, so a clip authored for a slightly different rig plays the
    // joints it shares rather than failing whole.
    usize bind(const Skeleton& skeleton);

    // Writes every bound track's value at `time` into `pose`.
    //
    // A channel the clip does not carry is *not* written, so the pose's existing
    // value (normally the rest pose, from `SkeletonPose::reset_to_rest`) stands.
    // That is what lets a clip that animates one arm leave the rest of the body
    // alone. `time` is taken as-is: wrap it with `wrap_time` first if the clip is
    // playing on a loop.
    //
    // A joint index past the end of `pose` is skipped rather than writing out of
    // bounds, so a clip bound to a longer rig degrades instead of corrupting
    // memory.
    void sample(f32 time, SkeletonPose& pose) const;

    // The playback time to sample for an elapsed `time`, under `mode`.
    //
    // `Once` clamps to the last instant, `Loop` wraps, and `PingPong` reflects.
    // A zero or negative duration returns 0, so a degenerate clip holds its first
    // key instead of dividing by zero.
    [[nodiscard]] static f32 wrap_time(f32 time, f32 duration, LoopMode mode) noexcept;

private:
    void recompute_duration();

    std::string name_;
    std::vector<JointTrack> tracks_;
    f32 duration_ = 0.0f;
};

// Blends two poses into `out`, weighted `t` towards `b`.
//
// Per channel: translations and scales lerp, rotations slerp. This is what a
// cross-fade between two clips is built from, and it is a free function on poses
// rather than a method on a clip because a blend does not care where either pose
// came from — a clip, a procedural pose, or another blend.
//
// The two poses may differ in length; `out` takes the shorter, and the extra
// joints of the longer are dropped rather than read past the end.
void blend_poses(const SkeletonPose& a, const SkeletonPose& b, f32 t, SkeletonPose& out);

} // namespace fe::anim
