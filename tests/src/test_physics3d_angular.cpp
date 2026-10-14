// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// CPU-side tests for 3D angular dynamics (ADR-0043): the quaternion-integrated
// orientation, shape-derived inertia, and the angular response of contacts and
// joints. These are pure-logic tests: no window, no GPU, no audio.

#include <doctest/doctest.h>

#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/Geometry3D.h"
#include "fe/physics3d/JointSolver3D.h"
#include "fe/physics3d/Physics3DSystem.h"
#include "fe/physics3d/Physics3DTypes.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <cmath>

using namespace fe;

namespace {

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A static floor whose top face is at y = 0 (a box centred at y = -0.5).
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

void run(physics3d::Physics3DSystem& system, Scene& scene, f32 delta, int steps) {
    for (int i = 0; i < steps; ++i) {
        system.update(step(delta), scene);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Shape-derived inertia.
// ---------------------------------------------------------------------------

TEST_CASE("a solid sphere's inertia is 2/5 m r^2") {
    physics3d::Collider3D collider;
    collider.shape = physics3d::ShapeType3D::Sphere;
    collider.radius = 2.0f;
    CHECK(collider.inertia_about_center(5.0f) == doctest::Approx(0.4f * 5.0f * 4.0f));
}

TEST_CASE("a solid cube's inertia is (1/6) m w^2 about any axis") {
    physics3d::Collider3D collider;
    collider.shape = physics3d::ShapeType3D::Box;
    collider.half_extent = Vec3{0.5f, 0.5f, 0.5f};
    // (1/18) m (w^2 + h^2 + d^2) = (1/18) m (1 + 1 + 1) = (1/6) m.
    CHECK(collider.inertia_about_center(1.0f) == doctest::Approx(1.0f / 6.0f));
}

TEST_CASE("a non-positive mass yields zero inertia") {
    physics3d::Collider3D collider;
    CHECK(collider.inertia_about_center(0.0f) == doctest::Approx(0.0f));
    CHECK(collider.inertia_about_center(-3.0f) == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// Integration: torque spins a body, angular velocity turns the transform.
// ---------------------------------------------------------------------------

TEST_CASE("angular velocity integrates into the node's orientation") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;
    body->angular_velocity = Vec3{0.0f, 0.0f, 2.0f}; // radians per second about +Z
    physics3d::Physics3DSystem system;

    // One second at 2 rad/s, with nothing to change the angular velocity.
    run(system, scene, 0.01f, 100);

    CHECK(body->angular_velocity.z == doctest::Approx(2.0f));
    // The node's Euler rotation about Z is now about 2 radians.
    CHECK(node->transform().rotation.z == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(node->transform().rotation.x == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(node->transform().rotation.y == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("a constant torque spins a body up") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    auto* collider = node->get_component<physics3d::Collider3D>();
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;

    // A unit cube: inertia = m (w^2 + h^2 + d^2) / 18 = (1 + 1 + 1) / 18 = 1/6.
    const f32 expected_inertia = collider->inertia_about_center(body->mass);
    CHECK(expected_inertia == doctest::Approx(1.0f / 6.0f));

    body->torque = Vec3{0.0f, 0.0f, 1.0f}; // 1 N*m about +Z
    physics3d::Physics3DSystem system;

    run(system, scene, 0.001f, 1000);

    // alpha = torque / inertia = 6 rad/s^2; after 1 s, w = 6 rad/s.
    CHECK(body->angular_velocity.z == doctest::Approx(6.0f).epsilon(0.02));
}

TEST_CASE("angular damping decays the spin") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;
    body->angular_velocity = Vec3{0.0f, 0.0f, 4.0f};
    body->angular_damping = 0.5f; // shed half per second
    physics3d::Physics3DSystem system;

    run(system, scene, 0.01f, 100);

    // After one second the spin is roughly halved.
    CHECK(body->angular_velocity.z < 2.5f);
    CHECK(body->angular_velocity.z > 1.5f);
}

TEST_CASE("angular momentum is conserved without damping") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 2.0f);
    auto* collider = node->get_component<physics3d::Collider3D>();
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;
    body->angular_velocity = Vec3{1.0f, -2.0f, 0.5f};

    // L = I * w; with no torque and no damping it must not change.
    const f32 inertia = collider->inertia_about_center(body->mass);
    const Vec3 angular_momentum_before = body->angular_velocity * inertia;

    physics3d::Physics3DSystem system;
    run(system, scene, 0.01f, 200);

    const Vec3 angular_momentum_after = body->angular_velocity * inertia;
    CHECK(angular_momentum_after.x == doctest::Approx(angular_momentum_before.x).epsilon(1e-4));
    CHECK(angular_momentum_after.y == doctest::Approx(angular_momentum_before.y).epsilon(1e-4));
    CHECK(angular_momentum_after.z == doctest::Approx(angular_momentum_before.z).epsilon(1e-4));
}

TEST_CASE("a fixed-rotation body keeps its authored rotation and ignores torque") {
    Scene scene;
    Node* node = add_dynamic_box(scene, Vec3{0, 0, 0}, 0.5f, 1.0f);
    node->transform().rotation = Vec3{0.0f, 0.0f, 0.3f};
    auto* body = node->get_component<physics3d::RigidBody3D>();
    body->affected_by_gravity = false;
    body->fixed_rotation = true;
    body->angular_velocity = Vec3{0.0f, 0.0f, 3.0f}; // would spin if not locked
    body->torque = Vec3{0.0f, 0.0f, 5.0f};
    physics3d::Physics3DSystem system;

    run(system, scene, 0.01f, 100);

    CHECK(node->transform().rotation.z == doctest::Approx(0.3f));
    CHECK(body->angular_velocity.z == doctest::Approx(3.0f)); // carried, not changed
}

// ---------------------------------------------------------------------------
// Contact angular response: friction spins a rolling ball; an immovable body
// has infinite inertia.
// ---------------------------------------------------------------------------

TEST_CASE("friction spins up a sphere rolling along a floor") {
    // A literal inclined plane needs an oriented collider, which this cut does
    // not have: the ramp would collide as its unrotated box. Instead the ball is
    // given a horizontal velocity on a frictioned floor, which is the same code
    // path (friction at a contact below the centre produces torque) and shows a
    // sphere spinning up as it rolls rather than sliding.
    Scene scene;

    Node* floor = scene.create_node("Floor");
    auto* floor_collider = floor->add_component<physics3d::Collider3D>();
    floor_collider->shape = physics3d::ShapeType3D::Box;
    floor_collider->half_extent = Vec3{20.0f, 0.5f, 20.0f};
    floor_collider->friction = 1.0f;
    floor->transform().position = Vec3{0.0f, -0.5f, 0.0f};

    Node* ball = add_dynamic_sphere(scene, Vec3{0.0f, 0.6f, 0.0f}, 0.5f, 1.0f);
    auto* ball_collider = ball->get_component<physics3d::Collider3D>();
    ball_collider->friction = 1.0f;
    auto* ball_body = ball->get_component<physics3d::RigidBody3D>();
    ball_body->velocity = Vec3{2.0f, 0.0f, 0.0f};

    physics3d::Physics3DSystem system;
    run(system, scene, 1.0f / 60.0f, 120);

    // Friction at the contact, below the ball's centre, has spun it up about the
    // axis perpendicular to its travel and the floor normal.
    CHECK(std::abs(ball_body->angular_velocity.z) > 0.1f);
}

TEST_CASE("an immovable body's inertia is treated as infinite") {
    // A body with non-positive mass has zero inverse inertia, so an angular
    // impulse cannot spin it. This is the same guard the linear solver has for
    // mass.
    physics3d::RigidBody3D body;
    body.mass = 0.0f;
    CHECK(body.inverse_inertia() == doctest::Approx(0.0f));

    body.mass = 1.0f;
    body.kinematic = true;
    CHECK(body.inverse_inertia() == doctest::Approx(0.0f));

    body.kinematic = false;
    body.fixed_rotation = true;
    CHECK(body.inverse_inertia() == doctest::Approx(0.0f));

    // A dynamic body with an author-set inertia reports its reciprocal.
    body.fixed_rotation = false;
    body.inertia = 4.0f;
    CHECK(body.inverse_inertia() == doctest::Approx(0.25f));
}

TEST_CASE("an angular contact does not spin a body that rests flat") {
    // A symmetric box dropped onto a flat floor contacts along its bottom face
    // through the centre line, so no net torque is produced and it must not
    // start spinning. This is the guard against a resting body jittering.
    Scene scene;
    add_floor(scene, 10.0f);
    Node* node = add_dynamic_box(scene, Vec3{0.0f, 1.0f, 0.0f}, 0.5f, 1.0f);
    physics3d::Physics3DSystem system;

    run(system, scene, 1.0f / 60.0f, 200);

    const auto* body = node->get_component<physics3d::RigidBody3D>();
    CHECK(std::abs(body->angular_velocity.z) < 0.05f);
    CHECK(std::abs(body->angular_velocity.x) < 0.05f);
}

// ---------------------------------------------------------------------------
// Joints: the row type carries angular terms, and the distance row leaves them
// zero (it measures between centres).
// ---------------------------------------------------------------------------

TEST_CASE("a distance joint row carries no angular Jacobian") {
    physics3d::JointBody3D a;
    a.position = Vec3{0.0f, 0.0f, 0.0f};
    a.inv_mass = 1.0f;
    a.inv_inertia = 1.0f;
    physics3d::JointBody3D b;
    b.position = Vec3{1.0f, 0.0f, 0.0f};
    b.inv_mass = 1.0f;
    b.inv_inertia = 1.0f;

    physics3d::DistanceJoint3DParams params;
    params.rest_length = 1.0f;

    auto row = physics3d::build_distance_row(a, b, params, 1.0f / 60.0f);
    REQUIRE(row);
    CHECK(row.value().angular_a.length() == doctest::Approx(0.0f));
    CHECK(row.value().angular_b.length() == doctest::Approx(0.0f));
    // Effective mass is therefore the linear-only 1 / (1 + 1) = 0.5.
    CHECK(row.value().effective_mass == doctest::Approx(0.5f));
}

TEST_CASE("an angular impulse spins a joint end") {
    // A hand-built row with an angular Jacobian: applying it must change the
    // angular velocity in proportion to the inverse inertia, which is the joint
    // counterpart of the contact angular response.
    physics3d::JointBody3D a;
    a.inv_mass = 1.0f;
    a.inv_inertia = 2.0f;
    a.angular_velocity = Vec3{0.0f, 1.0f, 0.0f};
    physics3d::JointBody3D b;
    b.inv_mass = 0.0f;
    b.inv_inertia = 0.0f;

    physics3d::ConstraintRow3D row;
    row.jacobian_a = Vec3{1.0f, 0.0f, 0.0f};
    row.angular_a = Vec3{0.0f, 1.0f, 0.0f};
    row.effective_mass = physics3d::row_effective_mass(row, a, b);
    // The apparent mass includes the angular term: 1 / (1 * 1 + 2 * 1) = 1/3.
    CHECK(row.effective_mass == doctest::Approx(1.0f / 3.0f));

    physics3d::apply_row(row, a, b);
    // The row velocity is a.angular_velocity . angular_a = 1, so the impulse is
    // -(1/3) * 1. The angular response is that impulse times the angular Jacobian
    // and the inverse inertia: 1 - (1/3) * 2 = 1/3.
    CHECK(a.angular_velocity.y == doctest::Approx(1.0f / 3.0f));
    CHECK(a.velocity.x == doctest::Approx(-1.0f / 3.0f));
}

// ---------------------------------------------------------------------------
// Serialization: the angular properties round-trip through reflection.
// ---------------------------------------------------------------------------

TEST_CASE("angular properties round-trip through the serializer") {
    reflect::register_builtin_types();
    reflect::register_physics3d_types();

    Scene scene;
    Node* node = scene.create_node("Spinner");
    auto* body = node->add_component<physics3d::RigidBody3D>();
    body->angular_velocity = Vec3{1.5f, -2.5f, 0.75f};
    body->torque = Vec3{3.0f, 0.0f, -1.0f};
    body->inertia = 0.75f;
    body->angular_damping = 0.25f;
    body->fixed_rotation = true;

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));

    Node* loaded_node = loaded.find_node_by_name("Spinner");
    REQUIRE(loaded_node != nullptr);
    const auto* loaded_body = loaded_node->get_component<physics3d::RigidBody3D>();
    REQUIRE(loaded_body != nullptr);
    CHECK(loaded_body->angular_velocity.x == doctest::Approx(1.5f));
    CHECK(loaded_body->angular_velocity.y == doctest::Approx(-2.5f));
    CHECK(loaded_body->angular_velocity.z == doctest::Approx(0.75f));
    CHECK(loaded_body->torque.x == doctest::Approx(3.0f));
    CHECK(loaded_body->torque.z == doctest::Approx(-1.0f));
    CHECK(loaded_body->inertia == doctest::Approx(0.75f));
    CHECK(loaded_body->angular_damping == doctest::Approx(0.25f));
    CHECK(loaded_body->fixed_rotation == true);
}
