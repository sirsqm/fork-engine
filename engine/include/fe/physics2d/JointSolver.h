// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"

namespace fe::physics2d {

// The pure half of joint solving: the per-body state a constraint row reads and
// writes, and the two constraint rows (distance and revolute) built from it.
//
// This header has no scene, no components, and no allocation, for the same
// reason Geometry.h has none: the maths is the part worth testing exhaustively,
// and it must be callable directly without a world. `Physics2DSystem` gathers
// the scene into these values, calls build_*, and applies the returned rows to
// its bodies; a test can call the same functions with hand-written inputs and
// assert the numbers.
//
// The rows are solved by sequential impulses with a velocity bias, the same
// shape as the contact solver (ADR-0009): the bias is the target relative
// velocity that drives the constraint error to zero over the next steps, so a
// joint and a contact can be solved by one loop without it knowing which is
// which.

// One end of a joint: the linear and angular state a constraint row reads and
// writes, plus the inverse mass and inertia that decide how the correction is
// shared. A static body carries zero inverses, so a joint to it moves only the
// other end, exactly as the contact solver treats a static collider.
struct JointBody {
    Vec2 position{};
    Vec2 velocity{};
    f32 rotation = 0.0f;
    f32 angular_velocity = 0.0f;
    f32 inv_mass = 0.0f;
    f32 inv_inertia = 0.0f;
};

// A scalar constraint row: a 1-DOF correction the solver applies by weighting
// each body by the row's Jacobian and the body's inverse mass and inertia.
//
// `jacobian_a`/`jacobian_b` are the row's linear Jacobians at each body centre,
// `angular_a`/`angular_b` the angular Jacobians, and `effective_mass` the
// inverse of the row's apparent mass (1 / (J M^-1 J^T)).
//
// The row carries two corrections, mirroring how the contact solver separates a
// velocity impulse from a positional one:
//   * `bias` is the target relative velocity along the row. A rigid rod leaves
//     it zero (the velocity impulse then simply cancels relative motion); a
//     spring sets it from the stretch so the joint pulls like a spring.
//   * `position_error` and `position_factor` drive a direct positional
//     correction: `position_factor` of the error is removed along the row each
//     pass. A rigid rod uses this; a spring sets `position_factor` to zero so
//     the soft bias is the only thing acting.
//
// The solver is given the row rather than the joint so that one loop can solve
// a contact impulse and a joint's rows without knowing which is which.
struct ConstraintRow {
    Vec2 jacobian_a{};
    Vec2 jacobian_b{};
    f32 angular_a = 0.0f;
    f32 angular_b = 0.0f;
    f32 effective_mass = 0.0f;
    f32 bias = 0.0f;
    f32 position_error = 0.0f;
    f32 position_factor = 0.0f;
};

// The scalar 2D cross product: the z component of the 3D cross of two vectors.
// Used to project a linear Jacobian onto an angular one through a lever arm.
[[nodiscard]] inline f32 cross(const Vec2& a, const Vec2& b) noexcept {
    return a.x * b.y - a.y * b.x;
}

// The velocity a body at offset `r` from its centre contributes from an angular
// velocity `w`: the rotation of `r` by 90 degrees scaled by `w`, matching the
// contact solver's convention.
[[nodiscard]] inline Vec2 cross(f32 scalar, const Vec2& vector) noexcept {
    return Vec2{-scalar * vector.y, scalar * vector.x};
}

// The apparent mass of a row: 1 / (J M^-1 J^T), summing the linear and angular
// terms of both bodies. A zero result means neither body can move along the row
// (both are static), which the callers report as an unsolvable constraint.
[[nodiscard]] inline f32 row_effective_mass(const ConstraintRow& row, const JointBody& a, const JointBody& b) noexcept {
    const f32 linear = a.inv_mass * row.jacobian_a.length_squared() + b.inv_mass * row.jacobian_b.length_squared();
    const f32 angular = a.inv_inertia * row.angular_a * row.angular_a + b.inv_inertia * row.angular_b * row.angular_b;
    const f32 denom = linear + angular;
    return denom > 0.0f ? 1.0f / denom : 0.0f;
}

// How many rows a joint needs: one for a distance joint, two for a revolute
// joint (the x and y components of the shared anchor).
inline constexpr usize kDistanceJointRows = 1;
inline constexpr usize kRevoluteJointRows = 2;

// The distance-joint parameters, decoupled from the component so the maths can
// be exercised on its own. The constraint measures between the two body
// centres. `stiffness` is a frequency in Hz (0 gives a rigid rod) and `damping`
// is a fraction in [0, 1] of the current relative velocity the joint opposes,
// which lets a spring lose energy instead of ringing forever.
struct DistanceJointParams {
    f32 rest_length = 0.0f;
    f32 stiffness = 0.0f;
    f32 damping = 0.0f;
};

// The revolute-joint parameters. `anchor_a` and `anchor_b` are the shared point
// expressed in each body's local frame, so a pin offset from a body's centre
// (the top of a pendulum bob) needs no separate world anchor to author.
struct RevoluteJointParams {
    Vec2 anchor_a{};
    Vec2 anchor_b{};
};

// Builds the single distance-constraint row for the current state of the two
// bodies, for a step of `delta_time` seconds. Returns an Error when the bodies
// are coincident (the constraint direction is undefined), when `delta_time` is
// non-positive, or when neither body can move, so the caller can skip an
// unsolvable joint rather than emit NaNs.
[[nodiscard]] Result<ConstraintRow> build_distance_row(const JointBody& a, const JointBody& b,
                                                       const DistanceJointParams& params, f32 delta_time);

// Builds the two revolute-constraint rows that pin the two bodies' local
// anchors together. Row 0 constrains the x separation, row 1 the y separation;
// together they remove both translational degrees of freedom while leaving
// relative rotation free. Errors for the same reasons as the distance row.
[[nodiscard]] Result<void> build_revolute_rows(const JointBody& a, const JointBody& b,
                                               const RevoluteJointParams& params, f32 delta_time,
                                               ConstraintRow out_rows[kRevoluteJointRows]);

// Applies a solved row's velocity impulse to the two bodies: the impulse is the
// current relative velocity along the row plus the row's bias, scaled by the
// row's effective mass. This is the one place the sign convention lives, so the
// solver and any test agree by construction.
void apply_row(const ConstraintRow& row, JointBody& a, JointBody& b) noexcept;

// Applies a solved row's positional correction: `position_factor` of the row's
// position error is removed along the row, split by inverse mass and inertia. A
// no-op when the row has no position term (a spring) or is unsolvable.
void apply_row_position(const ConstraintRow& row, JointBody& a, JointBody& b) noexcept;

// The world-space point a revolute joint currently pins, from body A's side.
// Useful for tests and for the editor's debug overlay.
[[nodiscard]] Vec2 revolute_world_anchor(const JointBody& a, const RevoluteJointParams& params) noexcept;

} // namespace fe::physics2d
