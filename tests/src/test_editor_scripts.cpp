// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for editor script integration (ADR-0063): attaching, replacing, and
// detaching a behaviour through the `ScriptCatalog`, and persisting the
// bindings through the document's save/load. Everything is headless and uses
// the real serializer and the real script host; no mocks.

#include <doctest/doctest.h>

#include "fe/editor/EditorContext.h"
#include "fe/editor/ScriptCatalog.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Node.h"
#include "fe/script/ScriptDesc.h"
#include "fe/serialization/Operation.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <variant>

using namespace fe;
using namespace fe::editor;

namespace {

// A behaviour with one reflected property, so the test can prove that a
// property set in the editor survives a save/load. The property names the
// concrete type, exactly as a component registration does.
class PatrolScript final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "PatrolScript"; }
    [[nodiscard]] Error update(const FrameContext&) override { return Error::none(); }

    f32 speed = 1.0f;
};

// A second behaviour, so "replace" has something to replace with.
class ChaseScript final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "ChaseScript"; }
    [[nodiscard]] Error update(const FrameContext&) override { return Error::none(); }
};

void register_patrol(ScriptRegistry& registry) {
    ScriptDesc desc;
    desc.name = "PatrolScript";
    desc.factory = [] { return std::unique_ptr<Script>(new PatrolScript()); };
    desc.properties.push_back(
        reflect::make_scalar<PatrolScript, f32>("speed", &PatrolScript::speed, reflect::ValueKind::Float));
    registry.register_desc(std::move(desc));
}

void register_chase(ScriptRegistry& registry) {
    ScriptDesc desc;
    desc.name = "ChaseScript";
    desc.factory = [] { return std::unique_ptr<Script>(new ChaseScript()); };
    registry.register_desc(std::move(desc));
}

[[nodiscard]] std::string scratch_path(const char* name) {
    return std::string("editor_scripts_") + name + ".json";
}

} // namespace

