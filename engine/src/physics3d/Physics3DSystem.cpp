// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics3d/Physics3DSystem.h"

#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/DistanceJoint3D.h"
#include "fe/physics3d/Geometry3D.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace fe::physics3d {
namespace {

// Fraction of the penetration corrected per iteration. Correcting the whole
// depth at once overshoots and makes stacks jitter; a fraction converges
// smoothly. This is the standard Baumgarte factor.
constexpr f32 kPositionCorrection = 0.8f;

// Penetration below which the positional correction is skipped, in world units.
// Without a slop, resting bodies are pushed apart a hair every frame and visibly
// vibrate.
constexpr f32 kPenetrationSlop = 0.005f;

[[nodiscard]] f32 clamp_unit(f32 value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

// Reads a node's current joint state: its transform for position and its rigid
// body for velocity and the inverse mass the joint shares its correction by. A
// node with no rigid body is immovable, which is how a joint to static geometry
// (or a bare pivot) moves only its other end.
[[nodiscard]] JointBody3D make_joint_body(const Node& node) noexcept {
    JointBody3D joint;
    joint.position = node.transform().position;
    const auto* body = node.get_component<RigidBody3D>();
    if (body == nullptr) {
        return joint;
    }
    joint.velocity = body->velocity;
    joint.inv_mass = body->inverse_mass();
    return joint;
}

// Writes a solved joint state back to the node's transform and rigid body. Only
// the position and velocity are written; this cut does not touch rotation.
void store_joint_body(Node& node, const JointBody3D& joint) noexcept {
    node.transform().position = joint.position;
    if (auto* body = node.get_component<RigidBody3D>(); body != nullptr) {
        body->velocity = joint.velocity;
    }
}

} // namespace

void Physics3DSystem::update(const FrameContext& context, Scene& scene) {
    contacts_ = 0;
    events_.clear();
    active_pairs_.clear();
    joints_solved_ = 0;
    joints_.clear();
    node_index_.clear();

    // Gather. Walk the scene once and keep every node that can participate in a
    // collision, with its world position and material data flattened out. The
    // solver below never touches the scene graph.
    bodies_.clear();
    scene.for_each_node([&](Node& node) {
        // Index every node, not just colliders: a joint end may be a bare pivot
        // with no collider at all.
        node_index_.emplace(node.id(), &node);

        // Collect joints here too, in the same single walk. The bodies they
        // reference are resolved in solve_joints, after the whole scene is
        // indexed, so a joint may point forward to a node that appears later.
        if (auto* distance = node.get_component<DistanceJoint3D>(); distance != nullptr) {
            joints_.push_back(Joint{distance, nullptr, nullptr});
        }

        auto* collider = node.get_component<Collider3D>();
        auto* body = node.get_component<RigidBody3D>();
        // Simulate a node that has either a collider (so it can collide) or a
        // rigid body (so a joint can pull on it even with no collider). A node
        // with neither is a bare pivot, which a joint may still hang from: it is
        // indexed above and its end is read on demand, but it is not simulated.
        if (collider == nullptr && body == nullptr) {
            return;
        }

        Body entry{};
        entry.collider = collider;
        entry.body = body;
        entry.node = &node;
        entry.position = node.transform().position;
        entry.velocity = body != nullptr ? body->velocity : Vec3{};
        entry.inv_mass = body != nullptr ? body->inverse_mass() : 0.0f;
        entry.restitution = collider != nullptr ? clamp_unit(collider->restitution) : 0.0f;
        entry.friction = collider != nullptr ? std::max(collider->friction, 0.0f) : 0.0f;

        bodies_.push_back(entry);
    });

    if (bodies_.empty()) {
        // No colliders at all: there can be no contacts. Reconcile against an
        // empty active set so every pair that touched last frame ends cleanly.
        // Joints still run: a joint end may be a bare body with no collider.
        reconcile_contacts(scene);
        solve_joints(scene, context.delta_time);
        return;
    }

    const f32 delta_time = context.delta_time;
    if (delta_time <= 0.0f) {
        // A paused or non-positive step advances nothing and produces no
        // contacts. It also emits no events: a paused world has not changed, so a
        // contact neither begins nor ends. `previous_pairs_` is deliberately left
        // untouched so a contact that was touching before the pause is not
        // re-reported as a Begin when the world resumes.
        return;
    }

    // Integrate velocity: gravity, then linear damping. A body with no dynamic
    // motion (infinite mass) skips all of it.
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

    // Integrate position from the updated velocities.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        entry.position += entry.velocity * delta_time;
    }

