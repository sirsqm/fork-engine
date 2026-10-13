// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/AssetId.h"
#include "fe/resources/ResourceManager.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace fe;

namespace {

// A unique temporary asset root per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_asset_test_" + name + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] std::string str() const { return path_.string(); }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// Writes a placeholder asset file so the database sees the asset as present.
void make_asset_file(const std::string& absolute_path) {
    REQUIRE_FALSE(fs::write_text_file(absolute_path, "placeholder"));
}

} // namespace

TEST_CASE("import mints a GUID and writes a sidecar that round-trips") {
    TempDir dir("import");
    AssetDatabase db;
    db.set_root(dir.str());

    make_asset_file(dir.file("assets/hero.png"));
    auto imported = db.import("assets/hero.png", "tex");
    REQUIRE(imported);
    CHECK(imported.value().scheme == "tex");
    CHECK(imported.value().path == "assets/hero.png");
    CHECK_FALSE(imported.value().id.is_nil());

    // The sidecar exists next to the asset and reads back identically.
    const std::string sidecar = dir.file("assets/hero.png.meta");
    CHECK(fs::exists(sidecar));
    auto meta = read_asset_meta(sidecar);
    REQUIRE(meta);
    CHECK(meta.value().id == imported.value().id);
    CHECK(meta.value().scheme == "tex");
    CHECK(meta.value().source == "hero.png");
}

TEST_CASE("a GUID is stable across a fresh database (session restart)") {
    TempDir dir("stable");
    make_asset_file(dir.file("assets/hero.png"));

    AssetId first;
    {
        AssetDatabase db;
        db.set_root(dir.str());
        auto imported = db.import("assets/hero.png", "tex");
        REQUIRE(imported);
        first = imported.value().id;
    }

    // A brand new database reading the same tree must find the same identity.
    AssetDatabase reopened;
    reopened.set_root(dir.str());
    const AssetDatabase::Asset* found = reopened.find_by_path("assets/hero.png");
    REQUIRE(found != nullptr);
    CHECK(found->id == first);
}

TEST_CASE("re-importing an asset reuses its GUID") {
    TempDir dir("reimport");
    AssetDatabase db;
    db.set_root(dir.str());
    make_asset_file(dir.file("assets/hero.png"));

    auto first = db.import("assets/hero.png", "tex");
    REQUIRE(first);
    auto second = db.import("assets/hero.png", "tex");
    REQUIRE(second);
    CHECK(second.value().id == first.value().id);
}

TEST_CASE("moving an asset and its sidecar keeps the identity and the path updates") {
    TempDir dir("move");
    AssetDatabase db;
    db.set_root(dir.str());
    make_asset_file(dir.file("assets/hero.png"));

    auto imported = db.import("assets/hero.png", "tex");
    REQUIRE(imported);
    const AssetId id = imported.value().id;

    // Move both files, as a user would in a file manager.
    std::filesystem::create_directories(dir.file("assets/textures"));
    std::filesystem::rename(dir.file("assets/hero.png"), dir.file("assets/textures/hero.png"));
    std::filesystem::rename(dir.file("assets/hero.png.meta"), dir.file("assets/textures/hero.png.meta"));

    db.rescan();

    const AssetDatabase::Asset* found = db.find(id);
    REQUIRE(found != nullptr);
    CHECK(found->path == "assets/textures/hero.png");
    CHECK(db.find_by_path("assets/hero.png") == nullptr);
    // Compare as paths: absolute_path() uses '/' while the native separator is
    // '\' on Windows, and path comparison normalises that.
    CHECK(std::filesystem::path(db.absolute_path(id)) == std::filesystem::path(dir.file("assets/textures/hero.png")));
}

TEST_CASE("a sidecar whose asset is gone is skipped, not indexed") {
    TempDir dir("orphan");
    AssetDatabase db;
    db.set_root(dir.str());
    make_asset_file(dir.file("assets/hero.png"));

    auto imported = db.import("assets/hero.png", "tex");
    REQUIRE(imported);

    // Remove only the asset, leaving the sidecar behind.
    std::filesystem::remove(dir.file("assets/hero.png"));
    db.rescan();
    CHECK(db.find(imported.value().id) == nullptr);
    CHECK(db.asset_count() == 0);
}

TEST_CASE("an asset database with no root is inert") {
    AssetDatabase db;
    CHECK(db.asset_count() == 0);
    auto imported = db.import("assets/hero.png", "tex");
    CHECK_FALSE(imported);
    CHECK(imported.error().message().find("no root") != std::string::npos);
}

TEST_CASE("read_asset_meta rejects malformed and foreign documents") {
    TempDir dir("badmeta");

    REQUIRE_FALSE(fs::write_text_file(dir.file("notjson.meta"), "{ not json"));
    auto bad = read_asset_meta(dir.file("notjson.meta"));
    CHECK_FALSE(bad);
    CHECK(bad.error().message().find("parse error") != std::string::npos);

    REQUIRE_FALSE(fs::write_text_file(dir.file("foreign.meta"), R"({"format":"other","format_version":1})"));
    auto foreign = read_asset_meta(dir.file("foreign.meta"));
    CHECK_FALSE(foreign);
    CHECK(foreign.error().message().find("not a fork asset") != std::string::npos);

    REQUIRE_FALSE(
        fs::write_text_file(dir.file("badguid.meta"), R"({"format":"fork_asset","format_version":1,"guid":"nope"})"));
    auto bad_guid = read_asset_meta(dir.file("badguid.meta"));
    CHECK_FALSE(bad_guid);
    CHECK(bad_guid.error().message().find("invalid guid") != std::string::npos);
}

