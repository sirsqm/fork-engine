// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for skeletal animation and skinning (docs/SHIPPING_QUEUE.md,
// `skeletal-animation`).
//
// These are pure-logic tests: a skeleton, a clip, a pose, and a palette are
// values, so every assertion runs with no scene, no device, and no context. That
// is the point of the design -- the palette is computed by a CPU walk the
// renderer only reads -- and it is what lets a test assert that a joint rotation
// moves a vertex the way the rig says it should, which no read-back of a frame
// can do precisely.
//
// The scene-facing parts (the system, extraction) are tested with a real Scene
// and the null RHI device, so the wiring is exercised end to end without a
// rasteriser.

#include <doctest/doctest.h>

#include "fe/anim/AnimationClip.h"
#include "fe/anim/Animator.h"
#include "fe/anim/SkeletalAnimationTypes.h"
#include "fe/anim/Skeleton.h"
#include "fe/anim/SkeletonAnimationSystem.h"
#include "fe/anim/SkinnedMesh.h"
#include "fe/anim/SkinnedMeshRenderer3D.h"
#include "fe/core/Frustum.h"
#include "fe/mesh/SkinnedVertex3D.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/render/RenderExtract.h"
#include "fe/rhi/Types.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace fe;
using namespace fe::anim;

namespace {

constexpr f32 kTolerance = 1e-4f;

[[nodiscard]] bool nearly_equal(f32 a, f32 b, f32 tolerance = kTolerance) {
    return std::fabs(a - b) <= tolerance;
}

[[nodiscard]] bool nearly_equal(const Vec3& a, const Vec3& b, f32 tolerance = kTolerance) {
    return nearly_equal(a.x, b.x, tolerance) && nearly_equal(a.y, b.y, tolerance) && nearly_equal(a.z, b.z, tolerance);
}

// A three-joint chain: root at the origin, `middle` one unit up, `tip` one unit
// further. Small enough to reason about by hand, deep enough that the
// parent-before-child walk and the parent-relative composition are both
// exercised.
[[nodiscard]] std::shared_ptr<Skeleton> make_chain_skeleton() {
    std::vector<Skeleton::Joint> joints(3);

    joints[0].name = "root";
    joints[0].parent = Skeleton::kNoParent;
    joints[0].rest.position = Vec3{0.0f, 0.0f, 0.0f};

    joints[1].name = "middle";
    joints[1].parent = 0;
    joints[1].rest.position = Vec3{0.0f, 1.0f, 0.0f};

    joints[2].name = "tip";
    joints[2].parent = 1;
    joints[2].rest.position = Vec3{0.0f, 1.0f, 0.0f};

    auto skeleton = std::make_shared<Skeleton>("skeleton:chain", std::move(joints));
    skeleton->compute_inverse_bind_matrices();
    return skeleton;
}

// A one-vertex mesh bound entirely to `joint`.
[[nodiscard]] std::shared_ptr<SkinnedMesh> make_single_vertex_mesh(std::shared_ptr<Skeleton> skeleton, f32 joint) {
    SkinnedVertex3D vertex;
    vertex.position = Vec3{0.0f, 0.0f, 0.0f};
    vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
    vertex.uv = Vec2{0.0f, 0.0f};
    vertex.joints = {joint, 0.0f, 0.0f, 0.0f};
    vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    return std::make_shared<SkinnedMesh>("mesh:dot", std::vector<SkinnedVertex3D>{vertex}, std::vector<u32>{0},
                                         std::move(skeleton));
}

// The same lookup the reflect tests use. A property is a vector entry, not a
// keyed map, so a test walks it.
[[nodiscard]] const reflect::Property* find_property(const reflect::TypeDesc& type, std::string_view name) {
    for (const reflect::Property& property : type.properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Skeleton: hierarchy, order, and the bind pose
// ---------------------------------------------------------------------------

TEST_CASE("a skeleton orders its joints parent-first whatever order they are written in") {
    // Children before parents, which glTF does not forbid and a hand-built rig
    // commonly does. The order must still be walkable.
    std::vector<Skeleton::Joint> joints(3);
    joints[0].name = "tip";
    joints[0].parent = 1;
    joints[1].name = "middle";
    joints[1].parent = 2;
    joints[2].name = "root";
    joints[2].parent = Skeleton::kNoParent;

    Skeleton skeleton("skeleton:reversed", std::move(joints));

    REQUIRE(skeleton.has_valid_hierarchy());
    const std::vector<i32>& order = skeleton.order();
    REQUIRE(order.size() == 3);

    // Every parent must appear before its child.
    std::vector<usize> position(3, 0);
    for (usize i = 0; i < order.size(); ++i) {
        position[static_cast<usize>(order[i])] = i;
    }
    for (usize i = 0; i < 3; ++i) {
        const i32 parent = skeleton.joint(i).parent;
        if (parent >= 0) {
            CHECK(position[static_cast<usize>(parent)] < position[i]);
        }
    }
}

TEST_CASE("a cyclic skeleton reports an invalid hierarchy rather than looping") {
    std::vector<Skeleton::Joint> joints(2);
    joints[0].name = "a";
    joints[0].parent = 1;
    joints[1].name = "b";
    joints[1].parent = 0;

    Skeleton skeleton("skeleton:cycle", std::move(joints));
    // No root, so the walk reaches nothing; the shortfall is what reports it.
    CHECK(skeleton.root_count() == 0);
    CHECK_FALSE(skeleton.has_valid_hierarchy());
}

TEST_CASE("the bind-pose palette is the identity for every joint") {
    // This is the property that proves the rest-pose walk and the inverse bind
    // matrices agree. It is also why a skinned mesh with no animation renders
    // exactly where its static twin would.
    auto skeleton = make_chain_skeleton();

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);

    std::vector<Mat4> palette;
    pose.skinning_matrices(*skeleton, palette);

    REQUIRE(palette.size() == skeleton->joint_count());
    for (usize i = 0; i < palette.size(); ++i) {
        for (usize column = 0; column < 4; ++column) {
            for (usize row = 0; row < 4; ++row) {
                const f32 expected = column == row ? 1.0f : 0.0f;
                CHECK(nearly_equal(palette[i].at(row, column), expected));
            }
        }
    }
}

TEST_CASE("a joint's world matrix composes its parents' transforms") {
    auto skeleton = make_chain_skeleton();

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);

    std::vector<Mat4> world;
    pose.world_matrices(*skeleton, world);
    REQUIRE(world.size() == 3);

    // Rest positions are 0, (0,1,0), (0,2,0) in world space.
    CHECK(nearly_equal(transform_point(world[0], Vec3{}), Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(transform_point(world[1], Vec3{}), Vec3{0.0f, 1.0f, 0.0f}));
    CHECK(nearly_equal(transform_point(world[2], Vec3{}), Vec3{0.0f, 2.0f, 0.0f}));
}

TEST_CASE("a joint name resolves to its index, and an unknown name does not") {
    auto skeleton = make_chain_skeleton();
    CHECK(skeleton->find_joint("root") == 0);
    CHECK(skeleton->find_joint("tip") == 2);
    CHECK(skeleton->find_joint("missing") == Skeleton::kNoParent);
}

// ---------------------------------------------------------------------------
// AnimationClip: sampling
// ---------------------------------------------------------------------------

TEST_CASE("a linear vector track interpolates between its keys and holds at the ends") {
    VectorTrack track;
    track.interpolation = Interpolation::Linear;
    track.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, {}, {}});
    track.add_key(VectorKey{1.0f, Vec3{10.0f, 0.0f, 0.0f}, {}, {}});

