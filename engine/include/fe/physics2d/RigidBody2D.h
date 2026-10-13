// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe::physics2d {

// Linear and angular motion and mass for a node, driven by the physics system.
//
// A node needs both a RigidBody2D and a Collider2D to be simulated: the rigid
// body says how it moves, the collider says what it collides with. Splitting
// them is what lets the same body logic drive different shapes, and what lets a
// static level piece carry a collider with no body at all.
//
// Rotation is about the Z axis (the 2D plane's normal). The solver integrates
// `angular_velocity` and applies angular impulses from contacts, using the same
// sequential-impulse approach as the linear solver. A body's collider stays an
// AABB: rotating the node's transform turns what is drawn, but the collider is
// not re-oriented yet, which is the honest limit of this cut (ADR-0009).
class RigidBody2D final : public Component {
public:
    static constexpr const char* kTypeName = "RigidBody2D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Linear velocity in world units per second.
    Vec2 velocity{};

    // Angular velocity in radians per second, about the Z axis.
    f32 angular_velocity = 0.0f;

    // A constant torque in newton-metres about the Z axis, applied every step.
    // It is the rotational counterpart of gravity: an angular acceleration of
    // `torque * inverse_inertia` is added to the angular velocity each frame. A
    // body with infinite inertia (static, kinematic, or fixed_rotation) ignores
    // it. A general force/torque accumulator that a script can add to during the
    // frame is later work; this is a single authored value, like gravity.
    f32 torque = 0.0f;

    // Mass in kilograms. Must be > 0 for a movable body; a zero or negative
    // mass is treated as infinite (immovable) by the solver, which is how a
    // "static" body is expressed without a separate flag.
    f32 mass = 1.0f;

    // Moment of inertia about the Z axis, in kg*m^2. A non-positive value (the
    // default) means "derive it from the collider's shape and mass", so a box
    // or circle gets a sensible rotational inertia without the author having to
    // compute one. A body with infinite mass has infinite inertia regardless.
    f32 inertia = 0.0f;

    // Fraction of velocity shed per second, in [0, 1]. Applied as a per-frame
    // factor so the decay is frame-rate independent.
    f32 linear_damping = 0.0f;

    // Fraction of angular velocity shed per second, in [0, 1]. The rotational
    // counterpart of linear_damping.
    f32 angular_damping = 0.0f;

    // When false the body keeps its collider (so others collide with it) but is
    // never moved. Set automatically for a body with non-positive mass, and
    // useful to freeze a body without removing its components.
    bool kinematic = false;

    // When false, gravity is not applied to this body. A body may still be
    // pushed by collisions, which is how a kinematic platform moves.
    bool affected_by_gravity = true;

    // When true, the solver never changes the body's rotation: it is locked to
    // whatever the transform holds. Collisions still move it linearly, and it
    // still carries its transform rotation into contacts (as zero effective
    // angular influence), which is what a body that must stay upright wants.
    bool fixed_rotation = false;

    [[nodiscard]] f32 inverse_mass() const noexcept {
        if (kinematic || mass <= 0.0f) {
            return 0.0f;
        }
        return 1.0f / mass;
    }

    // The inverse moment of inertia the solver uses. Zero for a kinematic body,
    // a body with non-positive mass, or one with fixed_rotation, so an angular
    // impulse applied to it has no effect rather than producing a divide by
    // zero. Otherwise the reciprocal of `inertia` if the author set one.
    [[nodiscard]] f32 inverse_inertia() const noexcept {
        if (kinematic || mass <= 0.0f || fixed_rotation) {
            return 0.0f;
        }
        if (inertia > 0.0f) {
            return 1.0f / inertia;
        }
        return 0.0f; // caller fills in the shape-derived inertia before solving
    }

    [[nodiscard]] bool is_dynamic() const noexcept { return inverse_mass() > 0.0f; }
};

} // namespace fe::physics2d