    // Broad phase: find the candidate overlapping pairs once, before the
    // iteration loop, so the solver does not re-enumerate the whole pair set on
    // every iteration. A body with no collider has no bounds and is not inserted,
    // so it never appears in a candidate.
    //
    // `bodies_` holds colliderless bodies too, so the grid's indices are into
    // `broad_phase_bounds_`, not into `bodies_`. `bounds_owner_` maps one to the
    // other; without it a scene with a colliderless body ahead of its colliders
    // would resolve the wrong bodies.
    broad_phase_bounds_.clear();
    bounds_owner_.clear();
    broad_phase_bounds_.reserve(bodies_.size());
    bounds_owner_.reserve(bodies_.size());
    for (usize i = 0; i < bodies_.size(); ++i) {
        const Body& entry = bodies_[i];
        if (entry.collider != nullptr) {
            broad_phase_bounds_.push_back(entry.collider->bounds(entry.position));
            bounds_owner_.push_back(i);
        }
    }
    broad_phase_.build(broad_phase_bounds_);
    const std::span<const BroadPhasePair3D> candidates = broad_phase_.pairs();

    // Resolve. Sequential impulses over the candidate pairs, repeated for a few
    // iterations. The candidate set is computed once above; each iteration walks
    // only it, so a step costs O(candidates * iterations) rather than
    // O(bodies^2 * iterations).
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        const bool first_iteration = iteration == 0;
        for (const BroadPhasePair3D& candidate : candidates) {
            Body& a = bodies_[bounds_owner_[candidate.first]];
            Body& b = bodies_[bounds_owner_[candidate.second]];

            // A body with no collider cannot collide. The grid never inserts one,
            // but the guard keeps the solver honest if a caller ever passes a
            // colliderless body through.
            if (a.collider == nullptr || b.collider == nullptr) {
                continue;
            }

            // Sharing a grid cell is necessary but not sufficient: keep the
            // existing bounds prefilter so a pair in the same cell but not
            // actually close is rejected before the narrow phase.
            if (!bounds_overlap(a.collider->bounds(a.position), b.collider->bounds(b.position))) {
                continue;
            }

            const Contact3D contact =
                intersect_shapes(a.collider->shape, a.collider->box(a.position), a.collider->sphere(a.position),
                                 b.collider->shape, b.collider->box(b.position), b.collider->sphere(b.position));
            if (!contact.overlapping) {
                continue;
            }

            // Record the touching pair once, on the first iteration, so the
            // contact-event pass can diff it against the previous frame. The key
            // is by node identity (ordered), so it is stable no matter how the
            // scene is walked.
            if (first_iteration) {
                ++contacts_;
                BodyPair pair;
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

            // Positional correction: push the pair apart along the normal. Split
            // by inverse mass, so a heavy body moves less than a light one, and a
            // static body not at all.
            const f32 depth = contact.penetration - kPenetrationSlop;
            if (depth > 0.0f) {
                const Vec3 correction = contact.normal * (kPositionCorrection * depth / inv_mass_sum);
                a.position -= correction * a.inv_mass;
                b.position += correction * b.inv_mass;
            }

            // Relative velocity at the contact along the normal. `normal` points
            // from A to B, so a separating pair has a positive projection and
            // needs no impulse.
            const Vec3 relative = b.velocity - a.velocity;
            const f32 normal_speed = relative.dot(contact.normal);
            if (normal_speed >= 0.0f) {
                continue;
            }

            // Restitution of the pair is the larger of the two, which is the
            // conventional choice and keeps a bouncy ball bouncy against a dead
            // floor.
            const f32 restitution = std::max(a.restitution, b.restitution);
            const f32 impulse_magnitude = -(1.0f + restitution) * normal_speed / inv_mass_sum;
            const Vec3 impulse = contact.normal * impulse_magnitude;

            a.velocity -= impulse * a.inv_mass;
            b.velocity += impulse * b.inv_mass;

            // Friction: an impulse opposing the tangential relative motion,
            // capped by the Coulomb cone. Linear-only, so it slows sliding
            // without spinning either body.
            const Vec3 tangent_raw = relative - contact.normal * normal_speed;
            const f32 tangent_length = tangent_raw.length();
            if (tangent_length > 0.0f) {
                const Vec3 tangent = tangent_raw / tangent_length;
                f32 friction_impulse = -relative.dot(tangent) / inv_mass_sum;
                const f32 max_friction = std::max(a.friction, b.friction) * impulse_magnitude;
                friction_impulse = std::clamp(friction_impulse, -max_friction, max_friction);
                const Vec3 friction = tangent * friction_impulse;
                a.velocity -= friction * a.inv_mass;
                b.velocity += friction * b.inv_mass;
            }
        }
    }