    CHECK(nearly_equal(track.sample(0.0f), Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(track.sample(0.5f), Vec3{5.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(track.sample(1.0f), Vec3{10.0f, 0.0f, 0.0f}));
    // Outside the range the ends hold rather than extrapolating.
    CHECK(nearly_equal(track.sample(-5.0f), Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(track.sample(5.0f), Vec3{10.0f, 0.0f, 0.0f}));
}

TEST_CASE("a step track holds the left key") {
    VectorTrack track;
    track.interpolation = Interpolation::Step;
    track.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, {}, {}});
    track.add_key(VectorKey{1.0f, Vec3{10.0f, 0.0f, 0.0f}, {}, {}});

    CHECK(nearly_equal(track.sample(0.99f), Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(track.sample(1.0f), Vec3{10.0f, 0.0f, 0.0f}));
}

TEST_CASE("keys added out of order are sorted") {
    VectorTrack track;
    track.add_key(VectorKey{1.0f, Vec3{10.0f, 0.0f, 0.0f}, {}, {}});
    track.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, {}, {}});
    track.add_key(VectorKey{0.5f, Vec3{5.0f, 0.0f, 0.0f}, {}, {}});

    REQUIRE(track.keys.size() == 3);
    CHECK(nearly_equal(track.keys[0].time, 0.0f));
    CHECK(nearly_equal(track.keys[1].time, 0.5f));
    CHECK(nearly_equal(track.keys[2].time, 1.0f));
    CHECK(nearly_equal(track.sample(0.5f), Vec3{5.0f, 0.0f, 0.0f}));
}

TEST_CASE("a rotation track slerps a quarter turn to its midpoint") {
    QuaternionTrack track;
    track.interpolation = Interpolation::Linear;
    track.add_key(QuaternionKey{0.0f, Quat::identity(), {}, {}});
    track.add_key(QuaternionKey{1.0f, Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(90.0f)), {}, {}});

