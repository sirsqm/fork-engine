// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

namespace fe::physics3d {

// A 3D collision shape: an axis-aligned box or a sphere.
//
// A collider is one of these, plus the material data that lives on the
// Collider3D component. As in 2D, the shape is a closed enum rather than a base
// class: there are exactly two and the solver wants to switch on them in a tight
// loop, not dispatch through a virtual.
enum class ShapeType3D : u8 {
    Box,
    Sphere,
};

// An axis-aligned box, stored as a half-extent around its centre.
//
// Half-extents (rather than min/max corners) are what the intersection maths
// wants, and the centre is the node's world position, so a box collider needs no
// separate offset to begin with. A box is axis-aligned: a rotated node is drawn
// rotated but collided as its unrotated box, the same limit the 2D collider has
// (ADR-0009), because there is no quaternion orientation in the solver yet.
struct Aabb3D {
    Vec3 center{};
    Vec3 half_extent{};

    [[nodiscard]] f32 min_x() const noexcept { return center.x - half_extent.x; }
    [[nodiscard]] f32 max_x() const noexcept { return center.x + half_extent.x; }
    [[nodiscard]] f32 min_y() const noexcept { return center.y - half_extent.y; }
    [[nodiscard]] f32 max_y() const noexcept { return center.y + half_extent.y; }
    [[nodiscard]] f32 min_z() const noexcept { return center.z - half_extent.z; }
    [[nodiscard]] f32 max_z() const noexcept { return center.z + half_extent.z; }
};

// A sphere, stored as a centre and a radius.
struct Sphere3D {
    Vec3 center{};
    f32 radius = 0.0f;
};

// The result of a narrow-phase test between two shapes.
//
// `overlapping` is the primary answer; `normal` and `penetration` are only
// meaningful when it is true. `normal` points from shape A towards shape B (the
// direction that separates them), and `penetration` is the depth along it. That
// convention is what the impulse solver needs to push the bodies apart, and it is
// stated here so callers do not have to guess which way the normal faces.
struct Contact3D {
    bool overlapping = false;
    Vec3 normal{};
    f32 penetration = 0.0f;
};

// --- Narrow-phase tests ----------------------------------------------------
//
// These are pure geometry: no scene, no components, no allocation. They are the
// part of physics worth testing exhaustively, so they are kept free of anything
// that would make that harder.

[[nodiscard]] Contact3D intersect(const Aabb3D& a, const Aabb3D& b) noexcept;
[[nodiscard]] Contact3D intersect(const Sphere3D& a, const Sphere3D& b) noexcept;
// `a` is the sphere, `b` is the box. The returned normal still points from the
// sphere towards the box, matching the convention above.
[[nodiscard]] Contact3D intersect(const Sphere3D& a, const Aabb3D& b) noexcept;

// The broad-phase prefilter: a cheap conservative overlap test used to skip the
// narrow phase for pairs that cannot touch. A false result is exact; a true
// result means "maybe".
[[nodiscard]] bool bounds_overlap(const Aabb3D& a, const Aabb3D& b) noexcept;
[[nodiscard]] bool bounds_overlap(const Aabb3D& box, const Sphere3D& sphere) noexcept;

// The axis-aligned bounding box that contains a shape. Used to place a sphere
// into the same broad-phase path as a box.
[[nodiscard]] Aabb3D bounds_of(const Sphere3D& sphere) noexcept;
[[nodiscard]] Aabb3D bounds_of(const Aabb3D& box) noexcept;

// Dispatches to the right overload for a pair of shape types.
[[nodiscard]] Contact3D intersect_shapes(ShapeType3D a_type, const Aabb3D& a_box, const Sphere3D& a_sphere,
                                         ShapeType3D b_type, const Aabb3D& b_box, const Sphere3D& b_sphere) noexcept;

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
[[nodiscard]] f32 raycast_aabb(const Vec3& origin, const Vec3& direction, const Aabb3D& box) noexcept;

// Distance along `direction` from `origin` to `sphere`, or -1 if the ray misses.
// A ray that starts inside the sphere hits at distance 0.
[[nodiscard]] f32 raycast_sphere(const Vec3& origin, const Vec3& direction, const Sphere3D& sphere) noexcept;

// Dispatches to the right ray test for a shape type.
[[nodiscard]] f32 raycast_shape(const Vec3& origin, const Vec3& direction, ShapeType3D type, const Aabb3D& box,
                                const Sphere3D& sphere) noexcept;

// The surface normal at the hit point, pointing back towards the ray origin.
// `direction` must be the unit direction used for the cast and `distance` a hit
// distance it returned. The AABB form picks the axis of the face that was
// crossed; the sphere form is the radial direction. A distance <= 0 yields a
// normal opposite the ray, which is well defined for a ray starting inside.
[[nodiscard]] Vec3 raycast_aabb_normal(const Vec3& origin, const Vec3& direction, const Aabb3D& box,
                                       f32 distance) noexcept;
[[nodiscard]] Vec3 raycast_sphere_normal(const Vec3& origin, const Vec3& direction, const Sphere3D& sphere,
                                         f32 distance) noexcept;
[[nodiscard]] Vec3 raycast_shape_normal(const Vec3& origin, const Vec3& direction, ShapeType3D type, const Aabb3D& box,
                                        const Sphere3D& sphere, f32 distance) noexcept;

// True when `point` is inside the shape.
[[nodiscard]] bool contains(const Aabb3D& box, const Vec3& point) noexcept;
[[nodiscard]] bool contains(const Sphere3D& sphere, const Vec3& point) noexcept;
[[nodiscard]] bool contains_shape(ShapeType3D type, const Aabb3D& box, const Sphere3D& sphere,
                                  const Vec3& point) noexcept;

} // namespace fe::physics3d
