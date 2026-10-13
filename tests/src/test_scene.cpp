// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

using namespace fe;

TEST_CASE("a new scene is empty and can create root nodes") {
    Scene scene;
    CHECK(scene.root_count() == 0);
    CHECK(scene.node_count() == 0);

    Node* node = scene.create_node("Root");
    REQUIRE(node != nullptr);
    CHECK(node->name() == "Root");
    CHECK(node->scene() == &scene);
    CHECK(scene.root_count() == 1);
    CHECK(scene.node_count() == 1);
}

TEST_CASE("nodes are looked up by id and by name") {
    Scene scene;
    Node* parent = scene.create_node("Parent");
    Node* child = parent->create_child<Node>("Child");

    CHECK(scene.find_node(parent->id()) == parent);
    CHECK(scene.find_node(child->id()) == child);
    CHECK(scene.find_node_by_name("Child") == child);
    CHECK(scene.find_node_by_name("Missing") == nullptr);

    // The child is in the scene even though it is not a root.
    CHECK(scene.root_count() == 1);
    CHECK(scene.node_count() == 2);
}

TEST_CASE("world transform composes down the hierarchy") {
    Scene scene;
    Node* parent = scene.create_node("Parent");
    parent->transform().position = Vec3{10.0f, 0.0f, 0.0f};

    Node* child = parent->create_child<Node>("Child");
    child->transform().position = Vec3{5.0f, 2.0f, 0.0f};

    const Vec3 world = transform_point(child->world_matrix(), Vec3{});
    CHECK(world.x == doctest::Approx(15.0f));
    CHECK(world.y == doctest::Approx(2.0f));
}

TEST_CASE("a child inherits its parent's scale") {
    Scene scene;
    Node* parent = scene.create_node("Parent");
    parent->transform().scale = Vec3{2.0f, 2.0f, 1.0f};

    Node* child = parent->create_child<Node>("Child");
    child->transform().position = Vec3{10.0f, 0.0f, 0.0f};

    const Vec3 world = transform_point(child->world_matrix(), Vec3{});
    CHECK(world.x == doctest::Approx(20.0f));
}

TEST_CASE("detaching a subtree removes every descendant from the scene") {
    Scene scene;
    Node* root = scene.create_node("Root");
    Node* child = root->create_child<Node>("Child");
    child->create_child<Node>("Grandchild");
    CHECK(scene.node_count() == 3);

    auto detached = scene.remove_root(root);
    REQUIRE(detached != nullptr);
    CHECK(scene.node_count() == 0);
    CHECK(scene.root_count() == 0);
    // The detached node keeps its own subtree.
    CHECK(detached->child_count() == 1);
}

TEST_CASE("reparenting a node moves it under a new parent") {
    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");
    Node* child = a->create_child<Node>("Child");

    b->add_child(a->remove_child(child));

    CHECK(child->parent() == b);
    CHECK(a->child_count() == 0);
    CHECK(b->child_count() == 1);
    CHECK(scene.node_count() == 3);
}

TEST_CASE("clearing a scene detaches everything") {
    Scene scene;
    Node* root = scene.create_node("Root");
    root->create_child<Node>("Child");

    scene.clear();
    CHECK(scene.root_count() == 0);
    CHECK(scene.node_count() == 0);
}

TEST_CASE("for_each_node visits every node depth-first") {
    Scene scene;
    Node* root = scene.create_node("Root");
    root->create_child<Node>("A");
    root->create_child<Node>("B");

    std::vector<std::string> names;
    scene.for_each_node([&](Node& node) { names.push_back(node.name()); });

    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Root");
    CHECK(names[1] == "A");
    CHECK(names[2] == "B");
}