    const Quat midpoint = track.sample(0.5f);
    // Half of a 90-degree turn about +Z is 45 degrees about +Z.
    const Quat expected = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(45.0f));
    CHECK(nearly_equal(std::fabs(midpoint.dot(expected)), 1.0f, 1e-3f));
}

TEST_CASE("a cubic vector track follows its tangents, not a straight line") {
    // Keys at 0 and 1 with a positive outgoing tangent at the first key: the
    // curve must leave the first key faster than a lerp would.
    VectorTrack track;
    track.interpolation = Interpolation::Cubic;
    track.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, Vec3{}, Vec3{2.0f, 0.0f, 0.0f}});
    track.add_key(VectorKey{1.0f, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}, Vec3{}});

    CHECK(nearly_equal(track.sample(0.0f), Vec3{0.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(track.sample(1.0f), Vec3{1.0f, 0.0f, 0.0f}));
    // The ease-out shape puts the quarter-time value above the linear 0.25.
    CHECK(track.sample(0.25f).x > 0.25f);
}

TEST_CASE("wrap_time maps playback into the clip's range for each loop mode") {
    CHECK(nearly_equal(AnimationClip::wrap_time(1.5f, 1.0f, LoopMode::Loop), 0.5f));
    CHECK(nearly_equal(AnimationClip::wrap_time(1.5f, 1.0f, LoopMode::Once), 1.0f));
    CHECK(nearly_equal(AnimationClip::wrap_time(1.5f, 1.0f, LoopMode::PingPong), 0.5f));
    CHECK(nearly_equal(AnimationClip::wrap_time(2.5f, 1.0f, LoopMode::PingPong), 0.5f));
    // A degenerate clip holds its first key rather than dividing by zero.
    CHECK(nearly_equal(AnimationClip::wrap_time(3.0f, 0.0f, LoopMode::Loop), 0.0f));
}

TEST_CASE("a clip samples only the channels it carries, leaving the rest of the pose") {
    auto skeleton = make_chain_skeleton();

    // A clip that rotates only the tip joint. Every other channel of every other
    // joint must be untouched, so the pose keeps its rest values.
    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "tip";
    QuaternionTrack rotation;
    rotation.add_key(QuaternionKey{0.0f, Quat::identity(), {}, {}});
    rotation.add_key(QuaternionKey{1.0f, Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, radians(90.0f)), {}, {}});
    tracks[0].rotation = rotation;

    AnimationClip clip("clip:tip", "Tip", std::move(tracks));
    CHECK(nearly_equal(clip.duration(), 1.0f));
    CHECK(clip.bind(*skeleton) == 1);

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);
    clip.sample(1.0f, pose);

    // The tip rotated; its parent and grandparent are still at their rest
    // transforms.
    CHECK(pose.at(0).translation.y == doctest::Approx(0.0f));
    CHECK(pose.at(1).translation.y == doctest::Approx(1.0f));
    CHECK(pose.at(1).rotation == Quat::identity());
    CHECK_FALSE(pose.at(2).rotation == Quat::identity());
}

TEST_CASE("a track whose joint is missing from the rig is skipped, not written to joint zero") {
    auto skeleton = make_chain_skeleton();

    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "thumb"; // Not in the chain.
    VectorTrack translation;
    translation.add_key(VectorKey{0.0f, Vec3{9.0f, 9.0f, 9.0f}, {}, {}});
    tracks[0].translation = translation;

    AnimationClip clip("clip:missing", "Missing", std::move(tracks));
    CHECK(clip.bind(*skeleton) == 0);

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);
    clip.sample(0.0f, pose);

    // Joint 0 must be untouched: the whole point of resolving the name to -1.
    CHECK(nearly_equal(pose.at(0).translation, Vec3{0.0f, 0.0f, 0.0f}));
}

