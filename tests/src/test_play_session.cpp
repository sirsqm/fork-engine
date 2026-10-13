// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for play-in-editor's scene copy. The copy is the part of play that can
// be checked without a GPU: it must reproduce the authored hierarchy and
// components exactly, while remaining a *separate* tree so that playing cannot
// disturb what the user is editing. The engine-driven frame part of play needs
// a GL context and is exercised by the graphics tests.

#include <doctest/doctest.h>

#include "fe/editor/PlaySession.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

using namespace fe;
using namespace fe::editor;

namespace {

// Registers the built-in component factories, exactly as EditorContext does.
// Components are move-only, so the factories copy fields explicitly.
void register_defaults(PlaySession& session) {
    session.register_component_factory(Camera2D::kTypeName, [](const Node& source, Node& destination) {
        const auto* component = source.get_component<Camera2D>();
        if (component == nullptr) {
            return;
        }
        auto* copy = destination.add_component<Camera2D>();
        copy->zoom = component->zoom;
        copy->primary = component->primary;
        copy->clear_r = component->clear_r;
        copy->clear_g = component->clear_g;
        copy->clear_b = component->clear_b;
        copy->clear_a = component->clear_a;
    });
    session.register_component_factory(Sprite2D::kTypeName, [](const Node& source, Node& destination) {
        const auto* component = source.get_component<Sprite2D>();
        if (component == nullptr) {
            return;
        }
        auto* copy = destination.add_component<Sprite2D>();
        copy->size = component->size;
        copy->color = component->color;
        copy->uv = component->uv;
        copy->layer = component->layer;
        copy->texture = component->texture;
    });
}

void build_authored_scene(Scene& scene) {
    scene.set_name("Authored");
    auto* camera = scene.create_node("Camera")->add_component<Camera2D>();
    camera->zoom = 2.0f;

    Node* group = scene.create_node("Group");
    auto* sprite = group->add_component<Sprite2D>();
    sprite->size = Vec2{64.0f, 64.0f};
    sprite->layer = 3;

    Node* child = group->create_child<Node>("Child");
    child->transform().position = Vec3{10.0f, 20.0f, 0.0f};
}

} // namespace

TEST_CASE("copy_scene reproduces the hierarchy, transforms, and components") {
    Scene authored;
    build_authored_scene(authored);

    PlaySession session;
    register_defaults(session);

    Scene copy;
    session.copy_scene(authored, copy);

    CHECK(copy.name() == "Authored");
    CHECK(copy.node_count() == authored.node_count());
    CHECK(copy.root_count() == authored.root_count());

    // The camera came across with its data intact.
    Node* camera = copy.find_node_by_name("Camera");
    REQUIRE(camera != nullptr);
    const auto* camera_component = camera->get_component<Camera2D>();
    REQUIRE(camera_component != nullptr);
    CHECK(camera_component->zoom == doctest::Approx(2.0f));

    // The child and its transform are preserved under its parent.
    Node* group = copy.find_node_by_name("Group");
    REQUIRE(group != nullptr);
    const auto* sprite = group->get_component<Sprite2D>();
    REQUIRE(sprite != nullptr);
    CHECK(sprite->size.x == doctest::Approx(64.0f));
    CHECK(sprite->layer == 3);

    Node* child = copy.find_node_by_name("Child");
    REQUIRE(child != nullptr);
    CHECK((child->parent() == group));
    CHECK(child->transform().position.y == doctest::Approx(20.0f));
}

TEST_CASE("the play copy is a separate tree from the authored scene") {
    Scene authored;
    build_authored_scene(authored);

    PlaySession session;
    register_defaults(session);

    Scene copy;
    session.copy_scene(authored, copy);

    // The two trees are distinct objects with distinct identities, so moving a
    // node in play cannot move the authored node.
    Node* authored_camera = authored.find_node_by_name("Camera");
    Node* copied_camera = copy.find_node_by_name("Camera");
    REQUIRE(authored_camera != nullptr);
    REQUIRE(copied_camera != nullptr);
    CHECK((authored_camera != copied_camera));
    CHECK_FALSE(authored_camera->id() == copied_camera->id());

    copied_camera->transform().position.x = 500.0f;
    authored_camera->transform().position.x = 1.0f;
    CHECK(authored_camera->transform().position.x == doctest::Approx(1.0f));
}

TEST_CASE("an unregistered component type is reported, not silently dropped") {
    // A component type the session has no factory for: the session must warn by
    // name rather than pretend the copy is complete.
    struct MarkerComponent final : public Component {
        [[nodiscard]] const char* type_name() const noexcept override { return "MarkerComponent"; }
    };

    Scene authored;
    authored.create_node("Marked")->add_component<MarkerComponent>();

    PlaySession session;
    // Deliberately no factory for MarkerComponent.
    Scene copy;
    session.copy_scene(authored, copy);

    CHECK(session.last_copy_warning() == "MarkerComponent");
    // The node itself still copies; only the unknown component is skipped.
    CHECK(copy.find_node_by_name("Marked") != nullptr);
    CHECK(copy.find_node_by_name("Marked")->component_count() == 0);
}

TEST_CASE("start and stop leave the authored scene untouched") {
    Scene authored;
    build_authored_scene(authored);
    const usize authored_nodes = authored.node_count();

    PlaySession session;
    register_defaults(session);

    // start() copies but does not touch an engine, so it is headless-safe.
    REQUIRE_FALSE(session.start(authored));
    CHECK(session.is_playing());
    CHECK(session.play_scene().node_count() == authored_nodes);

    session.stop();
    CHECK_FALSE(session.is_playing());
    CHECK(session.play_scene().node_count() == 0);

    // The authored scene is exactly as it was.
    CHECK(authored.node_count() == authored_nodes);
    CHECK(authored.find_node_by_name("Child") != nullptr);

    // Starting twice without stopping is a programming error, reported as one.
    REQUIRE_FALSE(session.start(authored));
    Error second = session.start(authored);
    CHECK(second);
    session.stop();
}
