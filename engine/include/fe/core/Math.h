// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <cmath>

// A small, dependency-free math library for the engine's own types.
//
// Scope: the vector and matrix operations the runtime and the 2D renderer need
// today. It is intentionally not a general-purpose linear algebra library; if
// the 3D renderer later needs a richer set (dual quaternions, SIMD batches), we
// will evaluate a proven library (e.g. GLM) rather than growing this header
// without bound.
//
// Conventions:
//   * Column vectors, right-handed coordinate system, matrices are
//     column-major to match OpenGL's memory layout exactly.
//   * Mat4 element (row r, column c) lives at index c * 4 + r.
//   * Angles are radians.

namespace fe {

inline constexpr f32 kPi = 3.14159265358979323846f;

[[nodiscard]] inline constexpr f32 radians(f32 degrees) noexcept {
    return degrees * (kPi / 180.0f);
}
[[nodiscard]] inline constexpr f32 degrees(f32 radians_value) noexcept {
    return radians_value * (180.0f / kPi);
}

// ---------------------------------------------------------------------------
// Vec2
// ---------------------------------------------------------------------------
struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(f32 x_value, f32 y_value) : x(x_value), y(y_value) {}
    explicit constexpr Vec2(f32 scalar) : x(scalar), y(scalar) {}

    [[nodiscard]] constexpr Vec2 operator+(const Vec2& o) const noexcept { return {x + o.x, y + o.y}; }
    [[nodiscard]] constexpr Vec2 operator-(const Vec2& o) const noexcept { return {x - o.x, y - o.y}; }
    [[nodiscard]] constexpr Vec2 operator*(f32 s) const noexcept { return {x * s, y * s}; }
    [[nodiscard]] constexpr Vec2 operator/(f32 s) const noexcept { return {x / s, y / s}; }
    constexpr Vec2& operator+=(const Vec2& o) noexcept {
        x += o.x;
        y += o.y;
        return *this;
    }
    constexpr Vec2& operator-=(const Vec2& o) noexcept {
        x -= o.x;
        y -= o.y;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Vec2&) const noexcept = default;

    [[nodiscard]] constexpr f32 dot(const Vec2& o) const noexcept { return x * o.x + y * o.y; }
    [[nodiscard]] constexpr f32 length_squared() const noexcept { return dot(*this); }
    [[nodiscard]] f32 length() const noexcept { return std::sqrt(length_squared()); }
    [[nodiscard]] Vec2 normalized() const noexcept {
        const f32 len = length();
        return len > 0.0f ? *this / len : Vec2{};
    }
};

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------
struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(f32 x_value, f32 y_value, f32 z_value) : x(x_value), y(y_value), z(z_value) {}
    explicit constexpr Vec3(f32 scalar) : x(scalar), y(scalar), z(scalar) {}

    [[nodiscard]] constexpr Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    [[nodiscard]] constexpr Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    [[nodiscard]] constexpr Vec3 operator*(f32 s) const noexcept { return {x * s, y * s, z * s}; }
    [[nodiscard]] constexpr Vec3 operator/(f32 s) const noexcept { return {x / s, y / s, z / s}; }
    constexpr Vec3& operator+=(const Vec3& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr Vec3& operator-=(const Vec3& o) noexcept {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Vec3&) const noexcept = default;

    [[nodiscard]] constexpr f32 dot(const Vec3& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] constexpr f32 length_squared() const noexcept { return dot(*this); }
    [[nodiscard]] f32 length() const noexcept { return std::sqrt(length_squared()); }
    [[nodiscard]] Vec3 normalized() const noexcept {
        const f32 len = length();
        return len > 0.0f ? *this / len : Vec3{};
    }
};

// ---------------------------------------------------------------------------
// Vec4
// ---------------------------------------------------------------------------
struct Vec4 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(f32 x_value, f32 y_value, f32 z_value, f32 w_value)
        : x(x_value), y(y_value), z(z_value), w(w_value) {}
    constexpr Vec4(const Vec3& xyz, f32 w_value) : x(xyz.x), y(xyz.y), z(xyz.z), w(w_value) {}

    [[nodiscard]] constexpr Vec4 operator+(const Vec4& o) const noexcept {
        return {x + o.x, y + o.y, z + o.z, w + o.w};
    }
    [[nodiscard]] constexpr Vec4 operator-(const Vec4& o) const noexcept {
        return {x - o.x, y - o.y, z - o.z, w - o.w};
    }
    [[nodiscard]] constexpr Vec4 operator*(f32 s) const noexcept { return {x * s, y * s, z * s, w * s}; }

    [[nodiscard]] constexpr bool operator==(const Vec4&) const noexcept = default;

    // Convenience for colour values. Components are interpreted as linear
    // values in [0, 1]; this is not colour-space management.
    [[nodiscard]] static constexpr Vec4 rgba(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept { return {r, g, b, a}; }
};

