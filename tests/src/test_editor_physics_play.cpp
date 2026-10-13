// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Play-in-editor with physics, headlessly. `PlaySession::advance` runs the
// schedule over the play scene with a caller-supplied FrameContext, so a test
// can step the simulation by a *fixed* delta and assert on the result without a
// window or a GPU. This is what makes "play simulates the authored scene" a
// checkable claim rather than a description: a body authored above a floor must
// fall, land, and rest, and the authored scene must never be touched.
//
// The engine-driven frame part of play (PlaySession::tick, which renders) needs
// a GL context and is covered by the graphics tests.

#include <doctest/doctest.h>

#include "fe/editor/EditorContext.h"
#include "fe/editor/PlaySession.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"

#include <cmath>

using namespace fe;
using namespace fe::editor;

namespace {

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A static floor whose top face is at y = 0.
void add_floor(Scene& scene, f32 half_width) {
    Node* node = scene.create_node("Floor");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{half_width, 0.5f};
    node->transform().position = Vec3{0.0f, -0.5f, 0.0f};
}

// A dynamic box that starts high above the floor.
Node* add_falling_box(Scene& scene, f32 centre_y) {
    Node* node = scene.create_node("Crate");
    auto* collider = node->add_component<physics2d::Collider2D>();
    collider->shape = physics2d::ShapeType::Box;
    collider->half_extent = Vec2{0.5f, 0.5f};
    node->add_component<physics2d::RigidBody2D>()->mass = 1.0f;
    node->transform().position = Vec3{0.0f, centre_y, 0.0f};
    return node;
}

} // namespace

TEST_CASE("the default play schedule includes the physics system") {
    Scene authored;
    add_floor(authored, 10.0f);

    PlaySession session;
    REQUIRE_FALSE(session.start(authored));
    CHECK(session.schedule().find("Physics2DSystem") != nullptr);
    CHECK(session.schedule().find("SpriteAnimationSystem") != nullptr);
}

TEST_CASE("advancing play simulates a falling body onto a static floor") {
    Scene authored;
    add_floor(authored, 20.0f);
    add_falling_box(authored, 6.0f);

    // Register the physics copy factories, exactly as EditorContext does, and
    // drive the context's own play session so the whole editor path is used.
    EditorContext context;
    context.register_default_component_factories();
    auto& session = context.play();
    REQUIRE_FALSE(session.start(authored));

    const Scene& play = session.play_scene();
    const Node* crate = play.find_node_by_name("Crate");
    REQUIRE(crate != nullptr);

    // Step two seconds of fixed 60 Hz frames.
    for (int i = 0; i < 120; ++i) {
        REQUIRE_FALSE(session.advance(step(1.0f / 60.0f)));
    }

    // The crate's half-height is 0.5, so resting on the floor puts its centre
    // near y = 0.5. It fell from 6, so it moved a lot and did not tunnel through.
    const f32 y = crate->transform().position.y;
    CHECK(y < 1.0f);
    CHECK(y > 0.4f);

    // And the authored scene is untouched: play simulates a copy.
    const Node* authored_crate = authored.find_node_by_name("Crate");
    REQUIRE(authored_crate != nullptr);
    CHECK(authored_crate->transform().position.y == doctest::Approx(6.0f));
}

TEST_CASE("a body keeps falling while play advances, and the authored scene is untouched") {
    Scene authored;
    add_floor(authored, 20.0f);
    add_falling_box(authored, 20.0f);

    EditorContext context;
    context.register_default_component_factories();
    auto& session = context.play();
    REQUIRE_FALSE(session.start(authored));

    REQUIRE_FALSE(session.advance(step(1.0f / 60.0f)));
    const f32 after_one = session.play_scene().find_node_by_name("Crate")->transform().position.y;
    REQUIRE_FALSE(session.advance(step(1.0f / 60.0f)));
    const f32 after_two = session.play_scene().find_node_by_name("Crate")->transform().position.y;
    CHECK(after_two < after_one);

    // Stopping discards the play scene; the authored scene is unchanged.
    session.stop();
    CHECK_FALSE(session.is_playing());
    CHECK(authored.find_node_by_name("Crate")->transform().position.y == doctest::Approx(20.0f));
}

TEST_CASE("advancing a stopped session is reported as an error") {
    PlaySession session;
    CHECK(session.advance(step(1.0f / 60.0f)));
}
