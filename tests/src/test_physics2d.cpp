// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Geometry.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <cmath>
#include <ostream>
#include <string>

using namespace fe;

namespace {

physics2d::Aabb2D box(f32 cx, f32 cy, f32 hx, f32 hy) {
    return physics2d::Aabb2D{Vec2{cx, cy}, Vec2{hx, hy}};
}

physics2d::Circle2D circle(f32 cx, f32 cy, f32 radius) {
    return physics2d::Circle2D{Vec2{cx, cy}, radius};
}

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A static floor: a box collider, no rigid body. Spans x in [-half_width,
// half_width], half a unit thick, with its top face at y = 0 (centre y = -0.5).
Node* add_floor(Scene& scene, f32 half_width) {
    Node* node = scene.create_node("Floor");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half_width, 0.5f};
    node->transform().position = Vec3{0.0f, -0.5f, 0.0f};
    return node;
}

// A dynamic box body.
Node* add_dynamic_box(Scene& scene, const Vec2& position, f32 half, f32 mass) {
    Node* node = scene.create_node("Body");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half, half};
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->mass = mass;
    node->transform().position = Vec3{position.x, position.y, 0.0f};
    return node;
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry: pure narrow-phase maths, tested without a scene.
// ---------------------------------------------------------------------------

TEST_CASE("separated boxes do not overlap") {
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(5.0f, 0.0f, 1.0f, 1.0f));
    CHECK_FALSE(contact.overlapping);
}

TEST_CASE("boxes touching exactly are not overlapping") {
    // A's right face is at x = 1, B's left face is at x = 1: zero depth.
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(2.0f, 0.0f, 1.0f, 1.0f));
    CHECK_FALSE(contact.overlapping);
}

TEST_CASE("overlapping boxes report the smallest axis as the contact normal") {
    // A spans x,y in [-1,1]. B spans x in [0.5,2.5] (overlap 0.5) and y in
    // [0.75,2.75] (overlap 0.25). The y axis is the separating axis, so the
    // normal is vertical and the penetration is the smaller y overlap.
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(1.5f, 1.75f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(0.0f));
    CHECK(contact.normal.y == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(0.25f));
}