// ---------------------------------------------------------------------------
// Mat4 (column-major)
// ---------------------------------------------------------------------------
struct Mat4;

// Declared here so member functions defined inside Mat4 can use it; defined
// after the type is complete.
[[nodiscard]] Mat4 operator*(const Mat4& a, const Mat4& b) noexcept;

struct Mat4 {
    // Column-major storage: element(row r, column c) is at m[c * 4 + r].
    f32 m[16]{};

    constexpr Mat4() = default;

    [[nodiscard]] static constexpr Mat4 identity() noexcept {
        Mat4 r;
        r.m[0] = 1.0f;
        r.m[5] = 1.0f;
        r.m[10] = 1.0f;
        r.m[15] = 1.0f;
        return r;
    }

    [[nodiscard]] static constexpr Mat4 translation(const Vec3& t) noexcept {
        Mat4 r = identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    [[nodiscard]] static constexpr Mat4 scale(const Vec3& s) noexcept {
        Mat4 r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        r.m[15] = 1.0f;
        return r;
    }

    [[nodiscard]] static Mat4 rotation_x(f32 angle_radians) noexcept {
        const f32 c = std::cos(angle_radians);
        const f32 s = std::sin(angle_radians);
        Mat4 r = identity();
        r.m[5] = c;
        r.m[6] = s;
        r.m[9] = -s;
        r.m[10] = c;
        return r;
    }

    [[nodiscard]] static Mat4 rotation_y(f32 angle_radians) noexcept {
        const f32 c = std::cos(angle_radians);
        const f32 s = std::sin(angle_radians);
        Mat4 r = identity();
        r.m[0] = c;
        r.m[2] = -s;
        r.m[8] = s;
        r.m[10] = c;
        return r;
    }

    [[nodiscard]] static Mat4 rotation_z(f32 angle_radians) noexcept {
        const f32 c = std::cos(angle_radians);
        const f32 s = std::sin(angle_radians);
        Mat4 r = identity();
        r.m[0] = c;
        r.m[1] = s;
        r.m[4] = -s;
        r.m[5] = c;
        return r;
    }

    // Euler rotation in radians, applied as Rz * Ry * Rx (X first). For purely
    // 2D content only the Z component is non-zero, which reduces to a plain
    // rotation about the view axis.
    [[nodiscard]] static Mat4 rotation_euler(const Vec3& euler_radians) noexcept {
        return rotation_z(euler_radians.z) * rotation_y(euler_radians.y) * rotation_x(euler_radians.x);
    }

    // Right-handed orthographic projection mapping the given box to clip space.
    [[nodiscard]] static constexpr Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane,
                                                     f32 far_plane) noexcept {
        Mat4 r = identity();
        r.m[0] = 2.0f / (right - left);
        r.m[5] = 2.0f / (top - bottom);
        r.m[10] = -2.0f / (far_plane - near_plane);
        r.m[12] = -(right + left) / (right - left);
        r.m[13] = -(top + bottom) / (top - bottom);
        r.m[14] = -(far_plane + near_plane) / (far_plane - near_plane);
        return r;
    }

    [[nodiscard]] constexpr f32& at(usize row, usize col) noexcept { return m[col * 4 + row]; }
    [[nodiscard]] constexpr f32 at(usize row, usize col) const noexcept { return m[col * 4 + row]; }

    // The transpose. For a pure rotation this is the inverse, which is the
    // cheap way to invert a view matrix built from an orthonormal basis.
    [[nodiscard]] constexpr Mat4 transposed() const noexcept {
        Mat4 r;
        for (usize col = 0; col < 4; ++col) {
            for (usize row = 0; row < 4; ++row) {
                r.at(row, col) = at(col, row);
            }
        }
        return r;
    }

