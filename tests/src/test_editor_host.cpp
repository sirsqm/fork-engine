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

#include "fe/core/FileSystem.h"
#include "fe/editor/EditorHost.h"
#include "fe/editor/FileDialogPanel.h"
#include "fe/editor/FileDialogViewModel.h"
#include "fe/editor/ProjectSettings.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/rhi/Resources.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/SceneSerializer.h"

#include <filesystem>
#include <fstream>
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

TEST_CASE("a configured project root is indexed by the embedded engine") {
    if (!graphics_available()) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    // A real asset tree on disk, with the asset already imported (its .meta
    // sidecar written through the engine's own importer) so the editor's scan
    // indexes it. The host's project root is set before the first scene load, so
    // the embedded engine's asset database is indexed by the time initialize()
    // returns, and the editor's browser has something to list.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "fe_editor_host_project_root_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "textures");
    const std::filesystem::path asset = root / "textures" / "checker.png";
    {
        std::ofstream(asset, std::ios::binary) << "png";
    }
    {
        fe::AssetDatabase seed;
        seed.set_root(root.string());
        auto imported = seed.import(asset.string(), "tex");
        REQUIRE(imported);
    }

    fe::editor::EditorHost::Config config;
    config.engine.window.title = "fork engine editor host project test";
    config.engine.window.width = 128;
    config.engine.window.height = 96;
    config.engine.window.visible = false;
    config.engine.window.vsync = false;
    config.engine.log_level = fe::LogLevel::Error;
    config.max_frames = 1;
    config.project_root = root.string();

    fe::editor::EditorHost host(std::move(config));
    REQUIRE_FALSE(host.initialize());

    CHECK(host.context().project_root() == root.string());
    // The engine's database, not a private copy, was indexed: this is what makes
    // the browser and GUID reference resolution agree.
    CHECK(host.engine().resources().assets().asset_count() == 1);

    host.shutdown();
    std::filesystem::remove_all(root);
}

TEST_CASE("a configured project file sets the asset root and opens its startup scene") {
    if (!graphics_available()) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "fe_editor_host_project_file_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "assets");
    std::filesystem::create_directories(root / "scenes");

    // A project whose startup scene is a real file, plus one imported asset so
    // the asset root has something to index. The sidecar is written through the
    // engine's own importer, and the seed root is the resolved asset directory
    // (matching the host's own root), so the index is found on rescan.
    const std::filesystem::path asset = root / "assets" / "hero.png";
    {
        std::ofstream(asset, std::ios::binary) << "png";
        fe::AssetDatabase seed;
        seed.set_root((root / "assets").string());
        auto imported = seed.import(asset.string(), "tex");
        REQUIRE(imported);
    }
    {
        fe::Scene scene;
        scene.set_name("StartupLevel");
        REQUIRE(scene.create_node("Marker") != nullptr);
        REQUIRE_FALSE(fe::serialization::scene::save(scene, (root / "scenes" / "main.json").string()));
    }
    {
        fe::editor::ProjectSettings settings;
        settings.set_name("HostProject");
        settings.set_startup_scene("scenes/main.json");
        REQUIRE_FALSE(settings.save((root / fe::editor::ProjectSettings::kDefaultFileName).string()));
    }

    fe::editor::EditorHost::Config config;
    config.engine.window.title = "fork engine editor host project file test";
    config.engine.window.width = 128;
    config.engine.window.height = 96;
    config.engine.window.visible = false;
    config.engine.window.vsync = false;
    config.engine.log_level = fe::LogLevel::Error;
    config.max_frames = 1;
    config.project_file = (root / fe::editor::ProjectSettings::kDefaultFileName).string();

    fe::editor::EditorHost host(std::move(config));
    REQUIRE_FALSE(host.initialize());

    // The project's asset root was indexed (relative "assets" resolved against
    // the project directory), and its startup scene is the open document.
    CHECK(host.context().project().name() == "HostProject");
    CHECK(fe::fs::normalize(host.context().project_root()) == fe::fs::normalize((root / "assets").string()));
    CHECK(host.engine().resources().assets().asset_count() == 1);
    CHECK(host.context().scene().name() == "StartupLevel");
    CHECK(host.context().scene().find_node_by_name("Marker") != nullptr);

    host.shutdown();
    std::filesystem::remove_all(root);
}

