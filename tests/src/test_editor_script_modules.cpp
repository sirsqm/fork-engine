// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for editor-driven gameplay-module hot reload (M7, ADR-0066). These load
// *real* shared-library images (the same module_a/module_b fixtures the loader
// tests use, ADR-0061), attach one through the editor's `ScriptCatalog`, and
// prove that a changed image is detected by a poll and swapped in without a
// restart - with the node rebound to the new code and running it through the
// ordinary schedule. No mock stands in for the platform or the loader.
//
// Everything is headless: the catalog is the editor-core type with no window and
// no ImGui, so the whole reload path is exercised without a display.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/ScriptCatalog.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptSystem.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"

#include <chrono>
#include <filesystem>
#include <string>

using namespace fe;
using namespace fe::editor;

namespace {

constexpr const char* kModuleAPath = FE_TEST_MODULE_A;
constexpr const char* kModuleBPath = FE_TEST_MODULE_B;

// A unique temporary directory per test case, removed on destruction. A module is
// hot-reloaded in place, so a test copies an image into one of these and moves
// its write time forward rather than rebuilding a real module.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ =
            std::filesystem::temp_directory_path() / ("fe_editor_module_" + name + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// Moves a file's write time forward, so a change check sees a difference
// regardless of the filesystem's timestamp resolution.
void touch_forward(const std::string& path) {
    const auto now = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, now + std::chrono::seconds(10));
}

// Replaces `path`'s contents with `source` the way a build system replaces a
// loaded image's file: stage a sibling and rename it over `path`. The rename
// gives the path a *new* inode, so the loader's existing mapping of the old
// inode stays valid; truncating the file in place would invalidate the pages the
// loaded code is executing from. Returns false on a platform that refuses to
// replace a loaded image's file (Windows), which the caller handles by loading
// the replacement from a distinct path instead.
[[nodiscard]] bool replace_image_in_place(const std::string& path, const std::string& source) {
    const std::string staged = path + ".new";
    std::error_code ec;
    std::filesystem::copy_file(source, staged, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    std::filesystem::rename(staged, path, ec);
    if (ec) {
        std::filesystem::remove(staged, ec);
        return false;
    }
    touch_forward(path);
    return true;
}

[[nodiscard]] f32 node_x(const Node& node) {
    return node.transform().position.x;
}

} // namespace

TEST_CASE("the catalog loads a real module and offers its behaviour") {
    ScriptCatalog catalog;
    CHECK(catalog.module_count() == 0);
    CHECK_FALSE(catalog.registry().contains("SpinBehaviour"));

    REQUIRE_FALSE(catalog.load_module(kModuleAPath));
    CHECK(catalog.module_count() == 1);
    CHECK(catalog.is_module_loaded(kModuleAPath));
    CHECK(catalog.registry().contains("SpinBehaviour"));

    // The behaviour the module provides is what the inspector's attach control
    // lists.
    const std::vector<std::string> names = catalog.behaviour_names();
    CHECK(std::find(names.begin(), names.end(), std::string("SpinBehaviour")) != names.end());
}

TEST_CASE("a module behaviour attached through the catalog runs through the schedule") {
    ScriptCatalog catalog;
    REQUIRE_FALSE(catalog.load_module(kModuleAPath));

    Scene scene;
    Node* node = scene.create_node("Mover");
    REQUIRE_FALSE(catalog.attach(*node, "SpinBehaviour"));
    CHECK(catalog.script_count() == 1);

    SceneSchedule schedule;
    schedule.add_system<ScriptSystem>(catalog.host());
    const FrameContext context;
    schedule.update(context, scene);
    schedule.update(context, scene);
    schedule.update(context, scene);

    // Version A advances by 1.0 per frame.
    CHECK(node_x(*node) == doctest::Approx(3.0f));
}

TEST_CASE("poll_changed_modules reports a rebuilt image once") {
    // A module is hot-reloaded in place, so load a copy in a temp directory and
    // move its write time forward to simulate a rebuild.
    TempDir dir("poll");
    const std::string image = dir.file(fs::filename(kModuleAPath));
    REQUIRE_FALSE(fs::copy_file(kModuleAPath, image));

    ScriptCatalog catalog;
    REQUIRE_FALSE(catalog.load_module(image));

    // Nothing changed yet: a poll is empty.
    CHECK(catalog.poll_changed_modules().empty());

    touch_forward(image);
    const std::vector<std::string> changed = catalog.poll_changed_modules();
    REQUIRE(changed.size() == 1);
    CHECK(changed.front() == image);

    // The change is reported once; a second poll before adopting it is empty.
    CHECK(catalog.poll_changed_modules().empty());
}

TEST_CASE("hot reload swaps the live code and rebinds the node") {
    // Load version A (advances by 1.0), run it, then reload the module from
    // version B's image (advances by 2.0) and prove the node runs the new code.
    // The replacement is given explicitly, which is the portable form: on Windows
    // a loaded image's file cannot be replaced, so a reload must load a different
    // file (ADR-0061).
    ScriptCatalog catalog;
    REQUIRE_FALSE(catalog.load_module(kModuleAPath));

    Scene scene;
    Node* node = scene.create_node("Mover");
    REQUIRE_FALSE(catalog.attach(*node, "SpinBehaviour"));

    SceneSchedule schedule;
    schedule.add_system<ScriptSystem>(catalog.host());
    const FrameContext context;
    schedule.update(context, scene);
    schedule.update(context, scene);
    CHECK(node_x(*node) == doctest::Approx(2.0f));

    REQUIRE_FALSE(catalog.reload_module(kModuleAPath, kModuleBPath));

    // The node still has exactly one script, rebound to the new code.
    CHECK(catalog.script_count() == 1);
    CHECK(catalog.behaviour_of(node->id()) == "SpinBehaviour");

    // Two more frames now advance by 2.0 each: x == 2.0 + 4.0 == 6.0.
    schedule.update(context, scene);
    schedule.update(context, scene);
    CHECK(node_x(*node) == doctest::Approx(6.0f));
}

#ifndef _WIN32
TEST_CASE("a module rebuilt in place is detected by a poll and reloaded") {
    // The ordinary watched-build case on Linux/macOS: the image at the loaded
    // path is replaced (a new inode renamed over the old), the poll sees the new
    // write time, and a reload swaps the code. Skipped on Windows, where a loaded
    // image's file cannot be replaced and a reload loads a distinct file instead.
    TempDir dir("inplace");
    const std::string image = dir.file(fs::filename(kModuleAPath));
    REQUIRE_FALSE(fs::copy_file(kModuleAPath, image));

    EditorContext context;
    REQUIRE_FALSE(context.scripts().load_module(image));

    Scene& scene = context.scene();
    Node* node = scene.create_node("Mover");
    REQUIRE_FALSE(context.scripts().attach(*node, "SpinBehaviour"));

    // No change yet: the context's poll reloads nothing.
    CHECK(context.poll_script_modules().empty());

    REQUIRE(replace_image_in_place(image, kModuleBPath));

    const std::vector<std::string> reloaded = context.poll_script_modules();
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded.front() == image);

    // The binding survived the reload, so the node is still scripted.
    CHECK(context.scripts().has_script(node->id()));
    CHECK(context.scripts().behaviour_of(node->id()) == "SpinBehaviour");
}
#endif

TEST_CASE("clear forgets the reload bookkeeping so a stale node is not rebound") {
    TempDir dir("clear");
    const std::string image = dir.file(fs::filename(kModuleAPath));
    REQUIRE_FALSE(fs::copy_file(kModuleAPath, image));

    ScriptCatalog catalog;
    REQUIRE_FALSE(catalog.load_module(image));

    Scene scene;
    Node* node = scene.create_node("Mover");
    REQUIRE_FALSE(catalog.attach(*node, "SpinBehaviour"));

    // Clearing the document (before a different scene loads) drops the scripts
    // and the tracked bindings, so a later reload does not resurrect a node that
    // belongs to the scene that was replaced.
    catalog.clear();
    CHECK(catalog.script_count() == 0);

    touch_forward(image);
    const std::vector<std::string> reloaded = catalog.poll_changed_modules();
    REQUIRE(reloaded.size() == 1);
    REQUIRE_FALSE(catalog.reload_module(reloaded.front()));
    CHECK(catalog.script_count() == 0);
}
