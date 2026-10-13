// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Undo/redo. Every command must be exactly reversible: after undo() the scene
// is in the state it had before execute(). These tests are headless - no window,
// no ImGui - because the command stack lives in the editor core precisely so it
// can be checked this way. See docs/adr/0011-editor-undo-redo.md.

#include <doctest/doctest.h>

#include "fe/editor/Command.h"
#include "fe/editor/Commands.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/SpriteAnimator.h"

#include <memory>
#include <string>
#include <vector>

using namespace fe;
using namespace fe::editor;

namespace {

// The names of a node's children, in order, for asserting order is preserved.
std::vector<std::string> child_names(const Node& node) {
    std::vector<std::string> names;
    for (const auto& child : node.children()) {
        names.push_back(child->name());
    }
    return names;
}

std::vector<std::string> root_names(const Scene& scene) {
    std::vector<std::string> names;
    for (const auto& root : scene.roots()) {
        names.push_back(root->name());
    }
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// CreateNodeCommand
// ---------------------------------------------------------------------------

TEST_CASE("create node executes, undoes, and redoes with identity preserved") {
    Scene scene;
    CommandStack stack;

    stack.execute(scene, std::make_unique<CreateNodeCommand>("Root", Uuid{}, 0));
    REQUIRE(scene.root_count() == 1);
    Node* created = scene.roots().front().get();
    const Uuid id = created->id();
    CHECK(created->name() == "Root");

    CHECK(stack.can_undo());
    CHECK(stack.can_redo() == false);
    CHECK(stack.undo(scene));
    CHECK(scene.root_count() == 0);

    // Redo restores the *same* node, so its id is unchanged: a command holds the
    // object while undone rather than rebuilding it.
    CHECK(stack.redo(scene));
    REQUIRE(scene.root_count() == 1);
    CHECK(scene.roots().front()->id() == id);
}

TEST_CASE("create node inserts under a parent at the requested index") {
    Scene scene;
    Node* parent = scene.create_node("Parent");
    parent->create_child<Node>("B");
    parent->create_child<Node>("D");
    // Insert "C" between B and D.
    CommandStack stack;
    stack.execute(scene, std::make_unique<CreateNodeCommand>("C", parent->id(), 1));

    CHECK(child_names(*parent) == std::vector<std::string>{"B", "C", "D"});

    stack.undo(scene);
    CHECK(child_names(*parent) == std::vector<std::string>{"B", "D"});
}

// ---------------------------------------------------------------------------
// DeleteNodeCommand
// ---------------------------------------------------------------------------

TEST_CASE("delete node restores the whole subtree at its original position") {
    Scene scene;
    (void)scene.create_node("A");
    Node* b = scene.create_node("B");
    Node* child = b->create_child<Node>("B1");
    (void)scene.create_node("C");

    const Uuid b_id = b->id();
    const Uuid child_id = child->id();

    CommandStack stack;
    stack.execute(scene, std::make_unique<DeleteNodeCommand>(b_id));
    CHECK(scene.root_count() == 2);
    CHECK(scene.find_node(b_id) == nullptr); // gone
    CHECK(root_names(scene) == std::vector<std::string>{"A", "C"});

    // Undo brings B back between A and C, with its child and ids intact.
    stack.undo(scene);
    CHECK(root_names(scene) == std::vector<std::string>{"A", "B", "C"});
    Node* restored = scene.find_node(b_id);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->child_count() == 1);
    CHECK(restored->children().front()->id() == child_id);
}

TEST_CASE("deleting the selected node's subtree and undoing keeps components") {
    Scene scene;
    Node* node = scene.create_node("Sprite");
    node->add_component<Sprite2D>()->layer = 4;

    const Uuid id = node->id();
    CommandStack stack;
    stack.execute(scene, std::make_unique<DeleteNodeCommand>(id));
    stack.undo(scene);

    Node* restored = scene.find_node(id);
    REQUIRE(restored != nullptr);
    const auto* sprite = restored->get_component<Sprite2D>();
    REQUIRE(sprite != nullptr);
    CHECK(sprite->layer == 4);
}

// ---------------------------------------------------------------------------
// ReparentNodeCommand
// ---------------------------------------------------------------------------

TEST_CASE("reparent moves a node and undo returns it to its old parent and index") {
    Scene scene;
    Node* parent = scene.create_node("Parent");
    parent->create_child<Node>("Keep");
    Node* moved = scene.create_node("Moved");
    Node* other = scene.create_node("Other");

    const Uuid moved_id = moved->id();

    CommandStack stack;
    stack.execute(scene, std::make_unique<ReparentNodeCommand>(moved_id, other->id(), 0));
    CHECK(other->child_count() == 1);
    CHECK(other->children().front()->id() == moved_id);
    CHECK(scene.root_count() == 2); // Parent, Other

    stack.undo(scene);
    CHECK(other->child_count() == 0);
    CHECK(scene.find_node(moved_id) != nullptr);
    // Back as a root, at its old position (after Parent, before nothing).
    CHECK(root_names(scene) == std::vector<std::string>{"Parent", "Moved", "Other"});
}

TEST_CASE("reparenting a child out to the root and back restores the sibling order") {
    Scene scene;
    Node* parent = scene.create_node("Parent");
    parent->create_child<Node>("First");
    Node* middle = parent->create_child<Node>("Middle");
    parent->create_child<Node>("Last");

    const Uuid middle_id = middle->id();
    CommandStack stack;

    // Out to a root.
    stack.execute(scene, std::make_unique<ReparentNodeCommand>(middle_id, Uuid{}, scene.root_count()));
    CHECK(parent->child_count() == 2);
    CHECK(scene.root_count() == 2);

    // Back to the middle of the parent.
    stack.undo(scene);
    CHECK(child_names(*parent) == std::vector<std::string>{"First", "Middle", "Last"});
}

// ---------------------------------------------------------------------------
// RenameNodeCommand
// ---------------------------------------------------------------------------

TEST_CASE("rename executes and undoes") {
    Scene scene;
    Node* node = scene.create_node("Old");

    CommandStack stack;
    stack.execute(scene, std::make_unique<RenameNodeCommand>(node->id(), "New"));
    CHECK(node->name() == "New");

    stack.undo(scene);
    CHECK(node->name() == "Old");

    stack.redo(scene);
    CHECK(node->name() == "New");
}

// ---------------------------------------------------------------------------
// Add/RemoveComponentCommand
// ---------------------------------------------------------------------------

TEST_CASE("add component executes through the registry and undoes with identity") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Node");

