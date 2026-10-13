// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"

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

} // namespace fe
