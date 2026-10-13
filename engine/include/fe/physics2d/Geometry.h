// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

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
// wants, and the centre is the node's world position, so a box collider needs
// no separate offset to begin with.
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

// The broad-phase prefilter: a cheap conservative overlap test used to skip the
// narrow phase for pairs that cannot touch. A false result is exact; a true
// result means "maybe".
[[nodiscard]] bool bounds_overlap(const Aabb2D& a, const Aabb2D& b) noexcept;
[[nodiscard]] bool bounds_overlap(const Aabb2D& box, const Circle2D& circle) noexcept;

// The axis-aligned bounding box that contains a shape. Used to place a circle
// into the same broad-phase path as a box.
[[nodiscard]] Aabb2D bounds_of(const Circle2D& circle) noexcept;
[[nodiscard]] Aabb2D bounds_of(const Aabb2D& box) noexcept;

// Dispatches to the right overload for a pair of shape types.
[[nodiscard]] Contact2D intersect_shapes(ShapeType a_type, const Aabb2D& a_box, const Circle2D& a_circle,
                                         ShapeType b_type, const Aabb2D& b_box, const Circle2D& b_circle) noexcept;

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
