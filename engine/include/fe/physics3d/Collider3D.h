// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/physics3d/Geometry3D.h"
#include "fe/scene/Component.h"

namespace fe::physics3d {

// A 3D collision shape attached to a node, with its surface material.
//
// A node that has a Collider3D but no RigidBody3D is a static obstacle: it
// participates in collisions but never moves. That is the common case for level
// geometry (a floor, a wall) and it needs no second component to express. The
// split between shape and motion is the composition model doing the work, exactly
// as in 2D: "static floor" and "falling crate" are the same two components in
// different configurations, not two classes.
//
// The shape is centred on the node's world position. There is deliberately no
// local offset yet, for the same reason as 2D: an offset would have to be
// transformed by the node's rotation to stay correct, and the solver does not
// track an orientation yet (ADR-0031).
class Collider3D final : public Component {
public:
    static constexpr const char* kTypeName = "Collider3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Which shape `half_extent` or `radius` describes.
    ShapeType3D shape = ShapeType3D::Box;

    // Half-width, half-height, and half-depth for a Box shape. Ignored for a
    // Sphere.
    Vec3 half_extent{0.5f, 0.5f, 0.5f};

    // Radius for a Sphere shape. Ignored for a Box.
    f32 radius = 0.5f;

    // Bounciness in [0, 1]. 0 is a dead stop, 1 is a perfectly elastic bounce.
    // Clamped by the solver rather than here, so a hand-edited document cannot
    // inject energy.
    f32 restitution = 0.0f;

    // Coulomb friction coefficient, >= 0. In this linear-only cut it slows
    // sliding along a contact; there is no angular dynamics for it to spin a body
    // yet.
    f32 friction = 0.5f;

    // The shape as a box, at the node's world position `center`.
    [[nodiscard]] Aabb3D box(const Vec3& center) const noexcept { return Aabb3D{center, half_extent}; }

    // The shape as a sphere, at the node's world position `center`.
    [[nodiscard]] Sphere3D sphere(const Vec3& center) const noexcept { return Sphere3D{center, radius}; }

    // The bounding box of the shape at `center`, for the broad phase.
    [[nodiscard]] Aabb3D bounds(const Vec3& center) const noexcept {
        return shape == ShapeType3D::Box ? box(center) : bounds_of(sphere(center));
    }
};

} // namespace fe::physics3d
