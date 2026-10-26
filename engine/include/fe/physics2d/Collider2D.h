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
// The shape is placed relative to the node by `offset`, a local displacement in
// the node's frame. It is rotated by the node's Z rotation before it is added to
// the node's world position, so an offset stays attached to the shape as the
// node turns -- an offset of (1, 0) on a node rotated 90 degrees places the
// shape one unit along the node's local +x, which is world +y. A zero offset
// centres the shape on the node, which is the default and the old behaviour.
//
// A box collider is oriented: it is placed as an `Obb2D` carrying the node's Z
// rotation, so a rotated node collides as its true rotated shape rather than as
// the axis-aligned box that would bound it. A circle is rotation-invariant in
// shape, so only its offset matters.
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

    // The shape's displacement from the node's origin, in the node's local
    // frame. Rotated by the node's Z rotation before it is applied, so it tracks
    // the shape as the node turns. Default zero: centred on the node.
    Vec2 offset{};

    // Bounciness in [0, 1]. 0 is a dead stop, 1 is a perfectly elastic bounce.
    // Clamped by the solver rather than here, so a hand-edited document cannot
    // inject energy.
    f32 restitution = 0.0f;

    // Coulomb friction coefficient, >= 0. Ignored in this cut beyond slowing
    // sliding along a contact, because there is no rotational dynamics yet.
    f32 friction = 0.5f;

    // --- Collision filtering ------------------------------------------------
    //
    // A pair of colliders is considered only when both agree to collide:
    //
    //   (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0
    //
    // `layer` is the single bit this collider belongs to; `mask` is the set of
    // layers it will collide with. The test is symmetric, so either side can
    // opt out without the other knowing -- a projectile that ignores its owner
    // only has to drop the owner's layer from its mask.
    //
    // The defaults reproduce the pre-filtering behaviour exactly: bit 0 is the
    // default layer and the mask is all-ones, so an unedited collider collides
    // with everything.
    u32 layer = 1u;

    // The set of layers this collider collides with. All-ones by default.
    u32 mask = 0xFFFFFFFFu;

    // When true the collider is a sensor: it reports contacts (the contact-event
    // stream is unchanged) but applies no impulse and no positional correction,
    // so a body passes through it. A pickup zone, a checkpoint, a trigger
    // volume.
    bool is_trigger = false;

    // Whether this collider and `other` may collide, by the symmetric layer/mask
    // rule above. Kept here rather than in the solver so the rule has one
    // definition that both physics modules and a future query API share.
    [[nodiscard]] bool can_collide_with(const Collider2D& other) const noexcept {
        return (layer & other.mask) != 0u && (other.layer & mask) != 0u;
    }

    // The shape's world centre when the node is at `center` with Z rotation
    // `rotation`: the local offset rotated into world space and added to the
    // node's centre. With a zero offset this is just `center`.
    [[nodiscard]] Vec2 world_center(const Vec2& center, f32 rotation) const noexcept {
        // A zero offset is the overwhelmingly common case and needs no rotation:
        // the shape sits exactly on the node. This keeps the common case free of
        // trigonometry.
        if (offset.x == 0.0f && offset.y == 0.0f) {
            return center;
        }
        const f32 c = std::cos(rotation);
        const f32 s = std::sin(rotation);
        return Vec2{center.x + offset.x * c - offset.y * s, center.y + offset.x * s + offset.y * c};
    }

    // The box shape at the node's world placement, oriented by `rotation`. This
    // is what the narrow phase tests, so a rotated node collides as its true
    // rotated box.
    [[nodiscard]] Obb2D oriented_box(const Vec2& center, f32 rotation) const noexcept {
        return Obb2D{world_center(center, rotation), half_extent, rotation};
    }

    // The circle shape at the node's world placement. A circle's shape does not
    // depend on rotation, but its offset does, so the placement still takes one.
    [[nodiscard]] Circle2D oriented_circle(const Vec2& center, f32 rotation) const noexcept {
        return Circle2D{world_center(center, rotation), radius};
    }

    // The world AABB of the shape at the node's placement, for the broad phase.
    // For a box this is the conservative bound of the oriented box, so the grid
    // stays a superset of the truly-touching pairs (ADR-0023).
    [[nodiscard]] Aabb2D bounds(const Vec2& center, f32 rotation) const noexcept {
        // A zero rotation and a zero offset leave the shape axis-aligned on the
        // node, which is the common case and the exact `Aabb2D` form -- no
        // oriented box is built. This runs once per collider per step and again
        // per pair in the island graph, so the shortcut matters.
        if (rotation == 0.0f && offset.x == 0.0f && offset.y == 0.0f) {
            return shape == ShapeType::Box ? box(center) : bounds_of(circle(center));
        }
        return shape == ShapeType::Box ? bounds_of(oriented_box(center, rotation))
                                       : bounds_of(oriented_circle(center, rotation));
    }

    // --- Axis-aligned, offset-free forms -----------------------------------
    //
    // The shape at an explicit centre, ignoring the local offset and the node's
    // rotation. Kept for the pure axis-aligned helpers and the legacy
    // broad-phase brute-force test, which reason about a shape at a bare centre.

    // The shape as a box, at `center`.
    [[nodiscard]] Aabb2D box(const Vec2& center) const noexcept { return Aabb2D{center, half_extent}; }

    // The shape as a circle, at `center`.
    [[nodiscard]] Circle2D circle(const Vec2& center) const noexcept { return Circle2D{center, radius}; }

    // The bounding box of the shape at `center`, ignoring offset and rotation.
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
