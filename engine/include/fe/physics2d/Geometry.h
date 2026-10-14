// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <cmath>

namespace fe::physics2d {

// A 2D shape: an axis-aligned box or a circle.
//
// A collider is one of these, plus the material data that lives on the
// Collider2D component. The shape itself is a closed enum rather than a base
// class because there are exactly two and the solver wants to switch on them in
// a tight loop, not dispatch through a virtual.
enum class ShapeType : u8 {
    Box,
    Circle,
};

// An axis-aligned box, stored as a half-extent around its centre.
//
// Half-extents (rather than min/max corners) are what the intersection maths
// wants. This is the axis-aligned form, used for the broad phase and for the
// ray/containment helpers; the narrow phase works on `Obb2D` below, which
// generalises it.
struct Aabb2D {
    Vec2 center{};
    Vec2 half_extent{};

    [[nodiscard]] f32 min_x() const noexcept { return center.x - half_extent.x; }
    [[nodiscard]] f32 max_x() const noexcept { return center.x + half_extent.x; }
    [[nodiscard]] f32 min_y() const noexcept { return center.y - half_extent.y; }
    [[nodiscard]] f32 max_y() const noexcept { return center.y + half_extent.y; }
};

// A circle, stored as a centre and a radius.
struct Circle2D {
    Vec2 center{};
    f32 radius = 0.0f;
};

// An oriented box: a centre, a half-extent, and an orientation about the Z axis.
//
// This is the general form of the box. An axis-aligned box is the special case
// `rotation == 0`, so `Aabb2D` is kept for the broad phase and the pure
// axis-aligned tests, and the oriented tests below take an `Obb2D`. A box
// collider is placed as an `Obb2D` (see Collider2D), which is what lets a
// rotated node collide as its true shape rather than its bounding box.
//
// `rotation` is an angle in radians about the Z axis, matching
// `Transform::rotation.z` -- the same rotation the renderer uses, so a rotated
// body is drawn and collided with the same orientation. There is deliberately no
// quaternion or matrix here: 2D rotation is one scalar and a new representation
// would be a second source of truth.
struct Obb2D {
    Vec2 center{};
    Vec2 half_extent{};
    f32 rotation = 0.0f;

    // The box's local axes in world space: `axis_x` is the local +x direction
    // rotated by `rotation`, `axis_y` the local +y. `half_extent.x` is measured
    // along `axis_x` and `half_extent.y` along `axis_y`. The axes are unit
    // length, which is what the projection in `intersect` relies on.
    [[nodiscard]] Vec2 axis_x() const noexcept {
        // The axis-aligned case is the common one, so it skips the trigonometry
        // entirely; the axes are exactly the world axes.
        if (rotation == 0.0f) {
            return Vec2{1.0f, 0.0f};
        }
        const f32 c = std::cos(rotation);
        const f32 s = std::sin(rotation);
        return Vec2{c, s};
    }
    [[nodiscard]] Vec2 axis_y() const noexcept {
        if (rotation == 0.0f) {
            return Vec2{0.0f, 1.0f};
        }
        const f32 c = std::cos(rotation);
        const f32 s = std::sin(rotation);
        return Vec2{-s, c};
    }

