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
#include "fe/scene/Node.h"
#include "fe/script/ScriptDesc.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

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