TEST_CASE("blending two poses lerps translations and slerps rotations") {
    SkeletonPose a;
    a.resize(1);
    a.at(0).translation = Vec3{0.0f, 0.0f, 0.0f};
    a.at(0).rotation = Quat::identity();
    a.at(0).scale = Vec3{1.0f, 1.0f, 1.0f};

    SkeletonPose b;
    b.resize(1);
    b.at(0).translation = Vec3{10.0f, 0.0f, 0.0f};
    b.at(0).rotation = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(90.0f));
    b.at(0).scale = Vec3{3.0f, 3.0f, 3.0f};

    SkeletonPose out;
    blend_poses(a, b, 0.5f, out);
    REQUIRE(out.joint_count() == 1);
    CHECK(nearly_equal(out.at(0).translation, Vec3{5.0f, 0.0f, 0.0f}));
    CHECK(nearly_equal(out.at(0).scale, Vec3{2.0f, 2.0f, 2.0f}));
    const Quat expected = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(45.0f));
    CHECK(nearly_equal(std::fabs(out.at(0).rotation.dot(expected)), 1.0f, 1e-3f));
}

// ---------------------------------------------------------------------------
// Skinning
// ---------------------------------------------------------------------------

// The vertex buffer is sized from `sizeof(SkinnedVertex3D)` and the attributes
// are described by `VertexLayout::skinned_position_normal_uv()`. If the two
// disagree on the stride the upload overruns the buffer the device allocated and
// the draw reads out of bounds -- a GL_INVALID_VALUE and a black frame, not a
// compile error. They agreed when this test was written and must keep agreeing.
TEST_CASE("the skinned vertex struct matches its GPU layout") {
    const rhi::VertexLayout layout = rhi::VertexLayout::skinned_position_normal_uv();
    CHECK(sizeof(SkinnedVertex3D) == layout.stride);

    // Each attribute must land on the member it names: position, normal, uv, the
    // four joints, the four weights.
    REQUIRE(layout.attributes.size() == 5);
    CHECK(layout.attributes[0].offset == offsetof(SkinnedVertex3D, position));
    CHECK(layout.attributes[1].offset == offsetof(SkinnedVertex3D, normal));
    CHECK(layout.attributes[2].offset == offsetof(SkinnedVertex3D, uv));
    CHECK(layout.attributes[3].offset == offsetof(SkinnedVertex3D, joints));
    CHECK(layout.attributes[4].offset == offsetof(SkinnedVertex3D, weights));
}

TEST_CASE("skinning by the bind-pose palette leaves every vertex where it was") {
    auto skeleton = make_chain_skeleton();
    auto mesh = make_single_vertex_mesh(skeleton, 0);

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);
    std::vector<Mat4> palette;
    pose.skinning_matrices(*skeleton, palette);

    const SkinnedVertex3D skinned = skin_vertex(mesh->vertices().front(), palette);
    CHECK(nearly_equal(skinned.position, mesh->vertices().front().position));
}

TEST_CASE("a joint rotation moves the vertices weighted to it") {
    auto skeleton = make_chain_skeleton();
    // The tip's rest world position is (0,2,0). Rotate the *middle* joint 90
    // degrees about +X: the middle joint sits at (0,1,0), so the tip swings about
    // that point. +Y maps to +Z under a +90 degree turn about +X, putting the tip
    // at (0,1,1): the middle's own height is untouched and the two-unit reach
    // becomes one unit along +Z.
    SkinnedVertex3D vertex;
    vertex.position = Vec3{0.0f, 2.0f, 0.0f};
    vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
    vertex.joints = {2.0f, 0.0f, 0.0f, 0.0f};
    vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    SkinnedMesh mesh("mesh:tip", std::vector<SkinnedVertex3D>{vertex}, std::vector<u32>{0}, skeleton);

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);
    pose.at(1).rotation = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, radians(90.0f));

    std::vector<Mat4> palette;
    pose.skinning_matrices(*skeleton, palette);

    const SkinnedVertex3D skinned = skin_vertex(mesh.vertices().front(), palette);
    CHECK(nearly_equal(skinned.position, Vec3{0.0f, 1.0f, 1.0f}, 1e-3f));
}