    // The four corners in world space, counter-clockwise from the local
    // (-x, -y) corner. Used for the bounds and, in tests, to reason about the
    // true shape.
    [[nodiscard]] Vec2 corner(usize index) const noexcept {
        const Vec2 ax = axis_x() * half_extent.x;
        const Vec2 ay = axis_y() * half_extent.y;
        const Vec2 signs[4] = {Vec2{-1.0f, -1.0f}, Vec2{1.0f, -1.0f}, Vec2{1.0f, 1.0f}, Vec2{-1.0f, 1.0f}};
        return center + ax * signs[index].x + ay * signs[index].y;
    }
};

// The result of a narrow-phase test between two shapes.
//
// `overlapping` is the primary answer; `normal` and `penetration` are only
// meaningful when it is true. `normal` points from shape A towards shape B (the
// direction that separates them), and `penetration` is the depth along it. That
// convention is what the impulse solver needs to push the bodies apart, and it
// is stated here so callers do not have to guess which way the normal faces.
struct Contact2D {
    bool overlapping = false;
    Vec2 normal{};
    f32 penetration = 0.0f;
};

// --- Narrow-phase tests ----------------------------------------------------
//
// These are pure geometry: no scene, no components, no allocation. They are the
// part of physics worth testing exhaustively, so they are kept free of anything
// that would make that harder.

[[nodiscard]] Contact2D intersect(const Aabb2D& a, const Aabb2D& b) noexcept;
[[nodiscard]] Contact2D intersect(const Circle2D& a, const Circle2D& b) noexcept;
// `a` is the circle, `b` is the box. The returned normal still points from the
// circle towards the box, matching the convention above.
[[nodiscard]] Contact2D intersect(const Circle2D& a, const Aabb2D& b) noexcept;

// Oriented variants. Same contract and the same normal convention ("from A
// towards B"); an `Obb2D` with `rotation == 0` is exactly the matching `Aabb2D`
// test, so these generalise rather than replace the axis-aligned ones.
[[nodiscard]] Contact2D intersect(const Obb2D& a, const Obb2D& b) noexcept;
[[nodiscard]] Contact2D intersect(const Circle2D& a, const Obb2D& b) noexcept;
[[nodiscard]] Contact2D intersect(const Obb2D& a, const Circle2D& b) noexcept;

// The broad-phase prefilter: a cheap conservative overlap test used to skip the
// narrow phase for pairs that cannot touch. A false result is exact; a true
// result means "maybe".
[[nodiscard]] bool bounds_overlap(const Aabb2D& a, const Aabb2D& b) noexcept;
[[nodiscard]] bool bounds_overlap(const Aabb2D& box, const Circle2D& circle) noexcept;

// The axis-aligned bounding box that contains a shape. Used to place a circle
// into the same broad-phase path as a box. For an `Obb2D` this is the
// conservative bound of the oriented box -- the smallest axis-aligned box that
// contains all four rotated corners -- which is what the broad phase inserts.
[[nodiscard]] Aabb2D bounds_of(const Circle2D& circle) noexcept;
[[nodiscard]] Aabb2D bounds_of(const Aabb2D& box) noexcept;
[[nodiscard]] Aabb2D bounds_of(const Obb2D& box) noexcept;

// The distance from an oriented box's centre to its farthest corner measured
// along the unit `axis` -- the `r` term of the separating-axis test, and the
// support radius a contact point is taken at. Exact for a box, whose axes are
// orthonormal. With `rotation == 0` and an axis-aligned `axis` this is just the
// matching half-extent.
[[nodiscard]] f32 support_radius(const Obb2D& box, const Vec2& axis) noexcept;

// Dispatches to the right overload for a pair of shape types. Two box overloads:
// the `Obb2D` form tests a rotated box as its oriented shape, and the `Aabb2D`
// form (the pre-orientation signature) is the zero-rotation special case. The
// axis-aligned form is kept so callers that only have an axis-aligned box --
// the broad-phase brute-force test, the legacy helpers -- do not have to build
// an `Obb2D` by hand.
[[nodiscard]] Contact2D intersect_shapes(ShapeType a_type, const Aabb2D& a_box, const Circle2D& a_circle,
                                         ShapeType b_type, const Aabb2D& b_box, const Circle2D& b_circle) noexcept;
[[nodiscard]] Contact2D intersect_shapes(ShapeType a_type, const Obb2D& a_box, const Circle2D& a_circle,
                                         ShapeType b_type, const Obb2D& b_box, const Circle2D& b_circle) noexcept;

// --- Ray casts -------------------------------------------------------------
//
// A ray is an origin and a unit direction; the test reports the distance along
// the ray to the first intersection. The functions return a negative distance
// when there is no hit (the ray misses, or starts past the shape and points
// away), which is a total, allocation-free answer the query layer turns into an
// explicit miss.

// Distance along `direction` from `origin` to the nearest face of `box`, or -1
// if the ray misses. `direction` must be unit length. A ray that starts inside
// the box hits at distance 0.
[[nodiscard]] f32 raycast_aabb(const Vec2& origin, const Vec2& direction, const Aabb2D& box) noexcept;

// Distance along `direction` from `origin` to `circle`, or -1 if the ray
// misses. A ray that starts inside the circle hits at distance 0.
[[nodiscard]] f32 raycast_circle(const Vec2& origin, const Vec2& direction, const Circle2D& circle) noexcept;

// Dispatches to the right ray test for a shape type.
[[nodiscard]] f32 raycast_shape(const Vec2& origin, const Vec2& direction, ShapeType type, const Aabb2D& box,
                                const Circle2D& circle) noexcept;

// The surface normal at the hit point, pointing back towards the ray origin.
// `direction` must be the unit direction used for the cast and `distance` a hit
// distance it returned. The AABB form picks the axis of the face that was
// crossed; the circle form is the radial direction. A distance <= 0 yields a
// normal opposite the ray, which is well defined for a ray starting inside.
[[nodiscard]] Vec2 raycast_aabb_normal(const Vec2& origin, const Vec2& direction, const Aabb2D& box,
                                       f32 distance) noexcept;
[[nodiscard]] Vec2 raycast_circle_normal(const Vec2& origin, const Vec2& direction, const Circle2D& circle,
                                         f32 distance) noexcept;
[[nodiscard]] Vec2 raycast_shape_normal(const Vec2& origin, const Vec2& direction, ShapeType type, const Aabb2D& box,
                                        const Circle2D& circle, f32 distance) noexcept;

// True when `point` is inside the shape.
[[nodiscard]] bool contains(const Aabb2D& box, const Vec2& point) noexcept;
[[nodiscard]] bool contains(const Circle2D& circle, const Vec2& point) noexcept;
[[nodiscard]] bool contains_shape(ShapeType type, const Aabb2D& box, const Circle2D& circle,
                                  const Vec2& point) noexcept;

} // namespace fe::physics2d
