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

} // namespace fe::physics2d
