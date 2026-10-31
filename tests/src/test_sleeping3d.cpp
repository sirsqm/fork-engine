// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Sleeping and island detection in 3D: the shared pure island builder (driven
// through its `fe::physics` names), and Physics3DSystem's sleep/wake behaviour.
// Everything here is deterministic -- no wall-clock thresholds, no frame rates
// -- so it runs headless and identically on CI.
//
// The 2D counterpart lives in test_sleeping.cpp; the graph itself is the one
// shared `fe::physics::IslandBuilder` (ADR-0111), so this file covers the 3D
// system's use of it and the pure graph's contract through the shared names.

#include <doctest/doctest.h>

#include "fe/physics/IslandSolver.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/DistanceJoint3D.h"
#include "fe/physics3d/Physics3DSystem.h"
#include "fe/physics3d/Physics3DTypes.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <cmath>
#include <vector>

using namespace fe;

namespace {

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// The default step used throughout: 60 Hz, so the default 0.5 s sleep_time is
// reached after exactly 30 steps.
constexpr f32 kDt = 1.0f / 60.0f;

// A static floor whose top face is at y = 0 (centred at y = -0.5).
Node* add_floor(Scene& scene, f32 half) {
    Node* node = scene.create_node("Floor");
    auto* collider = node->add_component<physics3d::Collider3D>();
    collider->shape = physics3d::ShapeType3D::Box;
    collider->half_extent = Vec3{half, 0.5f, half};
    node->transform().position = Vec3{0.0f, -0.5f, 0.0f};
    return node;
}

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

// A dynamic point mass with no collider: the simplest joint end, and an island
// vertex with no contacts.
Node* add_point_mass(Scene& scene, const Vec3& position, f32 mass) {
    Node* node = scene.create_node("Mass");
    auto* body = node->add_component<physics3d::RigidBody3D>();
    body->mass = mass;
    node->transform().position = position;
    return node;
}

void add_distance_joint(Node& host, const Node& a, const Node& b, f32 rest_length) {
    auto* joint = host.add_component<physics3d::DistanceJoint3D>();
    joint->body_a = a.id();
    joint->body_b = b.id();
    joint->rest_length = rest_length;
}

} // namespace

// ---------------------------------------------------------------------------
// The shared IslandBuilder, through its `fe::physics` names.
// ---------------------------------------------------------------------------

TEST_CASE("the shared island builder groups linked bodies") {
    physics::IslandBuilder builder;
    builder.build(4, {physics::IslandLink{0, 1}, physics::IslandLink{2, 3}});
    REQUIRE(builder.island_count() == 2);
    CHECK(builder.islands()[0] == std::vector<usize>{0, 1});
    CHECK(builder.islands()[1] == std::vector<usize>{2, 3});
    CHECK(builder.same_island(0, 1));
    CHECK_FALSE(builder.same_island(0, 2));
}

TEST_CASE("the shared island builder ignores a link to a missing body") {
    physics::IslandBuilder builder;
    builder.build(2, {physics::IslandLink{0, 5}, physics::IslandLink{9, 9}});
    CHECK(builder.island_count() == 2);
    CHECK_FALSE(builder.same_island(0, 1));
}

// ---------------------------------------------------------------------------
// A lone 3D body settles and sleeps.
// ---------------------------------------------------------------------------

TEST_CASE("a 3D body that is asleep does not move even if it still has a velocity") {
    // Sleeping means the solver and the integrator skip the body, not merely
    // that it happens to have zero velocity. A sleeping body is loaded with a
    // velocity and must still not move: this is the structural proof that
    // `asleep_` gates the integration, not the value of the velocity.
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;
    body->sleep();
    body->velocity = Vec3{10.0f, 10.0f, 10.0f};
    body->angular_velocity = Vec3{5.0f, 0.0f, 0.0f};

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(body->is_sleeping);
    CHECK(node->transform().position.x == doctest::Approx(0.0f));
    CHECK(node->transform().position.y == doctest::Approx(0.0f));
    CHECK(node->transform().position.z == doctest::Approx(0.0f));
}

TEST_CASE("a 3D body at rest sleeps after the sustained-time threshold and stays asleep") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 1.0f);
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 29; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK_FALSE(body->is_sleeping); // one step short of the threshold

    system.update(step(kDt), scene);
    CHECK(body->is_sleeping); // step 30: timer reached sleep_time
    CHECK(system.sleeping_body_count() == 1);

    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(body->is_sleeping); // still asleep, no flicker
    CHECK(node->transform().position.y == doctest::Approx(0.0f));
}

TEST_CASE("a 3D body that is still moving does not sleep") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0.0f, 10.0f, 0.0f}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
        CHECK_FALSE(node->get_component<physics3d::RigidBody3D>()->is_sleeping);
    }
    CHECK(system.sleeping_body_count() == 0);
}

TEST_CASE("a 3D body resting on a static floor sleeps") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* node = add_dynamic_box(scene, Vec3{0.0f, 0.5f, 0.0f}, 0.5f, 1.0f);

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
    }
    auto* body = node->get_component<physics3d::RigidBody3D>();
    CHECK(body->is_sleeping);
    CHECK(system.sleeping_body_count() == 1);
}