    // Exact component equality, not approximate. Two matrices built the same way
    // from the same inputs are bit-identical, so this is what a test uses to
    // assert that a parallel path reproduced the serial one exactly; an
    // approximate compare would hide a reordered or lost instance.
    [[nodiscard]] constexpr bool operator==(const Mat4&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Quat
// ---------------------------------------------------------------------------
//
// A unit quaternion representing a rotation. It exists because 3D angular
// dynamics integrates an orientation by an angular velocity, and doing that
// through the Euler-angle `Transform` would be the gimbal-lock problem: Euler
// rates do not compose the way an angular velocity does. `Transform` keeps its
// Euler API unchanged; the solver converts at the boundary
// (`Quat::from_euler` to read an authored rotation, `Quat::to_euler` to write
// one back).
//
// Storage is (x, y, z, w) with the scalar part last, the convention OpenGL and
// most graphics code use. Multiplication composes rotations: `(a * b)`
// applies `b` first, then `a`, matching how `Mat4` products compose.
//
// The engine is right-handed, so a positive angle about an axis is
// counter-clockwise when viewed from the positive end of that axis.
struct Quat {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;

    constexpr Quat() = default;
    constexpr Quat(f32 x_value, f32 y_value, f32 z_value, f32 w_value)
        : x(x_value), y(y_value), z(z_value), w(w_value) {}

    // The identity rotation.
    [[nodiscard]] static constexpr Quat identity() noexcept { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

    // A rotation of `angle_radians` about a unit `axis`.
    [[nodiscard]] static Quat from_axis_angle(const Vec3& axis, f32 angle_radians) noexcept {
        const f32 half = angle_radians * 0.5f;
        const f32 s = std::sin(half);
        const Vec3 unit = axis.normalized();
        return Quat{unit.x * s, unit.y * s, unit.z * s, std::cos(half)};
    }

    // The rotation described by a `Transform` Euler triple, using the same
    // Rz * Ry * Rx (X first) composition as `Mat4::rotation_euler`, so a
    // round-trip through `from_euler` / `to_euler` leaves an authored rotation
    // unchanged.
    [[nodiscard]] static Quat from_euler(const Vec3& euler_radians) noexcept {
        const Quat qx = from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, euler_radians.x);
        const Quat qy = from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, euler_radians.y);
        const Quat qz = from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, euler_radians.z);
        return qz * qy * qx;
    }

    // The equivalent Euler triple in the same Rz * Ry * Rx convention, so the
    // result can be stored back into `Transform::rotation`. This is the inverse
    // of `from_euler` for angles within the representable range (the y rotation
    // is recovered in [-pi/2, pi/2], the usual Euler extraction limit).
    [[nodiscard]] Vec3 to_euler() const noexcept {
        const Quat q = normalized();
        const f32 sin_x = 2.0f * (q.w * q.x + q.y * q.z);
        const f32 cos_x = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        const f32 sin_y = 2.0f * (q.w * q.y - q.z * q.x);
        // Clamp the asin argument: rounding can push it a hair outside [-1, 1].
        const f32 clamped_y = sin_y > 1.0f ? 1.0f : (sin_y < -1.0f ? -1.0f : sin_y);
        const f32 sin_z = 2.0f * (q.w * q.z + q.x * q.y);
        const f32 cos_z = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        return Vec3{std::atan2(sin_x, cos_x), std::asin(clamped_y), std::atan2(sin_z, cos_z)};
    }

    [[nodiscard]] constexpr Quat operator*(const Quat& o) const noexcept {
        return Quat{w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x,
                    w * o.z + x * o.y - y * o.x + z * o.w, w * o.w - x * o.x - y * o.y - z * o.z};
    }

    [[nodiscard]] constexpr Quat operator*(f32 scalar) const noexcept {
        return Quat{x * scalar, y * scalar, z * scalar, w * scalar};
    }

    [[nodiscard]] constexpr bool operator==(const Quat&) const noexcept = default;

    [[nodiscard]] constexpr f32 dot(const Quat& o) const noexcept { return x * o.x + y * o.y + z * o.z + w * o.w; }

    [[nodiscard]] constexpr f32 length_squared() const noexcept { return dot(*this); }

    [[nodiscard]] f32 length() const noexcept { return std::sqrt(length_squared()); }

    // The conjugate (the inverse for a unit quaternion). A degenerate input
    // returns the identity rather than a NaN result.
    [[nodiscard]] constexpr Quat conjugate() const noexcept { return Quat{-x, -y, -z, w}; }

    [[nodiscard]] Quat normalized() const noexcept {
        const f32 len = length();
        return len > 0.0f ? Quat{x / len, y / len, z / len, w / len} : Quat::identity();
    }

