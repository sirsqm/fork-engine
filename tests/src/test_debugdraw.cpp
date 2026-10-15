// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the debug-draw module (ADR-0058). The list-building half is pure
// and headless: it asserts the exact geometry a collider wireframe produces, so
// a wrong edge or a mis-counted circle is caught without a GPU. The render half
// runs against the null device, which proves the pass builds its shader and
// buffers and submits a line-list draw, without asserting pixels (those belong
// in the graphics suite).

#include <doctest/doctest.h>

#include "fe/debugdraw/DebugDraw.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Types.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <cmath>

using namespace fe;

TEST_CASE("a box wireframe is exactly twelve edges") {
    debugdraw::DebugDrawList list;
    list.add_box(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

    REQUIRE(list.size() == 12);

    // Every corner of a unit half-extent box appears, and every edge has length
    // 2 (the box spans -1..1 on each axis).
    for (const auto& line : list.lines()) {
        const f32 dx = line.b.x - line.a.x;
        const f32 dy = line.b.y - line.a.y;
        const f32 dz = line.b.z - line.a.z;
        const f32 length = std::sqrt(dx * dx + dy * dy + dz * dz);
        CHECK(length == doctest::Approx(2.0f));
    }
}

TEST_CASE("a sphere wireframe is three orthogonal circles") {
    debugdraw::DebugDrawList list;
    list.add_sphere(Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 8);

    // Three circles of 8 chords each, one chord per segment plus the closing one.
    REQUIRE(list.size() == 3U * 8U);

    // Every chord lies on the sphere: both endpoints are one unit from the
    // centre.
    for (const auto& line : list.lines()) {
        CHECK(std::sqrt(line.a.x * line.a.x + line.a.y * line.a.y + line.a.z * line.a.z) == doctest::Approx(1.0f));
        CHECK(std::sqrt(line.b.x * line.b.x + line.b.y * line.b.y + line.b.z * line.b.z) == doctest::Approx(1.0f));
    }
}

TEST_CASE("a collider wireframe uses the dynamic colour for a movable body") {
    Scene scene;

    // A static collider: a box, no rigid body.
    Node* floor = scene.create_node("Floor");
    auto* floor_collider = floor->add_component<physics3d::Collider3D>();
    floor_collider->shape = physics3d::ShapeType3D::Box;
    floor_collider->half_extent = Vec3{4.0f, 0.5f, 4.0f};

    // A dynamic collider: a sphere with a movable body.
    Node* ball = scene.create_node("Ball");
    auto* ball_collider = ball->add_component<physics3d::Collider3D>();
    ball_collider->shape = physics3d::ShapeType3D::Sphere;
    ball_collider->radius = 0.5f;
    ball->add_component<physics3d::RigidBody3D>()->mass = 1.0f;
    ball->transform().position = Vec3{0.0f, 3.0f, 0.0f};

    debugdraw::DebugDrawList list;
    const usize drawn = debugdraw::append_collider_wireframes(scene, list);

    CHECK(drawn == 2);
    // The box is 12 edges; the sphere is three 16-chord circles.
    CHECK(list.size() == 12U + 3U * 16U);

    // The first 12 lines are the box (static colour), the rest the sphere
    // (dynamic colour). The traversal is depth-first in scene order.
    CHECK(list.lines()[0].color == debugdraw::static_collider_color());
    CHECK(list.lines()[12].color == debugdraw::dynamic_collider_color());
}

TEST_CASE("a kinematic 3D body is drawn with the static colour") {
    Scene scene;
    Node* platform = scene.create_node("Platform");
    platform->add_component<physics3d::Collider3D>();
    auto* body = platform->add_component<physics3d::RigidBody3D>();
    body->kinematic = true;

    debugdraw::DebugDrawList list;
    REQUIRE(debugdraw::append_collider_wireframes(scene, list) == 1);
    CHECK(list.lines()[0].color == debugdraw::static_collider_color());
}

TEST_CASE("a 2D circle collider becomes a wire circle on the xy plane") {
    Scene scene;
    Node* node = scene.create_node("Blob");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Circle;
    collider->radius = 2.0f;

    debugdraw::DebugDrawList list;
    REQUIRE(debugdraw::append_collider_wireframes_2d(scene, list) == 1);
    CHECK(list.size() == 24U);

    // Every chord endpoint is two units from the origin, and every point is on
    // the z = 0 plane.
    for (const auto& line : list.lines()) {
        CHECK(line.a.z == doctest::Approx(0.0f));
        CHECK(line.b.z == doctest::Approx(0.0f));
        CHECK(std::sqrt(line.a.x * line.a.x + line.a.y * line.a.y) == doctest::Approx(2.0f));
    }
}

TEST_CASE("the debug renderer builds its pass and submits a line draw on the null device") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    debugdraw::DebugRenderer renderer;
    REQUIRE_FALSE(renderer.initialize(device));

    debugdraw::DebugDrawList list;
    list.add_box(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

    renderer.render(device, list, Mat4::identity());
    CHECK(renderer.last_line_count() == 12);
    CHECK(renderer.draw_calls() == 1);

    // An empty list draws nothing and does not advance the draw count.
    debugdraw::DebugDrawList empty;
    renderer.render(device, empty, Mat4::identity());
    CHECK(renderer.last_line_count() == 0);
    CHECK(renderer.draw_calls() == 1);

    // A list larger than the initial capacity still draws: the buffer grows.
    debugdraw::DebugDrawList big;
    for (u32 i = 0; i < 4096; ++i) {
        big.add_line(Vec3{static_cast<f32>(i), 0.0f, 0.0f}, Vec3{static_cast<f32>(i), 1.0f, 0.0f});
    }
    renderer.render(device, big, Mat4::identity());
    CHECK(renderer.last_line_count() == 4096);
    CHECK(renderer.draw_calls() == 2);
}

TEST_CASE("initializing the debug renderer twice is idempotent") {
    auto created = rhi::create_null_device();
    REQUIRE(created.ok());
    rhi::Device& device = *created.value();

    debugdraw::DebugRenderer renderer;
    REQUIRE_FALSE(renderer.initialize(device, 16));
    REQUIRE_FALSE(renderer.initialize(device, 8)); // Smaller: ignored, no error.

    debugdraw::DebugDrawList list;
    list.add_line(Vec3{}, Vec3{1.0f, 0.0f, 0.0f});
    renderer.render(device, list, Mat4::identity());
    CHECK(renderer.last_line_count() == 1);
}