    CommandStack stack;
    stack.execute(scene, std::make_unique<AddComponentCommand>(node->id(), std::string(Sprite2D::kTypeName), 0));
    REQUIRE(node->component_count() == 1);
    CHECK(std::string(node->components().front()->type_name()) == Sprite2D::kTypeName);

    stack.undo(scene);
    CHECK(node->component_count() == 0);

    // Redo restores the *same* component instance, so any edits made to it
    // before the undo would survive a redo.
    stack.redo(scene);
    REQUIRE(node->component_count() == 1);
}

TEST_CASE("remove component reinserts at the same index on undo") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Node");
    node->add_component<Sprite2D>();       // index 0
    (void)node->add_component<Camera2D>(); // index 1
    node->add_component<SpriteAnimator>(); // index 2

    // Removing the middle component and undoing must put it back in the middle.
    CommandStack stack;
    stack.execute(scene, std::make_unique<RemoveComponentCommand>(node->id(), 1));
    REQUIRE(node->component_count() == 2);
    CHECK(std::string(node->components()[0]->type_name()) == Sprite2D::kTypeName);
    CHECK(std::string(node->components()[1]->type_name()) == SpriteAnimator::kTypeName);

    stack.undo(scene);
    REQUIRE(node->component_count() == 3);
    CHECK(std::string(node->components()[0]->type_name()) == Sprite2D::kTypeName);
    CHECK(std::string(node->components()[1]->type_name()) == "Camera2D");
    CHECK(std::string(node->components()[2]->type_name()) == SpriteAnimator::kTypeName);
}

// ---------------------------------------------------------------------------
// SetPropertyCommand: generic through reflection
// ---------------------------------------------------------------------------

TEST_CASE("set property writes a leaf and undoes through the reflection setter") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->layer = 1;

    CommandStack stack;
    stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"layer"},
                                                              reflect::Value{static_cast<i64>(7)}));
    CHECK(sprite->layer == 7);

    stack.undo(scene);
    CHECK(sprite->layer == 1);
}

