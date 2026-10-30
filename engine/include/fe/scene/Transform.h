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

    // The inverse of `local_matrix()`: recovers a translation, an Euler rotation,
    // and a scale from an affine matrix.
    //
    // The translation is the fourth column. The scale is the length of each of
    // the first three columns, and the rotation is those columns with the scale
    // divided out, converted to a quaternion and then to the Euler triple this
    // struct stores. This is the standard TRS decomposition and is exact for a
    // matrix that was built as translation * rotation * scale, which is every
    // matrix the engine produces and every glTF node written in TRS form.
    //
    // A matrix that is not of that shape -- one carrying a shear, which glTF
    // permits as a raw `matrix` -- decomposes to the nearest rotation and scale,
    // losing the shear. That is a deliberate limit: `Transform` has no shear
    // field, so a caller that must preserve one keeps the matrix rather than
    // round-tripping through here. A zero-length column (a degenerate scale)
    // falls back to a unit axis rather than dividing by zero.
    [[nodiscard]] static Transform from_matrix(const Mat4& matrix) noexcept {
        Transform transform;
        transform.position = Vec3{matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3)};

        Vec3 columns[3];
        for (usize c = 0; c < 3; ++c) {
            columns[c] = Vec3{matrix.at(0, c), matrix.at(1, c), matrix.at(2, c)};
        }
        Vec3 scale_value{columns[0].length(), columns[1].length(), columns[2].length()};
        for (usize c = 0; c < 3; ++c) {
            const f32 axis = c == 0 ? scale_value.x : (c == 1 ? scale_value.y : scale_value.z);
            columns[c] = axis > 0.0f ? columns[c] / axis : Vec3{};
        }

        // A negative determinant means the basis is mirrored; fold the flip into
        // one axis so the recovered rotation stays a proper rotation.
        const f32 determinant = columns[0].dot(columns[1].cross(columns[2]));
        if (determinant < 0.0f) {
            scale_value.x = -scale_value.x;
            columns[0] = columns[0] * -1.0f;
        }

        // The rotation matrix's rows and columns, read back into a quaternion by
        // the largest-diagonal method (numerically stable near 180 degrees).
        const f32 trace = columns[0].x + columns[1].y + columns[2].z;
        Quat rotation;
        if (trace > 0.0f) {
            const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
            rotation = Quat{(columns[1].z - columns[2].y) / s, (columns[2].x - columns[0].z) / s,
                            (columns[0].y - columns[1].x) / s, 0.25f * s};
        } else if (columns[0].x > columns[1].y && columns[0].x > columns[2].z) {
            const f32 s = std::sqrt(1.0f + columns[0].x - columns[1].y - columns[2].z) * 2.0f;
            rotation = Quat{0.25f * s, (columns[1].x + columns[0].y) / s, (columns[2].x + columns[0].z) / s,
                            (columns[1].z - columns[2].y) / s};
        } else if (columns[1].y > columns[2].z) {
            const f32 s = std::sqrt(1.0f + columns[1].y - columns[0].x - columns[2].z) * 2.0f;
            rotation = Quat{(columns[1].x + columns[0].y) / s, 0.25f * s, (columns[2].y + columns[1].z) / s,
                            (columns[2].x - columns[0].z) / s};
        } else {
            const f32 s = std::sqrt(1.0f + columns[2].z - columns[0].x - columns[1].y) * 2.0f;
            rotation = Quat{(columns[2].x + columns[0].z) / s, (columns[2].y + columns[1].z) / s, 0.25f * s,
                            (columns[0].y - columns[1].x) / s};
        }

        transform.rotation = rotation.normalized().to_euler();
        transform.scale = scale_value;
        return transform;
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
