// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for reflection-backed script bindings (ADR-0063): a script's reflected
// properties round-trip through the scene serializer, and the binding survives a
// save/load cycle by name. Headless -- no window, no RHI, no editor -- and no
// mock stands in for the serializer: the tests write a real document and read it
// back through the real ScriptBinding.
#include <doctest/doctest.h>

#include "fe/core/Log.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptBinding.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <memory>
#include <string>
#include <string_view>

using namespace fe;

namespace {

// A gameplay behaviour with two authored fields: a speed and a label. The
// fields are reflected exactly as a component's would be, which is what makes
// them persist and (in the editor) appear in the inspector.
class PatrolScript final : public Script {
public:
    explicit PatrolScript(f32 speed = 1.0f, std::string label = "patrol") noexcept
        : speed_(speed), label_(std::move(label)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "PatrolScript"; }

    Error attach(Node& node) override {
        (void)node;
        ++attaches_;
        return Error::none();
    }

    Error update(const FrameContext& context) override {
        (void)context;
        ++ticks_;
        if (node() != nullptr) {
            node()->transform().position.x += speed_;
        }
        return Error::none();
    }

    [[nodiscard]] f32 speed() const noexcept { return speed_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }
    [[nodiscard]] u32 attaches() const noexcept { return attaches_; }
    [[nodiscard]] u32 ticks() const noexcept { return ticks_; }

    f32 speed_ = 1.0f;
    std::string label_;

private:
    u32 attaches_ = 0;
    u32 ticks_ = 0;
};

// A behaviour with no reflected properties, so a binding with no data still
// round-trips.
class BareScript final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "BareScript"; }

    Error update(const FrameContext& context) override {
        (void)context;
        return Error::none();
    }
};

ScriptRegistry make_registry() {
    ScriptRegistry registry;

    ScriptDesc desc;
    desc.name = "PatrolScript";
    desc.factory = [] { return std::unique_ptr<Script>(new PatrolScript()); };
    desc.properties.push_back(
        reflect::make_scalar<PatrolScript, f32>("speed", &PatrolScript::speed_, reflect::ValueKind::Float));
    desc.properties.push_back(
        reflect::make_scalar<PatrolScript, std::string>("label", &PatrolScript::label_, reflect::ValueKind::String));
    registry.register_desc(std::move(desc));

    registry.register_script("BareScript", [] { return std::unique_ptr<Script>(new BareScript()); });
    return registry;
}

} // namespace

TEST_CASE("a script's reflected properties survive a save/load cycle") {
    ScriptRegistry registry = make_registry();
    ScriptHost host;

    Scene authored;
    Node* hero = authored.create_node("Hero");
    REQUIRE_FALSE(host.attach_script(*hero, std::make_unique<PatrolScript>(3.5f, "left-right")));
    REQUIRE(host.script_count() == 1);

    script::ScriptBinding writer(host, registry);
    auto text = serialization::scene::to_json(authored, nullptr, &writer);
    REQUIRE(text);

    // The document names the behaviour and carries the authored values.
    CHECK(text.value().find("\"behaviour\": \"PatrolScript\"") != std::string::npos);
    CHECK(text.value().find("\"speed\": 3.5") != std::string::npos);
    CHECK(text.value().find("\"label\": \"left-right\"") != std::string::npos);

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded, nullptr, &reader));

    Node* loaded_hero = loaded.find_node_by_name("Hero");
    REQUIRE(loaded_hero != nullptr);
    REQUIRE(loaded_host.has_script(loaded_hero->id()));

    const auto* script = dynamic_cast<const PatrolScript*>(loaded_host.script(loaded_hero->id()));
    REQUIRE(script != nullptr);
    CHECK(script->speed() == doctest::Approx(3.5f));
    CHECK(script->label() == "left-right");
    // The instance was attached during the load, so attach() ran once.
    CHECK(script->attaches() == 1);
}

TEST_CASE("a loaded script runs through the ordinary schedule") {
    ScriptRegistry registry = make_registry();

    Scene authored;
    Node* hero = authored.create_node("Hero");
    ScriptHost host;
    REQUIRE_FALSE(host.attach_script(*hero, std::make_unique<PatrolScript>(2.0f)));

    script::ScriptBinding writer(host, registry);
    auto text = serialization::scene::to_json(authored, nullptr, &writer);
    REQUIRE(text);

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded, nullptr, &reader));

    Node* loaded_hero = loaded.find_node_by_name("Hero");
    REQUIRE(loaded_hero != nullptr);

    FrameContext context;
    loaded_host.update(context);
    loaded_host.update(context);
    // Two ticks of the loaded code moved the node by its persisted speed.
    CHECK(loaded_hero->transform().position.x == doctest::Approx(4.0f));
}

TEST_CASE("a script with no properties round-trips as a bare binding") {
    ScriptRegistry registry = make_registry();
    ScriptHost host;

    Scene authored;
    Node* node = authored.create_node("Node");
    REQUIRE_FALSE(host.attach_script(*node, std::make_unique<BareScript>()));

    script::ScriptBinding writer(host, registry);
    auto text = serialization::scene::to_json(authored, nullptr, &writer);
    REQUIRE(text);
    CHECK(text.value().find("\"behaviour\": \"BareScript\"") != std::string::npos);
    // Nothing to persist, so no properties object is written.
    CHECK(text.value().find("\"properties\"") == std::string::npos);

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded, nullptr, &reader));

    Node* loaded_node = loaded.find_node_by_name("Node");
    REQUIRE(loaded_node != nullptr);
    REQUIRE(loaded_host.has_script(loaded_node->id()));
    CHECK(loaded_host.script(loaded_node->id())->name() == "BareScript");
}