TEST_CASE("set property addresses a group member by path") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->size = Vec2{10.0f, 20.0f};

    CommandStack stack;
    // "size.x" is a two-element path: the group then the member.
    stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"size", "x"},
                                                              reflect::Value{static_cast<f64>(99.0)}));
    CHECK(sprite->size.x == doctest::Approx(99.0f));
    CHECK(sprite->size.y == doctest::Approx(20.0f)); // untouched

    stack.undo(scene);
    CHECK(sprite->size.x == doctest::Approx(10.0f));
}

TEST_CASE("consecutive edits of one property coalesce into a single undo step") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->layer = 0;

    CommandStack stack;
    // Three frames of a drag: each would be a step without coalescing.
    for (i64 value : {1, 2, 3}) {
        stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"layer"},
                                                                  reflect::Value{value}));
    }
    CHECK(sprite->layer == 3);
    CHECK(stack.undo_count() == 1); // one gesture, one step

    // A single undo reverts the whole drag to the value before it began.
    CHECK(stack.undo(scene));
    CHECK(sprite->layer == 0);
    CHECK_FALSE(stack.can_undo());
}

TEST_CASE("break_coalescing starts a new undo step for the same property") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->layer = 0;

    CommandStack stack;
    stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"layer"},
                                                              reflect::Value{static_cast<i64>(1)}));
    // The drag ended; the next edit is a separate gesture.
    stack.break_coalescing();
    stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"layer"},
                                                              reflect::Value{static_cast<i64>(2)}));
    CHECK(stack.undo_count() == 2);

    stack.undo(scene);
    CHECK(sprite->layer == 1);
    stack.undo(scene);
    CHECK(sprite->layer == 0);
}

TEST_CASE("different properties never coalesce") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->layer = 0;

    CommandStack stack;
    stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"layer"},
                                                              reflect::Value{static_cast<i64>(1)}));
    stack.execute(scene, std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"size", "x"},
                                                              reflect::Value{static_cast<f64>(5.0)}));
    CHECK(stack.undo_count() == 2);
    CHECK(sprite->size.x == doctest::Approx(5.0f));
}

// ---------------------------------------------------------------------------
// CommandStack invariants
// ---------------------------------------------------------------------------

TEST_CASE("a new command after undo discards the redo history") {
    Scene scene;
    CommandStack stack;
    stack.execute(scene, std::make_unique<CreateNodeCommand>("A", Uuid{}, 0));
    stack.undo(scene);
    CHECK(stack.can_redo());

    // A fresh edit invalidates the redo.
    stack.execute(scene, std::make_unique<CreateNodeCommand>("B", Uuid{}, 0));
    CHECK_FALSE(stack.can_redo());
    CHECK(root_names(scene) == std::vector<std::string>{"B"});
}

TEST_CASE("undo and redo past the ends are no-ops, not errors") {
    Scene scene;
    CommandStack stack;
    CHECK_FALSE(stack.undo(scene));
    CHECK_FALSE(stack.redo(scene));

    stack.execute(scene, std::make_unique<CreateNodeCommand>("A", Uuid{}, 0));
    CHECK_FALSE(stack.redo(scene)); // nothing after the cursor
    CHECK(stack.undo(scene));
    CHECK_FALSE(stack.undo(scene)); // nothing before the cursor
}

TEST_CASE("clear discards all history") {
    Scene scene;
    CommandStack stack;
    stack.execute(scene, std::make_unique<CreateNodeCommand>("A", Uuid{}, 0));
    stack.execute(scene, std::make_unique<CreateNodeCommand>("B", Uuid{}, 0));
    CHECK(stack.undo_count() == 2);

    stack.clear();
    CHECK_FALSE(stack.can_undo());
    CHECK_FALSE(stack.can_redo());
    CHECK(stack.undo_count() == 0);
    // The scene is untouched by clearing history.
    CHECK(scene.root_count() == 2);
}

TEST_CASE("undo label names the command that undo would reverse") {
    Scene scene;
    CommandStack stack;
    CHECK(stack.undo_label().empty());
    stack.execute(scene, std::make_unique<CreateNodeCommand>("A", Uuid{}, 0));
    CHECK(stack.undo_label() == "Create Node");
}