TEST_CASE("the editor's file dialog saves and reopens a scene through the host") {
    if (!graphics_available()) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "fe_editor_host_dialog_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    fe::editor::EditorHost::Config config;
    config.engine.window.title = "fork engine editor host dialog test";
    config.engine.window.width = 128;
    config.engine.window.height = 96;
    config.engine.window.visible = false;
    config.engine.window.vsync = false;
    config.engine.log_level = fe::LogLevel::Error;
    config.max_frames = 1;

    fe::editor::EditorHost host(std::move(config));
    REQUIRE_FALSE(host.initialize());

    fe::editor::FileDialogPanel* dialog = host.file_dialog();
    REQUIRE(dialog != nullptr);

    // A brand-new document has no path, so "Save Scene" opens the dialog rather
    // than writing silently. This is the whole point of the File menu: the
    // editor could not save a scene from the UI before.
    host.context().request_file(fe::editor::FileRequest::SaveScene);
    host.step();
    CHECK(dialog->is_open());

    // Point the dialog at the temporary directory with a name, and confirm. The
    // confirm path is exercised headlessly, exactly as the modal's Save button
    // drives it.
    dialog->model().set_directory_for_test(root.string());
    dialog->model().set_file_name("level.fescene");
    REQUIRE_FALSE(dialog->confirm(host.context()));
    CHECK_FALSE(dialog->is_open());
    CHECK(fe::fs::exists((root / "level.fescene").string()));
    CHECK(fe::fs::normalize(host.context().scene_path()) == fe::fs::normalize((root / "level.fescene").string()));
    // Saving cleared the dirty flag.
    CHECK_FALSE(host.context().is_dirty());

    // An edit marks the document dirty, so opening another scene asks first.
    fe::Node* node = host.context().scene().create_node("Edited");
    REQUIRE(node != nullptr);
    host.context().mark_dirty();
    CHECK(host.context().is_dirty());

    // Opening the saved scene through the dialog restores the document. Because
    // the document is dirty, the host asks before replacing it; confirm.
    host.context().request_file(fe::editor::FileRequest::OpenScene);
    host.step();
    REQUIRE(host.context().is_confirming());
    host.context().request_file(fe::editor::FileRequest::ConfirmDiscard);
    host.step();
    CHECK(dialog->is_open());
    dialog->model().set_directory_for_test(root.string());
    dialog->model().set_file_name("level.fescene");
    REQUIRE_FALSE(dialog->confirm(host.context()));
    CHECK_FALSE(host.context().is_dirty());
    // "Edited" was not saved, so it is gone after the reopen.
    CHECK(host.context().scene().find_node_by_name("Edited") == nullptr);

    host.shutdown();
    std::filesystem::remove_all(root);
}

TEST_CASE("a destructive request with unsaved changes asks before acting") {
    if (!graphics_available()) {
        MESSAGE("skipping: no graphics context");
        return;
    }

    fe::editor::EditorHost::Config config;
    config.engine.window.title = "fork engine editor host confirm test";
    config.engine.window.width = 128;
    config.engine.window.height = 96;
    config.engine.window.visible = false;
    config.engine.window.vsync = false;
    config.engine.log_level = fe::LogLevel::Error;
    config.max_frames = 1;

    fe::editor::EditorHost host(std::move(config));
    REQUIRE_FALSE(host.initialize());

    // Clean document: New is immediate, no prompt.
    host.context().request_file(fe::editor::FileRequest::NewScene);
    host.step();
    CHECK_FALSE(host.context().is_confirming());

    // Dirty document: New is held pending confirmation.
    (void)host.context().scene().create_node("Dirty");
    host.context().mark_dirty();
    host.context().request_file(fe::editor::FileRequest::NewScene);
    host.step();
    CHECK(host.context().is_confirming());
    CHECK_FALSE(host.context().confirm_message().empty());

    // Declining leaves the document as it was.
    const std::size_t count = host.context().scene().node_count();
    host.context().request_file(fe::editor::FileRequest::CancelDiscard);
    host.step();
    CHECK_FALSE(host.context().is_confirming());
    CHECK(host.context().scene().node_count() == count);
    CHECK(host.context().is_dirty());

    // Confirming performs the held New.
    host.context().request_file(fe::editor::FileRequest::NewScene);
    host.step();
    REQUIRE(host.context().is_confirming());
    host.context().request_file(fe::editor::FileRequest::ConfirmDiscard);
    host.step();
    CHECK_FALSE(host.context().is_confirming());
    CHECK_FALSE(host.context().is_dirty());
    CHECK(host.context().scene().find_node_by_name("Dirty") == nullptr);

    host.shutdown();
}