    // The rotation matrix, matching `Mat4::rotation_euler` for the same
    // orientation. Column-major, so the upper-left 3x3 is a pure rotation with
    // no translation or scale.
    [[nodiscard]] Mat4 to_matrix() const noexcept {
        const Quat q = normalized();
        const f32 xx = q.x * q.x;
        const f32 yy = q.y * q.y;
        const f32 zz = q.z * q.z;
        const f32 xy = q.x * q.y;
        const f32 xz = q.x * q.z;
        const f32 yz = q.y * q.z;
        const f32 wx = q.w * q.x;
        const f32 wy = q.w * q.y;
        const f32 wz = q.w * q.z;

        Mat4 r = Mat4::identity();
        r.m[0] = 1.0f - 2.0f * (yy + zz);
        r.m[1] = 2.0f * (xy + wz);
        r.m[2] = 2.0f * (xz - wy);
        r.m[4] = 2.0f * (xy - wz);
        r.m[5] = 1.0f - 2.0f * (xx + zz);
        r.m[6] = 2.0f * (yz + wx);
        r.m[8] = 2.0f * (xz + wy);
        r.m[9] = 2.0f * (yz - wx);
        r.m[10] = 1.0f - 2.0f * (xx + yy);
        return r;
    }

    // The orientation after spinning by `angular_velocity` (radians per second,
    // direction is the rotation axis) for `delta_time` seconds. The update is
    // q' = q + (1/2) * w_quat * q, with w_quat the pure quaternion (w, 0), then
    // renormalised. This is the standard first-order integration; it is stable
    // at the step sizes a game runs at and is what lets a body turn without
    // Euler angles.
    [[nodiscard]] Quat integrated(const Vec3& angular_velocity, f32 delta_time) const noexcept {
        const Quat omega{angular_velocity.x, angular_velocity.y, angular_velocity.z, 0.0f};
        const Quat derivative = (omega * *this) * (0.5f * delta_time);
        return Quat{x + derivative.x, y + derivative.y, z + derivative.z, w + derivative.w}.normalized();
    }

    // --- Interpolation ------------------------------------------------------
    //
    // Both forms exist because they trade differently and a keyframe track has
    // to choose: `slerp` is a constant-angular-velocity arc and is the correct
    // interpolation for a rotation, while `nlerp` is a straight line through
    // the 4D hypersphere's chord, renormalised. The chord is cheaper (a lerp and
    // a normalise, no trigonometry) but does not sweep at a constant rate. A
    // clip declares which one it wants per track; the default is `slerp`,
    // because a rotation that visibly speeds up and slows down between two keys
    // is the artifact an animator notices.
    //
    // Both take the shorter of the two arcs: if the endpoints are more than 180
    // degrees apart, `b` is negated first. `q` and `-q` are the same rotation,
    // so interpolating the "long way" is always a bug, and it is the one a
    // hand-authored keyframe pair trips over.
    //
    // A zero-length input returns a copy rather than a NaN, so a degenerate
    // track (both keys equal) holds instead of poisoning the pose.

    // Normalised linear interpolation between `a` and `b`, along the shorter arc.
    [[nodiscard]] static Quat nlerp(const Quat& a, const Quat& b, f32 t) noexcept {
        Quat end = b;
        if (a.dot(b) < 0.0f) {
            end = b * -1.0f;
        }
        const Quat blended{a.x + (end.x - a.x) * t, a.y + (end.y - a.y) * t, a.z + (end.z - a.z) * t,
                           a.w + (end.w - a.w) * t};
        return blended.normalized();
    }

    // Spherical linear interpolation between `a` and `b`, along the shorter arc.
    // Falls back to `nlerp` when the endpoints are nearly parallel, where the
    // sine in the denominator underflows and the arc is indistinguishable from a
    // chord anyway.
    [[nodiscard]] static Quat slerp(const Quat& a, const Quat& b, f32 t) noexcept {
        Quat end = b;
        f32 cos_theta = a.dot(b);
        if (cos_theta < 0.0f) {
            // Negate so the interpolation takes the short way round.
            end = b * -1.0f;
            cos_theta = -cos_theta;
        }
        // Above this the arc is under a degree or so; the chord and the arc
        // agree to within float precision and the acos/sin pair loses accuracy.
        constexpr f32 kParallelThreshold = 0.9995f;
        if (cos_theta > kParallelThreshold) {
            return nlerp(a, end, t);
        }
        const f32 theta = std::acos(cos_theta);
        const f32 sin_theta = std::sin(theta);
        const f32 from_weight = std::sin((1.0f - t) * theta) / sin_theta;
        const f32 to_weight = std::sin(t * theta) / sin_theta;
        return Quat{a.x * from_weight + end.x * to_weight, a.y * from_weight + end.y * to_weight,
                    a.z * from_weight + end.z * to_weight, a.w * from_weight + end.w * to_weight}
            .normalized();
    }
};

