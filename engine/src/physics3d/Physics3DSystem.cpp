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

// The velocity a body at offset `r` from its centre contributes from an angular
// velocity `w`: `w x r`, the standard rigid-body point-velocity term. This is
// what couples a body's rotation into the relative velocity at a contact or a
// joint anchor.
[[nodiscard]] Vec3 angular_velocity_at(const Vec3& angular_velocity, const Vec3& r) noexcept {
    return angular_velocity.cross(r);
}

// Reads a node's current joint state: its transform for position and rotation,
// and its rigid body for velocity and the inverse mass and inertia the joint
// shares its correction by. A node with no rigid body is immovable, which is how
// a joint to static geometry (or a bare pivot) moves only its other end.
[[nodiscard]] JointBody3D make_joint_body(const Node& node) noexcept {
    JointBody3D joint;
    joint.position = node.transform().position;
    joint.orientation = Quat::from_euler(node.transform().rotation);
    const auto* body = node.get_component<RigidBody3D>();
    if (body == nullptr) {
        return joint;
    }
    joint.velocity = body->velocity;
    joint.angular_velocity = body->angular_velocity;
    joint.inv_mass = body->inverse_mass();
    if (joint.inv_mass > 0.0f && !body->fixed_rotation) {
        // Author-set inertia wins; otherwise derive it from the collider's
        // shape. A body with no collider falls back to the author's inertia, or
        // to zero (a point mass, which cannot spin on its own).
        f32 inertia = body->inertia;
        if (inertia <= 0.0f) {
            if (const auto* collider = node.get_component<Collider3D>(); collider != nullptr) {
                inertia = collider->inertia_about_center(body->mass);
            }
        }
        joint.inv_inertia = inertia > 0.0f ? 1.0f / inertia : 0.0f;
    }
    return joint;
}

