// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/physics2d/Physics2DSystem.h"

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/DistanceJoint2D.h"
#include "fe/physics2d/Geometry.h"
#include "fe/physics2d/RevoluteJoint2D.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

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

// The "no island vertex" marker in the body-to-vertex map. A static body (one
// with infinite mass) is never a vertex in the island graph, so it maps to this.
constexpr usize kNoVertex = static_cast<usize>(-1);

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

// Reads a node's current joint state: its transform for position and rotation,
// and its rigid body (if any) for velocity and the inverse mass and inertia the
// joint shares its correction by. A node with no rigid body is immovable, which
// is exactly what a fixed pivot is.
[[nodiscard]] JointBody make_joint_body(const Node& node) noexcept {
    JointBody joint;
    joint.position = Vec2{node.transform().position.x, node.transform().position.y};
    joint.rotation = node.transform().rotation.z;

    const auto* body = node.get_component<RigidBody2D>();
    if (body == nullptr) {
        return joint;
    }
    joint.velocity = body->velocity;
    joint.angular_velocity = body->angular_velocity;
    joint.inv_mass = body->inverse_mass();
    if (joint.inv_mass > 0.0f && !body->fixed_rotation) {
        // Author-set inertia wins; otherwise derive it from the collider's
        // shape, the same rule the contact solver uses. A joint end with no
        // collider falls back to the author's inertia, or to zero (a point
        // mass, which a pin still constrains translationally).
        f32 inertia = body->inertia;
        if (inertia <= 0.0f) {
            if (const auto* collider = node.get_component<Collider2D>(); collider != nullptr) {
                inertia = collider->inertia_about_center(body->mass);
            }
        }
        joint.inv_inertia = inertia > 0.0f ? 1.0f / inertia : 0.0f;
    }
    return joint;
}

// Writes a solved joint state back to the node's transform and rigid body,
// mirroring the contact solver's write-back so the two agree on who owns what.
void store_joint_body(Node& node, const JointBody& joint) noexcept {
    node.transform().position.x = joint.position.x;
    node.transform().position.y = joint.position.y;
    node.transform().rotation.z = joint.rotation;
    if (auto* body = node.get_component<RigidBody2D>(); body != nullptr) {
        body->velocity = joint.velocity;
        body->angular_velocity = joint.angular_velocity;
    }
}

} // namespace

