// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Component.h"

namespace fe::physics3d {

// A distance constraint between two bodies: it holds their centres a fixed
// distance apart, or, with stiffness, pulls them back like a spring.
//
// The joint names its two bodies by node id rather than by pointer, so it
// serializes as a stable reference (the same rule DistanceJoint2D uses) and
// survives a save/load cycle. A joint is a component like any other: it lives on
// one node, points at two bodies, and `Physics3DSystem` gathers it with the rest
// of the world each step.
//
// The constraint is one row, so it removes a single degree of freedom: the
// bodies can still rotate freely about each other, which is what makes a
// distance joint a rope or a spring rather than a rigid link.
//
// This cut is linear only, matching the 3D solver: a joint does not spin its
// ends. Angular joints follow with angular bodies (ADR-0031).
class DistanceJoint3D final : public Component {
public:
    static constexpr const char* kTypeName = "DistanceJoint3D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The two bodies the joint connects, by node id. Either may be nil (the
    // default), in which case the joint has nothing to constrain and the solver
    // skips it. A body that is not in the scene, or that has no rigid body, is
    // likewise skipped rather than treated as a crash.
    Uuid body_a{};
    Uuid body_b{};

    // The distance to hold, in world units. Zero (the default) means "capture
    // the separation the two bodies have the first time the joint is solved",
    // which is what an author usually wants when dropping a joint onto an
    // already-placed pair. Set it explicitly to pin a specific length.
    f32 rest_length = 0.0f;

    // Spring frequency in Hz. Zero (the default) makes the joint a rigid rod,
    // corrected at the solver's usual rate. A positive value makes it a spring
    // that pulls proportionally to the stretch; larger is stiffer.
    f32 stiffness = 0.0f;

    // Damping ratio in [0, 1]. Zero lets a spring ring; one is critically
    // damped. Ignored by a rigid rod, which the solver already corrects without
    // overshoot.
    f32 damping = 0.0f;

    [[nodiscard]] bool is_valid() const noexcept { return !body_a.is_nil() && !body_b.is_nil() && body_a != body_b; }
};

} // namespace fe::physics3d
