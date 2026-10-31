// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

#include <cmath>

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

    // --- Sleeping ----------------------------------------------------------
    //
    // A body that has been nearly still for a sustained number of steps is put
    // to sleep: the solver skips it, so a settled pile stops costing solver
    // time. Sleeping is grouped by island (see ADR-0026): a body sleeps only
    // when every dynamic body it is connected to by a contact or a joint is also
    // ready, and waking any one of them wakes the whole group.
    //
    // The state is observable and controllable on purpose. A game can ask
    // `is_sleeping` and call `wake()`; the engine never hides the decision.

    // When false, this body never sleeps, and -- because a body that never
    // sleeps keeps its island awake -- neither does anything resting on it. Set
    // it on a body that must keep integrating regardless, such as a
    // platform driven by code.
    bool can_sleep = true;

    // True while the body is asleep. While asleep its velocity is treated as
    // zero and the solver skips it entirely; a sleeping body is woken by a new
    // contact or impulse from a moving body, by a jointed body moving, or by an
    // explicit call to `wake()`.
    bool is_sleeping = false;

    // How long the body's motion must stay below the thresholds before it may
    // sleep, in seconds. Counted while it is awake and reset whenever it moves
    // faster than the thresholds or is woken. The default is short enough that a
    // settled pile sleeps quickly, long enough that a body mid-bounce does not.
    f32 sleep_time = 0.5f;

    // Linear speed below which the body counts as at rest, in world units per
    // second. Compared against the velocity the solver leaves after a step, so a
    // body resting on a floor (whose velocity is driven to zero every frame)
    // settles below it.
    f32 sleep_linear_threshold = 0.05f;

    // Angular speed below which the body counts as at rest, in radians per
    // second.
    f32 sleep_angular_threshold = 0.05f;

    // Time the body's motion has been below the thresholds, in seconds. Read-only
    // in practice: the solver owns it. Exposed so a test or a debug overlay can
    // watch a body approach sleep.
    f32 sleep_timer = 0.0f;

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

    // True when the body's motion is slow enough to count as at rest: both the
    // linear speed and the angular speed are below their thresholds. A sleeping
    // body is at rest by definition. This is the raw test; the sustained-time
    // requirement lives in the system, which advances `sleep_timer`.
    [[nodiscard]] bool is_resting() const noexcept {
        if (is_sleeping) {
            return true;
        }
        return velocity.length_squared() <= sleep_linear_threshold * sleep_linear_threshold &&
               std::abs(angular_velocity) <= sleep_angular_threshold;
    }

    // Puts the body to sleep immediately: it stops being integrated or solved
    // until something wakes it. Clears its velocity, so it comes back to rest
    // rather than resuming a stale speed, and zeroes the rest timer. Waking is
    // per body here; the system wakes a whole island when one of its bodies is
    // disturbed.
    void sleep() noexcept {
        is_sleeping = true;
        velocity = Vec2{};
        angular_velocity = 0.0f;
        sleep_timer = 0.0f;
    }

    // Wakes the body: it is integrated and solved again, and its rest timer
    // restarts. A body that was already awake is unchanged apart from the timer.
    // Does nothing about the island; the system propagates a wake across a
    // group when it detects one, so a game calling this on one body of a jointed
    // pair will see the pair move on the next step.
    void wake() noexcept {
        is_sleeping = false;
        sleep_timer = 0.0f;
    }
};

} // namespace fe::physics2d
