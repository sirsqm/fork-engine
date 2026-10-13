// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The editor host, end to end. This is the only test that runs the whole
// editor: the embedded Engine, the document, the Dear ImGui docking host, and
// every panel (hierarchy, inspector, play) drawing for a few real frames. It
// proves the frame path - including the inspector reflecting the selected node's
// components - does not crash and that the host shuts down cleanly.
//
// It needs a window and an OpenGL context, so it lives with the graphics tests
// and skips cleanly when neither is available (no fake pass).

#include <doctest/doctest.h>

#include "fe/editor/EditorHost.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <string>
#include <variant>

namespace {

// The property named `name` in `type`, or nullptr.
[[nodiscard]] const fe::reflect::Property* find_property(const fe::reflect::TypeDesc& type, std::string_view name) {
    for (const auto& property : type.properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

// A context probe: if a hidden window and GL context cannot be created, there
// is no display, and the host test skips. The probe is destroyed before the
// host starts its own engine.
[[nodiscard]] bool graphics_available() {
    fe::Engine probe;
    fe::Engine::Config config;
    config.window.title = "fork engine editor host probe";
    config.window.width = 64;
    config.window.height = 64;
    config.window.visible = false;
    config.window.vsync = false;
    if (fe::Error error = probe.initialize(config); error) {
        MESSAGE("graphics context unavailable: " << error.message());
        return false;
    }
    probe.shutdown();
    return true;
}

} // namespace

TEST_CASE("EditorHost runs frames, reflects a Sprite2D, and exits cleanly") {
    if (!graphics_available()) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    fe::editor::EditorHost::Config config;
    config.engine.window.title = "fork engine editor host test";
    config.engine.window.width = 256;
    config.engine.window.height = 192;
    config.engine.window.visible = false;
    config.engine.window.vsync = false;
    config.engine.log_level = fe::LogLevel::Error;
    config.max_frames = 3;

    fe::editor::EditorHost host(std::move(config));

    REQUIRE_FALSE(host.initialize());

    // Select a node with a Sprite2D, so the inspector's reflection path is
    // exercised for a known component during the frames.
    fe::editor::EditorContext& context = host.context();
    CHECK(context.scene().node_count() > 0);

    fe::Node* sprite_node = nullptr;
    for (const auto& root : context.scene().roots()) {
        if (root->get_component<fe::Sprite2D>() != nullptr) {
            sprite_node = root.get();
            break;
        }
    }
    REQUIRE(sprite_node != nullptr);
    context.hierarchy().select(sprite_node);
    CHECK(context.hierarchy().selected_node() == sprite_node);

    // Drive a couple of frames with the sprite selected, then perform a
    // reflection-driven edit exactly as the inspector does: find the property in
    // the registry and call its setter on the component. Proves the panel's data
    // path end to end while the editor is live.
    for (int i = 0; i < 2; ++i) {
        host.step();
    }

    fe::reflect::register_builtin_types();
    const fe::reflect::TypeDesc* sprite_type = fe::reflect::Registry::instance().find("Sprite2D");
    REQUIRE(sprite_type != nullptr);
    const fe::reflect::Property* layer = find_property(*sprite_type, "layer");
    REQUIRE(layer != nullptr);

    fe::Sprite2D* sprite = sprite_node->get_component<fe::Sprite2D>();
    REQUIRE(sprite != nullptr);
    REQUIRE(layer->setter(sprite, fe::reflect::Value{static_cast<fe::i64>(5)}));
    CHECK(sprite->layer == 5);

    // One more frame draws the edited value.
    host.step();

    // The viewport allocated an off-screen target and rendered the scene into
    // it, not just the window: this is what makes the viewport a panel rather
    // than a second window.
    fe::rhi::RenderTarget* target = host.viewport_target();
    REQUIRE(target != nullptr);
    CHECK(target->width() > 0);
    CHECK(target->height() > 0);
    CHECK(target->color_texture()->width() == target->width());
    CHECK(target->color_texture()->native_handle() != 0U);

    // The authored scene survives the run.
    CHECK(context.scene().node_count() > 0);
    CHECK(sprite_node->get_component<fe::Sprite2D>() != nullptr);

    host.shutdown();

    // After shutdown the borrowed target is gone, proving it was released before
    // the device.
    CHECK(host.viewport_target() == nullptr);
}
