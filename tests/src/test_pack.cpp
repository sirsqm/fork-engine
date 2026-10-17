// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/pack/PackFile.h"
#include "fe/pack/PackSource.h"
#include "fe/pack/PackWriter.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/Image.h"
#include "fe/resources/ImageLoader.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/SceneSerializer.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace fe;
using namespace fe::pack;

namespace {

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_pack_test_" + name + "_" + std::to_string(counter_++));
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

[[nodiscard]] std::vector<u8> bytes_of(std::string_view text) {
    return std::vector<u8>(text.begin(), text.end());
}

// A 2x2 black/white checker TGA (uncompressed, 32-bit, top-left origin). A real
// image the loader can decode, so the mount test proves a packed asset is served
// through the ordinary loader path, not just read as bytes.
[[nodiscard]] std::vector<u8> checker_tga_bytes() {
    std::vector<u8> tga = {
        0, 0, 2, 0, 0,  0,    0, 0, 0, 0, 0, 0, // header: no colour map, uncompressed true-colour
        2, 0, 2, 0, 32, 0x28,                   // 2x2, 32-bit BGRA, top-left origin
    };
    const std::array<u8, 4> black{0, 0, 0, 255};
    const std::array<u8, 4> white{255, 255, 255, 255};
    for (const auto& texel : {black, white, white, black}) {
        tga.insert(tga.end(), texel.begin(), texel.end());
    }
    return tga;
}

} // namespace

TEST_CASE("a pack round-trips its entries and payloads") {
    PackFile pack;
    const Uuid id = Uuid::generate();
    pack.add_file("assets/hero.png", "tex", id, bytes_of("PNG DATA"));
    pack.add_file("assets/beep.wav", "wav", Uuid{}, bytes_of("WAVE"));

    auto bytes = pack.write();
    REQUIRE(bytes);

    auto parsed = PackFile::parse(bytes.value().data(), bytes.value().size());
    REQUIRE(parsed);
    CHECK(parsed.value().entry_count() == 2);

    const pack::PackEntry* hero = parsed.value().find_path("assets/hero.png");
    REQUIRE(hero != nullptr);
    CHECK(hero->scheme == "tex");
    CHECK(hero->guid == id);
    CHECK(hero->has_guid());
    CHECK(hero->data == bytes_of("PNG DATA"));

    const pack::PackEntry* beep = parsed.value().find_path("assets/beep.wav");
    REQUIRE(beep != nullptr);
    CHECK(beep->scheme == "wav");
    CHECK_FALSE(beep->has_guid());
    CHECK(beep->data == bytes_of("WAVE"));

    // A guid lookup resolves to the same entry; a nil id matches nothing.
    CHECK(parsed.value().find_guid(id) == hero);
    CHECK(parsed.value().find_guid(Uuid{}) == nullptr);
    CHECK(parsed.value().find_path("assets/missing.png") == nullptr);
}

TEST_CASE("a pack is written in a deterministic order") {
    PackFile first;
    first.add_file("b.txt", "", Uuid{}, bytes_of("b"));
    first.add_file("a.txt", "", Uuid{}, bytes_of("a"));

    PackFile second;
    second.add_file("a.txt", "", Uuid{}, bytes_of("a"));
    second.add_file("b.txt", "", Uuid{}, bytes_of("b"));

    auto first_bytes = first.write();
    auto second_bytes = second.write();
    REQUIRE(first_bytes);
    REQUIRE(second_bytes);
    // Insertion order must not change the output, so a rebuild is byte-stable.
    CHECK(first_bytes.value() == second_bytes.value());
}

TEST_CASE("a duplicate key is rejected rather than shadowed") {
    PackFile pack;
    pack.add_file("assets/a.txt", "", Uuid{}, bytes_of("one"));
    pack.add_file("assets/a.txt", "", Uuid{}, bytes_of("two"));

    auto bytes = pack.write();
    REQUIRE_FALSE(bytes);
}

