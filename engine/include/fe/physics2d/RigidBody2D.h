// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe::physics2d {

// Linear motion and mass for a node, driven by the physics system.
//
// A node needs both a RigidBody2D and a Collider2D to be simulated: the rigid
// body says how it moves, the collider says what it collides with. Splitting
// them is what lets the same body logic drive different shapes, and what lets a
// static level piece carry a collider with no body at all.
//
// This is a dynamic body in the simple sense: it has linear velocity, it is
// pushed by gravity, and it is moved by impulses. Angular velocity and torque
// are not modelled yet (ADR-0009), so `inv_inertia` does not exist and rotation
// is left to the transform, not the solver.
class RigidBody2D final : public Component {
public:
    static constexpr const char* kTypeName = "RigidBody2D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Linear velocity in world units per second.
    Vec2 velocity{};

    // Mass in kilograms. Must be > 0 for a movable body; a zero or negative
    // mass is treated as infinite (immovable) by the solver, which is how a
    // "static" body is expressed without a separate flag.
    f32 mass = 1.0f;

    // Fraction of velocity shed per second, in [0, 1]. Applied as a per-frame
    // factor so the decay is frame-rate independent.
    f32 linear_damping = 0.0f;

    // When false the body keeps its collider (so others collide with it) but is
    // never moved. Set automatically for a body with non-positive mass, and
    // useful to freeze a body without removing its components.
    bool kinematic = false;

    // When false, gravity is not applied to this body. A body may still be
    // pushed by collisions, which is how a kinematic platform moves.
    bool affected_by_gravity = true;

    [[nodiscard]] f32 inverse_mass() const noexcept {
        if (kinematic || mass <= 0.0f) {
            return 0.0f;
        }
        return 1.0f / mass;
    }

    [[nodiscard]] bool is_dynamic() const noexcept { return inverse_mass() > 0.0f; }
};

} // namespace fe::physics2d
