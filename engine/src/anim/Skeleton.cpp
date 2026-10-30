// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/Skeleton.h"

#include <algorithm>

namespace fe::anim {
namespace {

// The local matrix for a translation/rotation/scale triple, composed as
// translation * rotation * scale. Shared by the rest pose and a sampled pose so
// the two cannot drift apart.
[[nodiscard]] Mat4 compose(const Vec3& translation, const Quat& rotation, const Vec3& scale) noexcept {
    return Mat4::translation(translation) * rotation.to_matrix() * Mat4::scale(scale);
}

} // namespace

// ---------------------------------------------------------------------------
// JointPose
// ---------------------------------------------------------------------------

Mat4 JointPose::local_matrix() const noexcept {
    return compose(translation, rotation, scale);
}

JointPose JointPose::from_transform(const Transform& transform) noexcept {
    JointPose pose;
    pose.translation = transform.position;
    pose.rotation = Quat::from_euler(transform.rotation);
    pose.scale = transform.scale;
    return pose;
}

Transform JointPose::to_transform() const noexcept {
    Transform transform;
    transform.position = translation;
    transform.rotation = rotation.to_euler();
    transform.scale = scale;
    return transform;
}

// ---------------------------------------------------------------------------
// Skeleton
// ---------------------------------------------------------------------------

Skeleton::Skeleton(std::string path, std::vector<Joint> joints)
    : Resource(std::move(path)), joints_(std::move(joints)) {
    compute_order();
}

void Skeleton::compute_order() {
    order_.clear();
    order_.reserve(joints_.size());
    if (joints_.empty()) {
        return;
    }

    // A depth-first walk from every root, so a parent is appended before any
    // child. This is not a sort: it is the one order the pose walk needs, and
    // producing it here means nothing downstream has to check whether the file
    // happened to write its nodes topologically.
    //
    // A joint already visited is skipped, which is what makes a shared child
    // (illegal in glTF, but possible in a hand-built rig) contribute once rather
    // than looping. A cycle is caught the same way: the walk terminates because
    // no joint is entered twice, and `has_valid_hierarchy()` then reports the
    // shortfall against the joint count.
    std::vector<bool> visited(joints_.size(), false);
    std::vector<i32> stack;

    // Children are indexed once rather than scanned per joint: a rig of a
    // thousand joints would otherwise be quadratic to walk.
    std::vector<std::vector<i32>> children(joints_.size());
    for (usize i = 0; i < joints_.size(); ++i) {
        const i32 parent = joints_[i].parent;
        if (parent >= 0 && static_cast<usize>(parent) < joints_.size()) {
            children[static_cast<usize>(parent)].push_back(static_cast<i32>(i));
        }
    }

    for (usize i = 0; i < joints_.size(); ++i) {
        const i32 parent = joints_[i].parent;
        if (parent >= 0 && static_cast<usize>(parent) < joints_.size()) {
            continue; // Not a root.
        }
        stack.push_back(static_cast<i32>(i));
    }

    while (!stack.empty()) {
        const i32 index = stack.back();
        stack.pop_back();
        if (index < 0 || static_cast<usize>(index) >= joints_.size() || visited[static_cast<usize>(index)]) {
            continue;
        }
        visited[static_cast<usize>(index)] = true;
        order_.push_back(index);
        // Push children in reverse so the walk visits them in index order, which
        // makes `order()` a deterministic function of the input for a test.
        const std::vector<i32>& kids = children[static_cast<usize>(index)];
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            stack.push_back(*it);
        }
    }
}

usize Skeleton::root_count() const noexcept {
    usize roots = 0;
    for (const Joint& joint : joints_) {
        if (joint.parent < 0 || static_cast<usize>(joint.parent) >= joints_.size()) {
            ++roots;
        }
    }
    return roots;
}

i32 Skeleton::find_joint(std::string_view name) const noexcept {
    for (usize i = 0; i < joints_.size(); ++i) {
        if (joints_[i].name == name) {
            return static_cast<i32>(i);
        }
    }
    return kNoParent;
}

void Skeleton::compute_rest_world_matrices(std::vector<Mat4>& out) const {
    out.assign(joints_.size(), Mat4::identity());
    for (const i32 index : order_) {
        const usize i = static_cast<usize>(index);
        const Mat4 local = joints_[i].rest.local_matrix();
        const i32 parent = joints_[i].parent;
        if (parent >= 0 && static_cast<usize>(parent) < joints_.size()) {
            out[i] = out[static_cast<usize>(parent)] * local;
        } else {
            out[i] = local;
        }
    }
}

void Skeleton::compute_inverse_bind_matrices() {
    std::vector<Mat4> world;
    compute_rest_world_matrices(world);
    for (usize i = 0; i < joints_.size(); ++i) {
        joints_[i].inverse_bind = inverse(world[i]);
    }
}

usize Skeleton::memory_usage() const noexcept {
    usize total = sizeof(Skeleton);
    for (const Joint& joint : joints_) {
        total += sizeof(Joint) + joint.name.size();
    }
    total += order_.size() * sizeof(i32);
    return total;
}

// ---------------------------------------------------------------------------
// SkeletonPose
// ---------------------------------------------------------------------------

void SkeletonPose::resize(usize count) {
    joints_.resize(count);
}

void SkeletonPose::reset_to_rest(const Skeleton& skeleton) {
    joints_.resize(skeleton.joint_count());
    for (usize i = 0; i < joints_.size(); ++i) {
        joints_[i] = JointPose::from_transform(skeleton.joint(i).rest);
    }
}

void SkeletonPose::world_matrices(const Skeleton& skeleton, std::vector<Mat4>& out) const {
    out.assign(skeleton.joint_count(), Mat4::identity());
    for (const i32 index : skeleton.order()) {
        const usize i = static_cast<usize>(index);
        if (i >= joints_.size()) {
            // A clip or pose authored against a different rig: leave the joint at
            // the identity rather than reading past the end.
            continue;
        }
        const Mat4 local = joints_[i].local_matrix();
        const i32 parent = skeleton.joint(i).parent;
        if (parent >= 0 && static_cast<usize>(parent) < out.size()) {
            out[i] = out[static_cast<usize>(parent)] * local;
        } else {
            out[i] = local;
        }
    }
}

void SkeletonPose::skinning_matrices(const Skeleton& skeleton, std::vector<Mat4>& out) const {
    std::vector<Mat4> world;
    world_matrices(skeleton, world);
    out.resize(skeleton.joint_count());
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = world[i] * skeleton.joint(i).inverse_bind;
    }
}

} // namespace fe::anim