TEST_CASE("a malformed pack is rejected, not read as garbage") {
    PackFile pack;
    pack.add_file("assets/a.txt", "", Uuid{}, bytes_of("hello"));
    auto bytes = pack.write();
    REQUIRE(bytes);
    const std::vector<u8>& valid = bytes.value();

    SUBCASE("wrong magic") {
        std::vector<u8> corrupt = valid;
        corrupt[0] = 'X';
        auto parsed = PackFile::parse(corrupt.data(), corrupt.size());
        REQUIRE_FALSE(parsed);
    }

    SUBCASE("a version from the future") {
        std::vector<u8> corrupt = valid;
        corrupt[4] = 99; // format_version low byte
        auto parsed = PackFile::parse(corrupt.data(), corrupt.size());
        REQUIRE_FALSE(parsed);
    }

    SUBCASE("a truncated buffer") {
        auto parsed = PackFile::parse(valid.data(), valid.size() / 2);
        REQUIRE_FALSE(parsed);
    }

    SUBCASE("an index offset outside the buffer") {
        std::vector<u8> corrupt = valid;
        // index_offset is the u64 at byte 12; push it far past the end.
        for (int i = 12; i < 20; ++i) {
            corrupt[static_cast<usize>(i)] = 0xFF;
        }
        auto parsed = PackFile::parse(corrupt.data(), corrupt.size());
        REQUIRE_FALSE(parsed);
    }

    SUBCASE("a payload length that runs past the end") {
        std::vector<u8> corrupt = valid;
        // The single entry's payload is the last 5 bytes ("hello"); the u64
        // immediately before it is its length field. Make that length enormous,
        // so the span cannot fit and the reader must reject it.
        const usize length_field = corrupt.size() - 5 - 8;
        for (usize i = length_field; i < length_field + 8; ++i) {
            corrupt[i] = 0xFF;
        }
        auto parsed = PackFile::parse(corrupt.data(), corrupt.size());
        REQUIRE_FALSE(parsed);
    }
}

TEST_CASE("the writer packs a project's asset tree and its scene") {
    TempDir dir("write");
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/hero.png"), "PNG"));
    // A file that is never imported: still packed, under the default scheme,
    // so a project that references it by path runs from the pack.
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/loose.txt"), "LOOSE"));
    REQUIRE_FALSE(fs::write_text_file(dir.file("scenes/main.fescene"), "{\"format\":\"fork_scene\"}"));

    AssetDatabase assets;
    assets.set_root(dir.file("assets"));
    auto imported = assets.import("hero.png", "tex");
    REQUIRE(imported);

    auto built = pack::PackWriter::build(assets, dir.file("scenes/main.fescene"), "scenes/main.fescene");
    REQUIRE(built);

    // The indexed asset is present with its scheme and id; the unindexed file is
    // present with no scheme and no id; the scene is present under its key.
    const pack::PackEntry* hero = built.value().find_path("hero.png");
    REQUIRE(hero != nullptr);
    CHECK(hero->scheme == "tex");
    CHECK(hero->guid == imported.value().id);
    CHECK(hero->data == bytes_of("PNG"));

    const pack::PackEntry* loose = built.value().find_path("loose.txt");
    REQUIRE(loose != nullptr);
    CHECK(loose->scheme.empty());
    CHECK_FALSE(loose->has_guid());
    CHECK(loose->data == bytes_of("LOOSE"));

    const pack::PackEntry* scene = built.value().find_path("scenes/main.fescene");
    REQUIRE(scene != nullptr);
    CHECK_FALSE(scene->has_guid());

    // The sidecar the import wrote is metadata, not an asset, and is not packed.
    CHECK(built.value().find_path("hero.png.meta") == nullptr);
}

TEST_CASE("pruning packs only the assets the caller lists, plus the scene") {
    TempDir dir("prune");
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/hero.png"), "PNG"));
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/unused.png"), "UNUSED"));
    REQUIRE_FALSE(fs::write_text_file(dir.file("scenes/main.fescene"), "{\"format\":\"fork_scene\"}"));

    AssetDatabase assets;
    assets.set_root(dir.file("assets"));
    auto imported = assets.import("hero.png", "tex");
    REQUIRE(imported);

    PackOptions options;
    options.prune = true;
    options.include = {"hero.png"};
    auto built = pack::PackWriter::build(assets, dir.file("scenes/main.fescene"), "scenes/main.fescene", options);
    REQUIRE(built);

    // The listed asset keeps its scheme and id; the unlisted one is gone; the
    // scene is always present, because it is the entry point.
    const pack::PackEntry* hero = built.value().find_path("hero.png");
    REQUIRE(hero != nullptr);
    CHECK(hero->scheme == "tex");
    CHECK(hero->guid == imported.value().id);
    CHECK(built.value().find_path("unused.png") == nullptr);
    CHECK(built.value().find_path("scenes/main.fescene") != nullptr);
}

