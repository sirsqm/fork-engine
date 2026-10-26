// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"

#include <string_view>

namespace fe {

// A local-to-parent spatial transform.
//
// The transform is a value type. A Node stores one; hierarchy composition
// produces world matrices on demand. Rotation is Euler angles in radians for
// this milestone, which is enough for 2D (Z only) and for simple 3D scenes; a
// quaternion representation can replace it later without changing the
// composition API.
struct Transform {
    Vec3 position{};
    Vec3 rotation{}; // Euler angles, radians.
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] static Transform identity() noexcept { return Transform{}; }

    // Builds the local matrix as translation * rotation * scale.
    [[nodiscard]] Mat4 local_matrix() const noexcept {
        return Mat4::translation(position) * Mat4::rotation_euler(rotation) * Mat4::scale(scale);
    }

    // Convenience constructors for 2D authoring and tests.
    [[nodiscard]] static Transform from_position_2d(const Vec2& position_2d) noexcept {
        Transform t;
        t.position = Vec3{position_2d.x, position_2d.y, 0.0f};
        return t;
    }

    [[nodiscard]] static Transform from_2d(const Vec2& position_2d, f32 rotation_radians,
                                           const Vec2& scale_2d = Vec2{1.0f, 1.0f}) noexcept {
        Transform t;
        t.position = Vec3{position_2d.x, position_2d.y, 0.0f};
        t.rotation = Vec3{0.0f, 0.0f, rotation_radians};
        t.scale = Vec3{scale_2d.x, scale_2d.y, 1.0f};
        return t;
    }
};

// One field of a transform, addressed by name. A transform has no reflection
// entry (it is not a component), so an edit that has to travel as a serializable
// path names its field as a string: "position". These two functions are the
// single definition of that mapping, shared by the operation applier and the
// inspector, so the wire form and the widget agree.
//
// The whole field is addressed, not one axis, so a drag that changes two axes in
// a frame is still one operation and therefore one undo step.
//
// Returns nullptr for an unknown field, which is how a malformed or hand-written
// operation is rejected rather than silently ignored.
[[nodiscard]] inline Vec3* transform_field(Transform& transform, std::string_view field) noexcept {
    if (field == "position") {
        return &transform.position;
    }
    if (field == "rotation") {
        return &transform.rotation;
    }
    if (field == "scale") {
        return &transform.scale;
    }
    return nullptr;
}

[[nodiscard]] inline const Vec3* transform_field(const Transform& transform, std::string_view field) noexcept {
    if (field == "position") {
        return &transform.position;
    }
    if (field == "rotation") {
        return &transform.rotation;
    }
    if (field == "scale") {
        return &transform.scale;
    }
    return nullptr;
}

} // namespace fe
