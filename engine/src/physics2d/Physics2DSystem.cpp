// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/Physics2DSystem.h"

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Geometry.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <algorithm>

namespace fe::physics2d {
namespace {

// Fraction of the penetration corrected per iteration. Correcting the whole
// depth at once overshoots and makes stacks jitter; a fraction converges
// smoothly. This is the standard Baumgarte factor.
constexpr f32 kPositionCorrection = 0.8f;

// Penetration below which the positional correction is skipped, in world units.
// Without a slop, resting bodies are pushed apart a hair every frame and
// visibly vibrate.
constexpr f32 kPenetrationSlop = 0.005f;

[[nodiscard]] f32 clamp_unit(f32 value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void Physics2DSystem::update(const FrameContext& context, Scene& scene) {
    contacts_ = 0;

    // Gather. Walk the scene once and keep every node that can participate in a
    // collision, with its world position and material data flattened out. The
    // solver below never touches the scene graph.
    bodies_.clear();
    scene.for_each_node([&](Node& node) {
        auto* collider = node.get_component<Collider2D>();
        if (collider == nullptr) {
            return;
        }
        auto* body = node.get_component<RigidBody2D>();
        const Vec3& position = node.transform().position;

        Body entry{};
        entry.collider = collider;
        entry.body = body;
        entry.position = Vec2{position.x, position.y};
        entry.velocity = body != nullptr ? body->velocity : Vec2{};
        entry.inv_mass = body != nullptr ? body->inverse_mass() : 0.0f;
        entry.restitution = clamp_unit(collider->restitution);
        entry.friction = std::max(collider->friction, 0.0f);
        bodies_.push_back(entry);
    });

    if (bodies_.empty()) {
        return;
    }

    const f32 delta_time = context.delta_time;
    if (delta_time <= 0.0f) {
        return;
    }

    // Integrate velocity: gravity, then damping. A body with no dynamic motion
    // (infinite mass) skips both.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        if (entry.body->affected_by_gravity) {
            entry.velocity += gravity * delta_time;
        }
        const f32 damping = clamp_unit(entry.body->linear_damping);
        if (damping > 0.0f) {
            // Frame-rate-independent decay: the velocity is multiplied by
            // (1 - damping) once per second, distributed across this frame.
            entry.velocity = entry.velocity * std::max(0.0f, 1.0f - damping * delta_time);
        }
    }

    // Integrate position from the updated velocity.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        entry.position += entry.velocity * delta_time;
    }

    // Resolve. Sequential impulses over every colliding pair, repeated for a
    // few iterations. This is O(pairs * iterations); a spatial hash belongs
    // here once profiling shows it matters, not before.
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        const bool first_iteration = iteration == 0;
        for (usize i = 0; i < bodies_.size(); ++i) {
            for (usize j = i + 1; j < bodies_.size(); ++j) {
                Body& a = bodies_[i];
                Body& b = bodies_[j];

                // Broad phase: skip pairs whose bounds cannot touch.
                if (!bounds_overlap(a.collider->bounds(a.position), b.collider->bounds(b.position))) {
                    continue;
                }

                const Contact2D contact =
                    intersect_shapes(a.collider->shape, a.collider->box(a.position), a.collider->circle(a.position),
                                     b.collider->shape, b.collider->box(b.position), b.collider->circle(b.position));
                if (!contact.overlapping) {
                    continue;
                }
                if (first_iteration) {
                    ++contacts_;
                }

                const f32 inv_mass_sum = a.inv_mass + b.inv_mass;
                if (inv_mass_sum <= 0.0f) {
                    continue; // two immovable colliders: nothing to resolve
                }

                // Positional correction: push the pair apart along the normal.
                // Split by inverse mass, so a heavy body moves less than a light
                // one, and a static body not at all.
                const f32 depth = contact.penetration - kPenetrationSlop;
                if (depth > 0.0f) {
                    const Vec2 correction = contact.normal * (kPositionCorrection * depth / inv_mass_sum);
                    a.position -= correction * a.inv_mass;
                    b.position += correction * b.inv_mass;
                }

                // Relative velocity along the contact normal. `normal` points
                // from A to B, so a separating pair has a positive projection
                // and needs no impulse.
                const Vec2 relative = b.velocity - a.velocity;
                const f32 normal_speed = relative.dot(contact.normal);
                if (normal_speed >= 0.0f) {
                    continue;
                }

                // Restitution of the pair is the larger of the two, which is
                // the conventional choice and keeps a bouncy ball bouncy against
                // a dead floor.
                const f32 restitution = std::max(a.restitution, b.restitution);
                const f32 impulse_magnitude = -(1.0f + restitution) * normal_speed / inv_mass_sum;
                const Vec2 impulse = contact.normal * impulse_magnitude;
                a.velocity -= impulse * a.inv_mass;
                b.velocity += impulse * b.inv_mass;

                // Friction: an impulse opposing the tangential relative motion,
                // capped by the Coulomb cone. Without angular dynamics this only
                // slows sliding, but it is the same impulse form the full
                // solver will use.
                const Vec2 tangent_raw = relative - contact.normal * normal_speed;
                const f32 tangent_length = tangent_raw.length();
                if (tangent_length > 0.0f) {
                    const Vec2 tangent = tangent_raw / tangent_length;
                    const f32 tangent_speed = relative.dot(tangent);
                    f32 friction_impulse = -tangent_speed / inv_mass_sum;
                    const f32 max_friction = std::max(a.friction, b.friction) * impulse_magnitude;
                    friction_impulse = std::clamp(friction_impulse, -max_friction, max_friction);
                    const Vec2 friction = tangent * friction_impulse;
                    a.velocity -= friction * a.inv_mass;
                    b.velocity += friction * b.inv_mass;
                }
            }
        }
    }

    // Write back. The transform owns the node's position; the rigid body owns
    // its velocity. Writing both here is what keeps the two in sync, exactly as
    // the animation system writes UVs back onto the sprite.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        entry.body->velocity = entry.velocity;
        Node* node = entry.collider->node();
        if (node != nullptr) {
            node->transform().position.x = entry.position.x;
            node->transform().position.y = entry.position.y;
        }
    }
}

} // namespace fe::physics2d