TEST_CASE("the default pack options pack the whole tree, not a pruned one") {
    TempDir dir("noprune");
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/used.png"), "USED"));
    REQUIRE_FALSE(fs::write_text_file(dir.file("assets/spare.png"), "SPARE"));
    REQUIRE_FALSE(fs::write_text_file(dir.file("scenes/main.fescene"), "{\"format\":\"fork_scene\"}"));

    AssetDatabase assets;
    assets.set_root(dir.file("assets"));
    REQUIRE(assets.import("used.png", "tex"));

    // No options at all: the historical behaviour, so a caller that never heard
    // of pruning is unaffected.
    auto built = pack::PackWriter::build(assets, dir.file("scenes/main.fescene"), "scenes/main.fescene");
    REQUIRE(built);
    CHECK(built.value().find_path("used.png") != nullptr);
    CHECK(built.value().find_path("spare.png") != nullptr);
}

TEST_CASE("a pack mounts on a ResourceManager and serves loads with no loose files") {
    TempDir dir("mount");
    // A real, tiny scene with one sprite, written by the real serializer so the
    // packed bytes are a genuine scene document. The sprite carries no texture:
    // the CPU ImageLoader under the default scheme is what proves a packed
    // asset is decoded, without needing a GPU.
    Scene authored;
    authored.set_name("Packed");
    Node* node = authored.create_node("Hero");
    auto* sprite = node->add_component<Sprite2D>();
    sprite->size = Vec2{32.0f, 32.0f};
    REQUIRE_FALSE(serialization::scene::save(authored, dir.file("scenes/main.fescene")));

    const std::vector<u8> pixels = checker_tga_bytes();
    REQUIRE_FALSE(fs::write_binary_file(dir.file("assets/checker.tga"), pixels.data(), pixels.size()));

    AssetDatabase assets;
    assets.set_root(dir.file("assets"));
    auto imported = assets.import("checker.tga", "");
    REQUIRE(imported);

    // Pack it, then delete the loose files: everything below reads only from the
    // pack, which is the property a shipped build relies on.
    REQUIRE_FALSE(pack::PackWriter::write(dir.file("game.fepack"), assets, dir.file("scenes/main.fescene"),
                                          "scenes/main.fescene"));
    std::filesystem::remove_all(dir.file("assets"));
    std::filesystem::remove_all(dir.file("scenes"));

    auto source = pack::PackSource::open(dir.file("game.fepack"));
    REQUIRE(source);

    ResourceManager manager;
    manager.mount(source.value());

    // The startup scene loads through the ordinary path, from the pack.
    Scene loaded;
    REQUIRE_FALSE(serialization::scene::load("scenes/main.fescene", loaded, &manager));
    CHECK(loaded.name() == "Packed");
    REQUIRE(loaded.node_count() == 1);

    // A guid reference resolves through the pack's own index, with no asset
    // database on the manager, and the bytes decode through the real ImageLoader
    // (registered under the default scheme, which the entry records).
    manager.register_loader(std::make_unique<ImageLoader>());
    CHECK(manager.resolve_asset(imported.value().id) != nullptr);
    auto image = manager.load_as<Image>(to_reference(imported.value().id));
    REQUIRE(image);
    CHECK(image.value()->width() == 2);
    CHECK(image.value()->height() == 2);
    CHECK(image.value()->pixels().size() == 2U * 2U * 4U);

    // A missing key fails cleanly rather than reading a loose file.
    auto missing = manager.read_text("scenes/absent.fescene");
    CHECK_FALSE(missing);
}
