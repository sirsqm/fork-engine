// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/Physics2DSystem.h"

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Geometry.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <algorithm>
#include <cmath>

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

// The 2D cross product of a vector and a scalar: the tangential velocity a body
// at offset `r` from the contact contributes from an angular velocity `w`. In
// 2D this is (-r.y * w, r.x * w), the rotation of `r` by 90 degrees scaled by w.
[[nodiscard]] Vec2 cross_scalar_vec2(f32 scalar, const Vec2& vector) noexcept {
    return Vec2{-scalar * vector.y, scalar * vector.x};
}

// The scalar 2D cross of two vectors: the z component of their 3D cross, used
// to project a linear impulse onto an angular impulse about the contact.
[[nodiscard]] f32 cross_vec2(const Vec2& a, const Vec2& b) noexcept {
    return a.x * b.y - a.y * b.x;
}

} // namespace

void Physics2DSystem::update(const FrameContext& context, Scene& scene) {
    contacts_ = 0;
    events_.clear();
    active_pairs_.clear();

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
        entry.node = &node;
        entry.position = Vec2{position.x, position.y};
        entry.velocity = body != nullptr ? body->velocity : Vec2{};
        entry.rotation = node.transform().rotation.z;
        entry.angular_velocity = body != nullptr ? body->angular_velocity : 0.0f;
        entry.inv_mass = body != nullptr ? body->inverse_mass() : 0.0f;
        entry.restitution = clamp_unit(collider->restitution);
        entry.friction = std::max(collider->friction, 0.0f);
        entry.fixed_rotation = body != nullptr && body->fixed_rotation;

        // Rotational inertia. An author-set inertia on the body wins; otherwise
        // it is derived from the shape and mass, so a box or disc rotates with a
        // physically sensible resistance. A non-dynamic body keeps zero.
        if (body != nullptr && entry.inv_mass > 0.0f && !body->fixed_rotation) {
            const f32 inertia = body->inertia > 0.0f ? body->inertia : collider->inertia_about_center(body->mass);
            entry.inv_inertia = inertia > 0.0f ? 1.0f / inertia : 0.0f;
        } else {
            entry.inv_inertia = 0.0f;
        }
        bodies_.push_back(entry);
    });

    if (bodies_.empty()) {
        // No colliders at all: there can be no contacts. Reconcile against an
        // empty active set so every pair that touched last frame ends cleanly.
        reconcile_contacts(scene);
        return;
    }

    const f32 delta_time = context.delta_time;
    if (delta_time <= 0.0f) {
        // A paused or non-positive step advances nothing and produces no
        // contacts. It also emits no events: a paused world has not changed, so
        // a contact neither begins nor ends. `previous_pairs_` is deliberately
        // left untouched so a contact that was touching before the pause is not
        // re-reported as a Begin when the world resumes.
        return;
    }

    // Integrate velocity: gravity, torque, then linear and angular damping. A
    // body with no dynamic motion (infinite mass) skips all of it.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        if (entry.body->affected_by_gravity) {
            entry.velocity += gravity * delta_time;
        }
        if (entry.body->torque != 0.0f && entry.inv_inertia > 0.0f) {
            entry.angular_velocity += entry.body->torque * entry.inv_inertia * delta_time;
        }
        const f32 damping = clamp_unit(entry.body->linear_damping);
        if (damping > 0.0f) {
            // Frame-rate-independent decay: the velocity is multiplied by
            // (1 - damping) once per second, distributed across this frame.
            entry.velocity = entry.velocity * std::max(0.0f, 1.0f - damping * delta_time);
        }
        const f32 angular_damping = clamp_unit(entry.body->angular_damping);
        if (angular_damping > 0.0f) {
            entry.angular_velocity *= std::max(0.0f, 1.0f - angular_damping * delta_time);
        }
    }

    // Integrate position and rotation from the updated velocities.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        entry.position += entry.velocity * delta_time;
        // A fixed-rotation body keeps its authored rotation regardless of its
        // angular velocity; the body's angular state is carried through so a
        // contact can still see it, but it is never advanced.
        if (!entry.fixed_rotation) {
            entry.rotation += entry.angular_velocity * delta_time;
        }
    }

    // Resolve. Sequential impulses over every colliding pair, repeated for a few
    // iterations. This is O(pairs * iterations); a spatial hash belongs here
    // once profiling shows it matters, not before.
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

                // Record the touching pair once, on the first iteration, so the
                // contact-event pass can diff it against the previous frame. The
                // key is by node identity (ordered), so it is stable no matter
                // how the scene is walked.
                if (first_iteration) {
                    ++contacts_;
                    BodyPair pair;
                    // Order the pair by raw id bytes so the key is stable and
                    // independent of scene order.
                    const bool a_first =
                        std::lexicographical_compare(a.node->id().bytes().begin(), a.node->id().bytes().end(),
                                                     b.node->id().bytes().begin(), b.node->id().bytes().end());
                    pair.first = a_first ? a.node->id() : b.node->id();
                    pair.second = a_first ? b.node->id() : a.node->id();
                    active_pairs_.push_back(pair);
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

                // The contact point, taken on A's surface along the normal (a
                // single-point approximation, standard for box and disc
                // contacts). Angular impulses need the lever arm from each
                // centre to this point; a contact through a centre produces no
                // torque, which is what keeps a body resting flat stable.
                const f32 support_a = a.collider->shape == ShapeType::Circle
                                          ? a.collider->radius
                                          : a.collider->half_extent.x * std::abs(contact.normal.x) +
                                                a.collider->half_extent.y * std::abs(contact.normal.y);
                const Vec2 contact_point = a.position + contact.normal * (support_a - contact.penetration * 0.5f);

                // Lever arms from each body centre to the contact point.
                const Vec2 ra = contact_point - a.position;
                const Vec2 rb = contact_point - b.position;

                // Relative velocity at the contact, including the contribution
                // of each body's rotation. `normal` points from A to B, so a
                // separating pair has a positive projection and needs no impulse.
                const Vec2 relative = (b.velocity + cross_scalar_vec2(b.angular_velocity, rb)) -
                                      (a.velocity + cross_scalar_vec2(a.angular_velocity, ra));
                const f32 normal_speed = relative.dot(contact.normal);
                if (normal_speed >= 0.0f) {
                    continue;
                }

                // Effective mass along the normal, including the angular term.
                // This is what couples rotation into the linear response.
                const f32 ra_cross_n = cross_vec2(ra, contact.normal);
                const f32 rb_cross_n = cross_vec2(rb, contact.normal);
                const f32 effective_mass =
                    inv_mass_sum + a.inv_inertia * ra_cross_n * ra_cross_n + b.inv_inertia * rb_cross_n * rb_cross_n;
                if (effective_mass <= 0.0f) {
                    continue;
                }

                // Restitution of the pair is the larger of the two, which is the
                // conventional choice and keeps a bouncy ball bouncy against a
                // dead floor.
                const f32 restitution = std::max(a.restitution, b.restitution);
                const f32 impulse_magnitude = -(1.0f + restitution) * normal_speed / effective_mass;
                const Vec2 impulse = contact.normal * impulse_magnitude;

                a.velocity -= impulse * a.inv_mass;
                b.velocity += impulse * b.inv_mass;
                // Angular response: the impulse at a lever arm produces torque.
                a.angular_velocity -= a.inv_inertia * cross_vec2(ra, impulse);
                b.angular_velocity += b.inv_inertia * cross_vec2(rb, impulse);

                // Friction: an impulse opposing the tangential relative motion,
                // capped by the Coulomb cone. With angular dynamics it also spins
                // the bodies, which is what makes a box tumble rather than slide.
                const Vec2 tangent_raw = relative - contact.normal * normal_speed;
                const f32 tangent_length = tangent_raw.length();
                if (tangent_length > 0.0f) {
                    const Vec2 tangent = tangent_raw / tangent_length;
                    const f32 ra_cross_t = cross_vec2(ra, tangent);
                    const f32 rb_cross_t = cross_vec2(rb, tangent);
                    const f32 tangent_mass = inv_mass_sum + a.inv_inertia * ra_cross_t * ra_cross_t +
                                             b.inv_inertia * rb_cross_t * rb_cross_t;
                    if (tangent_mass > 0.0f) {
                        const f32 tangent_speed = relative.dot(tangent);
                        f32 friction_impulse = -tangent_speed / tangent_mass;
                        const f32 max_friction = std::max(a.friction, b.friction) * impulse_magnitude;
                        friction_impulse = std::clamp(friction_impulse, -max_friction, max_friction);
                        const Vec2 friction = tangent * friction_impulse;
                        a.velocity -= friction * a.inv_mass;
                        b.velocity += friction * b.inv_mass;
                        a.angular_velocity -= a.inv_inertia * cross_vec2(ra, friction);
                        b.angular_velocity += b.inv_inertia * cross_vec2(rb, friction);
                    }
                }
            }
        }
    }

    // Write back. The transform owns the node's position and rotation; the
    // rigid body owns its velocities. Writing both here is what keeps the two in
    // sync, exactly as the animation system writes UVs back onto the sprite.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        entry.body->velocity = entry.velocity;
        entry.body->angular_velocity = entry.angular_velocity;
        Node* node = entry.collider->node();
        if (node != nullptr) {
            node->transform().position.x = entry.position.x;
            node->transform().position.y = entry.position.y;
            node->transform().rotation.z = entry.rotation;
        }
    }

    reconcile_contacts(scene);
}

