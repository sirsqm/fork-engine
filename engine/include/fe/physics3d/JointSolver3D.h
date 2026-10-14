// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"

namespace fe::physics3d {

// The pure half of joint solving for 3D: the per-body state a constraint row
// reads and writes, and the distance-constraint row built from it.
//
// This header has no scene, no components, and no allocation, for the same
// reason Geometry3D.h has none: the maths is the part worth testing
// exhaustively, and it must be callable directly without a world.
// `Physics3DSystem` gathers the scene into these values, calls build_*, and
// applies the returned row to its bodies; a test can call the same functions
// with hand-written inputs and assert the numbers.
//
// The row is solved by sequential impulses with a velocity bias, the same shape
// as the contact solver (ADR-0009): the bias is the target relative velocity that
// drives the constraint error to zero over the next steps, so a joint and a
// contact can be solved by one loop without it knowing which is which.
//
// The row carries angular terms (ADR-0043), so a joint can spin its ends when the
// constraint acts off-centre. A distance joint measures between the two centres,
// so its angular Jacobians stay zero and it remains linear in practice; the terms
// exist so the row type is complete and a future anchor-based joint (a revolute
// or point-to-point constraint) needs no new machinery, exactly as the 2D row
// carries them for its revolute joint (ADR-0021).

// One end of a joint: the linear and angular state a constraint row reads and
// writes, plus the inverse mass and inertia that decide how the correction is
// shared. A static body carries zero inverses, so a joint to it moves only the
// other end, exactly as the contact solver treats a static collider.
struct JointBody3D {
    Vec3 position{};
    Vec3 velocity{};
    Quat orientation{};
    Vec3 angular_velocity{};
    f32 inv_mass = 0.0f;
    f32 inv_inertia = 0.0f;
};

// A scalar constraint row: a 1-DOF correction the solver applies by weighting
// each body by the row's Jacobian and the body's inverse mass.
//
// `jacobian_a`/`jacobian_b` are the row's linear Jacobians at each body centre,
// `angular_a`/`angular_b` the angular Jacobians (the lever arm crossed with the
// linear Jacobian), and `effective_mass` the inverse of the row's apparent mass
// (1 / (J M^-1 J^T)).
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
struct ConstraintRow3D {
    Vec3 jacobian_a{};
    Vec3 jacobian_b{};
    Vec3 angular_a{};
    Vec3 angular_b{};
    f32 effective_mass = 0.0f;
    f32 bias = 0.0f;
    f32 position_error = 0.0f;
    f32 position_factor = 0.0f;
};

// The apparent mass of a row: 1 / (J M^-1 J^T), summing the linear and angular
// terms of both bodies. A zero result means neither body can move along the row
// (both are static), which the callers report as an unsolvable constraint.
[[nodiscard]] inline f32 row_effective_mass(const ConstraintRow3D& row, const JointBody3D& a,
                                            const JointBody3D& b) noexcept {
    const f32 linear = a.inv_mass * row.jacobian_a.length_squared() + b.inv_mass * row.jacobian_b.length_squared();
    const f32 angular = a.inv_inertia * row.angular_a.length_squared() + b.inv_inertia * row.angular_b.length_squared();
    const f32 denom = linear + angular;
    return denom > 0.0f ? 1.0f / denom : 0.0f;
}

// How many rows a joint needs: one for a distance joint.
inline constexpr usize kDistanceJoint3DRows = 1;

// The distance-joint parameters, decoupled from the component so the maths can be
// exercised on its own. The constraint measures between the two body centres.
// `stiffness` is a frequency in Hz (0 gives a rigid rod) and `damping` is a
// fraction in [0, 1] of the current relative velocity the joint opposes, which
// lets a spring lose energy instead of ringing forever.
struct DistanceJoint3DParams {
    f32 rest_length = 0.0f;
    f32 stiffness = 0.0f;
    f32 damping = 0.0f;
};

// Builds the single distance-constraint row for the current state of the two
// bodies, for a step of `delta_time` seconds. Returns an Error when the bodies
// are coincident (the constraint direction is undefined), when `delta_time` is
// non-positive, or when neither body can move, so the caller can skip an
// unsolvable joint rather than emit NaNs.
[[nodiscard]] Result<ConstraintRow3D> build_distance_row(const JointBody3D& a, const JointBody3D& b,
                                                         const DistanceJoint3DParams& params, f32 delta_time);

// Applies a solved row's velocity impulse to the two bodies: the impulse is the
// current relative velocity along the row (linear and angular) plus the row's
// bias, scaled by the row's effective mass. This is the one place the sign
// convention lives, so the solver and any test agree by construction.
void apply_row(const ConstraintRow3D& row, JointBody3D& a, JointBody3D& b) noexcept;

// Applies a solved row's positional correction: `position_factor` of the row's
// position error is removed along the row, split by inverse mass and inertia. A
// no-op when the row has no position term (a spring) or is unsolvable.
void apply_row_position(const ConstraintRow3D& row, JointBody3D& a, JointBody3D& b) noexcept;

} // namespace fe::physics3d