TEST_CASE("weights that do not sum to one are normalised") {
    auto skeleton = make_chain_skeleton();

    SkinnedVertex3D vertex;
    vertex.position = Vec3{1.0f, 0.0f, 0.0f};
    vertex.joints = {0.0f, 0.0f, 0.0f, 0.0f};
    // Sums to 0.5. Without normalisation the vertex would shrink towards the
    // origin by half.
    vertex.weights = {0.25f, 0.25f, 0.0f, 0.0f};

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);
    std::vector<Mat4> palette;
    pose.skinning_matrices(*skeleton, palette);

    const SkinnedVertex3D skinned = skin_vertex(vertex, palette);
    CHECK(nearly_equal(skinned.position, Vec3{1.0f, 0.0f, 0.0f}));
}

TEST_CASE("a vertex with no usable influence stays at its bind position") {
    auto skeleton = make_chain_skeleton();

    SkinnedVertex3D vertex;
    vertex.position = Vec3{3.0f, 4.0f, 5.0f};
    vertex.joints = {99.0f, 99.0f, 99.0f, 99.0f}; // Past the end of any palette here.
    vertex.weights = {0.5f, 0.5f, 0.0f, 0.0f};

    std::vector<Mat4> palette; // Empty: nothing resolves.
    const SkinnedVertex3D skinned = skin_vertex(vertex, palette);
    CHECK(nearly_equal(skinned.position, Vec3{3.0f, 4.0f, 5.0f}));
}

TEST_CASE("skinned bounds grow when a pose reaches outside the bind box") {
    auto skeleton = make_chain_skeleton();

    SkinnedVertex3D vertex;
    vertex.position = Vec3{0.0f, 2.0f, 0.0f};
    vertex.joints = {2.0f, 0.0f, 0.0f, 0.0f};
    vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    SkinnedMesh mesh("mesh:arm", std::vector<SkinnedVertex3D>{vertex}, std::vector<u32>{0}, skeleton);

    SkeletonPose pose;
    pose.reset_to_rest(*skeleton);
    std::vector<Mat4> palette;
    pose.skinning_matrices(*skeleton, palette);

    Vec3 min{};
    Vec3 max{};
    REQUIRE(skinned_bounds(mesh.vertices(), palette, min, max));
    // In the bind pose the vertex is at (0,2,0), so the box is that single point.
    CHECK(nearly_equal(min, Vec3{0.0f, 2.0f, 0.0f}));
    CHECK(nearly_equal(max, Vec3{0.0f, 2.0f, 0.0f}));

    pose.at(1).rotation = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, radians(90.0f));
    pose.skinning_matrices(*skeleton, palette);
    REQUIRE(skinned_bounds(mesh.vertices(), palette, min, max));
    // The arm now reaches along +Z from the middle joint at (0,1,0), so the box
    // followed it to (0,1,1) rather than staying at the bind-pose point (0,2,0).
    CHECK(nearly_equal(max.z, 1.0f, 1e-3f));
    CHECK(nearly_equal(min.y, 1.0f, 1e-3f));
}

// ---------------------------------------------------------------------------
// Animator: playback and cross-fade
// ---------------------------------------------------------------------------

TEST_CASE("an animator with no skeleton or clip reports zero durations") {
    Animator animator;
    CHECK(animator.clip_duration() == doctest::Approx(0.0f));
    CHECK((animator.skeleton_resource() == nullptr));
    CHECK((animator.clip_resource() == nullptr));
    CHECK_FALSE(animator.has_palette());
}

TEST_CASE("play restarts the clip and clears any fade in flight") {
    auto skeleton = make_chain_skeleton();

    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "root";
    QuaternionTrack rotation;
    rotation.add_key(QuaternionKey{0.0f, Quat::identity(), {}, {}});
    rotation.add_key(QuaternionKey{2.0f, Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(90.0f)), {}, {}});
    tracks[0].rotation = rotation;
    auto clip = std::make_shared<AnimationClip>("clip:a", "A", std::move(tracks));
    clip->bind(*skeleton);

    Animator animator;
    animator.time = 1.5f;
    animator.blend_clip = clip;
    animator.blend_weight = 1.0f;

    animator.play(clip, LoopMode::Loop);
    CHECK(animator.time == doctest::Approx(0.0f));
    CHECK((animator.blend_clip == nullptr));
    CHECK(animator.blend_weight == doctest::Approx(0.0f));
    CHECK(animator.clip_duration() == doctest::Approx(2.0f));
}