TEST_CASE("an unscripted scene is unchanged by a present binding") {
    ScriptRegistry registry = make_registry();
    ScriptHost host;

    Scene scene;
    [[maybe_unused]] Node* root = scene.create_node("Root");
    root->create_child<Node>("Child");

    // A document written with a binding present but no scripts bound must be
    // identical to one written without, so adding the feature does not rewrite
    // every existing scene.
    script::ScriptBinding writer(host, registry);
    auto with_binding = serialization::scene::to_json(scene, nullptr, &writer);
    auto without_binding = serialization::scene::to_json(scene);
    REQUIRE(with_binding);
    REQUIRE(without_binding);
    CHECK(with_binding.value() == without_binding.value());
    CHECK(with_binding.value().find("\"scripts\"") == std::string::npos);
}

TEST_CASE("a binding naming an unknown behaviour is reported and skipped") {
    ScriptRegistry registry = make_registry();

    // A document from a build that provided a behaviour this one does not.
    const std::string text = R"({"format":"fork_scene","format_version":4,"name":"S","roots":[)"
                             R"({"name":"Node","type":"Node","scripts":[{"behaviour":"MissingScript"}]}]})";

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    // The unknown behaviour is not fatal: the scene still loads.
    REQUIRE_FALSE(serialization::scene::from_json(text, loaded, nullptr, &reader));
    CHECK(loaded_host.script_count() == 0);
    CHECK(loaded.find_node_by_name("Node") != nullptr);
}

TEST_CASE("a malformed property value keeps the script's default") {
    ScriptRegistry registry = make_registry();

    // "speed" is a float property but the document gives it a string. The
    // reader warns and keeps the default rather than failing the load.
    const std::string text = R"({"format":"fork_scene","format_version":4,"name":"S","roots":[)"
                             R"({"name":"Node","type":"Node","scripts":[{"behaviour":"PatrolScript",)"
                             R"("properties":{"speed":"fast","label":"kept"}}]}]})";

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text, loaded, nullptr, &reader));

    Node* loaded_node = loaded.find_node_by_name("Node");
    REQUIRE(loaded_node != nullptr);
    const auto* script = dynamic_cast<const PatrolScript*>(loaded_host.script(loaded_node->id()));
    REQUIRE(script != nullptr);
    CHECK(script->speed() == doctest::Approx(1.0f)); // the default, not "fast"
    CHECK(script->label() == "kept");                // the good property still applied
}

TEST_CASE("a document without scripts loads with every node unscripted") {
    ScriptRegistry registry = make_registry();

    Scene authored;
    [[maybe_unused]] Node* hero = authored.create_node("Hero");

    // Written without a binding: the pre-ADR-0063 document shape.
    auto text = serialization::scene::to_json(authored);
    REQUIRE(text);

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded, nullptr, &reader));
    CHECK(loaded_host.script_count() == 0);
}

TEST_CASE("a binding names a node by id, so it survives a rename") {
    ScriptRegistry registry = make_registry();
    ScriptHost host;

    Scene authored;
    Node* hero = authored.create_node("Hero");
    REQUIRE_FALSE(host.attach_script(*hero, std::make_unique<PatrolScript>(1.0f)));

    script::ScriptBinding writer(host, registry);
    auto text = serialization::scene::to_json(authored, nullptr, &writer);
    REQUIRE(text);

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded, nullptr, &reader));

    Node* loaded_hero = loaded.find_node_by_name("Hero");
    REQUIRE(loaded_hero != nullptr);
    // The binding resolves through the node id, so it lands on the right node.
    REQUIRE(loaded_host.has_script(loaded_hero->id()));
}

TEST_CASE("a script bound to a deep child is persisted with that child") {
    ScriptRegistry registry = make_registry();
    ScriptHost host;

    Scene authored;
    Node* root = authored.create_node("Root");
    Node* mid = root->create_child<Node>("Mid");
    Node* leaf = mid->create_child<Node>("Leaf");
    REQUIRE_FALSE(host.attach_script(*leaf, std::make_unique<PatrolScript>(7.0f)));

    script::ScriptBinding writer(host, registry);
    auto text = serialization::scene::to_json(authored, nullptr, &writer);
    REQUIRE(text);

    Scene loaded;
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded, nullptr, &reader));

    Node* loaded_leaf = loaded.find_node_by_name("Leaf");
    REQUIRE(loaded_leaf != nullptr);
    const auto* script = dynamic_cast<const PatrolScript*>(loaded_host.script(loaded_leaf->id()));
    REQUIRE(script != nullptr);
    CHECK(script->speed() == doctest::Approx(7.0f));
}

TEST_CASE("a reading binding reset clears the previous document's bindings") {
    ScriptRegistry registry = make_registry();

    Scene first;
    Node* a = first.create_node("A");
    ScriptHost host;
    REQUIRE_FALSE(host.attach_script(*a, std::make_unique<PatrolScript>(1.0f)));
    script::ScriptBinding writer(host, registry);
    auto text = serialization::scene::to_json(first, nullptr, &writer);
    REQUIRE(text);

    // The same binding reads two documents in turn. The second must not inherit
    // the first's recorded scripts.
    ScriptHost loaded_host;
    script::ScriptBinding reader(loaded_host, registry);

    Scene loaded_first;
    REQUIRE_FALSE(serialization::scene::from_json(text.value(), loaded_first, nullptr, &reader));
    CHECK(loaded_host.script_count() == 1);

    // A second, empty document: the reader must forget the first's scripts.
    Scene empty;
    [[maybe_unused]] Node* node = empty.create_node("Empty");
    auto empty_text = serialization::scene::to_json(empty);
    REQUIRE_FALSE(serialization::scene::from_json(empty_text.value(), loaded_first, nullptr, &reader));
    CHECK(loaded_host.script_count() == 0);
}