// Right-handed perspective projection with depth in [-1, 1] (the OpenGL
// convention). `fov_y_radians` is the vertical field of view; `aspect` is
// width / height. Maps the camera's -Z direction into the view volume.
[[nodiscard]] inline Mat4 perspective(f32 fov_y_radians, f32 aspect, f32 near_plane, f32 far_plane) noexcept {
    const f32 f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 r;
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (far_plane + near_plane) / (near_plane - far_plane);
    r.at(2, 3) = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
    r.at(3, 2) = -1.0f;
    return r;
}

// Right-handed view matrix looking from `eye` towards `target`, with `up`
// selecting the roll. The camera looks down its own -Z axis, matching
// perspective() above.
[[nodiscard]] inline Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept {
    const Vec3 forward = (target - eye).normalized();
    const Vec3 right = forward.cross(up).normalized();
    const Vec3 true_up = right.cross(forward);

    Mat4 r = Mat4::identity();
    r.at(0, 0) = right.x;
    r.at(1, 0) = right.y;
    r.at(2, 0) = right.z;
    r.at(0, 1) = true_up.x;
    r.at(1, 1) = true_up.y;
    r.at(2, 1) = true_up.z;
    r.at(0, 2) = -forward.x;
    r.at(1, 2) = -forward.y;
    r.at(2, 2) = -forward.z;
    r.at(0, 3) = -right.dot(eye);
    r.at(1, 3) = -true_up.dot(eye);
    r.at(2, 3) = forward.dot(eye);
    return r;
}

// The general 4x4 inverse, by cofactor expansion. Returns identity for a
// singular matrix rather than producing NaNs, so a degenerate transform fails
// visibly instead of poisoning every downstream vertex.
[[nodiscard]] inline Mat4 inverse(const Mat4& mat) noexcept {
    const f32* a = mat.m;
    f32 inv[16];

    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] -
              a[8] * a[2] * a[5];

    f32 det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0.0f) {
        return Mat4::identity();
    }
    det = 1.0f / det;

    Mat4 r;
    for (usize i = 0; i < 16; ++i) {
        r.m[i] = inv[i] * det;
    }
    return r;
}

// Matrix product `a * b`. Applying (a * b) to a vector is equivalent to
// applying b first, then a.
[[nodiscard]] inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r;
    for (usize col = 0; col < 4; ++col) {
        for (usize row = 0; row < 4; ++row) {
            f32 sum = 0.0f;
            for (usize k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            r.at(row, col) = sum;
        }
    }
    return r;
}

// Transforms a point (w = 1), including translation.
[[nodiscard]] inline Vec3 transform_point(const Mat4& mat, const Vec3& p) noexcept {
    return {mat.at(0, 0) * p.x + mat.at(0, 1) * p.y + mat.at(0, 2) * p.z + mat.at(0, 3),
            mat.at(1, 0) * p.x + mat.at(1, 1) * p.y + mat.at(1, 2) * p.z + mat.at(1, 3),
            mat.at(2, 0) * p.x + mat.at(2, 1) * p.y + mat.at(2, 2) * p.z + mat.at(2, 3)};
}

// Transforms a direction (w = 0), ignoring translation.
[[nodiscard]] inline Vec3 transform_direction(const Mat4& mat, const Vec3& v) noexcept {
    return {mat.at(0, 0) * v.x + mat.at(0, 1) * v.y + mat.at(0, 2) * v.z,
            mat.at(1, 0) * v.x + mat.at(1, 1) * v.y + mat.at(1, 2) * v.z,
            mat.at(2, 0) * v.x + mat.at(2, 1) * v.y + mat.at(2, 2) * v.z};
}

// Composes a 2D transform as translation * rotation * scale.
[[nodiscard]] inline Mat4 make_transform_2d(const Vec2& position, f32 rotation_radians,
                                            const Vec2& scale_value) noexcept {
    return Mat4::translation(Vec3{position.x, position.y, 0.0f}) * Mat4::rotation_z(rotation_radians) *
           Mat4::scale(Vec3{scale_value.x, scale_value.y, 1.0f});
}

} // namespace fe