void Physics2DSystem::update(const FrameContext& context, Scene& scene) {
    contacts_ = 0;
    events_.clear();
    active_pairs_.clear();
    joints_solved_ = 0;
    joint_diagnostics_.clear();
    joints_.clear();
    node_index_.clear();
    body_index_.clear();
    asleep_.clear();
    sleeping_bodies_ = 0;

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
        if (auto* distance = node.get_component<DistanceJoint2D>(); distance != nullptr) {
            joints_.push_back(Joint{distance, nullptr, nullptr, nullptr});
        }
        if (auto* revolute = node.get_component<RevoluteJoint2D>(); revolute != nullptr) {
            joints_.push_back(Joint{nullptr, revolute, nullptr, nullptr});
        }

        auto* collider = node.get_component<Collider2D>();
        auto* body = node.get_component<RigidBody2D>();
        // Simulate a node that has either a collider (so it can collide) or a
        // rigid body (so a joint can pull on it even with no collider). A node
        // with neither is a bare pivot, which a joint may still hang from: it is
        // indexed above and its end is read on demand, but it is not simulated.
        if (collider == nullptr && body == nullptr) {
            return;
        }
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
        entry.restitution = collider != nullptr ? clamp_unit(collider->restitution) : 0.0f;
        entry.friction = collider != nullptr ? std::max(collider->friction, 0.0f) : 0.0f;
        entry.fixed_rotation = body != nullptr && body->fixed_rotation;

        // Rotational inertia. An author-set inertia on the body wins; otherwise
        // it is derived from the shape and mass, so a box or disc rotates with a
        // physically sensible resistance. A non-dynamic body keeps zero. A body
        // with no collider and no author inertia is a point mass, which has zero
        // rotational inertia and cannot spin on its own.
        if (body != nullptr && entry.inv_mass > 0.0f && !body->fixed_rotation) {
            f32 inertia = body->inertia;
            if (inertia <= 0.0f && collider != nullptr) {
                inertia = collider->inertia_about_center(body->mass);
            }
            entry.inv_inertia = inertia > 0.0f ? 1.0f / inertia : 0.0f;
        } else {
            entry.inv_inertia = 0.0f;
        }

        // A sleeping body resumes from rest: its authored velocity is ignored so
        // a body that was asleep when the scene loaded does not spring into
        // motion on the first step. Its island is re-derived below and the wake
        // pass decides whether it stays asleep.
        const bool sleeping = body != nullptr && body->is_sleeping && entry.inv_mass > 0.0f;
        if (sleeping) {
            entry.velocity = Vec2{};
            entry.angular_velocity = 0.0f;
        }

        body_index_.emplace(node.id(), bodies_.size());
        asleep_.push_back(sleeping);
        if (sleeping) {
            ++sleeping_bodies_;
        }
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
        // contacts. It also emits no events: a paused world has not changed, so
        // a contact neither begins nor ends. `previous_pairs_` is deliberately
        // left untouched so a contact that was touching before the pause is not
        // re-reported as a Begin when the world resumes. Joints are not solved
        // either: there is no step to advance them.
        return;
    }

    // Islands, then wake checks, then integration -- all before the solver. The
    // island graph is built from the contacts that exist at the start of the
    // step and from the joints, so a moving body wakes a sleeper it has just
    // reached before the solver runs and can never pass through it.
    build_islands();
    apply_wake_checks();

    // Integrate velocity: gravity, torque, then linear and angular damping. A
    // body with no dynamic motion (infinite mass) skips all of it, and so does a
    // sleeping body: it is at rest and stays exactly where it is.
    for (usize i = 0; i < bodies_.size(); ++i) {
        Body& entry = bodies_[i];
        if (entry.inv_mass <= 0.0f || asleep_[i]) {
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
    for (usize i = 0; i < bodies_.size(); ++i) {
        Body& entry = bodies_[i];
        if (entry.inv_mass <= 0.0f || asleep_[i]) {
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

    // Broad phase: find the candidate overlapping pairs once, before the
    // iteration loop, so the solver does not re-enumerate the whole pair set on
    // every iteration. A body with no collider (a joint-only mass) has no bounds
    // and is not inserted, so it never appears in a candidate.
    //
    // `bodies_` holds colliderless bodies too, so the grid's indices are into
    // `broad_phase_bounds_`, not into `bodies_`. `bounds_owner_` maps one to the
    // other; without it a scene with a joint-only mass ahead of its colliders
    // would resolve the wrong bodies.
    broad_phase_bounds_.clear();
    bounds_owner_.clear();
    broad_phase_bounds_.reserve(bodies_.size());
    bounds_owner_.reserve(bodies_.size());
    for (usize i = 0; i < bodies_.size(); ++i) {
        const Body& entry = bodies_[i];
        if (entry.collider != nullptr) {
            // The world AABB of the oriented shape: for a rotated box this is the
            // conservative bound of its four corners, so the grid stays a
            // superset of the truly-touching pairs.
            broad_phase_bounds_.push_back(entry.collider->bounds(entry.position, entry.rotation));
            bounds_owner_.push_back(i);
        }
    }
    broad_phase_.build(broad_phase_bounds_, context.jobs);
    const std::span<const BroadPhasePair> candidates = broad_phase_.pairs();

    // Resolve. Sequential impulses over the candidate pairs, repeated for a few
    // iterations. The candidate set is computed once above; each iteration walks
    // only it, so a step costs O(candidates * iterations) rather than
    // O(bodies^2 * iterations).
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        const bool first_iteration = iteration == 0;
        for (const BroadPhasePair& candidate : candidates) {
            Body& a = bodies_[bounds_owner_[candidate.first]];
            Body& b = bodies_[bounds_owner_[candidate.second]];

            // A body with no collider (a joint-only mass) cannot collide. The
            // grid never inserts one, but the guard keeps the solver honest if a
            // caller ever passes a colliderless body through.
            if (a.collider == nullptr || b.collider == nullptr) {
                continue;
            }

            // Sharing a grid cell is necessary but not sufficient: keep the
            // existing bounds prefilter so a pair in the same cell but not
            // actually close is rejected before the narrow phase.
            if (!bounds_overlap(a.collider->bounds(a.position, a.rotation),
                                b.collider->bounds(b.position, b.rotation))) {
                continue;
            }

            // Place each shape by its node's world position, offset, and Z
            // rotation, so a rotated box collides as its true oriented shape.
            const Contact2D contact = intersect_shapes(
                a.collider->shape, a.collider->oriented_box(a.position, a.rotation),
                a.collider->oriented_circle(a.position, a.rotation), b.collider->shape,
                b.collider->oriented_box(b.position, b.rotation), b.collider->oriented_circle(b.position, b.rotation));
            if (!contact.overlapping) {
                continue;
            }

            // Record the touching pair once, on the first iteration, so the
            // contact-event pass can diff it against the previous frame. The
            // key is by node identity (ordered), so it is stable no matter how
            // the scene is walked. A sleeping body's contacts are still
            // recorded -- its position does not change, so they neither begin
            // nor end, and the event stream stays quiet -- but no impulse is
            // applied to it below.
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

            // A sleeping body is skipped by the solver. Its contacts were
            // recorded above, but it is not moved or impulsed: it is at rest
            // until something wakes it, which the wake pass did before this
            // loop if an awake body was touching it. The body indices come from
            // `bounds_owner_`, because the grid indexes its own bounds array,
            // not `bodies_`.
            const usize index_a = bounds_owner_[candidate.first];
            const usize index_b = bounds_owner_[candidate.second];
            if (asleep_[index_a] || asleep_[index_b]) {
                continue;
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
            // torque, which is what keeps a body resting flat stable. Each
            // shape is placed by its node's offset and Z rotation, so a
            // rotated box or an offset shape reports the lever arm at its
            // true surface rather than at a shape centred on the node.
            const Vec2 center_a = a.collider->world_center(a.position, a.rotation);
            const f32 support_a =
                a.collider->shape == ShapeType::Circle
                    ? a.collider->radius
                    : support_radius(a.collider->oriented_box(a.position, a.rotation), contact.normal);
            const Vec2 contact_point = center_a + contact.normal * (support_a - contact.penetration * 0.5f);

            // Lever arms from each body centre to the contact point. The
            // body's centre of mass is the node origin, so the offset shifts
            // where the surface is and therefore the lever arm.
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
                const f32 tangent_mass =
                    inv_mass_sum + a.inv_inertia * ra_cross_t * ra_cross_t + b.inv_inertia * rb_cross_t * rb_cross_t;
                if (tangent_mass > 0.0f) {
                    const f32 tangent_speed = relative.dot(tangent);
                    f32 friction_impulse = -tangent_speed / tangent_mass;
                    // Combined friction is the geometric mean, the rule Box2D
                    // uses and the one that keeps the endpoints meaningful: a
                    // material with friction 0 stays frictionless against a
                    // rough surface, and 1 against 1 is 1.
                    //
                    // Taking the larger of the two instead (the previous rule)
                    // makes friction 0 inexpressible -- a ball authored as
                    // frictionless picks up the wall's 0.5 -- so every glancing
                    // contact bleeds tangential speed. A ball bounced off a wall
                    // loses its vertical component a little each hit and ends up
                    // travelling dead horizontal forever.
                    const f32 max_friction = std::sqrt(a.friction * b.friction) * impulse_magnitude;
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

    // Write back. The transform owns the node's position and rotation; the
    // rigid body owns its velocities. Writing both here is what keeps the two in
    // sync, exactly as the animation system writes UVs back onto the sprite. A
    // sleeping body is skipped: its node transform is left exactly as authored,
    // so nothing moves it while it sleeps.
    for (usize i = 0; i < bodies_.size(); ++i) {
        Body& entry = bodies_[i];
        if (entry.inv_mass <= 0.0f || asleep_[i]) {
            continue;
        }
        entry.body->velocity = entry.velocity;
        entry.body->angular_velocity = entry.angular_velocity;
        Node* node = entry.node;
        if (node != nullptr) {
            node->transform().position.x = entry.position.x;
            node->transform().position.y = entry.position.y;
            node->transform().rotation.z = entry.rotation;
        }
    }

    // Joints last, on the freshly written-back transforms and velocities. A
    // joint is solved once (not per contact iteration) because a pin needs the
    // error corrected at a fixed rate rather than crushed to zero in one step.
    // A joint with a sleeping end is skipped, so a sleeping island is not pulled
    // by its own constraint; the wake pass already woke the island if the other
    // end was moving.
    solve_joints(scene, delta_time);

    // Sleep checks last, on the velocity the solver actually left behind, so a
    // body that was pushed this step does not count as at rest.
    apply_sleep_checks(delta_time);

    reconcile_contacts(scene);
}

void Physics2DSystem::solve_joints(Scene& scene, f32 delta_time) {
    if (joints_.empty()) {
        return;
    }
    if (delta_time <= 0.0f) {
        return;
    }

    for (Joint& joint : joints_) {
        // Resolve the two ends. A nil, missing, or self-referential reference is
        // a broken joint: record why and skip it rather than constraining
        // nothing or a body against itself.
        const Uuid& id_a = joint.distance != nullptr ? joint.distance->body_a : joint.revolute->body_a;
        const Uuid& id_b = joint.distance != nullptr ? joint.distance->body_b : joint.revolute->body_b;
        const std::string_view type =
            joint.distance != nullptr ? DistanceJoint2D::kTypeName : RevoluteJoint2D::kTypeName;

        if (id_a.is_nil() || id_b.is_nil()) {
            note_joint_skipped(type, id_a, id_b, Error("joint body reference is nil"));
            continue;
        }
        if (id_a == id_b) {
            note_joint_skipped(type, id_a, id_b, Error("joint connects a body to itself"));
            continue;
        }
        joint.a = scene.find_node(id_a);
        joint.b = scene.find_node(id_b);
        if (joint.a == nullptr || joint.b == nullptr) {
            note_joint_skipped(type, id_a, id_b, Error("joint body is not in the scene"));
            continue;
        }

        JointBody body_a = make_joint_body(*joint.a);
        JointBody body_b = make_joint_body(*joint.b);
        if (body_a.inv_mass <= 0.0f && body_b.inv_mass <= 0.0f) {
            // Two immovable ends: the constraint can never be satisfied by
            // moving anything, so report it and leave the world alone.
            note_joint_skipped(type, id_a, id_b, Error("joint has no movable body"));
            continue;
        }

        // A sleeping end behaves as an immovable one: the joint is still
        // enforced -- so joint_count() reports it and the row is built -- but it
        // cannot move the sleeper. The wake pass already woke the island if the
        // other end was moving, so a joint is only skipped here when the whole
        // island is asleep, in which case there is nothing to correct. This is
        // what keeps a pendulum that has come to rest reporting its constraint
        // rather than vanishing from the count.
        const auto index_a = body_index_.find(id_a);
        const auto index_b = body_index_.find(id_b);
        if (index_a != body_index_.end() && asleep_[index_a->second]) {
            body_a.inv_mass = 0.0f;
            body_a.inv_inertia = 0.0f;
        }
        if (index_b != body_index_.end() && asleep_[index_b->second]) {
            body_b.inv_mass = 0.0f;
            body_b.inv_inertia = 0.0f;
        }
        if (body_a.inv_mass <= 0.0f && body_b.inv_mass <= 0.0f) {
            // Both ends are immovable or asleep: nothing to correct, but the
            // joint is still counted (it was gathered and is not broken).
            ++joints_solved_;
            continue;
        }

        if (joint.distance != nullptr) {
            DistanceJointParams params;
            // A positive rest length pins that length. Zero or negative means
            // "hold the separation the two bodies have right now", which is the
            // usual intent when a joint is dropped onto an already-placed pair;
            // it is recomputed each step, so it stays a constant-length rod.
            params.rest_length = joint.distance->rest_length > 0.0f ? joint.distance->rest_length
                                                                    : (body_b.position - body_a.position).length();
            params.stiffness = joint.distance->stiffness;
            params.damping = std::clamp(joint.distance->damping, 0.0f, 1.0f);

            auto row = build_distance_row(body_a, body_b, params, delta_time);
            if (!row) {
                note_joint_skipped(type, id_a, id_b, row.error());
                continue;
            }
            apply_row(row.value(), body_a, body_b);
            apply_row_position(row.value(), body_a, body_b);
        } else {
            RevoluteJointParams params;
            params.anchor_a = joint.revolute->anchor_a;
            params.anchor_b = joint.revolute->anchor_b;

            ConstraintRow rows[kRevoluteJointRows];
            if (auto built = build_revolute_rows(body_a, body_b, params, delta_time, rows); !built) {
                note_joint_skipped(type, id_a, id_b, built.error());
                continue;
            }
            for (const ConstraintRow& row : rows) {
                apply_row(row, body_a, body_b);
                apply_row_position(row, body_a, body_b);
            }
        }

        store_joint_body(*joint.a, body_a);
        store_joint_body(*joint.b, body_b);
        ++joints_solved_;
    }
}

void Physics2DSystem::note_joint_skipped(std::string_view type, const Uuid& body_a, const Uuid& body_b, Error error) {
    joint_diagnostics_.push_back(JointDiagnostic{type, body_a, body_b, std::move(error)});
}

bool Physics2DSystem::same_island(const Uuid& a, const Uuid& b) const {
    const auto index_a = body_index_.find(a);
    const auto index_b = body_index_.find(b);
    if (index_a == body_index_.end() || index_b == body_index_.end()) {
        return false;
    }
    const usize vertex_a = body_vertex_[index_a->second];
    const usize vertex_b = body_vertex_[index_b->second];
    if (vertex_a == kNoVertex || vertex_b == kNoVertex) {
        return false;
    }
    return islands_.same_island(vertex_a, vertex_b);
}

void Physics2DSystem::build_islands() {
    // Only dynamic bodies are island vertices: static geometry is collided with
    // but never conducts motion between the bodies resting on it, so a link to a
    // static body is dropped. That is what makes a crate on the left of a floor
    // and a crate on the right separate islands even though both touch it.
    island_vertex_body_.clear();
    body_vertex_.assign(bodies_.size(), kNoVertex);
    for (usize i = 0; i < bodies_.size(); ++i) {
        if (bodies_[i].inv_mass > 0.0f) {
            body_vertex_[i] = island_vertex_body_.size();
            island_vertex_body_.push_back(i);
        }
    }

    // Edges. A link is a pair of dynamic bodies whose bounds overlap (the broad
    // phase's notion of "may be touching") or a joint between two dynamic
    // bodies. The bounds test is deliberately conservative: it may join two
    // bodies that are near but not quite touching, which only makes sleeping
    // more conservative (the group wakes and sleeps together), never less safe.
    // Using bounds rather than the exact contact is what lets the graph be built
    // before the solver runs, so a moving body wakes a sleeper in the same step
    // it reaches it.
    //
    // This enumeration is separate from the solver's uniform-grid broad phase
    // (ADR-0023): the island graph is built before integration, over the
    // pre-step bounds, while the broad phase is built after it, over the
    // post-step bounds. It is O(n^2) and is the next thing to fold into the
    // broad phase if the two passes are ever brought to the same point in the
    // step. The order is bodies in array order, so the link order -- and
    // therefore the islands -- is deterministic.
    island_links_.clear();
    for (usize i = 0; i < bodies_.size(); ++i) {
        if (body_vertex_[i] == kNoVertex || bodies_[i].collider == nullptr) {
            continue;
        }
        const Aabb2D bounds_a = bodies_[i].collider->bounds(bodies_[i].position, bodies_[i].rotation);
        for (usize j = i + 1; j < bodies_.size(); ++j) {
            if (body_vertex_[j] == kNoVertex || bodies_[j].collider == nullptr) {
                continue;
            }
            const Aabb2D bounds_b = bodies_[j].collider->bounds(bodies_[j].position, bodies_[j].rotation);
            if (bounds_overlap(bounds_a, bounds_b)) {
                island_links_.push_back(IslandLink{body_vertex_[i], body_vertex_[j]});
            }
        }
    }

    // Joints are resolved here from the node index rather than from the Joint
    // struct's cached ends, which solve_joints fills later in the step. A joint
    // end that is nil, missing, or not a simulated dynamic body contributes no
    // link (a joint to an immovable pivot does not join two islands).
    for (const Joint& joint : joints_) {
        if (joint.distance == nullptr && joint.revolute == nullptr) {
            continue;
        }
        const Uuid& id_a = joint.distance != nullptr ? joint.distance->body_a : joint.revolute->body_a;
        const Uuid& id_b = joint.distance != nullptr ? joint.distance->body_b : joint.revolute->body_b;
        const auto index_a = body_index_.find(id_a);
        const auto index_b = body_index_.find(id_b);
        if (index_a == body_index_.end() || index_b == body_index_.end()) {
            continue;
        }
        const usize vertex_a = body_vertex_[index_a->second];
        const usize vertex_b = body_vertex_[index_b->second];
        if (vertex_a == kNoVertex || vertex_b == kNoVertex) {
            continue;
        }
        island_links_.push_back(IslandLink{vertex_a, vertex_b});
    }

    islands_.build(island_vertex_body_.size(), island_links_);

    // Seed the awake flags from the bodies' current state. A body that is awake
    // (including one that can never sleep) makes its island awake.
    island_awake_.assign(islands_.island_count(), false);
    for (usize vertex = 0; vertex < island_vertex_body_.size(); ++vertex) {
        if (!asleep_[island_vertex_body_[vertex]]) {
            island_awake_[islands_.island_of_body()[vertex]] = true;
        }
    }
}

void Physics2DSystem::wake_island(usize index) {
    if (index >= islands_.island_count()) {
        return;
    }
    island_awake_[index] = true;
    for (const usize vertex : islands_.islands()[index]) {
        const usize body = island_vertex_body_[vertex];
        if (asleep_[body]) {
            asleep_[body] = false;
            --sleeping_bodies_;
            // Clear the body's own sleeping flag and restart its rest timer. An
            // already-awake body in the island keeps its timer, so a body that
            // has been settling is not sent back to the start by a neighbour
            // being woken.
            if (bodies_[body].body != nullptr) {
                bodies_[body].body->wake();
            }
        }
    }
}

void Physics2DSystem::apply_wake_checks() {
    // A contact or a joint to an *awake* dynamic body wakes the island. The
    // island awake flags were seeded from the bodies above, so a flag is true
    // exactly when some body in it was already awake; here that wake is
    // propagated across the whole island, which is what wakes a sleeping body
    // joined to a moving one or resting against a moving one.
    //
    // Because the graph already links the pair that is touching this step, a
    // moving body wakes a sleeper before the solver runs: the solver can never
    // move an awake body into a still-sleeping one.
    for (usize island = 0; island < islands_.island_count(); ++island) {
        if (island_awake_[island]) {
            wake_island(island);
        }
    }
}

void Physics2DSystem::apply_sleep_checks(f32 delta_time) {
    for (usize island = 0; island < islands_.island_count(); ++island) {
        if (island_awake_[island]) {
            // An awake island advances each body's rest timer. A body that is
            // still moving resets its own timer; a body below the thresholds
            // accumulates. The island may only sleep when every body in it has
            // accumulated long enough and can_sleep.
            bool ready = true;
            for (const usize vertex : islands_.islands()[island]) {
                const usize body_index = island_vertex_body_[vertex];
                RigidBody2D* component = bodies_[body_index].body;
                if (component == nullptr) {
                    continue;
                }
                if (!component->can_sleep) {
                    ready = false;
                    component->sleep_timer = 0.0f;
                    continue;
                }
                // The write-back above has already synced the component's
                // velocity, so the component's own rest test is current.
                if (component->is_resting()) {
                    component->sleep_timer += delta_time;
                } else {
                    component->sleep_timer = 0.0f;
                }
                if (component->sleep_timer < component->sleep_time) {
                    ready = false;
                }
            }
            if (ready) {
                // Every body in the island has been still long enough: sleep the
                // whole island at once, so the group stops as a unit rather than
                // one body at a time.
                for (const usize vertex : islands_.islands()[island]) {
                    const usize body_index = island_vertex_body_[vertex];
                    RigidBody2D* component = bodies_[body_index].body;
                    if (component == nullptr) {
                        continue;
                    }
                    if (!asleep_[body_index]) {
                        asleep_[body_index] = true;
                        ++sleeping_bodies_;
                    }
                    component->sleep();
                }
                island_awake_[island] = false;
            }
        }
    }
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