TEST_CASE("the catalog attaches a registered behaviour to a node") {
    ScriptCatalog catalog;
    register_patrol(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");

    REQUIRE_FALSE(catalog.attach(*node, "PatrolScript"));
    CHECK(catalog.has_script(node->id()));
    CHECK(catalog.behaviour_of(node->id()) == "PatrolScript");
    // The host sets the back-pointer on a successful attach, so the script knows
    // its node.
    REQUIRE(catalog.script(node->id()) != nullptr);
    CHECK((catalog.script(node->id())->node() == node));
}

TEST_CASE("attaching an unknown behaviour fails and leaves the node unscripted") {
    ScriptCatalog catalog;

    Scene scene;
    Node* node = scene.create_node("Hero");

    CHECK(catalog.attach(*node, "DoesNotExist"));
    CHECK_FALSE(catalog.has_script(node->id()));
    CHECK(catalog.script_count() == 0);
}

TEST_CASE("attaching a second behaviour replaces the first") {
    ScriptCatalog catalog;
    register_patrol(catalog.registry());
    register_chase(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");

    REQUIRE_FALSE(catalog.attach(*node, "PatrolScript"));
    REQUIRE_FALSE(catalog.attach(*node, "ChaseScript"));
    CHECK(catalog.script_count() == 1);
    CHECK(catalog.behaviour_of(node->id()) == "ChaseScript");
}

TEST_CASE("detach removes the binding") {
    ScriptCatalog catalog;
    register_patrol(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");
    REQUIRE_FALSE(catalog.attach(*node, "PatrolScript"));

    CHECK(catalog.detach(node->id()));
    CHECK_FALSE(catalog.has_script(node->id()));
    // Detaching again is a no-op, not an error.
    CHECK_FALSE(catalog.detach(node->id()));
}

TEST_CASE("desc_of exposes the bound behaviour's properties to the inspector") {
    ScriptCatalog catalog;
    register_patrol(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");
    REQUIRE_FALSE(catalog.attach(*node, "PatrolScript"));

    const ScriptDesc* desc = catalog.desc_of(node->id());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->properties.size() == 1);
    CHECK(desc->properties.front().name == "speed");
}

TEST_CASE("a scene saved from the editor records scripts and reloads them") {
    const std::string path = scratch_path("roundtrip");
    std::filesystem::remove(path);

    {
        EditorContext context;
        register_patrol(context.scripts().registry());
        Node* node = context.scene().create_node("Hero");
        REQUIRE_FALSE(context.scripts().attach(*node, "PatrolScript"));

        // A tuned property value, so the reload proves the value persists and
        // not merely the behaviour name.
        auto* script = dynamic_cast<PatrolScript*>(context.scripts().script(node->id()));
        REQUIRE(script != nullptr);
        script->speed = 4.5f;

        REQUIRE_FALSE(context.save_scene(path));
    }

    // A fresh editor session: the behaviour must be registered before the load,
    // because the document names it rather than carrying its code.
    EditorContext reopened;
    register_patrol(reopened.scripts().registry());
    REQUIRE_FALSE(reopened.load_scene(path));

    REQUIRE(reopened.scene().node_count() == 1);
    const Uuid node_id = reopened.scene().roots().front()->id();
    CHECK(reopened.scripts().behaviour_of(node_id) == "PatrolScript");
    auto* script = dynamic_cast<PatrolScript*>(reopened.scripts().script(node_id));
    REQUIRE(script != nullptr);
    CHECK(script->speed == doctest::Approx(4.5f));

    std::filesystem::remove(path);
}

TEST_CASE("loading a scene drops the previous document's scripts") {
    const std::string with_scripts = scratch_path("with_scripts");
    const std::string without = scratch_path("without");
    std::filesystem::remove(with_scripts);
    std::filesystem::remove(without);

    EditorContext context;
    register_patrol(context.scripts().registry());

    Node* node = context.scene().create_node("Hero");
    REQUIRE_FALSE(context.scripts().attach(*node, "PatrolScript"));
    REQUIRE_FALSE(context.save_scene(with_scripts));

    // A second document, saved with no scripts at all.
    REQUIRE_FALSE(context.new_scene());
    CHECK(context.scripts().script_count() == 0);
    REQUIRE_FALSE(context.save_scene(without));

    // Loading the scripted document brings its binding back.
    REQUIRE_FALSE(context.load_scene(with_scripts));
    CHECK(context.scripts().script_count() == 1);

    // Loading the unscripted document must not inherit the previous binding.
    REQUIRE_FALSE(context.load_scene(without));
    CHECK(context.scripts().script_count() == 0);

    std::filesystem::remove(with_scripts);
    std::filesystem::remove(without);
}

TEST_CASE("a plain scene document is unchanged by the editor's script catalog") {
    const std::string path = scratch_path("plain");
    std::filesystem::remove(path);

    EditorContext context;
    register_patrol(context.scripts().registry());
    [[maybe_unused]] Node* hero = context.scene().create_node("Hero");
    REQUIRE_FALSE(context.save_scene(path));

    // No node carried a script, so the document has no "scripts" array. The
    // stream is closed before the file is removed: Windows refuses to delete a
    // file that is still open.
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("scripts") == std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("editing a script property through the editor is undoable and redoable") {
    EditorContext context;
    register_patrol(context.scripts().registry());

    Node* node = context.scene().create_node("Hero");
    REQUIRE_FALSE(context.scripts().attach(*node, "PatrolScript"));

    auto* script = dynamic_cast<PatrolScript*>(context.scripts().script(node->id()));
    REQUIRE(script != nullptr);
    REQUIRE(script->speed == doctest::Approx(1.0f));

    // The edit goes through the operation model as a serializable
    // `SetScriptProperty`, so the command stack owns it like any other edit.
    REQUIRE(context.edit_script_property(node->id(), "PatrolScript", {"speed"}, reflect::Value{3.5}));
    CHECK(script->speed == doctest::Approx(3.5f));
    REQUIRE(context.can_undo());

    REQUIRE(context.undo());
    CHECK(script->speed == doctest::Approx(1.0f));
    REQUIRE(context.can_redo());

    REQUIRE(context.redo());
    CHECK(script->speed == doctest::Approx(3.5f));
}

TEST_CASE("a script property edit survives a save and reload") {
    const std::string path = scratch_path("edited_property");
    std::filesystem::remove(path);

    {
        EditorContext context;
        register_patrol(context.scripts().registry());
        Node* node = context.scene().create_node("Hero");
        REQUIRE_FALSE(context.scripts().attach(*node, "PatrolScript"));
        REQUIRE(context.edit_script_property(node->id(), "PatrolScript", {"speed"}, reflect::Value{7.25}));
        REQUIRE_FALSE(context.save_scene(path));
    }

    EditorContext reopened;
    register_patrol(reopened.scripts().registry());
    REQUIRE_FALSE(reopened.load_scene(path));
    const Uuid node_id = reopened.scene().roots().front()->id();
    auto* script = dynamic_cast<PatrolScript*>(reopened.scripts().script(node_id));
    REQUIRE(script != nullptr);
    CHECK(script->speed == doctest::Approx(7.25f));

    std::filesystem::remove(path);
}

TEST_CASE("the catalog reads and writes a script property through the operation seam") {
    ScriptCatalog catalog;
    register_patrol(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");
    REQUIRE_FALSE(catalog.attach(*node, "PatrolScript"));

    CHECK(catalog.behaviour_of(node->id()) == "PatrolScript");
    auto before = catalog.read(node->id(), {"speed"});
    REQUIRE(before.has_value());
    CHECK(std::get<f64>(before.value()) == doctest::Approx(1.0));

    CHECK(catalog.write(node->id(), {"speed"}, reflect::Value{6.0}));
    auto after = catalog.read(node->id(), {"speed"});
    REQUIRE(after.has_value());
    CHECK(std::get<f64>(after.value()) == doctest::Approx(6.0));

    // An unscripted node and an unknown path both report absence, never a guess.
    Node* bare = scene.create_node("Bare");
    CHECK(catalog.behaviour_of(bare->id()).empty());
    CHECK_FALSE(catalog.read(bare->id(), {"speed"}).has_value());
    CHECK_FALSE(catalog.write(bare->id(), {"speed"}, reflect::Value{1.0}));
    CHECK_FALSE(catalog.read(node->id(), {"missing"}).has_value());
}

TEST_CASE("a script property operation is a no-op when the node runs another behaviour") {
    ScriptCatalog catalog;
    register_patrol(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");
    REQUIRE_FALSE(catalog.attach(*node, "PatrolScript"));

    auto* script = dynamic_cast<PatrolScript*>(catalog.script(node->id()));
    REQUIRE(script != nullptr);
    script->speed = 2.0f;

    // The operation names a behaviour the node does not run, so apply skips it
    // rather than writing into the wrong script.
    serialization::Operation op;
    op.kind = serialization::OperationKind::SetScriptProperty;
    op.node_id = node->id();
    op.name = "SomeOtherScript";
    op.path = {"speed"};
    op.value = reflect::Value{9.0};
    REQUIRE_FALSE(serialization::apply(op, scene, nullptr, nullptr, &catalog));
    CHECK(script->speed == doctest::Approx(2.0f));

    // The matching behaviour writes, and the inverse (read before the apply, as
    // the command stack reads it) holds the value the script started with.
    op.name = "PatrolScript";
    Result<serialization::Operation> inv = serialization::inverse(op, scene, &catalog);
    REQUIRE(inv);
    CHECK(inv.value().kind == serialization::OperationKind::SetScriptProperty);
    CHECK(inv.value().name == "PatrolScript");
    CHECK(std::get<f64>(inv.value().value) == doctest::Approx(2.0));

    REQUIRE_FALSE(serialization::apply(op, scene, nullptr, nullptr, &catalog));
    CHECK(script->speed == doctest::Approx(9.0f));
}

// A behaviour with a resource-reference property, so the test can prove a script
// reference is edited through the operation model like a component one. The
// accessors mirror a component registration exactly: the getter yields the
// manager key, the resolver turns a key back into a live resource.
class ArmedScript final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "ArmedScript"; }
    [[nodiscard]] Error update(const FrameContext&) override { return Error::none(); }

    std::shared_ptr<Resource> weapon;
};

void register_armed(ScriptRegistry& registry) {
    ScriptDesc desc;
    desc.name = "ArmedScript";
    desc.factory = [] { return std::unique_ptr<Script>(new ArmedScript()); };
    desc.properties.push_back(reflect::make_resource_ref<ArmedScript, std::shared_ptr<Resource>>(
        "weapon", &ArmedScript::weapon, "Weapon",
        [](const std::shared_ptr<Resource>& slot) -> std::string {
            return slot != nullptr ? slot->path() : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));
    registry.register_desc(std::move(desc));
}

TEST_CASE("the catalog reads and writes a script resource reference through the seam") {
    ScriptCatalog catalog;
    register_armed(catalog.registry());

    Scene scene;
    Node* node = scene.create_node("Hero");
    REQUIRE_FALSE(catalog.attach(*node, "ArmedScript"));

    auto* script = dynamic_cast<ArmedScript*>(catalog.script(node->id()));
    REQUIRE(script != nullptr);

    // An empty slot reads as an empty reference, which is what a clear writes.
    auto before = catalog.read_resource(node->id(), {"weapon"});
    REQUIRE(before.has_value());
    CHECK(before.value().empty());

    // An empty reference clears the slot, exactly as the component resolver does.
    ResourceManager resources;
    CHECK(catalog.write_resource(node->id(), {"weapon"}, "", resources));
    CHECK(script->weapon == nullptr);

    // A non-resource path and an unscripted node both report absence, never a
    // guess, so the operation model can treat them as un-invertible.
    CHECK_FALSE(catalog.read_resource(node->id(), {"speed"}).has_value());
    CHECK_FALSE(catalog.write_resource(node->id(), {"speed"}, "wpn:x", resources));
    Node* bare = scene.create_node("Bare");
    CHECK_FALSE(catalog.read_resource(bare->id(), {"weapon"}).has_value());
    CHECK_FALSE(catalog.write_resource(bare->id(), {"weapon"}, "wpn:x", resources));
}

TEST_CASE("a script resource edit is undoable and is a no-op for another behaviour") {
    EditorContext context;
    register_armed(context.scripts().registry());
    Node* node = context.scene().create_node("Hero");
    REQUIRE_FALSE(context.scripts().attach(*node, "ArmedScript"));

    // The manager has no loader for the scheme, so the reference cannot resolve
    // and the slot is left untouched - a bad key never clobbers a good one. The
    // operation still applies and inverts (both are empty), which is the point:
    // the edit is a normal undoable step.
    REQUIRE(context.edit_script_resource(node->id(), "ArmedScript", {"weapon"}, "wpn:sword"));
    auto* script = dynamic_cast<ArmedScript*>(context.scripts().script(node->id()));
    REQUIRE(script != nullptr);
    CHECK(script->weapon == nullptr);
    REQUIRE(context.can_undo());
    REQUIRE(context.undo());

    // The operation names a behaviour the node does not run, so apply skips it.
    serialization::Operation op;
    op.kind = serialization::OperationKind::SetScriptResource;
    op.node_id = node->id();
    op.name = "SomeOtherScript";
    op.path = {"weapon"};
    op.reference = "wpn:axe";
    CHECK_FALSE(serialization::apply(op, context.scene(), nullptr, nullptr, &context.scripts()));
}

TEST_CASE("a script resource operation round-trips through JSON") {
    serialization::Operation op;
    op.kind = serialization::OperationKind::SetScriptResource;
    op.node_id = Uuid::generate();
    op.name = "ArmedScript";
    op.path = {"weapon"};
    op.reference = "wpn:sword";

    const std::string text = serialization::to_json(op);
    serialization::Operation parsed;
    REQUIRE_FALSE(serialization::from_json(text, parsed));
    CHECK(parsed.kind == serialization::OperationKind::SetScriptResource);
    CHECK(parsed.node_id == op.node_id);
    CHECK(parsed.name == "ArmedScript");
    REQUIRE(parsed.path.size() == 1);
    CHECK(parsed.path.front() == "weapon");
    CHECK(parsed.reference == "wpn:sword");
}