TEST_CASE("a cross-fade ramps its weight to zero and retires the outgoing clip") {
    auto skeleton = make_chain_skeleton();

    auto make_clip = [&](std::string name, f32 duration) {
        std::vector<JointTrack> tracks(1);
        tracks[0].joint_name = "root";
        VectorTrack translation;
        translation.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, {}, {}});
        translation.add_key(VectorKey{duration, Vec3{0.0f, 1.0f, 0.0f}, {}, {}});
        tracks[0].translation = translation;
        auto clip = std::make_shared<AnimationClip>("clip:" + name, name, std::move(tracks));
        clip->bind(*skeleton);
        return clip;
    };

    auto idle = make_clip("idle", 1.0f);
    auto walk = make_clip("walk", 1.0f);

    Animator animator;
    animator.skeleton = skeleton;
    animator.play(idle);
    animator.time = 0.5f;

    animator.cross_fade_to(walk, 1.0f);
    CHECK((animator.blend_clip == idle));
    CHECK((animator.clip == walk));
    CHECK(animator.blend_weight == doctest::Approx(1.0f));
    CHECK(animator.blend_time() == doctest::Approx(0.5f));
    CHECK(animator.blend_remaining() == doctest::Approx(1.0f));

    animator.advance_blend(0.5f);
    CHECK(animator.blend_weight == doctest::Approx(0.5f));
    CHECK(animator.blend_time() == doctest::Approx(1.0f));

    animator.advance_blend(0.5f);
    // The fade is over: the outgoing clip is released rather than held at zero.
    CHECK((animator.blend_clip == nullptr));
    CHECK(animator.blend_weight == doctest::Approx(0.0f));
}

TEST_CASE("a zero-duration cross-fade is a cut") {
    auto skeleton = make_chain_skeleton();
    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "root";
    VectorTrack translation;
    translation.add_key(VectorKey{0.0f, Vec3{1.0f, 0.0f, 0.0f}, {}, {}});
    tracks[0].translation = translation;
    auto clip = std::make_shared<AnimationClip>("clip:x", "X", std::move(tracks));
    clip->bind(*skeleton);

    Animator animator;
    animator.play(clip);
    animator.cross_fade_to(clip, 0.0f);
    CHECK((animator.blend_clip == nullptr));
    CHECK(animator.blend_weight == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// SkeletonAnimationSystem: the frame step
// ---------------------------------------------------------------------------

TEST_CASE("the system advances an animator and publishes a palette") {
    auto skeleton = make_chain_skeleton();

    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "root";
    QuaternionTrack rotation;
    rotation.add_key(QuaternionKey{0.0f, Quat::identity(), {}, {}});
    rotation.add_key(QuaternionKey{1.0f, Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(90.0f)), {}, {}});
    tracks[0].rotation = rotation;
    auto clip = std::make_shared<AnimationClip>("clip:turn", "Turn", std::move(tracks));
    clip->bind(*skeleton);

    Scene scene;
    Node* node_ptr = scene.create_node("character");
    auto* animator = node_ptr->add_component<Animator>();
    REQUIRE((animator != nullptr));
    animator->skeleton = skeleton;
    animator->play(clip);

    SkeletonAnimationSystem system;
    FrameContext context;
    context.delta_time = 0.25f;
    system.update(context, scene);

    CHECK(system.last_advanced_count() == 1);
    CHECK(system.last_skipped_count() == 0);
    CHECK(animator->time == doctest::Approx(0.25f));
    CHECK(animator->has_palette());
    CHECK(animator->palette().size() == skeleton->joint_count());
    // The clock reached a quarter of the way through the turn.
    CHECK(animator->pose().joint_count() == skeleton->joint_count());
}

TEST_CASE("the system skips an animator with no skeleton") {
    Scene scene;
    Node* node_ptr = scene.create_node("unrigged");
    node_ptr->add_component<Animator>();

    SkeletonAnimationSystem system;
    FrameContext context;
    context.delta_time = 0.016f;
    system.update(context, scene);

    CHECK(system.last_advanced_count() == 0);
    CHECK(system.last_skipped_count() == 1);
}

TEST_CASE("a paused animator holds its pose while the blend clock still runs") {
    auto skeleton = make_chain_skeleton();

    auto make_clip = [&](std::string name) {
        std::vector<JointTrack> tracks(1);
        tracks[0].joint_name = "root";
        QuaternionTrack rotation;
        rotation.add_key(QuaternionKey{0.0f, Quat::identity(), {}, {}});
        rotation.add_key(QuaternionKey{1.0f, Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, radians(90.0f)), {}, {}});
        tracks[0].rotation = rotation;
        auto clip = std::make_shared<AnimationClip>("clip:" + name, name, std::move(tracks));
        clip->bind(*skeleton);
        return clip;
    };

    Scene scene;
    Node* node_ptr = scene.create_node("character");
    auto* animator = node_ptr->add_component<Animator>();
    animator->skeleton = skeleton;
    animator->play(make_clip("a"));
    animator->time = 0.3f;
    animator->playing = false;
    animator->cross_fade_to(make_clip("b"), 1.0f);
    // `cross_fade_to` promotes the incoming clip to primary and restarts its
    // clock, which is the documented shape of a fade: the new clip begins while
    // the old one runs out. The pause is what must not move the clock afterwards.
    animator->time = 0.3f;

    SkeletonAnimationSystem system;
    FrameContext context;
    context.delta_time = 0.5f;
    system.update(context, scene);

    // The playback clock did not advance...
    CHECK(animator->time == doctest::Approx(0.3f));
    // ...but the fade did, so a pause does not freeze a blend forever.
    CHECK(animator->blend_weight == doctest::Approx(0.5f));
}

