// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene3d/Camera3D.h"

#include "fe/scene/Node.h"

#include <algorithm>

namespace fe {

Vec3 Camera3D::eye() const noexcept {
    const Node* owner = node();
    if (owner == nullptr) {
        return Vec3{};
    }
    return transform_point(owner->world_matrix(), Vec3{});
}

Vec3 Camera3D::forward() const noexcept {
    const Node* owner = node();
    if (owner == nullptr) {
        return Vec3{0.0f, 0.0f, -1.0f};
    }
    // The camera looks down its local -Z; rotating that by the world matrix
    // gives the world-space forward direction.
    return transform_direction(owner->world_matrix(), Vec3{0.0f, 0.0f, -1.0f}).normalized();
}

Vec3 Camera3D::up() const noexcept {
    const Node* owner = node();
    if (owner == nullptr) {
        return Vec3{0.0f, 1.0f, 0.0f};
    }
    // The camera's local up is +Y; rotating it by the world matrix gives the
    // world-space up, roll included.
    return transform_direction(owner->world_matrix(), Vec3{0.0f, 1.0f, 0.0f}).normalized();
}

Mat4 Camera3D::view_projection(u32 viewport_width, u32 viewport_height) const noexcept {
    const f32 width = static_cast<f32>(viewport_width);
    const f32 height = static_cast<f32>(viewport_height);
    const f32 aspect = height > 0.0f ? width / height : 1.0f;

    const Vec3 eye_position = eye();
    const Mat4 view = look_at(eye_position, eye_position + forward(), Vec3{0.0f, 1.0f, 0.0f});
    // Guard against a zero or inverted near/far pair, which would produce a
    // degenerate projection rather than an obvious failure.
    const f32 near_plane_safe = std::max(near_plane, 1e-4f);
    const f32 far_plane_safe = std::max(far_plane, near_plane_safe + 1e-3f);
    const Mat4 projection = perspective(radians(fov_degrees), aspect, near_plane_safe, far_plane_safe);
    return projection * view;
}

} // namespace fe
