// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/JointSolver.h"

#include <cmath>

namespace fe::physics2d {
namespace {

// Fraction of a rigid joint's position error removed per solve pass. Correcting
// the whole error at once overshoots and rings; a fraction converges smoothly.
// This is the joint counterpart of the contact solver's Baumgarte factor.
constexpr f32 kJointPositionFactor = 0.2f;

// Separation below which the distance constraint direction is undefined. Two
// coincident centres have no line to measure along, so the joint is reported as
// unsolvable rather than producing a NaN direction.
constexpr f32 kMinSeparation = 1e-6f;

// Rotates a local offset into world space by a Z rotation, matching
// Mat4::rotation_z. Used to place a joint anchor on a rotating body.
[[nodiscard]] Vec2 rotate(const Vec2& vector, f32 angle) noexcept {
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    return Vec2{vector.x * c - vector.y * s, vector.x * s + vector.y * c};
}

} // namespace

Result<ConstraintRow> build_distance_row(const JointBody& a, const JointBody& b, const DistanceJointParams& params,
                                         f32 delta_time) {
    if (delta_time <= 0.0f) {
        return Error("distance joint: non-positive delta time");
    }

    const Vec2 separation = b.position - a.position;
    const f32 length = separation.length();
    if (length <= kMinSeparation) {
        return Error("distance joint: the two bodies are coincident, so the constraint has no direction");
    }

    // The row measures the separation along the unit vector from A to B: the
    // Jacobian is -n on A and +n on B, so a positive row velocity means the
    // bodies are separating.
    const Vec2 direction = separation / length;
    ConstraintRow row;
    row.jacobian_a = direction * -1.0f;
    row.jacobian_b = direction;
    row.effective_mass = row_effective_mass(row, a, b);
    if (row.effective_mass <= 0.0f) {
        return Error("distance joint: neither body can move, so the constraint is unsolvable");
    }

    const f32 error = length - params.rest_length;
    if (params.stiffness > 0.0f) {
        // A spring: the bias is a restoring velocity proportional to the
        // stretch, and `damping` is a derivative gain that opposes the current
        // relative velocity so the spring loses energy instead of ringing. The
        // position is left to the velocity response, so no direct correction.
        const f32 row_velocity = row.jacobian_a.dot(a.velocity) + row.jacobian_b.dot(b.velocity);
        row.bias = params.stiffness * error + params.damping * row_velocity;
    } else {
        // A rigid rod: cancel the relative velocity, then remove a fixed
        // fraction of the position error each pass. Reaching the error at a
        // fixed rate is frame-rate independent, where a velocity bias is not.
        row.position_error = error;
        row.position_factor = kJointPositionFactor;
    }
    return row;
}

Result<void> build_revolute_rows(const JointBody& a, const JointBody& b, const RevoluteJointParams& params,
                                 f32 delta_time, ConstraintRow out_rows[kRevoluteJointRows]) {
    if (delta_time <= 0.0f) {
        return Error("revolute joint: non-positive delta time");
    }

    // The world offset of each body's local anchor. The lever arm from the
    // centre to the anchor is what couples the anchor constraint to rotation,
    // which is what makes a pendulum swing instead of sliding.
    const Vec2 lever_a = rotate(params.anchor_a, a.rotation);
    const Vec2 lever_b = rotate(params.anchor_b, b.rotation);

    // Position error: how far apart the two anchors currently are. Each axis is
    // a separate row, so a shared anchor removes both translational degrees of
    // freedom while leaving relative rotation free.
    const Vec2 error = (a.position + lever_a) - (b.position + lever_b);
    const Vec2 axes[kRevoluteJointRows] = {Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}};
    const f32 errors[kRevoluteJointRows] = {error.x, error.y};

    bool any_solvable = false;
    for (usize i = 0; i < kRevoluteJointRows; ++i) {
        ConstraintRow& row = out_rows[i];
        row.jacobian_a = axes[i];
        row.jacobian_b = axes[i] * -1.0f;
        // The angular Jacobian is the lever arm crossed with the linear one:
        // moving A's centre along +axis rotates its anchor by cross(r, axis).
        row.angular_a = cross(lever_a, row.jacobian_a);
        row.angular_b = cross(lever_b, row.jacobian_b);
        row.effective_mass = row_effective_mass(row, a, b);
        row.position_error = errors[i];
        row.position_factor = kJointPositionFactor;
        any_solvable = any_solvable || row.effective_mass > 0.0f;
    }

    if (!any_solvable) {
        return Error("revolute joint: neither body can move, so the constraint is unsolvable");
    }
    return Result<void>{};
}

void apply_row(const ConstraintRow& row, JointBody& a, JointBody& b) noexcept {
    if (row.effective_mass <= 0.0f) {
        return; // unsolvable row: both bodies are immovable along it
    }

    // The relative velocity along the row, then the impulse that cancels it and
    // applies the bias. One sign convention, shared with the builder.
    const f32 row_velocity = row.jacobian_a.dot(a.velocity) + row.angular_a * a.angular_velocity +
                             row.jacobian_b.dot(b.velocity) + row.angular_b * b.angular_velocity;
    const f32 impulse = -row.effective_mass * (row_velocity + row.bias);

    a.velocity += row.jacobian_a * (impulse * a.inv_mass);
    a.angular_velocity += row.angular_a * impulse * a.inv_inertia;
    b.velocity += row.jacobian_b * (impulse * b.inv_mass);
    b.angular_velocity += row.angular_b * impulse * b.inv_inertia;
}

void apply_row_position(const ConstraintRow& row, JointBody& a, JointBody& b) noexcept {
    if (row.effective_mass <= 0.0f || row.position_factor == 0.0f) {
        return;
    }

    // Remove `position_factor` of the error along the row. Scaling by the
    // effective mass is what makes the amount removed independent of the two
    // bodies' masses: the resulting change in the constraint value is exactly
    // position_factor * position_error regardless of how the motion is shared.
    const f32 correction = row.position_factor * row.position_error * row.effective_mass;
    a.position -= row.jacobian_a * (correction * a.inv_mass);
    a.rotation -= row.angular_a * correction * a.inv_inertia;
    b.position -= row.jacobian_b * (correction * b.inv_mass);
    b.rotation -= row.angular_b * correction * b.inv_inertia;
}

Vec2 revolute_world_anchor(const JointBody& a, const RevoluteJointParams& params) noexcept {
    return a.position + rotate(params.anchor_a, a.rotation);
}

} // namespace fe::physics2d