    // Write back. The transform owns the node's position; the rigid body owns its
    // velocity. Writing both here is what keeps the two in sync, exactly as the
    // animation system writes UVs back onto the sprite. Rotation is not touched:
    // this cut is linear only.
    for (Body& entry : bodies_) {
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        entry.body->velocity = entry.velocity;
        Node* node = entry.node;
        if (node != nullptr) {
            node->transform().position = entry.position;
        }
    }

    // Joints. Solved after the contact pass and the write-back, so a joint
    // correction is the last word on position: a body pinned to a pivot stays
    // pinned even while it also rests on a collider. A joint writes its own node
    // transforms and velocities.
    solve_joints(scene, delta_time);

    reconcile_contacts(scene);
}

void Physics3DSystem::solve_joints(Scene& scene, f32 delta_time) {
    if (joints_.empty() || delta_time <= 0.0f) {
        return;
    }

    for (Joint& joint : joints_) {
        // Resolve the two ends. A nil, missing, or self-referential reference is
        // a broken joint: skip it rather than constraining nothing or a body
        // against itself.
        const Uuid& id_a = joint.distance->body_a;
        const Uuid& id_b = joint.distance->body_b;
        if (id_a.is_nil() || id_b.is_nil() || id_a == id_b) {
            continue;
        }
        joint.a = scene.find_node(id_a);
        joint.b = scene.find_node(id_b);
        if (joint.a == nullptr || joint.b == nullptr) {
            continue;
        }

        JointBody3D body_a = make_joint_body(*joint.a);
        JointBody3D body_b = make_joint_body(*joint.b);
        if (body_a.inv_mass <= 0.0f && body_b.inv_mass <= 0.0f) {
            // Two immovable ends: the constraint can never be satisfied by moving
            // anything, so leave the world alone.
            continue;
        }

        DistanceJoint3DParams params;
        // A positive rest length pins that length. Zero or negative means "hold
        // the separation the two bodies have right now", which is the usual intent
        // when a joint is dropped onto an already-placed pair; it is recomputed
        // each step, so it stays a constant-length rod.
        params.rest_length = joint.distance->rest_length > 0.0f ? joint.distance->rest_length
                                                                : (body_b.position - body_a.position).length();
        params.stiffness = joint.distance->stiffness;
        params.damping = std::clamp(joint.distance->damping, 0.0f, 1.0f);

        auto row = build_distance_row(body_a, body_b, params, delta_time);
        if (!row) {
            continue; // unsolvable this step (coincident, or both immovable)
        }
        apply_row(row.value(), body_a, body_b);
        apply_row_position(row.value(), body_a, body_b);

        store_joint_body(*joint.a, body_a);
        store_joint_body(*joint.b, body_b);
        ++joints_solved_;
    }
}

void Physics3DSystem::reconcile_contacts(Scene& scene) {
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

void Physics3DSystem::append_event(ContactPhase phase, const BodyPair& pair, Scene& scene) {
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

} // namespace fe::physics3d
