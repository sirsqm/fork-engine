// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/systems/System.h"

#include <string_view>
#include <vector>

namespace fe {

class Scene;

namespace physics2d {

class Collider2D;
class RigidBody2D;

// Advances the 2D physics world by one frame: integration, then collision
// resolution.
//
// This is the first system that actually changes the scene as it runs, which is
// what the systems layer (ADR-0008) was built for. It is a single-threaded,
// fixed-iteration solver, not a full physics engine: it has linear motion,
// gravity, restitution, and friction, and deliberately no angular dynamics
// (ADR-0009 states why).
//
// Shape and motion are separate components. A node with a Collider2D alone is a
// static obstacle; a node with both a Collider2D and a dynamic RigidBody2D is
// moved by the solver. That is the composition model doing the work: "static
// floor" and "falling crate" are the same two components in different
// configurations, not two classes.
//
// The solver is sequential-impulse with Baumgarte position correction, run for a
// fixed number of iterations. It reads the bodies into a flat array first, so
// the inner loop walks contiguous memory rather than the scene's pointer graph.
class Physics2DSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Physics2DSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    // Gravity in world units per second squared. Points down by default, which
    // matches the renderer's y-up convention.
    Vec2 gravity{0.0f, -9.81f};

    // Sequential-impulse iterations per frame. More iterations resolve stacked
    // bodies more stably at a linear cost. Four is enough for the shapes this
    // cut supports and is cheap; it is a tunable, not a constant.
    u32 iterations = 4;

    // Contacts found in the most recent update. Exposed for tests and the
    // editor's debug overlay.
    [[nodiscard]] usize contact_count() const noexcept { return contacts_; }

private:
    // One simulated node, flattened out of the scene for the duration of the
    // update so the solver works on a contiguous array.
    struct Body {
        Collider2D* collider;
        RigidBody2D* body; // null for a static collider
        Vec2 position;
        Vec2 velocity;
        f32 inv_mass;
        f32 restitution;
        f32 friction;
    };

    std::vector<Body> bodies_;
    usize contacts_ = 0;
};

} // namespace physics2d
} // namespace fe