void Physics2DSystem::reconcile_contacts(Scene& scene) {
    // Diff the pairs that touch now against those that touched last frame.
    // Sorting both makes the diff a linear merge rather than an O(n^2) scan, and
    // makes the emitted order deterministic.
    std::sort(active_pairs_.begin(), active_pairs_.end());
    std::sort(previous_pairs_.begin(), previous_pairs_.end());

    usize i = 0;
    usize j = 0;
    while (i < active_pairs_.size() && j < previous_pairs_.size()) {
        if (active_pairs_[i] == previous_pairs_[j]) {
            ++i;
            ++j;
        } else if (active_pairs_[i] < previous_pairs_[j]) {
            append_event(ContactPhase::Begin, active_pairs_[i], scene);
            ++i;
        } else {
            append_event(ContactPhase::End, previous_pairs_[j], scene);
            ++j;
        }
    }
    for (; i < active_pairs_.size(); ++i) {
        append_event(ContactPhase::Begin, active_pairs_[i], scene);
    }
    for (; j < previous_pairs_.size(); ++j) {
        append_event(ContactPhase::End, previous_pairs_[j], scene);
    }

    previous_pairs_.swap(active_pairs_);
}

void Physics2DSystem::append_event(ContactPhase phase, const BodyPair& pair, Scene& scene) {
    ContactEvent event;
    event.phase = phase;
    event.a = scene.find_node(pair.first);
    event.b = scene.find_node(pair.second);
    // A node that left the scene between the previous frame and this one cannot
    // be resolved; skip the event rather than reporting a dangling pointer.
    if (event.a == nullptr || event.b == nullptr) {
        return;
    }
    events_.push_back(event);
}

} // namespace fe::physics2d