// Writes a solved joint state back to the node's transform and rigid body. The
// orientation is written back as Euler angles, since that is what `Transform`
// stores; the boundary conversion keeps `Transform`'s API unchanged.
void store_joint_body(Node& node, const JointBody3D& joint) noexcept {
    node.transform().position = joint.position;
    node.transform().rotation = joint.orientation.to_euler();
    if (auto* body = node.get_component<RigidBody3D>(); body != nullptr) {
        body->velocity = joint.velocity;
        body->angular_velocity = joint.angular_velocity;
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
    body_index_.clear();
    sleeping_bodies_ = 0;
    asleep_.clear();

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
        entry.orientation = Quat::from_euler(node.transform().rotation);
        entry.angular_velocity = body != nullptr ? body->angular_velocity : Vec3{};
        entry.inv_mass = body != nullptr ? body->inverse_mass() : 0.0f;
        entry.restitution = collider != nullptr ? clamp_unit(collider->restitution) : 0.0f;
        entry.friction = collider != nullptr ? std::max(collider->friction, 0.0f) : 0.0f;
        entry.fixed_rotation = body != nullptr && body->fixed_rotation;

        // Rotational inertia. An author-set inertia on the body wins; otherwise
        // derive it from the collider's shape. A dynamic body with no collider
        // and no author inertia is a point mass, which has zero rotational
        // inertia and cannot spin on its own.
        entry.inv_inertia = 0.0f;
        if (body != nullptr && entry.inv_mass > 0.0f && !entry.fixed_rotation) {
            f32 inertia = body->inertia;
            if (inertia <= 0.0f && collider != nullptr) {
                inertia = collider->inertia_about_center(body->mass);
            }
            entry.inv_inertia = inertia > 0.0f ? 1.0f / inertia : 0.0f;
        }

        // A sleeping body resumes from rest: its authored velocity is ignored so
        // a body that was asleep when the scene loaded does not spring into
        // motion on the first step. Its island is re-derived below and the wake
        // pass decides whether it stays asleep.
        const bool sleeping = body != nullptr && body->is_sleeping && entry.inv_mass > 0.0f;
        if (sleeping) {
            entry.velocity = Vec3{};
            entry.angular_velocity = Vec3{};
        }

        bodies_.push_back(entry);
        body_index_.emplace(node.id(), bodies_.size() - 1);
        asleep_.push_back(sleeping);
        if (sleeping) {
            ++sleeping_bodies_;
        }
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
        // Torque is the rotational counterpart of gravity: a constant angular
        // acceleration of `torque * inverse_inertia`. A body with infinite
        // inertia (fixed_rotation, or no shape-derived inertia) ignores it.
        if (entry.inv_inertia > 0.0f) {
            entry.angular_velocity += entry.body->torque * (entry.inv_inertia * delta_time);
        }
        const f32 damping = clamp_unit(entry.body->linear_damping);
        if (damping > 0.0f) {
            // Frame-rate-independent decay: the velocity is multiplied by
            // (1 - damping) once per second, distributed across this frame.
            entry.velocity = entry.velocity * std::max(0.0f, 1.0f - damping * delta_time);
        }
        const f32 angular_damping = clamp_unit(entry.body->angular_damping);
        if (angular_damping > 0.0f) {
            entry.angular_velocity = entry.angular_velocity * std::max(0.0f, 1.0f - angular_damping * delta_time);
        }
    }

    // Integrate position from the updated velocities, and orientation from the
    // angular velocity. The orientation is a quaternion integrated in place; a
    // fixed-rotation body keeps its authored rotation regardless of its angular
    // velocity (the angular state is still carried so a later contact can read
    // it, but it never changes the transform).
    for (usize i = 0; i < bodies_.size(); ++i) {
        Body& entry = bodies_[i];
        if (entry.inv_mass <= 0.0f || asleep_[i]) {
            continue;
        }
        entry.position += entry.velocity * delta_time;
        if (!entry.fixed_rotation) {
            entry.orientation = entry.orientation.integrated(entry.angular_velocity, delta_time);
        }
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

            // Collision filtering. A pair whose layers and masks disagree is
            // ignored entirely: it never reaches the narrow phase, is never
            // recorded as touching, and so produces no contact event. "Ignored"
            // has to mean ignored for events too, or a game would get a begin
            // event for a pair it told the engine never to touch.
            if (!a.collider->can_collide_with(*b.collider)) {
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
            // scene is walked. A sleeping body's contacts are still recorded --
            // its position does not change, so they neither begin nor end, and
            // the event stream stays quiet -- but no impulse is applied to it
            // below.
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

            // A trigger is a sensor: the touching pair was recorded above (so
            // the contact-event pass reports Begin and End exactly as for a
            // solid pair), but no impulse and no positional correction is
            // applied, so bodies pass through it.
            if (a.collider->is_trigger || b.collider->is_trigger) {
                continue;
            }

            // A sleeping body is not impulsed: it is at rest until something
            // wakes it, which the wake pass did before this loop if an awake body
            // was touching it. Its contacts were still recorded above, so the
            // event stream does not change when it sleeps. Skipping here is what
            // makes a settled pile cost nothing in the solver.
            if (asleep_[bounds_owner_[candidate.first]] || asleep_[bounds_owner_[candidate.second]]) {
                continue;
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

            // The contact point, taken on A's surface along the normal (a
            // single-point approximation, standard for box and sphere contacts).
            // Angular impulses need the lever arm from each body centre to this
            // point; a contact through a centre produces no torque, which is what
            // keeps a body resting flat stable. The collider shape is
            // axis-aligned, so the support distance is the extent along the
            // normal with no orientation to account for.
            const Vec3 support = a.collider->shape == ShapeType3D::Sphere
                                     ? contact.normal * a.collider->radius
                                     : Vec3{std::abs(contact.normal.x) * a.collider->half_extent.x,
                                            std::abs(contact.normal.y) * a.collider->half_extent.y,
                                            std::abs(contact.normal.z) * a.collider->half_extent.z};
            const f32 support_distance = support.dot(contact.normal);
            const Vec3 contact_point = a.position + contact.normal * (support_distance - contact.penetration * 0.5f);

            // Lever arms from each body centre to the contact point. The body's
            // centre of mass is the node origin.
            const Vec3 ra = contact_point - a.position;
            const Vec3 rb = contact_point - b.position;

            // Relative velocity at the contact, including the contribution of
            // each body's rotation. `normal` points from A to B, so a separating
            // pair has a positive projection and needs no impulse.
            const Vec3 relative = (b.velocity + angular_velocity_at(b.angular_velocity, rb)) -
                                  (a.velocity + angular_velocity_at(a.angular_velocity, ra));
            const f32 normal_speed = relative.dot(contact.normal);
            if (normal_speed >= 0.0f) {
                continue;
            }

            // Effective mass along the normal, including the angular terms
            // (r x n)^2 * inv_inertia. This is what couples rotation into the
            // linear response.
            const Vec3 ra_cross_n = ra.cross(contact.normal);
            const Vec3 rb_cross_n = rb.cross(contact.normal);
            const f32 effective_mass = inv_mass_sum + a.inv_inertia * ra_cross_n.length_squared() +
                                       b.inv_inertia * rb_cross_n.length_squared();
            if (effective_mass <= 0.0f) {
                continue;
            }

            // Restitution of the pair is the larger of the two, which is the
            // conventional choice and keeps a bouncy ball bouncy against a dead
            // floor.
            const f32 restitution = std::max(a.restitution, b.restitution);
            const f32 impulse_magnitude = -(1.0f + restitution) * normal_speed / effective_mass;
            const Vec3 impulse = contact.normal * impulse_magnitude;

            a.velocity -= impulse * a.inv_mass;
            b.velocity += impulse * b.inv_mass;
            // Angular response: the impulse at a lever arm produces torque.
            a.angular_velocity -= ra.cross(impulse) * a.inv_inertia;
            b.angular_velocity += rb.cross(impulse) * b.inv_inertia;

            // Friction: an impulse opposing the tangential relative motion,
            // capped by the Coulomb cone. With angular dynamics it also spins the
            // bodies, which is what makes a box tumble and a ball roll rather
            // than slide.
            const Vec3 tangent_raw = relative - contact.normal * normal_speed;
            const f32 tangent_length = tangent_raw.length();
            if (tangent_length > 0.0f) {
                const Vec3 tangent = tangent_raw / tangent_length;
                const Vec3 ra_cross_t = ra.cross(tangent);
                const Vec3 rb_cross_t = rb.cross(tangent);
                const f32 tangent_mass = inv_mass_sum + a.inv_inertia * ra_cross_t.length_squared() +
                                         b.inv_inertia * rb_cross_t.length_squared();
                if (tangent_mass > 0.0f) {
                    f32 friction_impulse = -relative.dot(tangent) / tangent_mass;
                    // Geometric mean, matching the 2D solver and Box2D. The
                    // previous maximum-of-the-two rule made friction 0
                    // inexpressible, so a frictionless body inherited the other
                    // surface's friction and bled tangential speed on every
                    // glancing contact.
                    const f32 max_friction = std::sqrt(a.friction * b.friction) * impulse_magnitude;
                    friction_impulse = std::clamp(friction_impulse, -max_friction, max_friction);
                    const Vec3 friction = tangent * friction_impulse;
                    a.velocity -= friction * a.inv_mass;
                    b.velocity += friction * b.inv_mass;
                    a.angular_velocity -= ra.cross(friction) * a.inv_inertia;
                    b.angular_velocity += rb.cross(friction) * b.inv_inertia;
                }
            }
        }
    }

    // Write back. The transform owns the node's position and rotation; the rigid
    // body owns its velocities. Writing all of them here is what keeps the two in
    // sync, exactly as the animation system writes UVs back onto the sprite. The
    // orientation is written as Euler angles, the boundary conversion that keeps
    // Transform's API unchanged.
    for (usize i = 0; i < bodies_.size(); ++i) {
        Body& entry = bodies_[i];
        if (entry.inv_mass <= 0.0f) {
            continue;
        }
        if (asleep_[i]) {
            // A sleeping body is at rest. Keep its stored velocity zero, so the
            // component never carries a stale speed into a wake, and leave its
            // transform exactly where it is -- nothing moves while it sleeps.
            entry.body->velocity = Vec3{};
            entry.body->angular_velocity = Vec3{};
            continue;
        }
        entry.body->velocity = entry.velocity;
        entry.body->angular_velocity = entry.angular_velocity;
        Node* node = entry.node;
        if (node != nullptr) {
            node->transform().position = entry.position;
            if (!entry.fixed_rotation) {
                node->transform().rotation = entry.orientation.to_euler();
            }
        }
    }

    // Joints. Solved after the contact pass and the write-back, so a joint
    // correction is the last word on position: a body pinned to a pivot stays
    // pinned even while it also rests on a collider. A joint writes its own node
    // transforms and velocities.
    solve_joints(scene, delta_time);

    // Sleep checks last, on the final velocities: an island that has been still
    // for long enough sleeps, so the next step skips it. A body that is already
    // asleep is left asleep; the wake pass is what brings it back.
    apply_sleep_checks(delta_time);

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
        // A sleeping end behaves as an immovable one: the joint is still enforced
        // -- so joint_count() reports it -- but it cannot move the sleeper. The
        // wake pass already woke the island if the other end was moving, so a
        // joint is only one-sided here when the whole island is asleep, in which
        // case there is nothing to correct. This keeps a pendulum that has come
        // to rest reporting its constraint rather than vanishing from the count.
        if (const auto* body = joint.a->get_component<RigidBody3D>(); body != nullptr && body->is_sleeping) {
            body_a.inv_mass = 0.0f;
            body_a.inv_inertia = 0.0f;
        }
        if (const auto* body = joint.b->get_component<RigidBody3D>(); body != nullptr && body->is_sleeping) {
            body_b.inv_mass = 0.0f;
            body_b.inv_inertia = 0.0f;
        }
        if (body_a.inv_mass <= 0.0f && body_b.inv_mass <= 0.0f) {
            // Two immovable ends: the constraint can never be satisfied by moving
            // anything, so leave the world alone. It is still counted -- it was
            // gathered and is not broken -- exactly as in 2D.
            ++joints_solved_;
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

void Physics3DSystem::build_islands() {
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
    // This enumeration is separate from the solver's uniform-grid broad phase:
    // the island graph is built before integration, over the pre-step bounds,
    // while the broad phase is built after it, over the post-step bounds. It is
    // O(n^2) and is the next thing to fold into the broad phase if the two
    // passes are ever brought to the same point in the step. The order is bodies
    // in array order, so the link order -- and therefore the islands -- is
    // deterministic.
    island_links_.clear();
    for (usize i = 0; i < bodies_.size(); ++i) {
        if (body_vertex_[i] == kNoVertex || bodies_[i].collider == nullptr) {
            continue;
        }
        const Aabb3D bounds_a = bodies_[i].collider->bounds(bodies_[i].position);
        for (usize j = i + 1; j < bodies_.size(); ++j) {
            if (body_vertex_[j] == kNoVertex || bodies_[j].collider == nullptr) {
                continue;
            }
            const Aabb3D bounds_b = bodies_[j].collider->bounds(bodies_[j].position);
            if (bounds_overlap(bounds_a, bounds_b)) {
                island_links_.push_back(physics::IslandLink{body_vertex_[i], body_vertex_[j]});
            }
        }
    }

    // Joints link two dynamic bodies. An end that is nil, missing, or not a
    // simulated body contributes no link, so a joint to an immovable pivot does
    // not join two islands.
    for (const Joint& joint : joints_) {
        if (joint.distance == nullptr) {
            continue;
        }
        const auto index_a = body_index_.find(joint.distance->body_a);
        const auto index_b = body_index_.find(joint.distance->body_b);
        if (index_a == body_index_.end() || index_b == body_index_.end()) {
            continue;
        }
        const usize vertex_a = body_vertex_[index_a->second];
        const usize vertex_b = body_vertex_[index_b->second];
        if (vertex_a == kNoVertex || vertex_b == kNoVertex) {
            continue;
        }
        island_links_.push_back(physics::IslandLink{vertex_a, vertex_b});
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

void Physics3DSystem::wake_island(usize index) {
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

void Physics3DSystem::apply_wake_checks() {
    // A contact or a joint to an *awake* dynamic body wakes the island. The
    // island awake flags were seeded from the bodies in `build_islands`, so a
    // flag is true exactly when some body in it was already awake; here that
    // wake is propagated across the whole island, which is what wakes a sleeping
    // body joined to a moving one or resting against a moving one.
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

void Physics3DSystem::apply_sleep_checks(f32 delta_time) {
    for (usize island = 0; island < islands_.island_count(); ++island) {
        if (!island_awake_[island]) {
            continue;
        }
        // An awake island advances each body's rest timer. A body that is still
        // moving resets its own timer; a body below the thresholds accumulates.
        // The island may only sleep when every body in it has accumulated long
        // enough and can_sleep.
        bool ready = true;
        for (const usize vertex : islands_.islands()[island]) {
            const usize body_index = island_vertex_body_[vertex];
            RigidBody3D* component = bodies_[body_index].body;
            if (component == nullptr) {
                continue;
            }
            if (!component->can_sleep) {
                ready = false;
                component->sleep_timer = 0.0f;
                continue;
            }
            // The write-back above has already synced the component's velocity,
            // so the component's own rest test is current.
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
            // whole island at once, so the group stops as a unit rather than one
            // body at a time.
            for (const usize vertex : islands_.islands()[island]) {
                const usize body_index = island_vertex_body_[vertex];
                RigidBody3D* component = bodies_[body_index].body;
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

bool Physics3DSystem::same_island(const Uuid& a, const Uuid& b) const {
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
