// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Component.h"

namespace fe::physics2d {

// A revolute (pin) joint: two bodies share a single anchor point. Their
// relative translation is removed; their relative rotation is left free, so one
// body can swing about the other -- a pendulum, a wheel on an axle, a ragdoll
// limb.
//
// The joint names its two bodies by node id, like the distance joint, so it
// serializes as a stable reference and survives a save/load cycle.
//
// The shared point is authored as a local anchor on each body. Both default to
// the body centre, which pins the two centres together; set one to a local
// offset to pin a point away from the centre. For a pendulum pinned at the top
// of a bob hanging from a fixed pivot, put the bob's local anchor at its top
// and leave the pivot's at its centre.
class RevoluteJoint2D final : public Component {
public:
    static constexpr const char* kTypeName = "RevoluteJoint2D";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    // The two bodies the joint connects, by node id. Either may be nil (the
    // default), in which case the joint is skipped. A missing or non-dynamic
    // body is likewise skipped cleanly.
    Uuid body_a{};
    Uuid body_b{};

    // The shared anchor point expressed in each body's local frame. A local
    // offset is rotated by the body's rotation before it is constrained, so a
    // pendulum pinned off-centre swings correctly.
    Vec2 anchor_a{};
    Vec2 anchor_b{};

    [[nodiscard]] bool is_valid() const noexcept { return !body_a.is_nil() && !body_b.is_nil() && body_a != body_b; }
};

} // namespace fe::physics2d
