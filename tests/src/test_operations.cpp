// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The local operation model (docs/adr/0047-local-operation-model.md). An
// operation is the serializable, id-addressed form of an editor edit: it
// round-trips through JSON, applies to a scene, and has an inverse derived from
// the document. The editor's `OperationCommand` wraps one onto the existing
// `CommandStack`, and every operation must leave the document in the same state
// its command counterpart did. These tests are headless - no window, no ImGui.

#include <doctest/doctest.h>

#include "fe/editor/Command.h"
#include "fe/editor/Commands.h"
#include "fe/editor/OperationCommand.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/Value.h"
#include "fe/resources/Resource.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/Operation.h"

#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace fe;
using namespace fe::editor;
using serialization::Operation;
using serialization::OperationKind;

namespace {

// A round-trips an operation through JSON and checks it comes back equal.
void check_round_trip(const Operation& op) {
    const std::string text = serialization::to_json(op);
    Operation parsed;
    const Error error = serialization::from_json(text, parsed);
    REQUIRE_FALSE(error);
    CHECK(parsed.kind == op.kind);
    CHECK(parsed.node_id == op.node_id);
    CHECK(parsed.parent_id == op.parent_id);
    CHECK(parsed.component_id == op.component_id);
    CHECK(parsed.index == op.index);
    CHECK(parsed.name == op.name);
    CHECK(parsed.property_name == op.property_name);
    CHECK(parsed.path == op.path);
    CHECK(parsed.value == op.value);
    CHECK(parsed.reference == op.reference);
}

// A stub resource loader so a resource edit can be tested without a GPU (the
// same shape test_commands and test_serialization use).
class StubTexture final : public Resource {
public:
    explicit StubTexture(std::string path) : Resource(std::move(path)) {}
    [[nodiscard]] const char* type_name() const noexcept override { return "Texture2D"; }
};

class StubTextureLoader final : public ResourceLoader {
public:
    [[nodiscard]] std::string_view scheme() const noexcept override { return "tex"; }
    [[nodiscard]] bool supports(std::string_view extension) const noexcept override { return extension == ".png"; }
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override {
        return std::static_pointer_cast<Resource>(std::make_shared<StubTexture>(std::string(path)));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Serialization round-trip
// ---------------------------------------------------------------------------

TEST_CASE("every operation kind round-trips through JSON unchanged") {
    Operation create;
    create.kind = OperationKind::CreateNode;
    create.node_id = Uuid::generate();
    create.parent_id = Uuid::generate();
    create.index = 3;
    create.name = "Sprite";
    check_round_trip(create);

    Operation del;
    del.kind = OperationKind::DeleteNode;
    del.node_id = Uuid::generate();
    check_round_trip(del);

    Operation reparent;
    reparent.kind = OperationKind::ReparentNode;
    reparent.node_id = Uuid::generate();
    reparent.parent_id = Uuid::generate();
    reparent.index = 1;
    check_round_trip(reparent);

    Operation rename;
    rename.kind = OperationKind::RenameNode;
    rename.node_id = Uuid::generate();
    rename.name = "Hero";
    check_round_trip(rename);

    Operation add;
    add.kind = OperationKind::AddComponent;
    add.node_id = Uuid::generate();
    add.component_id = Uuid::generate();
    add.name = "Sprite2D";
    add.index = 2;
    check_round_trip(add);

    Operation remove;
    remove.kind = OperationKind::RemoveComponent;
    remove.node_id = Uuid::generate();
    remove.component_id = Uuid::generate();
    check_round_trip(remove);

    Operation set;
    set.kind = OperationKind::SetProperty;
    set.node_id = Uuid::generate();
    set.component_id = Uuid::generate();
    set.path = {"size", "x"};
    set.value = static_cast<f64>(12.5);
    check_round_trip(set);

    Operation resource;
    resource.kind = OperationKind::SetResource;
    resource.node_id = Uuid::generate();
    resource.component_id = Uuid::generate();
    resource.property_name = "texture";
    resource.reference = "tex:assets/hero.png";
    check_round_trip(resource);

    Operation script;
    script.kind = OperationKind::SetScriptProperty;
    script.node_id = Uuid::generate();
    script.name = "PatrolScript";
    script.path = {"speed"};
    script.value = static_cast<f64>(2.5);
    check_round_trip(script);
}

TEST_CASE("a value's kind is preserved, so an int is not read back as a float") {
    Operation op;
    op.kind = OperationKind::SetProperty;
    op.node_id = Uuid::generate();
    op.component_id = Uuid::generate();
    op.path = {"layer"};
    op.value = static_cast<i64>(7);

    const std::string text = serialization::to_json(op);
    CHECK(text.find("\"int\"") != std::string::npos);

    Operation parsed;
    REQUIRE_FALSE(serialization::from_json(text, parsed));
    REQUIRE(std::holds_alternative<i64>(parsed.value));
    CHECK(std::get<i64>(parsed.value) == 7);
}

TEST_CASE("the reader rejects an unknown kind rather than guessing") {
    Operation op;
    const Error error = serialization::from_json(R"({"kind":"teleport_node"})", op);
    CHECK(error);
}

TEST_CASE("the reader rejects a missing or malformed field") {
    Operation op;
    CHECK(serialization::from_json(R"({"kind":"delete_node"})", op));               // no node_id
    CHECK(serialization::from_json(R"({"kind":"delete_node","node_id":"x"})", op)); // malformed uuid
    CHECK(serialization::from_json(R"({"kind":"rename_node","node_id":"00000000-0000-0000-0000-000000000000"})",
                                   op)); // no name
    CHECK(serialization::from_json("not json", op));
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

TEST_CASE("a create operation makes a node with the exact id it carries") {
    reflect::register_builtin_types();
    Scene scene;

    const Uuid node_id = Uuid::generate();
    Operation op;
    op.kind = OperationKind::CreateNode;
    op.node_id = node_id;
    op.name = "Hero";
    op.index = 0;

    REQUIRE_FALSE(serialization::apply(op, scene));
    Node* node = scene.find_node(node_id);
    REQUIRE(node != nullptr);
    CHECK(node->name() == "Hero");

    // Applying the same operation again is a no-op: the id is already present.
    REQUIRE_FALSE(serialization::apply(op, scene));
    CHECK(scene.root_count() == 1);
}

TEST_CASE("apply names a component by id, not by index") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Node");
    // Two components of the same type: an index would be ambiguous, an id is not.
    auto* first = node->add_component<Sprite2D>();
    auto* second = node->add_component<Sprite2D>();
    first->layer = 1;
    second->layer = 2;

    Operation op;
    op.kind = OperationKind::SetProperty;
    op.node_id = node->id();
    op.component_id = second->id();
    op.path = {"layer"};
    op.value = static_cast<i64>(9);

    REQUIRE_FALSE(serialization::apply(op, scene));
    CHECK(first->layer == 1); // untouched
    CHECK(second->layer == 9);
}

TEST_CASE("a delete operation hands the removed subtree to the capture sink") {
    reflect::register_builtin_types();
    Scene scene;
    Node* root = scene.create_node("Root");
    Node* child = root->create_child<Node>("Child");
    const Uuid child_id = child->id();

    Operation op;
    op.kind = OperationKind::DeleteNode;
    op.node_id = child_id;

    serialization::AppliedObjects captured;
    REQUIRE_FALSE(serialization::apply(op, scene, nullptr, &captured));
    CHECK(scene.find_node(child_id) == nullptr);
    REQUIRE(captured.node != nullptr);
    CHECK(captured.node->id() == child_id);
    CHECK(captured.parent_id == root->id());
}

TEST_CASE("a resource operation resolves through the manager and a null manager is a no-op") {
    reflect::register_builtin_types();
    ResourceManager resources;
    resources.register_loader(std::make_unique<StubTextureLoader>());

    Scene scene;
    Node* node = scene.create_node("Hero");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = std::make_shared<StubTexture>("assets/old.png");

    Operation op;
    op.kind = OperationKind::SetResource;
    op.node_id = node->id();
    op.component_id = sprite->id();
    op.property_name = "texture";
    op.reference = "tex:assets/new.png";

    // A null manager cannot resolve, so nothing changes.
    REQUIRE_FALSE(serialization::apply(op, scene, nullptr));
    REQUIRE(sprite->texture != nullptr);
    CHECK(sprite->texture->path() == "assets/old.png");

    REQUIRE_FALSE(serialization::apply(op, scene, &resources));
    REQUIRE(sprite->texture != nullptr);
    CHECK(sprite->texture->path() == "assets/new.png");
}

// ---------------------------------------------------------------------------
// inverse
// ---------------------------------------------------------------------------

TEST_CASE("a property write's inverse restores the value the document held") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->layer = 4;

    Operation op;
    op.kind = OperationKind::SetProperty;
    op.node_id = node->id();
    op.component_id = sprite->id();
    op.path = {"layer"};
    op.value = static_cast<i64>(11);

    Result<Operation> inv = serialization::inverse(op, scene);
    REQUIRE(inv);
    REQUIRE(std::holds_alternative<i64>(inv.value().value));
    CHECK(std::get<i64>(inv.value().value) == 4);

    // The inverse is a pure function of the document: applying it restores 4.
    REQUIRE_FALSE(serialization::apply(inv.value(), scene));
    CHECK(sprite->layer == 4);
}

TEST_CASE("a reparent's inverse names the old parent and index") {
    reflect::register_builtin_types();
    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");
    Node* moved = a->create_child<Node>("Moved");
    const Uuid moved_id = moved->id();

    Operation op;
    op.kind = OperationKind::ReparentNode;
    op.node_id = moved_id;
    op.parent_id = b->id();
    op.index = 0;

    Result<Operation> inv = serialization::inverse(op, scene);
    REQUIRE(inv);
    CHECK(inv.value().kind == OperationKind::ReparentNode);
    CHECK(inv.value().parent_id == a->id());
    CHECK(inv.value().index == 0);
}

TEST_CASE("a create's inverse is a delete, and a delete has no single-operation inverse") {
    reflect::register_builtin_types();
    Scene scene;

    Operation create;
    create.kind = OperationKind::CreateNode;
    create.node_id = Uuid::generate();
    Result<Operation> inv = serialization::inverse(create, scene);
    REQUIRE(inv);
    CHECK(inv.value().kind == OperationKind::DeleteNode);
    CHECK(inv.value().node_id == create.node_id);

    Operation del;
    del.kind = OperationKind::DeleteNode;
    del.node_id = Uuid::generate();
    CHECK(serialization::inverse(del, scene).error());
}

// ---------------------------------------------------------------------------
// OperationCommand on the CommandStack
// ---------------------------------------------------------------------------

TEST_CASE("an operation command creates a node and undoes to nothing") {
    reflect::register_builtin_types();
    Scene scene;

    const Uuid node_id = Uuid::generate();
    Operation op;
    op.kind = OperationKind::CreateNode;
    op.node_id = node_id;
    op.name = "Root";
    op.index = 0;

    CommandStack stack;
    REQUIRE(stack.execute(scene, std::make_unique<OperationCommand>(op)));
    REQUIRE(scene.find_node(node_id) != nullptr);

    REQUIRE(stack.undo(scene));
    CHECK(scene.find_node(node_id) == nullptr);

    // Redo restores the *same* node id, which is what lets a later operation
    // address it deterministically.
    REQUIRE(stack.redo(scene));
    Node* node = scene.find_node(node_id);
    REQUIRE(node != nullptr);
    CHECK(node->name() == "Root");
}

TEST_CASE("an operation command deletes a subtree and restores it with identity on undo") {
    reflect::register_builtin_types();
    Scene scene;
    Node* root = scene.create_node("Root");
    Node* child = root->create_child<Node>("Child");
    auto* sprite = child->add_component<Sprite2D>();
    sprite->layer = 5;
    const Uuid child_id = child->id();
    const Uuid sprite_id = sprite->id();

    Operation op;
    op.kind = OperationKind::DeleteNode;
    op.node_id = child_id;

    CommandStack stack;
    REQUIRE(stack.execute(scene, std::make_unique<OperationCommand>(op)));
    CHECK(scene.find_node(child_id) == nullptr);

    REQUIRE(stack.undo(scene));
    Node* restored = scene.find_node(child_id);
    REQUIRE(restored != nullptr);
    // A raw-pointer identity check: the extra parens stop MSVC's doctest from
    // picking the shared_ptr operator<< (see AGENTS.md).
    CHECK((restored->parent() == root));
    REQUIRE(restored->component_count() == 1);
    CHECK(restored->components().front()->id() == sprite_id); // identity survived
    CHECK(restored->components().front()->type_name() == std::string(Sprite2D::kTypeName));
}

TEST_CASE("an operation command's property edit coalesces a drag into one undo step") {
    reflect::register_builtin_types();
    Scene scene;
    Node* node = scene.create_node("Sprite");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->layer = 0;

    const Uuid node_id = node->id();
    const Uuid component_id = sprite->id();

    CommandStack stack;
    for (i64 value : {1, 2, 3}) {
        Operation op;
        op.kind = OperationKind::SetProperty;
        op.node_id = node_id;
        op.component_id = component_id;
        op.path = {"layer"};
        op.value = value;
        stack.execute(scene, std::make_unique<OperationCommand>(op));
    }
    CHECK(sprite->layer == 3);
    CHECK(stack.undo_count() == 1); // one gesture, one step

    CHECK(stack.undo(scene));
    CHECK(sprite->layer == 0);
    CHECK_FALSE(stack.can_undo());
}

TEST_CASE("an operation command's resource edit is undoable through the registry") {
    reflect::register_builtin_types();
    ResourceManager resources;
    resources.register_loader(std::make_unique<StubTextureLoader>());

    Scene scene;
    Node* node = scene.create_node("Hero");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->texture = std::make_shared<StubTexture>("assets/old.png");

    Operation op;
    op.kind = OperationKind::SetResource;
    op.node_id = node->id();
    op.component_id = sprite->id();
    op.property_name = "texture";
    op.reference = "tex:assets/new.png";

    CommandStack stack;
    REQUIRE(stack.execute(scene, std::make_unique<OperationCommand>(op, &resources)));
    REQUIRE(sprite->texture != nullptr);
    CHECK(sprite->texture->path() == "assets/new.png");

    REQUIRE(stack.undo(scene));
    REQUIRE(sprite->texture != nullptr);
    CHECK(sprite->texture->path() == "assets/old.png");
}

// ---------------------------------------------------------------------------
// Equivalence with the existing commands
// ---------------------------------------------------------------------------

TEST_CASE("an operation and its command counterpart leave the same document state") {
    reflect::register_builtin_types();

    // Add a component both ways and compare the resulting component count and
    // type; the ids differ (each is fresh), so compare by type and count.
    {
        Scene via_command;
        Node* node = via_command.create_node("Node");
        CommandStack stack;
        stack.execute(via_command,
                      std::make_unique<AddComponentCommand>(node->id(), std::string(Sprite2D::kTypeName), 0));

        Scene via_operation;
        Node* other = via_operation.create_node("Node");
        Operation op;
        op.kind = OperationKind::AddComponent;
        op.node_id = other->id();
        op.component_id = Uuid::generate();
        op.name = std::string(Sprite2D::kTypeName);
        op.index = 0;
        (void)serialization::apply(op, via_operation);

        REQUIRE(node->component_count() == other->component_count());
        CHECK(node->components().front()->type_name() == other->components().front()->type_name());
    }

    // Set a property both ways and compare the value.
    {
        Scene via_command;
        Node* node = via_command.create_node("Sprite");
        auto* sprite = node->add_component<Sprite2D>();
        sprite->layer = 1;
        CommandStack stack;
        stack.execute(via_command,
                      std::make_unique<SetPropertyCommand>(node->id(), 0, std::vector<std::string>{"layer"},
                                                           reflect::Value{static_cast<i64>(8)}));

        Scene via_operation;
        Node* other = via_operation.create_node("Sprite");
        auto* other_sprite = other->add_component<Sprite2D>();
        other_sprite->layer = 1;
        Operation op;
        op.kind = OperationKind::SetProperty;
        op.node_id = other->id();
        op.component_id = other_sprite->id();
        op.path = {"layer"};
        op.value = static_cast<i64>(8);
        (void)serialization::apply(op, via_operation);

        CHECK(sprite->layer == other_sprite->layer);
    }

    // Reparent both ways and compare the resulting root order.
    {
        Scene via_command;
        Node* a = via_command.create_node("A");
        Node* b = via_command.create_node("B");
        Node* moved = a->create_child<Node>("Moved");
        CommandStack stack;
        stack.execute(via_command, std::make_unique<ReparentNodeCommand>(moved->id(), b->id(), 0));

        Scene via_operation;
        Node* oa = via_operation.create_node("A");
        Node* ob = via_operation.create_node("B");
        Node* omoved = oa->create_child<Node>("Moved");
        Operation op;
        op.kind = OperationKind::ReparentNode;
        op.node_id = omoved->id();
        op.parent_id = ob->id();
        op.index = 0;
        (void)serialization::apply(op, via_operation);

        CHECK(a->child_count() == oa->child_count());
        CHECK(b->child_count() == ob->child_count());
    }
}

// --- SetTransform -----------------------------------------------------------
//
// A node's transform is not a component and has no reflection entry, so it needs
// its own operation kind. These tests prove the operation round-trips, applies,
// inverts, and - crucially - that a drag over several frames coalesces into one
// undo step, which is what makes drag-to-move in the viewport usable.

TEST_CASE("a SetTransform operation round-trips through JSON") {
    Operation op;
    op.kind = OperationKind::SetTransform;
    op.node_id = Uuid::generate();
    op.path = {"position"};
    op.transform_value = Vec3{1.5f, -2.5f, 3.5f};

    check_round_trip(op);

    // The parsed operation must carry the vector, not just the path.
    Operation parsed;
    REQUIRE_FALSE(serialization::from_json(serialization::to_json(op), parsed));
    CHECK(parsed.transform_value.x == doctest::Approx(1.5f));
    CHECK(parsed.transform_value.y == doctest::Approx(-2.5f));
    CHECK(parsed.transform_value.z == doctest::Approx(3.5f));
}

TEST_CASE("a SetTransform operation writes one field and leaves the others") {
    Scene scene;
    Node* node = scene.create_node("Node");
    node->transform().position = Vec3{1.0f, 2.0f, 3.0f};
    node->transform().rotation = Vec3{0.1f, 0.2f, 0.3f};

    Operation op;
    op.kind = OperationKind::SetTransform;
    op.node_id = node->id();
    op.path = {"position"};
    op.transform_value = Vec3{9.0f, 8.0f, 7.0f};
    REQUIRE_FALSE(serialization::apply(op, scene));

    CHECK(node->transform().position.x == doctest::Approx(9.0f));
    CHECK(node->transform().position.y == doctest::Approx(8.0f));
    CHECK(node->transform().position.z == doctest::Approx(7.0f));
    // Rotation is untouched: the path names one field.
    CHECK(node->transform().rotation.x == doctest::Approx(0.1f));
}

TEST_CASE("inverting a SetTransform operation restores the previous value") {
    Scene scene;
    Node* node = scene.create_node("Node");
    node->transform().position = Vec3{1.0f, 2.0f, 3.0f};

    Operation op;
    op.kind = OperationKind::SetTransform;
    op.node_id = node->id();
    op.path = {"position"};
    op.transform_value = Vec3{9.0f, 8.0f, 7.0f};

    Result<Operation> inverse = serialization::inverse(op, scene);
    REQUIRE(inverse);
    CHECK(inverse.value().kind == OperationKind::SetTransform);
    CHECK(inverse.value().transform_value.x == doctest::Approx(1.0f));
    CHECK(inverse.value().transform_value.y == doctest::Approx(2.0f));
    CHECK(inverse.value().transform_value.z == doctest::Approx(3.0f));

    // Applying the inverse restores the original exactly.
    REQUIRE_FALSE(serialization::apply(inverse.value(), scene));
    CHECK(node->transform().position.x == doctest::Approx(1.0f));
}

TEST_CASE("a malformed SetTransform operation is a no-op, not a NaN") {
    Scene scene;
    Node* node = scene.create_node("Node");
    node->transform().position = Vec3{1.0f, 2.0f, 3.0f};

    // An unknown field name.
    Operation bad_field;
    bad_field.kind = OperationKind::SetTransform;
    bad_field.node_id = node->id();
    bad_field.path = {"nonsense"};
    bad_field.transform_value = Vec3{9.0f, 9.0f, 9.0f};
    REQUIRE_FALSE(serialization::apply(bad_field, scene));
    CHECK(node->transform().position.x == doctest::Approx(1.0f));

    // A non-finite component.
    Operation bad_value;
    bad_value.kind = OperationKind::SetTransform;
    bad_value.node_id = node->id();
    bad_value.path = {"position"};
    bad_value.transform_value = Vec3{std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f};
    REQUIRE_FALSE(serialization::apply(bad_value, scene));
    CHECK(node->transform().position.x == doctest::Approx(1.0f));

    // A path of the wrong length.
    Operation bad_path;
    bad_path.kind = OperationKind::SetTransform;
    bad_path.node_id = node->id();
    bad_path.path = {"position", "x"};
    bad_path.transform_value = Vec3{9.0f, 9.0f, 9.0f};
    REQUIRE_FALSE(serialization::apply(bad_path, scene));
    CHECK(node->transform().position.x == doctest::Approx(1.0f));
}

TEST_CASE("a drag over several frames coalesces into one undo step") {
    Scene scene;
    Node* node = scene.create_node("Node");
    node->transform().position = Vec3{0.0f, 0.0f, 0.0f};

    CommandStack stack;

    // Frame 1 of the drag.
    Operation first;
    first.kind = OperationKind::SetTransform;
    first.node_id = node->id();
    first.path = {"position"};
    first.transform_value = Vec3{1.0f, 1.0f, 0.0f};
    stack.execute(scene, std::make_unique<OperationCommand>(first));
    REQUIRE(node->transform().position.x == doctest::Approx(1.0f));

    // Frames 2 and 3: the same node and axis, so each coalesces with the last.
    for (const f32 x : {2.0f, 3.0f}) {
        Operation next;
        next.kind = OperationKind::SetTransform;
        next.node_id = node->id();
        next.path = {"position"};
        next.transform_value = Vec3{x, x, 0.0f};
        stack.execute(scene, std::make_unique<OperationCommand>(next));
    }
    CHECK(node->transform().position.x == doctest::Approx(3.0f));

    // The whole gesture is one step, and one undo returns to where it started.
    CHECK(stack.undo_count() == 1);
    CHECK(stack.undo_label() == "Set Transform");
    REQUIRE(stack.undo(scene));
    CHECK(node->transform().position.x == doctest::Approx(0.0f));
    CHECK(node->transform().position.y == doctest::Approx(0.0f));
    CHECK_FALSE(stack.can_undo());
}

TEST_CASE("two different transform fields do not coalesce into one step") {
    Scene scene;
    Node* node = scene.create_node("Node");

    CommandStack stack;
    Operation move;
    move.kind = OperationKind::SetTransform;
    move.node_id = node->id();
    move.path = {"position"};
    move.transform_value = Vec3{1.0f, 0.0f, 0.0f};
    stack.execute(scene, std::make_unique<OperationCommand>(move));

    // A rotation edit is a different field, so it is its own undo step even
    // though the gesture has not been explicitly ended.
    Operation rotate;
    rotate.kind = OperationKind::SetTransform;
    rotate.node_id = node->id();
    rotate.path = {"rotation"};
    rotate.transform_value = Vec3{0.0f, 0.0f, 1.0f};
    stack.execute(scene, std::make_unique<OperationCommand>(rotate));

    CHECK(stack.undo_count() == 2);
    REQUIRE(stack.undo(scene));
    CHECK(node->transform().rotation.z == doctest::Approx(0.0f));
    CHECK(node->transform().position.x == doctest::Approx(1.0f));
}