TEST_CASE("root motion moves the node and is removed from the pose") {
    auto skeleton = make_chain_skeleton();

    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "root";
    VectorTrack translation;
    translation.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, {}, {}});
    translation.add_key(VectorKey{1.0f, Vec3{1.0f, 0.0f, 0.0f}, {}, {}});
    tracks[0].translation = translation;
    auto clip = std::make_shared<AnimationClip>("clip:walk", "Walk", std::move(tracks));
    clip->bind(*skeleton);

    Scene scene;
    Node* node_ptr = scene.create_node("character");
    auto* animator = node_ptr->add_component<Animator>();
    animator->skeleton = skeleton;
    animator->play(clip);
    animator->apply_root_motion = true;

    SkeletonAnimationSystem system;
    FrameContext context;
    context.delta_time = 0.25f;

    system.update(context, scene);
    // First frame: the root moved from 0 to 0.25, so the node moved by 0.25.
    CHECK(node_ptr->transform().position.x == doctest::Approx(0.25f));
    // The consumed translation is removed, so the geometry does not move twice.
    CHECK(animator->pose().at(0).translation.x == doctest::Approx(0.0f));

    system.update(context, scene);
    CHECK(node_ptr->transform().position.x == doctest::Approx(0.5f));
}

TEST_CASE("root motion is not applied when the flag is off") {
    auto skeleton = make_chain_skeleton();

    std::vector<JointTrack> tracks(1);
    tracks[0].joint_name = "root";
    VectorTrack translation;
    translation.add_key(VectorKey{0.0f, Vec3{0.0f, 0.0f, 0.0f}, {}, {}});
    translation.add_key(VectorKey{1.0f, Vec3{1.0f, 0.0f, 0.0f}, {}, {}});
    tracks[0].translation = translation;
    auto clip = std::make_shared<AnimationClip>("clip:walk", "Walk", std::move(tracks));
    clip->bind(*skeleton);

    Scene scene;
    Node* node_ptr = scene.create_node("character");
    auto* animator = node_ptr->add_component<Animator>();
    animator->skeleton = skeleton;
    animator->play(clip);
    animator->apply_root_motion = false;

    SkeletonAnimationSystem system;
    FrameContext context;
    context.delta_time = 0.5f;
    system.update(context, scene);

    // The node did not move; the clip's translation is still in the pose.
    CHECK(node_ptr->transform().position.x == doctest::Approx(0.0f));
    CHECK(animator->pose().at(0).translation.x == doctest::Approx(0.5f));
}

// ---------------------------------------------------------------------------
// Extraction seam
// ---------------------------------------------------------------------------

