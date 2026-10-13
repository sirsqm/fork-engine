// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the editor document: building, loading, and saving a scene through
// the public serialization API, and the play-request handshake between the UI
// and the host. These are headless and touch the filesystem only through the
// engine's own fs helpers.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/editor/AssetBrowserViewModel.h"
#include "fe/editor/EditorContext.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Node.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace fe;
using namespace fe::editor;

namespace {

// A path under the build's working directory, unique per test file.
[[nodiscard]] std::string scratch_path(const char* name) {
    return std::string("editor_test_") + name + ".json";
}

// A throwaway directory removed on destruction, for the project-root tests that
// need a real asset tree on disk.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ =
            std::filesystem::temp_directory_path() / ("fe_editor_project_" + tag + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::string str() const { return path_.string(); }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

void write_file(const std::string& path, const std::string& contents) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// Makes `dir` look like a real, already-imported project: writes each asset and
// its sidecar through the engine's own importer, so the sidecar format is the
// engine's, not the test's idea of it. The editor's set_project_root then only
// has to index what is already on disk.
void import_into(const std::string& dir, const std::vector<std::pair<std::string, std::string>>& assets) {
    AssetDatabase seed;
    seed.set_root(dir);
    for (const auto& [relative, scheme] : assets) {
        write_file((std::filesystem::path(dir) / relative).string(), "asset");
        auto imported = seed.import((std::filesystem::path(dir) / relative).string(), scheme);
        if (!imported) {
            MESSAGE("import failed: " << imported.error().message());
        }
        REQUIRE(imported);
    }
}

} // namespace

TEST_CASE("a new scene has a camera, sprites, and a refreshed hierarchy") {
    EditorContext context;
    context.register_default_component_factories();

    REQUIRE_FALSE(context.new_scene(nullptr));

    CHECK(context.scene().name() == "Untitled");
    CHECK(context.scene().node_count() == 6); // Camera + 3 sprites + Group + child.
    CHECK(context.scene().find_node_by_name("Camera") != nullptr);

    // Sprites are created without a texture when no resource manager is given,
    // which is a valid authoring state; the sprites still exist as nodes.
    CHECK(context.scene().find_node_by_name("Sprite0") != nullptr);
    CHECK(context.scene().find_node_by_name("GroupChild") != nullptr);

    // The hierarchy was rebuilt, so it reflects the new scene.
    CHECK(context.hierarchy().rows().size() == context.scene().node_count());
}

TEST_CASE("a scene round-trips through save and load") {
    EditorContext context;
    context.register_default_component_factories();
    REQUIRE_FALSE(context.new_scene(nullptr));

    const std::string path = scratch_path("roundtrip");
    REQUIRE_FALSE(context.save_scene(path));
    CHECK(context.scene_path() == path);
    CHECK(fs::exists(path));

    EditorContext reloaded;
    REQUIRE_FALSE(reloaded.load_scene(path, nullptr));
    CHECK(reloaded.scene().node_count() == context.scene().node_count());
    CHECK(reloaded.scene().find_node_by_name("GroupChild") != nullptr);
    CHECK(reloaded.scene_path() == path);

    // The test writes into the working directory (the build tree); the file is
    // intentionally left in place, like the other serialization tests.
}

TEST_CASE("loading a missing scene leaves the document untouched") {
    EditorContext context;
    REQUIRE_FALSE(context.new_scene(nullptr));
    const usize before = context.scene().node_count();

    Error error = context.load_scene("this_file_does_not_exist_12345.json", nullptr);
    CHECK(error);

    // The failed load must not have modified the existing document.
    CHECK(context.scene().node_count() == before);
    CHECK(context.scene_path().empty());
}

TEST_CASE("saving with no path and no prior save is an error") {
    EditorContext context;
    REQUIRE_FALSE(context.new_scene(nullptr));
    // A fresh scene has never been saved and no path was given.
    Error error = context.save_scene("");
    CHECK(error);
}

TEST_CASE("play requests are a one-shot handshake") {
    EditorContext context;

    CHECK(context.play_request() == PlayRequest::None);
    CHECK(context.take_play_request() == PlayRequest::None);

    context.request_start_play();
    CHECK(context.play_request() == PlayRequest::Start);
    CHECK(context.take_play_request() == PlayRequest::Start);
    // Taking the request consumes it; a second take sees nothing.
    CHECK(context.take_play_request() == PlayRequest::None);

    context.request_stop_play();
    CHECK(context.take_play_request() == PlayRequest::Stop);
}

TEST_CASE("setting a project root indexes assets for the browser and GUID resolution") {
    TempDir dir("index");
    // A real project already has its assets imported (each has a .meta sidecar);
    // the editor's job is only to index them, not to invent them.
    import_into(dir.str(), {{"textures/checker.png", "tex"}, {"audio/beep.wav", "wav"}});

    ResourceManager resources;
    EditorContext context;
    context.set_project_root(dir.str(), &resources);

    CHECK(context.project_root() == dir.str());

    AssetBrowserViewModel browser;
    browser.rebuild(&resources);
    CHECK(browser.entry_count() == 2);

    // The browser shows the same identity the engine resolves: a path resolves
    // to the GUID the browser hands out for that same asset.
    REQUIRE_FALSE(browser.empty());
    const auto& entry = browser.entries().front();
    browser.select(entry.id);
    CHECK(browser.selected_reference() == resources.resolve_reference(entry.path));
    CHECK(browser.selected_reference().rfind("guid:", 0) == 0);
}

TEST_CASE("setting an empty project root clears the asset index") {
    TempDir dir("clear");
    import_into(dir.str(), {{"a.txt", ""}});

    ResourceManager resources;
    EditorContext context;
    context.set_project_root(dir.str(), &resources);

    AssetBrowserViewModel browser;
    browser.rebuild(&resources);
    CHECK(browser.entry_count() == 1);

    context.set_project_root("", &resources);
    CHECK(context.project_root().empty());
    browser.rebuild(&resources);
    CHECK(browser.empty());
}

TEST_CASE("setting a project root without a manager records the path only") {
    // No manager: there is nothing to index against, so the path is remembered
    // but no asset database is touched. This is the headless-editor case.
    EditorContext context;
    context.set_project_root("/tmp/fe_no_manager_root", nullptr);
    CHECK(context.project_root() == "/tmp/fe_no_manager_root");
}
