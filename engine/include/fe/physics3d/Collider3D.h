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
// transformed by the node's rotation to stay correct, and while the solver now
// integrates a body's orientation (ADR-0043) the collider shape itself is still
// axis-aligned, so an offset would be misleading. Offsets and oriented shapes are
// later work.
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
    [[nodiscard]] bool can_collide_with(const Collider3D& other) const noexcept {
        return (layer & other.mask) != 0u && (other.layer & mask) != 0u;
    }

    // The shape as a box, at the node's world position `center`.
    [[nodiscard]] Aabb3D box(const Vec3& center) const noexcept { return Aabb3D{center, half_extent}; }

    // The shape as a sphere, at the node's world position `center`.
    [[nodiscard]] Sphere3D sphere(const Vec3& center) const noexcept { return Sphere3D{center, radius}; }

    // The bounding box of the shape at `center`, for the broad phase.
    [[nodiscard]] Aabb3D bounds(const Vec3& center) const noexcept {
        return shape == ShapeType3D::Box ? box(center) : bounds_of(sphere(center));
    }

    // The moment of inertia about the shape's centre for a body of `mass`. This
    // is what lets a rigid body derive a sensible rotational inertia from its
    // shape without the author computing one, mirroring
    // `Collider2D::inertia_about_center`:
    //
    //   * solid sphere: (2/5) m r^2
    //   * solid box:    (1/18) m (w^2 + h^2 + d^2), the mean of the box's three
    //     principal moments, so a cube reports the exact (1/6) m w^2 about any
    //     axis and a non-cube reports the average of its three axes
    //
    // The solver carries a single scalar moment, an isotropic approximation of
    // the inertia tensor, exactly as the 2D solver does with its one in-plane
    // axis. A body with non-uniform extents therefore spins the same about every
    // axis; a full diagonal inertia tensor is later work. A non-positive mass
    // yields zero, matching an immovable body; the solver treats zero as
    // "infinite inertia".
    [[nodiscard]] f32 inertia_about_center(f32 mass) const noexcept {
        if (mass <= 0.0f) {
            return 0.0f;
        }
        if (shape == ShapeType3D::Sphere) {
            return 0.4f * mass * radius * radius;
        }
        const f32 width = 2.0f * half_extent.x;
        const f32 height = 2.0f * half_extent.y;
        const f32 depth = 2.0f * half_extent.z;
        return mass * (width * width + height * height + depth * depth) / 18.0f;
    }
};

} // namespace fe::physics3d
