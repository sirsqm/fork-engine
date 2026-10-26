// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/physics3d/BroadPhase3D.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/DistanceJoint3D.h"
#include "fe/physics3d/Geometry3D.h"
#include "fe/physics3d/JointSolver3D.h"
#include "fe/physics3d/Physics3DQueries.h"
#include "fe/physics3d/Physics3DSystem.h"
#include "fe/physics3d/Physics3DTypes.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fe;

namespace {

physics3d::Aabb3D box(f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz) {
    return physics3d::Aabb3D{Vec3{cx, cy, cz}, Vec3{hx, hy, hz}};
}

physics3d::Sphere3D sphere(f32 cx, f32 cy, f32 cz, f32 radius) {
    return physics3d::Sphere3D{Vec3{cx, cy, cz}, radius};
}

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A static floor: a box collider, no rigid body. Its top face is at y = 0 (the
// box is centred at y = -0.5, half a unit thick), so a body resting on it has a
// centre at y = half_extent.
Node* add_floor(Scene& scene, f32 half) {
    Node* node = scene.create_node("Floor");
    auto* collider = node->add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Box;
    collider->half_extent = Vec3{half, 0.5f, half};
    node->transform().position = Vec3{0.0f, -0.5f, 0.0f};
    return node;
}

// A dynamic box body.
Node* add_dynamic_box(Scene& scene, const Vec3& position, f32 half, f32 mass) {
    Node* node = scene.create_node("Body");
    auto* collider = node->add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Box;
    collider->half_extent = Vec3{half, half, half};
    auto* body = node->add_component<physics3d::RigidBody3D>();
    body->mass = mass;
    node->transform().position = position;
    return node;
}

// A dynamic sphere body.
Node* add_dynamic_sphere(Scene& scene, const Vec3& position, f32 radius, f32 mass) {
    Node* node = scene.create_node("Ball");
    auto* collider = node->add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Sphere;
    collider->radius = radius;
    auto* body = node->add_component<physics3d::RigidBody3D>();
    body->mass = mass;
    node->transform().position = position;
    return node;
}

// Runs `steps` fixed steps of `delta` through a schedule-less system.
void run(physics3d::Physics3DSystem& system, Scene& scene, f32 delta, int steps) {
    for (int i = 0; i < steps; ++i) {
        system.update(step(delta), scene);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pure geometry: box/box, sphere/sphere, sphere/box.
// ---------------------------------------------------------------------------

TEST_CASE("separated boxes do not overlap") {
    const auto contact = physics3d::intersect(box(0, 0, 0, 0.5f, 0.5f, 0.5f), box(2, 0, 0, 0.5f, 0.5f, 0.5f));
    CHECK_FALSE(contact.overlapping);
}

TEST_CASE("boxes touching exactly are not overlapping") {
    const auto contact = physics3d::intersect(box(0, 0, 0, 0.5f, 0.5f, 0.5f), box(1, 0, 0, 0.5f, 0.5f, 0.5f));
    CHECK_FALSE(contact.overlapping);
}

TEST_CASE("overlapping boxes report the smallest axis as the contact normal") {
    // Overlap 0.2 on x, 1.0 on y and z: the normal is +x.
    const auto contact = physics3d::intersect(box(0, 0, 0, 0.5f, 0.5f, 0.5f), box(0.8f, 0, 0, 0.5f, 0.5f, 0.5f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(1.0f));
    CHECK(contact.normal.y == doctest::Approx(0.0f));
    CHECK(contact.normal.z == doctest::Approx(0.0f));
    CHECK(contact.penetration == doctest::Approx(0.2f));
}

TEST_CASE("a box normal points from the first box towards the second") {
    const auto contact = physics3d::intersect(box(0.8f, 0, 0, 0.5f, 0.5f, 0.5f), box(0, 0, 0, 0.5f, 0.5f, 0.5f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(-1.0f));
}

TEST_CASE("boxes stacked in z report a z normal") {
    const auto contact = physics3d::intersect(box(0, 0, 0, 0.5f, 0.5f, 0.5f), box(0, 0, 0.8f, 0.5f, 0.5f, 0.5f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.z == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(0.2f));
}

TEST_CASE("separated spheres do not overlap") {
    CHECK_FALSE(physics3d::intersect(sphere(0, 0, 0, 1.0f), sphere(3, 0, 0, 1.0f)).overlapping);
}

TEST_CASE("overlapping spheres report the distance along the centre line") {
    const auto contact = physics3d::intersect(sphere(0, 0, 0, 1.0f), sphere(1.5f, 0, 0, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.penetration == doctest::Approx(0.5f));
    CHECK(contact.normal.x == doctest::Approx(1.0f));
}

TEST_CASE("concentric spheres still produce a usable normal") {
    const auto contact = physics3d::intersect(sphere(0, 0, 0, 1.0f), sphere(0, 0, 0, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.length() == doctest::Approx(1.0f));
}

TEST_CASE("a sphere against a box face reports a face normal") {
    // Sphere to the right of the box, overlapping the +x face. The box face is
    // at x = 0.5, so a sphere of radius 0.5 centred at 0.8 penetrates by 0.2.
    const auto contact = physics3d::intersect(sphere(0.8f, 0, 0, 0.5f), box(0, 0, 0, 0.5f, 0.5f, 0.5f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(-1.0f));
    CHECK(contact.penetration == doctest::Approx(0.2f));
}

TEST_CASE("a sphere near a box corner reports a diagonal normal") {
    // Sphere past the +x+y+z corner. The normal points from the sphere towards
    // the box, so every component is negative.
    const auto contact = physics3d::intersect(sphere(0.7f, 0.7f, 0.7f, 0.5f), box(0, 0, 0, 0.5f, 0.5f, 0.5f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x < 0.0f);
    CHECK(contact.normal.y < 0.0f);
    CHECK(contact.normal.z < 0.0f);
    CHECK(contact.normal.length() == doctest::Approx(1.0f));
}

TEST_CASE("intersect_shapes flips the normal so it always points from the first shape") {
    const auto sphere_first = physics3d::intersect_shapes(
        physics3d::ShapeType3D::Sphere, box(0, 0, 0, 0.5f, 0.5f, 0.5f), sphere(0.8f, 0, 0, 0.5f),
        physics3d::ShapeType3D::Box, box(0, 0, 0, 0.5f, 0.5f, 0.5f), sphere(0.8f, 0, 0, 0.5f));
    REQUIRE(sphere_first.overlapping);
    // From the sphere (at +x) towards the box (at origin): -x.
    CHECK(sphere_first.normal.x == doctest::Approx(-1.0f));

    const auto box_first = physics3d::intersect_shapes(physics3d::ShapeType3D::Box, box(0, 0, 0, 0.5f, 0.5f, 0.5f),
                                                       sphere(0.8f, 0, 0, 0.5f), physics3d::ShapeType3D::Sphere,
                                                       box(0, 0, 0, 0.5f, 0.5f, 0.5f), sphere(0.8f, 0, 0, 0.5f));
    REQUIRE(box_first.overlapping);
    // From the box towards the sphere: +x.
    CHECK(box_first.normal.x == doctest::Approx(1.0f));
}

TEST_CASE("bounds_overlap is a conservative prefilter") {
    const physics3d::Aabb3D a = box(0, 0, 0, 1, 1, 1);
    CHECK(physics3d::bounds_overlap(a, box(1.5f, 0, 0, 1, 1, 1)));
    CHECK_FALSE(physics3d::bounds_overlap(a, box(2.5f, 0, 0, 1, 1, 1)));
    CHECK(physics3d::bounds_overlap(a, sphere(1.5f, 0, 0, 1.0f)));
}

TEST_CASE("raycast_aabb reports the distance to the near face") {
    const physics3d::Aabb3D b = box(0, 0, 0, 1, 1, 1);
    const f32 distance = physics3d::raycast_aabb(Vec3{-5, 0, 0}, Vec3{1, 0, 0}, b);
    CHECK(distance == doctest::Approx(4.0f));
}

TEST_CASE("raycast_aabb misses when the ray points away") {
    const physics3d::Aabb3D b = box(0, 0, 0, 1, 1, 1);
    CHECK(physics3d::raycast_aabb(Vec3{5, 0, 0}, Vec3{1, 0, 0}, b) < 0.0f);
}

TEST_CASE("a ray that starts inside a shape hits at distance zero") {
    CHECK(physics3d::raycast_aabb(Vec3{0, 0, 0}, Vec3{1, 0, 0}, box(0, 0, 0, 1, 1, 1)) == doctest::Approx(0.0f));
    CHECK(physics3d::raycast_sphere(Vec3{0, 0, 0}, Vec3{1, 0, 0}, sphere(0, 0, 0, 1)) == doctest::Approx(0.0f));
}

TEST_CASE("raycast_sphere reports the near intersection") {
    CHECK(physics3d::raycast_sphere(Vec3{-5, 0, 0}, Vec3{1, 0, 0}, sphere(0, 0, 0, 1)) == doctest::Approx(4.0f));
}

TEST_CASE("the raycast normal points back towards the origin") {
    const physics3d::Aabb3D b = box(0, 0, 0, 1, 1, 1);
    const f32 distance = physics3d::raycast_aabb(Vec3{-5, 0, 0}, Vec3{1, 0, 0}, b);
    const Vec3 normal = physics3d::raycast_aabb_normal(Vec3{-5, 0, 0}, Vec3{1, 0, 0}, b, distance);
    CHECK(normal.x == doctest::Approx(-1.0f));
}

// ---------------------------------------------------------------------------
// Broad phase.
// ---------------------------------------------------------------------------

TEST_CASE("the 3D broad phase finds overlapping pairs and never drops one") {
    // Three boxes: 0 and 1 overlap, 2 is far away.
    std::vector<physics3d::Aabb3D> bounds = {
        box(0, 0, 0, 0.5f, 0.5f, 0.5f),
        box(0.5f, 0, 0, 0.5f, 0.5f, 0.5f),
        box(50, 0, 0, 0.5f, 0.5f, 0.5f),
    };
    physics3d::BroadPhase3D broad_phase;
    broad_phase.build(bounds);

    // The pair (0, 1) must be present; nothing may pair with 2.
    bool has_01 = false;
    for (const auto& pair : broad_phase.pairs()) {
        if (pair.first == 0 && pair.second == 1) {
            has_01 = true;
        }
        CHECK(pair.second != 2);
    }
    CHECK(has_01);

    // Every candidate must at least pass the bounds prefilter (the grid is a
    // superset, so we assert it contains the true pairs, not that it equals
    // them).
    for (const auto& pair : broad_phase.pairs()) {
        CHECK(physics3d::bounds_overlap(bounds[pair.first], bounds[pair.second]));
    }
}

TEST_CASE("the 3D broad phase matches a brute-force reference on a grid") {
    std::vector<physics3d::Aabb3D> bounds;
    for (int x = 0; x < 5; ++x) {
        for (int y = 0; y < 5; ++y) {
            for (int z = 0; z < 5; ++z) {
                bounds.push_back(box(static_cast<f32>(x) * 1.5f, static_cast<f32>(y) * 1.5f, static_cast<f32>(z) * 1.5f,
                                     0.6f, 0.6f, 0.6f));
            }
        }
    }
    physics3d::BroadPhase3D broad_phase;
    broad_phase.build(bounds);

    // Build the true overlap set by brute force.
    std::vector<std::pair<usize, usize>> truth;
    for (usize i = 0; i < bounds.size(); ++i) {
        for (usize j = i + 1; j < bounds.size(); ++j) {
            if (physics3d::bounds_overlap(bounds[i], bounds[j])) {
                truth.emplace_back(i, j);
            }
        }
    }
    std::sort(truth.begin(), truth.end());

    std::vector<std::pair<usize, usize>> candidates;
    for (const auto& pair : broad_phase.pairs()) {
        candidates.emplace_back(pair.first, pair.second);
    }
    // The grid is a superset: every true pair is a candidate.
    for (const auto& pair : truth) {
        CHECK(std::find(candidates.begin(), candidates.end(), pair) != candidates.end());
    }
}

TEST_CASE("an empty 3D broad phase yields no candidates") {
    physics3d::BroadPhase3D broad_phase;
    broad_phase.build({});
    CHECK(broad_phase.candidate_count() == 0);
}

// ---------------------------------------------------------------------------
// The solver: gravity, landing, restitution, stacking, static geometry.
// ---------------------------------------------------------------------------

TEST_CASE("gravity moves a dynamic body downwards") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 10, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    run(system, scene, 0.1f, 1);

    CHECK(node->transform().position.y < 10.0f);
    const auto* body = node->get_component<physics3d::RigidBody3D>();
    CHECK(body->velocity.y < 0.0f);
}

TEST_CASE("a body lands on a static floor and does not fall through") {
    Scene scene;
    add_floor(scene, 10.0f);
    Node* node = add_dynamic_box(scene, Vec3{0, 5, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 300);

    // The box half-extent is 0.5 and the floor top is at y = 0, so a resting
    // body settles at about y = 0.5 (within the penetration slop).
    CHECK(node->transform().position.y > 0.4f);
    CHECK(node->transform().position.y < 0.6f);
}

TEST_CASE("a static collider with no rigid body never moves") {
    Scene scene;
    Node* floor = add_floor(scene, 10.0f);
    add_dynamic_box(scene, Vec3{0, 5, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 300);

    CHECK(floor->transform().position.y == doctest::Approx(-0.5f));
    CHECK(floor->transform().position.x == doctest::Approx(0.0f));
}

TEST_CASE("a bouncy body reverses its vertical velocity on impact") {
    Scene scene;
    add_floor(scene, 10.0f);
    Node* node = add_dynamic_box(scene, Vec3{0, 2, 0}, 0.5f, 1.0f);
    node->get_component<physics3d::Collider3D>()->restitution = 1.0f;
    physics3d::Physics3DSystem system;

    // Fall until it is about to hit, then step once and check it bounces up.
    bool bounced = false;
    for (int i = 0; i < 400; ++i) {
        const f32 before = node->get_component<physics3d::RigidBody3D>()->velocity.y;
        system.update(step(1.0f / 60.0f), scene);
        const f32 after = node->get_component<physics3d::RigidBody3D>()->velocity.y;
        if (before < 0.0f && after > 0.0f) {
            bounced = true;
            break;
        }
    }
    CHECK(bounced);
}

TEST_CASE("a dead body does not bounce") {
    Scene scene;
    add_floor(scene, 10.0f);
    Node* node = add_dynamic_box(scene, Vec3{0, 2, 0}, 0.5f, 1.0f);
    node->get_component<physics3d::Collider3D>()->restitution = 0.0f;
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 400);

    // It has come to rest on the floor and is not moving.
    CHECK(node->transform().position.y > 0.4f);
    CHECK(node->transform().position.y < 0.6f);
    CHECK(std::abs(node->get_component<physics3d::RigidBody3D>()->velocity.y) < 0.5f);
}

TEST_CASE("two dynamic bodies pushed together separate") {
    Scene scene;
    Node* a = add_dynamic_box(scene, Vec3{-0.4f, 0, 0}, 0.5f, 1.0f);
    Node* b = add_dynamic_box(scene, Vec3{0.4f, 0, 0}, 0.5f, 1.0f);
    a->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    b->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 30);

    // They were overlapping by 0.2; after resolving they are at least touching.
    CHECK(b->transform().position.x - a->transform().position.x >= 1.0f - 0.01f);
}

TEST_CASE("a sphere rests on a box floor") {
    Scene scene;
    add_floor(scene, 10.0f);
    Node* ball = add_dynamic_sphere(scene, Vec3{0, 3, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 300);

    CHECK(ball->transform().position.y > 0.4f);
    CHECK(ball->transform().position.y < 0.6f);
}

TEST_CASE("the system reports the contacts it resolved") {
    Scene scene;
    add_floor(scene, 10.0f);
    add_dynamic_box(scene, Vec3{0, 0.4f, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 5);

    CHECK(system.contact_count() >= 1);
}

TEST_CASE("a contact event is emitted when a body lands") {
    Scene scene;
    add_floor(scene, 10.0f);
    add_dynamic_box(scene, Vec3{0, 1.0f, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    bool began = false;
    for (int i = 0; i < 200 && !began; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        for (const auto& event : system.contact_events()) {
            if (event.phase == physics3d::ContactPhase::Begin) {
                began = true;
            }
        }
    }
    CHECK(began);
}

TEST_CASE("a non-positive delta time is a no-op") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 10, 0}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    system.update(step(0.0f), scene);
    CHECK(node->transform().position.y == doctest::Approx(10.0f));
    CHECK(system.contact_count() == 0);
}

TEST_CASE("an empty scene updates cleanly") {
    Scene scene;
    physics3d::Physics3DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.contact_count() == 0);
    CHECK(system.contact_event_count() == 0);
}

TEST_CASE("a kinematic body ignores gravity and stays put") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 5, 0}, 0.5f, 1.0f);
    node->get_component<physics3d::RigidBody3D>()->kinematic = true;
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 60);
    CHECK(node->transform().position.y == doctest::Approx(5.0f));
}

TEST_CASE("linear damping decays a body's speed") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;
    body->velocity = Vec3{10, 0, 0};
    body->linear_damping = 0.5f;
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 60);
    CHECK(body->velocity.x < 10.0f);
    CHECK(body->velocity.x > 0.0f);
}

// ---------------------------------------------------------------------------
// Collision filtering and triggers.
// ---------------------------------------------------------------------------

TEST_CASE("the 3D layer/mask rule is symmetric and default-open") {
    physics3d::Collider3D a;
    physics3d::Collider3D b;
    CHECK(a.can_collide_with(b));

    a.layer = 0x1u;
    a.mask = 0x1u;
    b.layer = 0x2u;
    b.mask = 0x2u;
    CHECK_FALSE(a.can_collide_with(b));

    b.mask = 0xFFFFFFFFu;
    a.mask = 0xFFFFFFFFu;
    CHECK(a.can_collide_with(b));
}

TEST_CASE("a filtered 3D pair passes through and reports no contact") {
    Scene scene;
    Node* floor = add_floor(scene, 20.0f);
    Node* body = add_dynamic_box(scene, Vec3{0.0f, 5.0f, 0.0f}, 0.5f, 1.0f);

    floor->get_component<physics3d::Collider3D>()->layer = 0x1u;
    floor->get_component<physics3d::Collider3D>()->mask = 0x1u;
    body->get_component<physics3d::Collider3D>()->layer = 0x2u;
    body->get_component<physics3d::Collider3D>()->mask = 0x2u;

    physics3d::Physics3DSystem system;
    run(system, scene, 1.0f / 60.0f, 120);

    CHECK(body->transform().position.y < -1.0f);
    CHECK(system.contact_count() == 0);
    CHECK(system.contact_event_count() == 0);
}

TEST_CASE("a 3D trigger reports a contact but applies no impulse") {
    Scene scene;
    Node* floor = add_floor(scene, 20.0f);
    Node* body = add_dynamic_box(scene, Vec3{0.0f, 5.0f, 0.0f}, 0.5f, 1.0f);
    floor->get_component<physics3d::Collider3D>()->is_trigger = true;

    physics3d::Physics3DSystem system;
    bool saw_begin = false;
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        for (const physics3d::ContactEvent& event : system.contact_events()) {
            if (event.phase == physics3d::ContactPhase::Begin) {
                saw_begin = true;
            }
        }
    }

    CHECK(body->transform().position.y < -1.0f);
    CHECK(saw_begin);
}

// ---------------------------------------------------------------------------
// Queries.
// ---------------------------------------------------------------------------

TEST_CASE("scene raycast returns the nearest collider and its hit point") {
    Scene scene;
    add_dynamic_box(scene, Vec3{5, 0, 0}, 1.0f, 1.0f);
    add_dynamic_box(scene, Vec3{10, 0, 0}, 1.0f, 1.0f);

    const physics3d::RaycastHit hit = physics3d::raycast(scene, Vec3{0, 0, 0}, Vec3{1, 0, 0});
    REQUIRE(hit.hit);
    CHECK(hit.distance == doctest::Approx(4.0f));
    CHECK(hit.point.x == doctest::Approx(4.0f));
}

TEST_CASE("scene raycast respects a max distance") {
    Scene scene;
    add_dynamic_box(scene, Vec3{5, 0, 0}, 1.0f, 1.0f);
    const physics3d::RaycastHit hit = physics3d::raycast(scene, Vec3{0, 0, 0}, Vec3{1, 0, 0}, 2.0f);
    CHECK_FALSE(hit.hit);
}

TEST_CASE("overlap_sphere finds colliders inside the query") {
    Scene scene;
    add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    add_dynamic_box(scene, Vec3{10, 0, 0}, 0.5f, 1.0f);

    std::vector<physics3d::OverlapHit> hits;
    const usize count = physics3d::overlap_sphere(scene, Vec3{0.5f, 0, 0}, 0.5f, hits);
    CHECK(count == 1);
    CHECK(hits.size() == 1);
}

TEST_CASE("overlap_point reports a collider containing the point") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 1.0f, 1.0f);
    physics3d::OverlapHit hit;
    CHECK(physics3d::overlap_point(scene, Vec3{0.2f, 0.2f, 0.2f}, &hit));
    CHECK(hit.node == node);
    CHECK_FALSE(physics3d::overlap_point(scene, Vec3{5, 5, 5}, nullptr));
}

// ---------------------------------------------------------------------------
// Reflection and serialization.
// ---------------------------------------------------------------------------

TEST_CASE("the 3D physics components are registered with reflection") {
    reflect::register_physics3d_types();
    const reflect::Registry& registry = reflect::Registry::instance();
    CHECK(registry.contains("Collider3D"));
    CHECK(registry.contains("RigidBody3D"));
}

TEST_CASE("a 3D collider and rigid body round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics3d_types();

    Scene scene;
    Node* node = scene.create_node("Crate");
    auto* collider = node->add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Sphere;
    collider->radius = 1.25f;
    collider->half_extent = Vec3{1.0f, 2.0f, 3.0f};
    collider->restitution = 0.4f;
    collider->friction = 0.2f;
    auto* body = node->add_component<physics3d::RigidBody3D>();
    body->velocity = Vec3{3.0f, -2.0f, 1.5f};
    body->mass = 2.5f;
    body->kinematic = true;
    body->affected_by_gravity = false;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_node = loaded.find_node_by_name("Crate");
    REQUIRE(loaded_node != nullptr);

    const auto* loaded_collider = loaded_node->get_component<physics3d::Collider3D>();
    REQUIRE(loaded_collider != nullptr);
    CHECK(loaded_collider->shape == physics3d::ShapeType3D::Sphere);
    CHECK(loaded_collider->radius == doctest::Approx(1.25f));
    CHECK(loaded_collider->half_extent.y == doctest::Approx(2.0f));
    CHECK(loaded_collider->restitution == doctest::Approx(0.4f));
    CHECK(loaded_collider->friction == doctest::Approx(0.2f));

    const auto* loaded_body = loaded_node->get_component<physics3d::RigidBody3D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->velocity.x == doctest::Approx(3.0f));
    CHECK(loaded_body->velocity.y == doctest::Approx(-2.0f));
    CHECK(loaded_body->velocity.z == doctest::Approx(1.5f));
    CHECK(loaded_body->mass == doctest::Approx(2.5f));
    CHECK(loaded_body->kinematic == true);
    CHECK(loaded_body->affected_by_gravity == false);
}

// ---------------------------------------------------------------------------
// Distance joints: the pure row, then the system wiring.
// ---------------------------------------------------------------------------

namespace {

physics3d::JointBody3D joint_body(f32 x, f32 y, f32 z, f32 inv_mass) {
    physics3d::JointBody3D body;
    body.position = Vec3{x, y, z};
    body.inv_mass = inv_mass;
    return body;
}

} // namespace

TEST_CASE("a distance row points its Jacobian from A to B") {
    const physics3d::JointBody3D a = joint_body(0, 0, 0, 1.0f);
    const physics3d::JointBody3D b = joint_body(2, 0, 0, 1.0f);
    physics3d::DistanceJoint3DParams params;
    params.rest_length = 2.0f;

    auto row = physics3d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    CHECK(row.value().jacobian_a.x == doctest::Approx(-1.0f));
    CHECK(row.value().jacobian_b.x == doctest::Approx(1.0f));
    // Two equal masses: the effective mass is 1 / (1 + 1) = 0.5.
    CHECK(row.value().effective_mass == doctest::Approx(0.5f));
    // A rigid rod with no error needs no positional correction.
    CHECK(row.value().position_error == doctest::Approx(0.0f));
}

TEST_CASE("a coincident distance joint is unsolvable") {
    const physics3d::JointBody3D a = joint_body(1, 1, 1, 1.0f);
    const physics3d::JointBody3D b = joint_body(1, 1, 1, 1.0f);
    physics3d::DistanceJoint3DParams params;
    CHECK_FALSE(physics3d::build_distance_row(a, b, params, 1.0f / 60.0f));
}

TEST_CASE("a distance joint between two static bodies is unsolvable") {
    const physics3d::JointBody3D a = joint_body(0, 0, 0, 0.0f);
    const physics3d::JointBody3D b = joint_body(1, 0, 0, 0.0f);
    physics3d::DistanceJoint3DParams params;
    CHECK_FALSE(physics3d::build_distance_row(a, b, params, 1.0f / 60.0f));
}

TEST_CASE("applying a distance row pulls a stretched joint back together") {
    physics3d::JointBody3D a = joint_body(0, 0, 0, 1.0f);
    physics3d::JointBody3D b = joint_body(3, 0, 0, 1.0f);
    physics3d::DistanceJoint3DParams params;
    params.rest_length = 1.0f;

    auto row = physics3d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    physics3d::apply_row(row.value(), a, b);
    physics3d::apply_row_position(row.value(), a, b);

    // Stretched to 3 with a rest length of 1: the rod pulls them together, so A
    // moves towards +x and B towards -x.
    CHECK(a.position.x > 0.0f);
    CHECK(b.position.x < 3.0f);
    CHECK((b.position - a.position).length() < 3.0f);
}

TEST_CASE("a distance joint holds a dynamic body at the pivot's distance") {
    Scene scene;
    Node* pivot = scene.create_node("Pivot");
    pivot->transform().position = Vec3{0, 0, 0};
    Node* bob = add_dynamic_sphere(scene, Vec3{2, 0, 0}, 0.25f, 1.0f);

    Node* joint_node = scene.create_node("Rope");
    auto* joint = joint_node->add_component<physics3d::DistanceJoint3D>();
    joint->body_a = pivot->id();
    joint->body_b = bob->id();
    joint->rest_length = 2.0f;

    physics3d::Physics3DSystem system;
    run(system, scene, 1.0f / 60.0f, 120);

    // The rod keeps the distance near the rest length while gravity swings the
    // bob down; the bob cannot simply fall away from the pivot.
    const f32 distance = (bob->transform().position - pivot->transform().position).length();
    CHECK(distance == doctest::Approx(2.0f).epsilon(0.1));
    CHECK(system.joint_count() == 1);
}

TEST_CASE("a joint with a nil body reference is skipped") {
    Scene scene;
    Node* joint_node = scene.create_node("Broken");
    joint_node->add_component<physics3d::DistanceJoint3D>();

    physics3d::Physics3DSystem system;
    run(system, scene, 1.0f / 60.0f, 10);
    CHECK(system.joint_count() == 0);
}

TEST_CASE("a joint connects a body to itself is skipped") {
    Scene scene;
    Node* body = add_dynamic_sphere(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    Node* joint_node = scene.create_node("Self");
    auto* joint = joint_node->add_component<physics3d::DistanceJoint3D>();
    joint->body_a = body->id();
    joint->body_b = body->id();

    physics3d::Physics3DSystem system;
    run(system, scene, 1.0f / 60.0f, 10);
    CHECK(system.joint_count() == 0);
}

TEST_CASE("a joint pulls on a body that has no collider") {
    // A bare pivot (no collider, no rigid body) and a colliderless dynamic mass
    // connected by a joint: the mass is simulated because it has a rigid body,
    // and the joint holds it.
    Scene scene;
    Node* pivot = scene.create_node("Pivot");
    pivot->transform().position = Vec3{0, 0, 0};
    Node* mass = scene.create_node("Mass");
    mass->transform().position = Vec3{4, 0, 0};
    auto* body = mass->add_component<physics3d::RigidBody3D>();
    body->mass = 1.0f;
    body->affected_by_gravity = false;

    Node* joint_node = scene.create_node("Rod");
    auto* joint = joint_node->add_component<physics3d::DistanceJoint3D>();
    joint->body_a = pivot->id();
    joint->body_b = mass->id();
    joint->rest_length = 1.0f;

    physics3d::Physics3DSystem system;
    run(system, scene, 1.0f / 60.0f, 120);

    CHECK(system.joint_count() == 1);
    CHECK((mass->transform().position - pivot->transform().position).length() == doctest::Approx(1.0f).epsilon(0.1));
}

TEST_CASE("a spring joint with stiffness is soft") {
    physics3d::JointBody3D a = joint_body(0, 0, 0, 0.0f);
    physics3d::JointBody3D b = joint_body(3, 0, 0, 1.0f);
    physics3d::DistanceJoint3DParams params;
    params.rest_length = 1.0f;
    params.stiffness = 2.0f;
    params.damping = 0.0f;

    auto row = physics3d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    // A spring acts through the velocity bias and leaves position to it, so it
    // carries no direct positional correction.
    CHECK(row.value().position_factor == doctest::Approx(0.0f));
    CHECK(row.value().bias != doctest::Approx(0.0f));
}

TEST_CASE("a distance joint round-trips through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics3d_types();

    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");
    Node* joint_node = scene.create_node("Joint");
    auto* joint = joint_node->add_component<physics3d::DistanceJoint3D>();
    joint->body_a = a->id();
    joint->body_b = b->id();
    joint->rest_length = 3.5f;
    joint->stiffness = 4.0f;
    joint->damping = 0.25f;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_joint = loaded.find_node_by_name("Joint");
    REQUIRE(loaded_joint != nullptr);
    const auto* loaded_component = loaded_joint->get_component<physics3d::DistanceJoint3D>();
    REQUIRE(loaded_component != nullptr);
    CHECK(loaded_component->body_a == a->id());
    CHECK(loaded_component->body_b == b->id());
    CHECK(loaded_component->rest_length == doctest::Approx(3.5f));
    CHECK(loaded_component->stiffness == doctest::Approx(4.0f));
    CHECK(loaded_component->damping == doctest::Approx(0.25f));
}

TEST_CASE("the 3D distance joint is registered with reflection") {
    reflect::register_physics3d_types();
    CHECK(reflect::Registry::instance().contains("DistanceJoint3D"));
}