TEST_CASE("a sleeping 3D body is not moved by the solver") {
    Scene scene;
    add_floor(scene, 20.0f);
    Node* node = add_dynamic_box(scene, Vec3{0.0f, 0.5f, 0.0f}, 0.5f, 1.0f);

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 120; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(node->get_component<physics3d::RigidBody3D>()->is_sleeping);
    const Vec3 resting = node->transform().position;

    for (int i = 0; i < 300; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(node->transform().position.x == resting.x);
    CHECK(node->transform().position.y == resting.y);
    CHECK(node->transform().position.z == resting.z);
}

// ---------------------------------------------------------------------------
// Waking on disturbance.
// ---------------------------------------------------------------------------

TEST_CASE("a moving 3D body wakes the sleeping body it reaches") {
    Scene scene;
    Node* a_node = add_dynamic_box(scene, Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 1.0f);
    Node* b_node = add_dynamic_box(scene, Vec3{-4.0f, 0.0f, 0.0f}, 0.5f, 1.0f);
    auto* a_body = a_node->get_component<physics3d::RigidBody3D>();
    auto* b_body = b_node->get_component<physics3d::RigidBody3D>();
    a_body->affected_by_gravity = false;
    b_body->affected_by_gravity = false;
    b_body->velocity = Vec3{2.0f, 0.0f, 0.0f};

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 40; ++i) {
        system.update(step(kDt), scene);
    }
    REQUIRE(a_body->is_sleeping);
    CHECK(a_node->transform().position.x == doctest::Approx(0.0f));

    bool a_woke = false;
    for (int i = 0; i < 200; ++i) {
        system.update(step(kDt), scene);
        if (!a_body->is_sleeping) {
            a_woke = true;
        }
    }
    CHECK(a_woke);
    CHECK(a_node->transform().position.x > 0.1f);
    CHECK(b_node->transform().position.x < a_node->transform().position.x);
}

// ---------------------------------------------------------------------------
// Islands.
// ---------------------------------------------------------------------------

TEST_CASE("two 3D bodies joined by a distance joint are one island") {
    Scene scene;
    Node* a = add_point_mass(scene, Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
    Node* b = add_point_mass(scene, Vec3{1.0f, 0.0f, 0.0f}, 1.0f);
    a->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    b->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *a, *b, 1.0f);

    physics3d::Physics3DSystem system;
    system.update(step(kDt), scene);

    CHECK(system.island_count() == 1);
    CHECK(system.same_island(a->id(), b->id()));
}

TEST_CASE("two 3D bodies resting on separate parts of one floor are separate islands") {
    // Static geometry does not conduct motion: both boxes touch the same floor,
    // but the floor is not an island vertex, so they are not joined by it.
    Scene scene;
    add_floor(scene, 20.0f);
    Node* a = add_dynamic_box(scene, Vec3{-5.0f, 0.5f, 0.0f}, 0.5f, 1.0f);
    Node* b = add_dynamic_box(scene, Vec3{5.0f, 0.5f, 0.0f}, 0.5f, 1.0f);

    physics3d::Physics3DSystem system;
    system.update(step(kDt), scene);

    CHECK(system.island_count() == 2);
    CHECK_FALSE(system.same_island(a->id(), b->id()));
}

TEST_CASE("a 3D body with can_sleep false keeps its island awake") {
    Scene scene;
    Node* a = add_dynamic_box(scene, Vec3{0.0f, 0.5f, 0.0f}, 0.5f, 1.0f);
    Node* b = add_dynamic_box(scene, Vec3{1.0f, 0.5f, 0.0f}, 0.5f, 1.0f);
    a->get_component<physics3d::RigidBody3D>()->can_sleep = false;
    b->get_component<physics3d::RigidBody3D>()->can_sleep = false;
    a->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    b->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 200; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK_FALSE(a->get_component<physics3d::RigidBody3D>()->is_sleeping);
    CHECK_FALSE(b->get_component<physics3d::RigidBody3D>()->is_sleeping);
    CHECK(system.sleeping_body_count() == 0);
}

TEST_CASE("a jointed pair sleeps as one island") {
    // Two point masses joined by a distance joint and at rest. Because they are
    // one island, they must sleep together.
    Scene scene;
    Node* a = add_point_mass(scene, Vec3{0.0f, 0.0f, 0.0f}, 1.0f);
    Node* b = add_point_mass(scene, Vec3{1.0f, 0.0f, 0.0f}, 1.0f);
    a->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    b->get_component<physics3d::RigidBody3D>()->affected_by_gravity = false;
    Node* host = scene.create_node("Joint");
    add_distance_joint(*host, *a, *b, 1.0f);

    physics3d::Physics3DSystem system;
    for (int i = 0; i < 60; ++i) {
        system.update(step(kDt), scene);
    }
    CHECK(a->get_component<physics3d::RigidBody3D>()->is_sleeping);
    CHECK(b->get_component<physics3d::RigidBody3D>()->is_sleeping);
    CHECK(system.sleeping_body_count() == 2);
    CHECK(system.joint_count() == 1); // the joint still reports while asleep
}

// ---------------------------------------------------------------------------
// Serialization.
// ---------------------------------------------------------------------------

TEST_CASE("the 3D sleep fields round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics3d_types();

    Scene scene;
    Node* node = scene.create_node("Sleeper");
    node->add_component<physics3d::Collider3D>();
    auto* body = node->add_component<physics3d::RigidBody3D>();
    body->can_sleep = false;
    body->sleep_time = 1.25f;
    body->sleep_linear_threshold = 0.2f;
    body->sleep_angular_threshold = 0.3f;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));
    Node* loaded_node = loaded.find_node_by_name("Sleeper");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_body = loaded_node->get_component<physics3d::RigidBody3D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->can_sleep == false);
    CHECK(loaded_body->sleep_time == doctest::Approx(1.25f));
    CHECK(loaded_body->sleep_linear_threshold == doctest::Approx(0.2f));
    CHECK(loaded_body->sleep_angular_threshold == doctest::Approx(0.3f));
}
