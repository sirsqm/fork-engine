// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/scene/Camera2D.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

using namespace fe;

namespace {

// A component used to observe attach/detach ordering in the tests below.
class LifecycleProbe final : public Component {
public:
    static constexpr const char* kTypeName = "LifecycleProbe";

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    int attach_count = 0;
    int detach_count = 0;

    // Widened to public so the test can observe the counts directly; the base
    // class hooks stay protected.
    void on_attach(Node&) override { ++attach_count; }
    void on_detach(Node&) override { ++detach_count; }
};

} // namespace

TEST_CASE("a node can hold and retrieve components by type") {
    Scene scene;
    Node* node = scene.create_node("Entity");

    auto* sprite = node->add_component<Sprite2D>();
    auto* camera = node->add_component<Camera2D>();
    REQUIRE(sprite != nullptr);
    REQUIRE(camera != nullptr);

    CHECK(node->component_count() == 2);
    CHECK(node->get_component<Sprite2D>() == sprite);
    CHECK(node->get_component<Camera2D>() == camera);
    CHECK(node->get_component<LifecycleProbe>() == nullptr);
}

TEST_CASE("a component knows its owning node") {
    Scene scene;
    Node* node = scene.create_node("Entity");
    auto* sprite = node->add_component<Sprite2D>();
    CHECK(sprite->node() == node);
}

TEST_CASE("adding a component to a node already in a scene fires on_attach") {
    Scene scene;
    Node* node = scene.create_node("Entity");

    auto* probe = node->add_component<LifecycleProbe>();
    // The node was already attached, so the component must be told immediately.
    CHECK(probe->attach_count == 1);
    CHECK(probe->detach_count == 0);
}

TEST_CASE("a component added before the node is attached fires on_attach later") {
    // Build a child, detach it (so it has no scene), add the component, then
    // reattach it.
    Scene scene;
    Node* parent = scene.create_node("Parent");
    auto detached = parent->remove_child(parent->create_child<Node>("Child"));
    REQUIRE(detached != nullptr);

    auto* probe = detached->add_component<LifecycleProbe>();
    CHECK(probe->attach_count == 0);

    parent->add_child(std::move(detached));
    CHECK(probe->attach_count == 1);
}

TEST_CASE("removing a component fires on_detach and clears the link") {
    Scene scene;
    Node* node = scene.create_node("Entity");
    auto* probe = node->add_component<LifecycleProbe>();

    CHECK(node->remove_component<LifecycleProbe>());
    CHECK(probe->detach_count == 1);
    CHECK(node->get_component<LifecycleProbe>() == nullptr);
    CHECK_FALSE(node->remove_component<LifecycleProbe>());
}

TEST_CASE("detaching a node from the scene detaches its components") {
    Scene scene;
    Node* root = scene.create_node("Root");
    auto* probe = root->add_component<LifecycleProbe>();
    CHECK(probe->attach_count == 1);

    // remove_root() hands ownership back; keep the node alive through the
    // assertions that follow.
    auto removed = scene.remove_root(root);
    CHECK(probe->detach_count == 1);
    CHECK(removed != nullptr);
}

TEST_CASE("a camera builds a view-projection matrix centred on its node") {
    Scene scene;
    Node* node = scene.create_node("Camera");
    auto* camera = node->add_component<Camera2D>();
    node->transform().position = Vec3{100.0f, 50.0f, 0.0f};

    const Mat4 view_projection = camera->view_projection(800, 600);

    // The camera's own world position must map to the centre of clip space.
    const Vec3 centre = transform_point(view_projection, Vec3{100.0f, 50.0f, 0.0f});
    CHECK(centre.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(centre.y == doctest::Approx(0.0f).epsilon(1e-4));

    // A point 400 units to the right (half the viewport at zoom 1) is the right
    // edge of the view.
    const Vec3 right_edge = transform_point(view_projection, Vec3{500.0f, 50.0f, 0.0f});
    CHECK(right_edge.x == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("zooming in narrows the visible world extent") {
    Scene scene;
    Node* node = scene.create_node("Camera");
    auto* camera = node->add_component<Camera2D>();
    camera->zoom = 2.0f;

    const Mat4 view_projection = camera->view_projection(800, 600);
    // At zoom 2, the half-width is 800/2/2 = 200 world units, so world x=200 is
    // the right edge of the view.
    const Vec3 right_edge = transform_point(view_projection, Vec3{200.0f, 0.0f, 0.0f});
    CHECK(right_edge.x == doctest::Approx(1.0f).epsilon(1e-4));
}
