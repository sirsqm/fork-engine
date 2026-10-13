// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/editor/AssetBrowserViewModel.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ResourceManager.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using fe::AssetId;
using fe::ResourceManager;
using fe::editor::AssetBrowserViewModel;

// A throwaway directory removed on destruction, mirroring the helper the other
// resource tests use. It exists so a test can build a real asset database on
// disk, which is the code path the browser actually reads.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ = std::filesystem::temp_directory_path() / ("fe_asset_browser_" + tag + "_" + std::to_string(counter_++));
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

AssetBrowserViewModel::Entry make_entry(const std::string& path, const std::string& scheme) {
    AssetBrowserViewModel::Entry entry;
    entry.id = AssetId::generate();
    entry.path = path;
    entry.scheme = scheme;
    const auto slash = path.rfind('/');
    entry.name = slash == std::string::npos ? path : path.substr(slash + 1);
    entry.directory = slash == std::string::npos ? "." : path.substr(0, slash);
    return entry;
}

} // namespace

TEST_CASE("an asset browser with no manager lists nothing and does not crash") {
    AssetBrowserViewModel model;
    model.rebuild(nullptr);
    CHECK(model.empty());
    CHECK(model.entry_count() == 0);
    CHECK(model.visible_entries().empty());
    CHECK(model.selected_entry() == nullptr);
    CHECK(model.selected_reference().empty());
}

TEST_CASE("the browser lists what the database indexed and derives name and directory") {
    TempDir dir("list");
    ResourceManager resources;
    resources.assets().set_root(dir.str());

    write_file(dir.file("assets/textures/hero.png"), "png");
    write_file(dir.file("assets/audio/theme.wav"), "wav");
    write_file(dir.file("logo.png"), "png");
    REQUIRE(resources.assets().import("assets/textures/hero.png", "tex"));
    REQUIRE(resources.assets().import("assets/audio/theme.wav", "audio"));
    REQUIRE(resources.assets().import("logo.png", ""));

    AssetBrowserViewModel model;
    model.rebuild(&resources);

    CHECK(model.entry_count() == 3);
    REQUIRE(model.visible_entries().size() == 3);

    // Sorted by path, case-insensitively. The root-level file sorts by its own
    // name among the nested ones.
    CHECK(model.visible_entries()[0].path == "assets/audio/theme.wav");
    CHECK(model.visible_entries()[1].path == "assets/textures/hero.png");
    CHECK(model.visible_entries()[2].path == "logo.png");

    const AssetBrowserViewModel::Entry& hero = model.visible_entries()[1];
    CHECK(hero.name == "hero.png");
    CHECK(hero.directory == "assets/textures");
    CHECK(hero.scheme == "tex");

    const AssetBrowserViewModel::Entry& logo = model.visible_entries()[2];
    CHECK(logo.directory == ".");
    CHECK(logo.scheme.empty());
}

TEST_CASE("the filter is a case-insensitive substring of the path") {
    AssetBrowserViewModel model;
    model.set_entries({
        make_entry("assets/textures/Hero.png", "tex"),
        make_entry("assets/audio/theme.wav", "audio"),
        make_entry("assets/textures/ground.png", "tex"),
    });

    model.set_filter("hero");
    REQUIRE(model.visible_entries().size() == 1);
    CHECK(model.visible_entries()[0].name == "Hero.png");

    model.set_filter("TEXTURES");
    CHECK(model.visible_entries().size() == 2);

    model.set_filter("nope");
    CHECK(model.visible_entries().empty());
    // The full listing is retained, so clearing the filter restores it.
    CHECK(model.entry_count() == 3);

    model.set_filter("");
    CHECK(model.visible_entries().size() == 3);
}

TEST_CASE("selection is by id and yields the asset's guid reference") {
    AssetBrowserViewModel model;
    const auto hero = make_entry("assets/textures/hero.png", "tex");
    const auto theme = make_entry("assets/audio/theme.wav", "audio");
    model.set_entries({hero, theme});

    CHECK_FALSE(model.has_selection());
    CHECK(model.selected_entry() == nullptr);

    model.select(theme.id);
    REQUIRE(model.selected_entry() != nullptr);
    CHECK(model.selected_entry()->path == "assets/audio/theme.wav");
    CHECK(model.selected_reference() == fe::to_reference(theme.id));

    model.clear_selection();
    CHECK_FALSE(model.has_selection());
    CHECK(model.selected_reference().empty());
}

TEST_CASE("a selection survives filtering and still resolves its reference") {
    AssetBrowserViewModel model;
    const auto hero = make_entry("assets/textures/hero.png", "tex");
    const auto theme = make_entry("assets/audio/theme.wav", "audio");
    model.set_entries({hero, theme});
    model.select(theme.id);

    model.set_filter("hero");
    // The audio asset is not listed while the filter is active, but the
    // selection is kept so the detail view and the copyable reference remain
    // available.
    CHECK(model.visible_entries().size() == 1);
    REQUIRE(model.selected_entry() != nullptr);
    CHECK(model.selected_entry()->id == theme.id);
    CHECK(model.selected_reference() == fe::to_reference(theme.id));

    model.set_filter("");
    REQUIRE(model.selected_entry() != nullptr);
    CHECK(model.selected_entry()->id == theme.id);
}

TEST_CASE("a selection whose asset is gone reports no entry") {
    AssetBrowserViewModel model;
    const auto hero = make_entry("assets/textures/hero.png", "tex");
    model.set_entries({hero});
    model.select(hero.id);
    REQUIRE(model.selected_entry() != nullptr);

    // A rescan that dropped the asset leaves the id held but unresolvable.
    model.set_entries({});
    CHECK(model.has_selection());
    CHECK(model.selected_entry() == nullptr);
    CHECK(model.selected_reference().empty());
}

TEST_CASE("the browser reports each asset's import settings kind") {
    TempDir dir("settings_kind");
    ResourceManager resources;
    resources.assets().set_root(dir.str());

    write_file(dir.file("assets/hero.png"), "png");
    write_file(dir.file("assets/level.obj"), "obj");
    REQUIRE(resources.assets().import("assets/hero.png", "tex"));
    REQUIRE(resources.assets().import("assets/level.obj", "obj"));

    AssetBrowserViewModel model;
    model.rebuild(&resources);
    REQUIRE(model.visible_entries().size() == 2);

    // The texture importer stamps the "texture" kind; a scheme with no typed
    // importer stays generic.
    CHECK(model.visible_entries()[0].settings_kind == "texture"); // assets/hero.png
    CHECK(model.visible_entries()[1].settings_kind == "generic"); // assets/level.obj
}