TEST_CASE("guid references format and parse") {
    const Uuid id = Uuid::generate();
    const std::string reference = to_reference(id);
    CHECK(reference.rfind("guid:", 0) == 0);

    AssetId parsed;
    REQUIRE(parse_reference(reference, parsed));
    CHECK(parsed == id);

    // A path key is not a GUID reference, and a malformed one is rejected.
    CHECK_FALSE(parse_reference("tex:assets/hero.png", parsed));
    CHECK_FALSE(parse_reference("guid:not-a-uuid", parsed));
}

TEST_CASE("import stores per-asset settings in the sidecar and they round-trip") {
    TempDir dir("settings");
    AssetDatabase db;
    db.set_root(dir.str());
    make_asset_file(dir.file("assets/hero.png"));

    ImportSettings settings = ImportSettings::texture_defaults();
    settings.filter = rhi::TextureFilter::Nearest;
    settings.wrap = rhi::TextureWrap::Repeat;
    settings.mipmaps = true;

    auto imported = db.import("assets/hero.png", "tex", settings);
    REQUIRE(imported);
    CHECK(imported.value().settings.filter == rhi::TextureFilter::Nearest);
    CHECK(imported.value().settings.wrap == rhi::TextureWrap::Repeat);
    CHECK(imported.value().settings.mipmaps);

    // The sidecar carries the settings, and a fresh database reads them back.
    auto meta = read_asset_meta(dir.file("assets/hero.png.meta"));
    REQUIRE(meta);
    CHECK(meta.value().settings.filter == rhi::TextureFilter::Nearest);

    AssetDatabase reopened;
    reopened.set_root(dir.str());
    const AssetDatabase::Asset* found = reopened.find_by_path("assets/hero.png");
    REQUIRE(found != nullptr);
    CHECK(found->settings.filter == rhi::TextureFilter::Nearest);
    CHECK(found->settings.wrap == rhi::TextureWrap::Repeat);
    CHECK(found->settings.mipmaps);
}

TEST_CASE("a two-argument texture import records the texture defaults") {
    TempDir dir("texdefaults");
    AssetDatabase db;
    db.set_root(dir.str());
    make_asset_file(dir.file("assets/hero.png"));

    auto imported = db.import("assets/hero.png", "tex");
    REQUIRE(imported);
    // The scheme selects the importer, and its defaults are the pre-settings
    // behaviour; because they are defaults the sidecar omits them.
    CHECK(imported.value().settings.kind == ImportSettings::Kind::Texture);
    CHECK(imported.value().settings.is_default());

    auto meta = read_asset_meta(dir.file("assets/hero.png.meta"));
    REQUIRE(meta);
    CHECK(meta.value().settings.kind == ImportSettings::Kind::Texture);
}

TEST_CASE("a sidecar written before import settings existed still loads") {
    TempDir dir("legacy");
    make_asset_file(dir.file("assets/hero.png"));

    // Exactly the document the old engine wrote: no "settings" key.
    REQUIRE_FALSE(fs::write_text_file(
        dir.file("assets/hero.png.meta"),
        R"({"format":"fork_asset","format_version":1,"guid":"3f2504e0-4f89-41d3-9a0c-0305e82c3301","source":"hero.png","scheme":"tex"})"));

    AssetDatabase db;
    db.set_root(dir.str());
    const AssetDatabase::Asset* found = db.find_by_path("assets/hero.png");
    REQUIRE(found != nullptr);
    CHECK(found->scheme == "tex");
    // Absent settings are the generic default, not an error.
    CHECK(found->settings.kind == ImportSettings::Kind::Generic);
    CHECK(found->settings.is_default());
}

TEST_CASE("a malformed settings block in a sidecar is reported, not ignored") {
    TempDir dir("badsettings");
    make_asset_file(dir.file("assets/hero.png"));
    REQUIRE_FALSE(fs::write_text_file(
        dir.file("assets/hero.png.meta"),
        R"({"format":"fork_asset","format_version":1,"guid":"3f2504e0-4f89-41d3-9a0c-0305e82c3301","source":"hero.png","scheme":"tex","settings":{"kind":"texture","filter":"cubic"}})"));

    auto meta = read_asset_meta(dir.file("assets/hero.png.meta"));
    CHECK_FALSE(meta);
    CHECK(meta.error().message().find("unknown texture filter") != std::string::npos);

    // The scan drops the bad entry rather than indexing it with wrong settings.
    AssetDatabase db;
    db.set_root(dir.str());
    CHECK(db.asset_count() == 0);
}

TEST_CASE("re-importing with new settings keeps the GUID and rewrites the sidecar") {
    TempDir dir("reimport_settings");
    AssetDatabase db;
    db.set_root(dir.str());
    make_asset_file(dir.file("assets/hero.png"));

    auto first = db.import("assets/hero.png", "tex");
    REQUIRE(first);

    ImportSettings nearest = ImportSettings::texture_defaults();
    nearest.filter = rhi::TextureFilter::Nearest;
    auto second = db.import("assets/hero.png", "tex", nearest);
    REQUIRE(second);
    // Changing how an asset is imported must not break references to it.
    CHECK(second.value().id == first.value().id);
    CHECK(second.value().settings.filter == rhi::TextureFilter::Nearest);

    // The change survives a rescan.
    db.rescan();
    const AssetDatabase::Asset* found = db.find(first.value().id);
    REQUIRE(found != nullptr);
    CHECK(found->settings.filter == rhi::TextureFilter::Nearest);
}