TEST_CASE("a box normal points from the first box towards the second") {
    const auto contact = physics2d::intersect(box(0.0f, 0.0f, 1.0f, 1.0f), box(1.5f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(0.5f));
}

TEST_CASE("separated circles do not overlap") {
    CHECK_FALSE(physics2d::intersect(circle(0.0f, 0.0f, 1.0f), circle(5.0f, 0.0f, 1.0f)).overlapping);
}

TEST_CASE("circles touching exactly are not overlapping") {
    CHECK_FALSE(physics2d::intersect(circle(0.0f, 0.0f, 1.0f), circle(2.0f, 0.0f, 1.0f)).overlapping);
}

TEST_CASE("overlapping circles report the distance along the centre line") {
    const auto contact = physics2d::intersect(circle(0.0f, 0.0f, 2.0f), circle(3.0f, 0.0f, 2.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.x == doctest::Approx(1.0f));
    CHECK(contact.normal.y == doctest::Approx(0.0f));
    // radii sum 4, distance 3 -> penetration 1.
    CHECK(contact.penetration == doctest::Approx(1.0f));
}

TEST_CASE("concentric circles still produce a usable normal") {
    const auto contact = physics2d::intersect(circle(0.0f, 0.0f, 2.0f), circle(0.0f, 0.0f, 2.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.length() == doctest::Approx(1.0f));
    CHECK(contact.penetration == doctest::Approx(4.0f));
}

TEST_CASE("a circle against a box face reports a face normal") {
    // Box occupies x in [-1, 1], y in [-1, 1]. Circle sits above the top face.
    const auto contact = physics2d::intersect(circle(0.0f, 1.5f, 1.0f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    // Normal points from the circle towards the box: downwards.
    CHECK(contact.normal.x == doctest::Approx(0.0f));
    CHECK(contact.normal.y == doctest::Approx(-1.0f));
    CHECK(contact.penetration == doctest::Approx(0.5f));
}

TEST_CASE("a circle near a box corner reports a diagonal normal") {
    // Circle centred just outside the top-right corner (1, 1).
    const auto contact = physics2d::intersect(circle(1.5f, 1.5f, 1.0f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    const f32 inv = 1.0f / std::sqrt(2.0f);
    CHECK(contact.normal.x == doctest::Approx(-inv));
    CHECK(contact.normal.y == doctest::Approx(-inv));
}

TEST_CASE("a circle whose centre is inside a box is separated along the nearest face") {
    // Centre is 0.1 above the box centre, closer to the top face.
    const auto contact = physics2d::intersect(circle(0.0f, 0.1f, 0.5f), box(0.0f, 0.0f, 1.0f, 1.0f));
    REQUIRE(contact.overlapping);
    CHECK(contact.normal.y == doctest::Approx(1.0f));
    CHECK(contact.penetration > 0.0f);
}

TEST_CASE("intersect_shapes flips the normal so it always points from the first shape") {
    const auto a_box = box(0.0f, 0.0f, 1.0f, 1.0f);
    const auto b_circle = circle(0.0f, 1.5f, 1.0f);

    // Circle first: normal points from the circle down towards the box.
    const auto circle_first = physics2d::intersect_shapes(physics2d::ShapeType::Circle, a_box, b_circle,
                                                          physics2d::ShapeType::Box, a_box, b_circle);
    // Box first: the same pair, but the normal must point from the box up.
    const auto box_first = physics2d::intersect_shapes(physics2d::ShapeType::Box, a_box, b_circle,
                                                       physics2d::ShapeType::Circle, a_box, b_circle);

    REQUIRE(circle_first.overlapping);
    REQUIRE(box_first.overlapping);
    CHECK(circle_first.normal.y == doctest::Approx(-box_first.normal.y));
    CHECK(circle_first.penetration == doctest::Approx(box_first.penetration));
}

TEST_CASE("bounds_overlap is a conservative prefilter") {
    CHECK(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), box(1.5f, 0.0f, 1.0f, 1.0f)));
    CHECK_FALSE(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), box(3.0f, 0.0f, 1.0f, 1.0f)));
    CHECK(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), circle(1.5f, 0.0f, 1.0f)));
    CHECK_FALSE(physics2d::bounds_overlap(box(0.0f, 0.0f, 1.0f, 1.0f), circle(5.0f, 0.0f, 1.0f)));
}

// ---------------------------------------------------------------------------
// The system: integration and resolution against a scene.
// ---------------------------------------------------------------------------

TEST_CASE("gravity moves a dynamic body downwards") {
    Scene scene;
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 10.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 10; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(body_node->transform().position.y < 10.0f);
    CHECK(body_node->get_component<physics2d::RigidBody2D>()->velocity.y < 0.0f);
}

TEST_CASE("a body lands on a static floor and does not fall through") {
    Scene scene;
    add_floor(scene, 20.0f); // top face at y = 0
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 5.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 300; ++i) { // 5 seconds at 60 Hz
        system.update(step(1.0f / 60.0f), scene);
    }

    // The body's half-height is 0.5, so resting on the floor puts its centre
    // near y = 0.5. It must not have tunnelled below the floor.
    CHECK(body_node->transform().position.y > 0.4f);
    CHECK(body_node->transform().position.y < 0.7f);
    CHECK(std::abs(body_node->get_component<physics2d::RigidBody2D>()->velocity.y) < 0.5f);
}

TEST_CASE("a static collider with no rigid body never moves") {
    Scene scene;
    Node* floor_node = add_floor(scene, 5.0f);
    const Vec3 before = floor_node->transform().position;

    // Drop a heavy body onto it; the floor must stay put.
    add_dynamic_box(scene, Vec2{0.0f, 3.0f}, 0.5f, 100.0f);
    physics2d::Physics2DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(floor_node->transform().position.y == doctest::Approx(before.y));
    CHECK(floor_node->transform().position.x == doctest::Approx(before.x));
}

TEST_CASE("a bouncy body reverses its vertical velocity on impact") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 4.0f}, 0.5f, 1.0f);
    body_node->get_component<physics2d::Collider2D>()->restitution = 1.0f;
    // Give the floor bounciness too; the pair takes the larger restitution.
    scene.roots().front()->get_component<physics2d::Collider2D>()->restitution = 1.0f;

    physics2d::Physics2DSystem system;
    f32 max_upward = 0.0f;
    bool saw_downward = false;
    for (int i = 0; i < 200; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        const f32 vy = body_node->get_component<physics2d::RigidBody2D>()->velocity.y;
        if (vy < -0.1f) {
            saw_downward = true;
        }
        if (saw_downward && vy > max_upward) {
            max_upward = vy;
        }
    }

    CHECK(saw_downward);
    CHECK(max_upward > 1.0f); // it bounced back up with real speed
}