TEST_CASE("extraction collects a skinned instance and uses the posed bounds") {
    auto skeleton = make_chain_skeleton();

    SkinnedVertex3D vertex;
    vertex.position = Vec3{0.0f, 2.0f, 0.0f};
    vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
    vertex.joints = {2.0f, 0.0f, 0.0f, 0.0f};
    vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    auto mesh =
        std::make_shared<SkinnedMesh>("mesh:arm", std::vector<SkinnedVertex3D>{vertex}, std::vector<u32>{0}, skeleton);

    Scene scene;
    Node* node_ptr = scene.create_node("character");
    auto* renderer = node_ptr->add_component<SkinnedMeshRenderer3D>();
    renderer->mesh = mesh;
    auto* animator = node_ptr->add_component<Animator>();
    animator->skeleton = skeleton;

    // Before the system runs there is no palette: the bind-pose bounds are used.
    std::vector<render::SkinnedInstance> instances;
    render::extract_skinned_meshes(scene, instances);
    REQUIRE(instances.size() == 1);
    CHECK((instances[0].palette == nullptr));
    CHECK(nearly_equal(instances[0].world_min, Vec3{0.0f, 2.0f, 0.0f}));
    CHECK(nearly_equal(instances[0].world_max, Vec3{0.0f, 2.0f, 0.0f}));

    // Step the system once so the palette is published, then overwrite the pose
    // with one that reaches along +Z so the posed bound is distinguishable from
    // the bind box.
    SkeletonAnimationSystem system;
    FrameContext context;
    context.delta_time = 0.0f;
    system.update(context, scene);
    REQUIRE(animator->has_palette());

    animator->mutable_pose().at(1).rotation = Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, radians(90.0f));
    animator->mutable_pose().skinning_matrices(*skeleton, animator->mutable_palette());

    instances.clear();
    render::extract_skinned_meshes(scene, instances);
    REQUIRE(instances.size() == 1);
    REQUIRE((instances[0].palette != nullptr));
    CHECK(instances[0].palette->size() == skeleton->joint_count());
    // The vertex was at (0,2,0) in the bind pose and the pose put it at (0,1,1),
    // so the extracted world box is that point rather than the bind-pose one.
    CHECK(instances[0].world_max.z == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(instances[0].world_min.y == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("extraction skips a skinned renderer with no mesh") {
    Scene scene;
    Node* node_ptr = scene.create_node("empty");
    node_ptr->add_component<SkinnedMeshRenderer3D>();

    std::vector<render::SkinnedInstance> instances;
    render::extract_skinned_meshes(scene, instances);
    CHECK(instances.empty());
}

TEST_CASE("a skinned instance whose AABB is outside the frustum is culled") {
    render::SkinnedInstance instance;
    instance.world_min = Vec3{100.0f, 100.0f, 100.0f};
    instance.world_max = Vec3{101.0f, 101.0f, 101.0f};
    std::vector<render::SkinnedInstance> instances{instance};

    const Mat4 view = look_at(Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 projection = perspective(radians(60.0f), 1.0f, 0.1f, 100.0f);
    const Frustum frustum = Frustum::from_view_projection(projection * view);

    CHECK(render::cull_skinned_meshes(instances, &frustum) == 1);
    CHECK(instances.empty());
    // A null frustum culls nothing, the explicit "draw everything" case.
    std::vector<render::SkinnedInstance> kept{instance};
    CHECK(render::cull_skinned_meshes(kept, nullptr) == 0);
    CHECK(kept.size() == 1);
}

// ---------------------------------------------------------------------------
// Reflection
// ---------------------------------------------------------------------------

TEST_CASE("the skeletal animation components are registered for reflection") {
    reflect::register_skeletal_animation_types();

    const reflect::TypeDesc* animator = reflect::Registry::instance().find(Animator::kTypeName);
    REQUIRE((animator != nullptr));
    CHECK(animator->properties.size() >= 8);

    const reflect::TypeDesc* renderer = reflect::Registry::instance().find(SkinnedMeshRenderer3D::kTypeName);
    REQUIRE((renderer != nullptr));
    CHECK_FALSE(renderer->properties.empty());
}

TEST_CASE("a reflected animator round-trips its authored state") {
    reflect::register_skeletal_animation_types();

    const reflect::TypeDesc* desc = reflect::Registry::instance().find(Animator::kTypeName);
    REQUIRE((desc != nullptr));

    Animator animator;
    animator.speed = 2.5f;
    animator.loop = LoopMode::PingPong;
    animator.playing = false;
    animator.apply_root_motion = true;

    const reflect::Property* speed = find_property(*desc, "speed");
    REQUIRE((speed != nullptr));
    CHECK(std::get<f64>(speed->getter(&animator)) == doctest::Approx(2.5f));

    // The setter writes through to the component.
    REQUIRE(speed->setter(&animator, f64{3.5}));
    CHECK(animator.speed == doctest::Approx(3.5f));

    // An out-of-range loop value is clamped, not written through.
    const reflect::Property* loop = find_property(*desc, "loop");
    REQUIRE((loop != nullptr));
    REQUIRE(loop->setter(&animator, i64{99}));
    CHECK(animator.loop == LoopMode::PingPong);
}
