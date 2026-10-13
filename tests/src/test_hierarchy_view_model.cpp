// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the scene hierarchy view model. These are pure: they build a scene
// with the public API and assert on the rows the panel would draw, with no
// window and no ImGui involved. That is the point of keeping the row/expansion/
// selection logic out of the panel.

#include <doctest/doctest.h>

#include "fe/editor/HierarchyViewModel.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

using namespace fe;
using namespace fe::editor;

namespace {

// scene
//  - A
//      - B
//          - C
//  - D
void build_tree(Scene& scene) {
    Node* a = scene.create_node("A");
    a->create_child<Node>("B")->create_child<Node>("C");
    (void)scene.create_node("D");
}

} // namespace

TEST_CASE("hierarchy lists nodes depth-first with correct depths") {
    Scene scene;
    build_tree(scene);

    HierarchyViewModel model;
    model.rebuild(scene);

    REQUIRE(model.rows().size() == 4);
    CHECK(model.rows()[0].node->name() == "A");
    CHECK(model.rows()[0].depth == 0);
    CHECK(model.rows()[1].node->name() == "B");
    CHECK(model.rows()[1].depth == 1);
    CHECK(model.rows()[2].node->name() == "C");
    CHECK(model.rows()[2].depth == 2);
    CHECK(model.rows()[3].node->name() == "D");
    CHECK(model.rows()[3].depth == 0);

    // Only nodes with children advertise an expander.
    CHECK(model.rows()[0].has_children);       // A has B.
    CHECK(model.rows()[1].has_children);       // B has C.
    CHECK_FALSE(model.rows()[2].has_children); // C is a leaf.
    CHECK_FALSE(model.rows()[3].has_children); // D is a leaf.
}

TEST_CASE("collapsing a node hides its subtree") {
    Scene scene;
    build_tree(scene);

    HierarchyViewModel model;
    model.rebuild(scene);

    Node* b = scene.find_node_by_name("B");
    REQUIRE(b != nullptr);

    // Nodes are expanded by default, so a collapse removes C from the rows.
    model.set_expanded(b->id(), false);
    model.rebuild(scene);
    CHECK(model.rows().size() == 3);
    CHECK(model.rows()[2].node->name() == "D");

    model.set_expanded(b->id(), true);
    model.rebuild(scene);
    CHECK(model.rows().size() == 4);
}

TEST_CASE("selection is stored by identity and survives a rebuild") {
    Scene scene;
    build_tree(scene);

    HierarchyViewModel model;
    model.rebuild(scene);

    Node* c = scene.find_node_by_name("C");
    REQUIRE(c != nullptr);
    model.select(c);

    CHECK(model.has_selection());
    CHECK(model.selected_name() == "C");
    CHECK(model.is_selected(*c));

    // A rebuild re-resolves the selected node from its id, so the selection
    // survives without the model holding a raw pointer across the rebuild.
    model.rebuild(scene);
    CHECK(model.is_selected(*c));
    CHECK((model.selected_node() == c));
}

TEST_CASE("a filter matches names case-insensitively and reveals matches") {
    Scene scene;
    build_tree(scene);

    HierarchyViewModel model;
    model.set_filter("c");
    model.rebuild(scene);

    // Only C contains "c".
    REQUIRE(model.rows().size() == 1);
    CHECK(model.rows()[0].node->name() == "C");

    // Filtering descends regardless of expansion state, so a nested match is
    // reachable without the user expanding its ancestors first.
    Node* b = scene.find_node_by_name("B");
    model.set_expanded(b->id(), false);
    model.rebuild(scene);
    REQUIRE(model.rows().size() == 1);
    CHECK(model.rows()[0].node->name() == "C");
}

TEST_CASE("selection is cleared when the selected node leaves the scene") {
    Scene scene;
    build_tree(scene);

    HierarchyViewModel model;
    model.rebuild(scene);
    Node* c = scene.find_node_by_name("C");
    REQUIRE(c != nullptr);
    model.select(c);
    REQUIRE(model.has_selection());

    // Remove the root that owns C. The selected pointer is now dangling, and a
    // rebuild must not resurrect a stale selection.
    std::unique_ptr<Node> removed = scene.remove_root(scene.find_node_by_name("A"));
    REQUIRE(removed != nullptr);

    model.rebuild(scene);
    CHECK_FALSE(model.has_selection());
    CHECK((model.selected_node() == nullptr));
    CHECK(model.selected_name().empty());
}
