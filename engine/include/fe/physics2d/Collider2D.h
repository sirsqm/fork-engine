// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/physics2d/Geometry.h"
#include "fe/scene/Component.h"

namespace fe::physics2d {

// A collision shape attached to a node, with its surface material.
//
// A node that has a Collider2D but no RigidBody2D is a static obstacle: it
// participates in collisions but never moves. That is the common case for level
// geometry (a floor, a wall) and it needs no second component to express.
//
// The shape is centred on the node's world position. There is deliberately no
// local offset yet: an offset would have to be transformed by the node's
// rotation to stay correct, and rotational dynamics are not in this first cut
// (see ADR-0009). Keeping the collider centred is honest about that limit
// rather than shipping an offset that silently breaks under rotation.
class Collider2D final : public Component {
public:
    static constexpr const char* kTypeName = "Collider2D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Which shape `half_extent` or `radius` describes.
    ShapeType shape = ShapeType::Box;

    // Half-width and half-height for a Box shape. Ignored for a Circle.
    Vec2 half_extent{0.5f, 0.5f};

    // Radius for a Circle shape. Ignored for a Box.
    f32 radius = 0.5f;

    // Bounciness in [0, 1]. 0 is a dead stop, 1 is a perfectly elastic bounce.
    // Clamped by the solver rather than here, so a hand-edited document cannot
    // inject energy.
    f32 restitution = 0.0f;

    // Coulomb friction coefficient, >= 0. Ignored in this cut beyond slowing
    // sliding along a contact, because there is no rotational dynamics yet.
    f32 friction = 0.5f;

    // The shape as a box, at the node's world position `center`.
    [[nodiscard]] Aabb2D box(const Vec2& center) const noexcept { return Aabb2D{center, half_extent}; }

    // The shape as a circle, at the node's world position `center`.
    [[nodiscard]] Circle2D circle(const Vec2& center) const noexcept { return Circle2D{center, radius}; }

    // The bounding box of the shape at `center`, for the broad phase.
    [[nodiscard]] Aabb2D bounds(const Vec2& center) const noexcept {
        return shape == ShapeType::Box ? box(center) : bounds_of(circle(center));
    }

    // The moment of inertia about the shape's centre for a body of `mass`. This
    // is what lets a rigid body derive a sensible rotational inertia from its
    // shape without the author computing one:
    //
    //   * solid box:    (1/12) m (w^2 + h^2), with w, h the full side lengths
    //   * solid disc:   (1/2) m r^2
    //
    // A non-positive mass yields zero, matching an immovable body. The value is
    // always >= 0; the solver treats zero as "infinite inertia".
    [[nodiscard]] f32 inertia_about_center(f32 mass) const noexcept {
        if (mass <= 0.0f) {
            return 0.0f;
        }
        if (shape == ShapeType::Circle) {
            return 0.5f * mass * radius * radius;
        }
        const f32 width = 2.0f * half_extent.x;
        const f32 height = 2.0f * half_extent.y;
        return mass * (width * width + height * height) / 12.0f;
    }
};

} // namespace fe::physics2d
