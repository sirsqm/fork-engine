// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/scene/Component.h"

namespace fe::physics3d {

// Linear and angular motion and mass for a node, driven by the physics system.
//
// A node needs both a RigidBody3D and a Collider3D to be simulated: the rigid
// body says how it moves, the collider says what it collides with. Splitting them
// is what lets the same body logic drive different shapes, and what lets a static
// level piece carry a collider with no body at all.
//
// Rotation is a full 3D orientation. The solver integrates `angular_velocity`
// into a quaternion and applies angular impulses from contacts and joints, using
// the same sequential-impulse approach as the linear solver (ADR-0043). The
// collider shape stays axis-aligned: a rotated body is drawn rotated, but it is
// still collided as its unrotated box or sphere, the same honest limit the 2D
// collider has (ADR-0009); oriented shapes are later work.
class RigidBody3D final : public Component {
public:
    static constexpr const char* kTypeName = "RigidBody3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // Linear velocity in world units per second.
    Vec3 velocity{};

    // Angular velocity in radians per second. Its direction is the instantaneous
    // rotation axis, its magnitude the rate. Integrated into the node's
    // orientation through a quaternion so a body turns without Euler-angle
    // gimbal lock.
    Vec3 angular_velocity{};

    // A constant torque in newton-metres, applied every step. It is the
    // rotational counterpart of gravity: an angular acceleration of
    // `torque * inverse_inertia` is added to the angular velocity each frame. A
    // body with infinite inertia (static, kinematic, or fixed_rotation) ignores
    // it. A general force/torque accumulator that a script can add to during the
    // frame is later work; this is a single authored value, like gravity.
    Vec3 torque{};

    // Mass in kilograms. Must be > 0 for a movable body; a zero or negative mass
    // is treated as infinite (immovable) by the solver, which is how a "static"
    // body is expressed without a separate flag.
    f32 mass = 1.0f;

    // Moment of inertia, in kg*m^2. A non-positive value (the default) means
    // "derive it from the collider's shape and mass", so a box or sphere gets a
    // sensible rotational inertia without the author computing one. A body with
    // infinite mass has infinite inertia regardless. This is a single scalar, an
    // isotropic approximation of the inertia tensor: a non-cube spins the same
    // about every axis. A full diagonal tensor is later work.
    f32 inertia = 0.0f;

    // Fraction of velocity shed per second, in [0, 1]. Applied as a per-frame
    // factor so the decay is frame-rate independent.
    f32 linear_damping = 0.0f;

    // Fraction of angular velocity shed per second, in [0, 1]. The rotational
    // counterpart of linear_damping.
    f32 angular_damping = 0.0f;

    // When false the body keeps its collider (so others collide with it) but is
    // never moved. Set automatically for a body with non-positive mass, and useful
    // to freeze a body without removing its components.
    bool kinematic = false;

    // When false, gravity is not applied to this body. A body may still be pushed
    // by collisions, which is how a kinematic platform moves.
    bool affected_by_gravity = true;

    // When true, the solver never changes the body's rotation: it is locked to
    // whatever the transform holds. Collisions still move it linearly, and it
    // still carries its transform rotation into contacts (as zero effective
    // angular influence), which is what a body that must stay upright wants.
    bool fixed_rotation = false;

    // --- Sleeping ----------------------------------------------------------
    // Sleeping is the optimisation that stops a settled pile costing solver time
    // every frame: a body at rest for long enough is skipped by integration and
    // by the solver until something wakes it. Sleeping is grouped by island (see
    // ADR-0026 and ADR-0111): a body sleeps only when every dynamic body in its
    // contact-and-joint island is ready, and waking any one of them wakes the
    // whole group. The state lives on the component, so a caller can read
    // `is_sleeping` and call `wake()`; the engine never hides the decision.
    //
    // When false, this body never sleeps, and -- because a body that never sleeps
    // keeps its island awake -- neither does anything resting on it. Set it on a
    // body that a gameplay system moves every frame, so the island does not
    // thrash between asleep and awake.
    bool can_sleep = true;

    // True while the body is asleep. While asleep its velocity is treated as zero
    // and the solver skips it entirely; a sleeping body is woken by a new
    // contact, by a joint to a moving body, or by an explicit call to `wake()`.
    bool is_sleeping = false;

    // How long a body must be at rest before it may sleep, in seconds. Counted
    // while it is awake and reset whenever it moves. Short enough that a settled
    // pile sleeps quickly, long enough that a body mid-bounce does not.
    f32 sleep_time = 0.5f;

    // Linear speed below which the body counts as at rest, in world units per
    // second, and the angular counterpart in radians per second. Both are
    // compared against the component's own velocity, which the solver writes
    // back before the sleep check, so the test is current. Sized so a body
    // resting on a floor (whose velocity is driven to zero every frame) reads as
    // at rest, while a body still drifting does not.
    f32 sleep_linear_threshold = 0.05f;
    f32 sleep_angular_threshold = 0.05f;

    // Seconds the body has been continuously at rest. Advanced by the system for
    // a body on an awake island; reset to zero when the body moves. Exposed so a
    // test (or an inspector) can watch a body approach sleep.
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
               angular_velocity.length_squared() <= sleep_angular_threshold * sleep_angular_threshold;
    }

    // Puts the body to sleep immediately: it stops being integrated or solved
    // until something wakes it. Clears its velocity, so it comes back to rest
    // rather than resuming a stale speed, and zeroes the rest timer. Waking is
    // per body here; the system wakes a whole island when one of its bodies is
    // woken.
    void sleep() noexcept {
        is_sleeping = true;
        velocity = Vec3{};
        angular_velocity = Vec3{};
        sleep_timer = 0.0f;
    }

    // Wakes the body: it is integrated and solved again, and its rest timer
    // restarts. A body that was already awake is unchanged apart from the timer.
    // Does nothing about the island; the system propagates a wake across a whole
    // island.
    void wake() noexcept {
        is_sleeping = false;
        sleep_timer = 0.0f;
    }
};

} // namespace fe::physics3d