TEST_CASE("a dead body does not bounce") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 4.0f}, 0.5f, 1.0f);
    // Default restitution is 0 on both.
    physics2d::Physics2DSystem system;

    f32 max_upward = 0.0f;
    bool saw_downward = false;
    for (int i = 0; i < 200; ++i) {
        system.update(step(1.0f / 60.0f), scene);
        const f32 vy = body_node->get_component<physics2d::RigidBody2D>()->velocity.y;
        if (vy < -0.1f) {
            saw_downward = true;
        }
        if (saw_downward && vy > max_upward) {
            max_upward = vy;
        }
    }

    CHECK(saw_downward);
    CHECK(max_upward < 0.2f);
}

TEST_CASE("two dynamic bodies pushed together separate") {
    Scene scene;
    // Start overlapping on x by 0.5 (centres 0.5 apart, half-widths 0.5 each),
    // so the solver must push them apart. No gravity, so the only motion is the
    // separation.
    Node* left = add_dynamic_box(scene, Vec2{-0.25f, 0.0f}, 0.5f, 1.0f);
    Node* right = add_dynamic_box(scene, Vec2{0.25f, 0.0f}, 0.5f, 1.0f);

    physics2d::Physics2DSystem system;
    system.gravity = Vec2{0.0f, 0.0f};
    for (int i = 0; i < 120; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    const f32 gap = right->transform().position.x - left->transform().position.x;
    // Their faces must end up at least touching (separation ~1.0).
    CHECK(gap > 0.9f);
    // And they must have moved apart symmetrically (equal mass).
    CHECK(left->transform().position.x == doctest::Approx(-right->transform().position.x));
}

TEST_CASE("the system reports the contacts it resolved") {
    Scene scene;
    add_floor(scene, 20.0f);
    add_dynamic_box(scene, Vec2{0.0f, 0.5f}, 0.5f, 1.0f); // already resting on the floor

    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.contact_count() >= 1);
}

TEST_CASE("a non-positive delta time is a no-op") {
    Scene scene;
    Node* body_node = add_dynamic_box(scene, Vec2{0.0f, 10.0f}, 0.5f, 1.0f);
    const Vec3 before = body_node->transform().position;

    physics2d::Physics2DSystem system;
    system.update(step(0.0f), scene);
    system.update(step(-1.0f), scene);

    CHECK(body_node->transform().position.y == doctest::Approx(before.y));
    CHECK(system.contact_count() == 0);
}

TEST_CASE("an empty scene updates cleanly") {
    Scene scene;
    physics2d::Physics2DSystem system;
    system.update(step(1.0f / 60.0f), scene);
    CHECK(system.contact_count() == 0);
}

TEST_CASE("a collider with no rigid body does not move on its own") {
    Scene scene;
    Node* node = scene.create_node("Static");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->half_extent = Vec2{1.0f, 1.0f};
    node->transform().position = Vec3{3.0f, 4.0f, 0.0f};

    physics2d::Physics2DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(1.0f / 60.0f), scene);
    }

    CHECK(node->transform().position.y == doctest::Approx(4.0f));
}

// ---------------------------------------------------------------------------
// Reflection: the components persist and appear in the registry.
// ---------------------------------------------------------------------------

TEST_CASE("the physics components are registered with reflection") {
    reflect::register_physics_types();
    const reflect::Registry& registry = reflect::Registry::instance();
    CHECK(registry.contains("Collider2D"));
    CHECK(registry.contains("RigidBody2D"));
}

TEST_CASE("a collider and rigid body round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics_types();

    Scene scene;
    Node* node = scene.create_node("Crate");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Circle;
    collider->radius = 1.25f;
    collider->restitution = 0.4f;
    collider->friction = 0.2f;
    auto* body = node->add_component<physics2d::RigidBody2D>();
    body->velocity = Vec2{3.0f, -2.0f};
    body->mass = 2.5f;
    body->kinematic = true;
    body->affected_by_gravity = false;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_node = loaded.find_node_by_name("Crate");
    REQUIRE(loaded_node != nullptr);

    const auto* loaded_collider = loaded_node->get_component<physics2d::Collider2D>();
    REQUIRE(loaded_collider != nullptr);
    CHECK(loaded_collider->shape == physics2d::ShapeType::Circle);
    CHECK(loaded_collider->radius == doctest::Approx(1.25f));
    CHECK(loaded_collider->restitution == doctest::Approx(0.4f));
    CHECK(loaded_collider->friction == doctest::Approx(0.2f));

    const auto* loaded_body = loaded_node->get_component<physics2d::RigidBody2D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->velocity.x == doctest::Approx(3.0f));
    CHECK(loaded_body->velocity.y == doctest::Approx(-2.0f));
    CHECK(loaded_body->mass == doctest::Approx(2.5f));
    CHECK(loaded_body->kinematic == true);
    CHECK(loaded_body->affected_by_gravity == false);
}
