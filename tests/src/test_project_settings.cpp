// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the project file: parsing, writing, resolved paths, and the version
// and format guards. These are headless and touch the filesystem only through
// the engine's own fs helpers and a throwaway directory.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/ProjectSettings.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ResourceManager.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace fe;
using namespace fe::editor;

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ = std::filesystem::temp_directory_path() / ("fe_project_" + tag + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

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

// Joins a child onto a base through std::filesystem and normalizes it, so the
// expectation matches what the engine returns on every platform (Windows would
// otherwise compare backslashes against forward slashes).
[[nodiscard]] std::string under(const std::string& base, const std::string& child) {
    return fe::fs::normalize((std::filesystem::path(base) / child).string());
}

} // namespace

TEST_CASE("project settings default to the conventional asset root") {
    ProjectSettings settings;
    CHECK(settings.name().empty());
    CHECK(settings.asset_root() == "assets");
    CHECK(settings.startup_scene().empty());
    // No directory yet, so the root resolves to itself.
    CHECK(settings.resolved_asset_root() == "assets");
    CHECK(settings.resolved_startup_scene().empty());
}

TEST_CASE("project settings resolve paths against the project directory") {
    TempDir dir("resolve");
    ProjectSettings settings;
    settings.set_directory(dir.str());
    settings.set_name("Demo");
    settings.set_asset_root("assets");
    settings.set_startup_scene("scenes/main.json");

    CHECK(settings.resolved_asset_root() == under(dir.str(), "assets"));
    CHECK(settings.resolved_startup_scene() == under(dir.str(), "scenes/main.json"));
}

TEST_CASE("project settings round-trip through JSON, storing portable paths") {
    TempDir dir("roundtrip_json");
    ProjectSettings settings;
    settings.set_directory(dir.str());
    settings.set_name("Demo");
    settings.set_startup_scene("scenes/main.json");

    auto text = settings.to_json();
    REQUIRE(text);

    ProjectSettings loaded;
    REQUIRE_FALSE(ProjectSettings::from_json(text.value(), dir.str(), loaded));
    CHECK(loaded.name() == "Demo");
    CHECK(loaded.asset_root() == "assets");
    CHECK(loaded.startup_scene() == "scenes/main.json");
    // The document itself carries no absolute paths, so it travels with the
    // project instead of naming the machine it was authored on.
    CHECK(text.value().find(dir.str()) == std::string::npos);
    CHECK(text.value().find("\"asset_root\": \"assets\"") != std::string::npos);
}

TEST_CASE("an absolute asset root outside the project is preserved verbatim") {
    // A genuinely absolute root: built from the platform's own absolute temp
    // path so the test means the same thing on POSIX and Windows.
    const std::string shared = (std::filesystem::temp_directory_path() / "fe_shared_assets").string();

    ProjectSettings settings;
    settings.set_directory((std::filesystem::temp_directory_path() / "fe_proj_dir").string());
    settings.set_asset_root(shared);

    auto text = settings.to_json();
    REQUIRE(text);

    ProjectSettings loaded;
    REQUIRE_FALSE(ProjectSettings::from_json(text.value(), settings.directory(), loaded));
    // Stored and read back normalized, so compare against the normalized form.
    CHECK(loaded.asset_root() == fs::normalize(shared));
    // An absolute root is used as-is, not joined onto the project directory.
    CHECK(loaded.resolved_asset_root() == fs::normalize(shared));
}

TEST_CASE("project settings reject a document that is not a project") {
    ProjectSettings loaded;
    const Error error = ProjectSettings::from_json("{\"format\":\"fork_scene\",\"format_version\":2}", "", loaded);
    CHECK(error);
    CHECK(error.message().find("not a fork project") != std::string::npos);
}

TEST_CASE("project settings reject a future format version") {
    ProjectSettings loaded;
    const Error error = ProjectSettings::from_json("{\"format\":\"fork_project\",\"format_version\":99}", "", loaded);
    CHECK(error);
    CHECK(error.message().find("newer than supported") != std::string::npos);
}

TEST_CASE("project settings reject malformed JSON rather than throwing") {
    ProjectSettings loaded;
    const Error error = ProjectSettings::from_json("{not json", "", loaded);
    CHECK(error);
    CHECK(error.message().find("parse error") != std::string::npos);
}

TEST_CASE("a project file saves and loads through the filesystem") {
    TempDir dir("roundtrip");
    const std::string path = dir.file(ProjectSettings::kDefaultFileName);

    ProjectSettings settings;
    settings.set_name("Roundtrip");
    settings.set_startup_scene("scenes/main.json");
    REQUIRE_FALSE(settings.save(path));

    auto loaded = ProjectSettings::load(path);
    REQUIRE(loaded);
    CHECK(loaded.value().name() == "Roundtrip");
    // Saving set the project directory to the file's directory, so the asset
    // root resolves inside the project without an explicit set_directory.
    CHECK(loaded.value().resolved_asset_root() == under(dir.str(), "assets"));
}

TEST_CASE("loading a missing project file returns an error") {
    auto loaded = ProjectSettings::load("/nonexistent/dir/fork.project.json");
    CHECK_FALSE(loaded);
    CHECK_FALSE(loaded.error().message().empty());
}

TEST_CASE("opening a project indexes its asset root for the browser and GUIDs") {
    TempDir dir("index");
    // A real, already-imported project: one asset with a sidecar, written
    // through the engine's own importer.
    AssetDatabase seed;
    seed.set_root(dir.str());
    write_file(dir.file("assets/hero.png"), "png");
    REQUIRE(seed.import(dir.file("assets/hero.png"), "tex"));

    ProjectSettings settings;
    settings.set_name("Index");
    REQUIRE_FALSE(settings.save(dir.file(ProjectSettings::kDefaultFileName)));

    ResourceManager resources;
    EditorContext context;
    REQUIRE_FALSE(context.load_project(dir.file(ProjectSettings::kDefaultFileName), &resources));

    CHECK(context.project_path() == dir.file(ProjectSettings::kDefaultFileName));
    CHECK(context.project().name() == "Index");
    // The asset root was resolved and indexed: this is the effect that makes the
    // browser and GUID references agree.
    CHECK(context.project_root() == under(dir.str(), "assets"));
    CHECK(resources.assets().asset_count() == 1);
}

TEST_CASE("opening a bad project file leaves the current project untouched") {
    ResourceManager resources;
    EditorContext context;

    TempDir dir("bad");
    ProjectSettings good;
    good.set_name("Good");
    REQUIRE_FALSE(good.save(dir.file(ProjectSettings::kDefaultFileName)));
    REQUIRE_FALSE(context.load_project(dir.file(ProjectSettings::kDefaultFileName), &resources));
    CHECK(context.project().name() == "Good");

    const std::string bad = dir.file("broken.json");
    write_file(bad, "{\"format\":\"fork_scene\"}");
    CHECK(context.load_project(bad, &resources));
    // The failed load did not adopt the bad document's settings.
    CHECK(context.project().name() == "Good");
    CHECK(context.project_path() == dir.file(ProjectSettings::kDefaultFileName));
}

TEST_CASE("a project with no asset root clears the index") {
    TempDir dir("noroot");
    ProjectSettings settings;
    settings.set_asset_root("");
    REQUIRE_FALSE(settings.save(dir.file(ProjectSettings::kDefaultFileName)));

    ResourceManager resources;
    EditorContext context;
    REQUIRE_FALSE(context.load_project(dir.file(ProjectSettings::kDefaultFileName), &resources));
    CHECK(context.project_root().empty());
    CHECK(resources.assets().asset_count() == 0);
}
