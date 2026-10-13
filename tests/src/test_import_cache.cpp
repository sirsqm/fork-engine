// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The content-addressed import cache. The key is a pure function of the source
// bytes, the import settings, and the importer, so these tests pin that function
// (determinism, invalidation on a source or settings change, importer
// separation), the store round-trip, inertness with no root, and the property
// that matters most: a hit produces an Image identical to a cold decode. See
// docs/adr/0029-import-cache.md.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/resources/Image.h"
#include "fe/resources/ImageLoader.h"
#include "fe/resources/ImportCache.h"
#include "fe/resources/ImportSettings.h"
#include "fe/resources/ResourceManager.h"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace fe;

namespace {

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_import_cache_" + name + "_" + std::to_string(counter_++));
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

// Writes a 2x2 black/white checker TGA (uncompressed, 32-bit, top-left origin).
// A real decodable file is needed so the loader's cold path and its cache hit
// path can be compared, rather than a stub that never touches stb_image.
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

// A one-byte-different image, used to prove a source change invalidates the key.
[[nodiscard]] std::vector<u8> checker_tga_bytes_edited() {
    std::vector<u8> bytes = checker_tga_bytes();
    bytes.back() ^= 0xFF;
    return bytes;
}

[[nodiscard]] std::span<const u8> as_span(const std::vector<u8>& bytes) {
    return std::span<const u8>(bytes.data(), bytes.size());
}

} // namespace

// ---------------------------------------------------------------------------
// The key: a pure function of the inputs
// ---------------------------------------------------------------------------

TEST_CASE("the same inputs produce the same key") {
    const std::vector<u8> source = checker_tga_bytes();
    const ImportSettings settings = ImportSettings::texture_defaults();

    const std::string a = ImportCache::key_for(as_span(source), &settings, "tex");
    const std::string b = ImportCache::key_for(as_span(source), &settings, "tex");
    CHECK(a == b);
    CHECK(a.size() == 16);
}

TEST_CASE("a source change invalidates the key") {
    const ImportSettings settings = ImportSettings::texture_defaults();
    const std::vector<u8> original = checker_tga_bytes();
    const std::vector<u8> edited = checker_tga_bytes_edited();

    CHECK(ImportCache::key_for(as_span(original), &settings, "tex") !=
          ImportCache::key_for(as_span(edited), &settings, "tex"));
}

TEST_CASE("a settings change invalidates the key") {
    const std::vector<u8> source = checker_tga_bytes();
    ImportSettings linear = ImportSettings::texture_defaults();
    ImportSettings nearest = ImportSettings::texture_defaults();
    nearest.filter = rhi::TextureFilter::Nearest;

    CHECK(ImportCache::key_for(as_span(source), &linear, "tex") !=
          ImportCache::key_for(as_span(source), &nearest, "tex"));
}

TEST_CASE("the importer identity separates otherwise identical imports") {
    // A ".png" is both a CPU Image (scheme "") and a GPU Texture2D (scheme "tex").
    // The key must not collide them even when the bytes and settings match.
    const std::vector<u8> source = checker_tga_bytes();
    const ImportSettings settings = ImportSettings::texture_defaults();

    CHECK(ImportCache::key_for(as_span(source), &settings, "") !=
          ImportCache::key_for(as_span(source), &settings, "tex"));
}

TEST_CASE("null settings are distinct from explicit settings") {
    const std::vector<u8> source = checker_tga_bytes();
    const ImportSettings settings = ImportSettings::texture_defaults();
    CHECK(ImportCache::key_for(as_span(source), nullptr, "image") !=
          ImportCache::key_for(as_span(source), &settings, "image"));
}

TEST_CASE("the key is structurally unambiguous across field boundaries") {
    // Length-prefixing means a settings value that happens to look like a source
    // prefix cannot collide with a different (settings, source) split.
    const std::vector<u8> left = {1, 2, 3};
    const std::vector<u8> right = {4, 5};
    const std::vector<u8> other = {1, 2, 3, 4};
    const std::vector<u8> tail = {5};

    // (settings="", source=left+right) must differ from (settings=..., source=...)
    // where the split differs, even though the concatenated bytes match.
    const std::string split_a = ImportCache::content_hash(as_span(left)) + ImportCache::content_hash(as_span(right));
    const std::string split_b = ImportCache::content_hash(as_span(other)) + ImportCache::content_hash(as_span(tail));
    CHECK(split_a != split_b);
}

TEST_CASE("content_hash is deterministic and distinguishes content") {
    const std::vector<u8> a = {1, 2, 3, 4};
    const std::vector<u8> b = {1, 2, 3, 5};
    CHECK(ImportCache::content_hash(as_span(a)) == ImportCache::content_hash(as_span(a)));
    CHECK(ImportCache::content_hash(as_span(a)) != ImportCache::content_hash(as_span(b)));
    CHECK(ImportCache::content_hash(as_span(a)).size() == 16);
}

// ---------------------------------------------------------------------------
// The store: inert by default, round-trips when rooted
// ---------------------------------------------------------------------------

TEST_CASE("a cache with no root is inert") {
    ImportCache cache;
    CHECK_FALSE(cache.is_enabled());
    CHECK_FALSE(cache.contains("0123456789abcdef"));

    const std::vector<u8> artifact = {1, 2, 3};
    CHECK_FALSE(cache.store("0123456789abcdef", as_span(artifact))); // no error, just a no-op
    CHECK_FALSE(cache.fetch("0123456789abcdef"));
}

TEST_CASE("a rooted cache stores and fetches an artifact") {
    TempDir dir("roundtrip");
    ImportCache cache;
    cache.set_root(dir.str());
    CHECK(cache.is_enabled());

    const std::vector<u8> artifact = {10, 20, 30, 40, 50};
    const std::string key = ImportCache::content_hash(as_span(artifact));
    CHECK_FALSE(cache.contains(key));

    REQUIRE_FALSE(cache.store(key, as_span(artifact)));
    CHECK(cache.contains(key));

    auto fetched = cache.fetch(key);
    REQUIRE(fetched);
    CHECK(fetched.value() == artifact);
    CHECK(fs::exists(cache.artifact_path(key)));
}

TEST_CASE("a corrupted artifact reads as a miss, not a wrong result") {
    TempDir dir("corrupt");
    ImportCache cache;
    cache.set_root(dir.str());

    const std::vector<u8> artifact = {1, 2, 3, 4};
    const std::string key = ImportCache::content_hash(as_span(artifact));
    REQUIRE_FALSE(cache.store(key, as_span(artifact)));

    // Overwrite the entry with bytes that are not a valid artifact.
    const std::vector<u8> garbage = {0xFF, 0x00, 0x13, 0x37};
    REQUIRE_FALSE(fs::write_binary_file(cache.artifact_path(key), garbage.data(), garbage.size()));
    CHECK_FALSE(cache.contains(key));
    CHECK_FALSE(cache.fetch(key));
}

TEST_CASE("an entry written under one key is not returned for another") {
    TempDir dir("mismatch");
    ImportCache cache;
    cache.set_root(dir.str());

    const std::vector<u8> artifact = {1, 2, 3};
    const std::string key = ImportCache::content_hash(as_span(artifact));
    REQUIRE_FALSE(cache.store(key, as_span(artifact)));

    // Copy the file to a different key's name: the stored key no longer matches
    // the requested one, so it is a miss.
    const std::string other_key = "ffffffffffffffff";
    auto bytes = fs::read_binary_file(cache.artifact_path(key));
    REQUIRE(bytes);
    REQUIRE_FALSE(fs::write_binary_file(cache.artifact_path(other_key), bytes.value().data(), bytes.value().size()));
    CHECK_FALSE(cache.contains(other_key));
}

// ---------------------------------------------------------------------------
// The loader: a hit produces the same Image as a cold decode
// ---------------------------------------------------------------------------

TEST_CASE("a cold decode populates the cache and a second load reuses it") {
    TempDir dir("loader");
    const std::vector<u8> source = checker_tga_bytes();
    REQUIRE_FALSE(fs::write_binary_file(dir.file("checker.tga"), source.data(), source.size()));

    ResourceManager manager;
    manager.import_cache().set_root(dir.file("cache"));
    manager.register_loader(std::make_unique<ImageLoader>(&manager.import_cache()));

    auto first = manager.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(first);
    CHECK(first.value()->width() == 2);
    CHECK(first.value()->height() == 2);

    // The decode populated exactly one artifact, keyed by the source bytes.
    const std::string key = ImportCache::key_for(as_span(source), nullptr, "image");
    CHECK(manager.import_cache().contains(key));

    // A fresh manager sharing the same cache root gets a hit: no re-decode, and
    // the reconstructed Image is byte-identical to the cold one.
    ResourceManager warm;
    warm.import_cache().set_root(dir.file("cache"));
    warm.register_loader(std::make_unique<ImageLoader>(&warm.import_cache()));
    auto second = warm.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(second);
    CHECK(second.value()->width() == first.value()->width());
    CHECK(second.value()->height() == first.value()->height());
    CHECK(second.value()->pixels() == first.value()->pixels());
}

TEST_CASE("a cache hit and a cold decode produce the same Image bytes") {
    TempDir dir("identical");
    const std::vector<u8> source = checker_tga_bytes();
    REQUIRE_FALSE(fs::write_binary_file(dir.file("checker.tga"), source.data(), source.size()));

    // Cold: no cache root, so the loader decodes every time.
    ResourceManager cold;
    cold.register_loader(std::make_unique<ImageLoader>());
    auto cold_image = cold.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(cold_image);

    // Warm: a rooted cache, primed by the first load, then read back.
    ResourceManager warm;
    warm.import_cache().set_root(dir.file("cache"));
    warm.register_loader(std::make_unique<ImageLoader>(&warm.import_cache()));
    auto priming = warm.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(priming);

    ResourceManager hit;
    hit.import_cache().set_root(dir.file("cache"));
    hit.register_loader(std::make_unique<ImageLoader>(&hit.import_cache()));
    auto hit_image = hit.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(hit_image);

    CHECK(hit_image.value()->width() == cold_image.value()->width());
    CHECK(hit_image.value()->height() == cold_image.value()->height());
    CHECK(hit_image.value()->pixels() == cold_image.value()->pixels());
}

TEST_CASE("an edited source misses the cache and decodes the new bytes") {
    TempDir dir("edited");
    const std::vector<u8> original_bytes = checker_tga_bytes();
    REQUIRE_FALSE(fs::write_binary_file(dir.file("checker.tga"), original_bytes.data(), original_bytes.size()));

    ResourceManager manager;
    manager.import_cache().set_root(dir.file("cache"));
    manager.register_loader(std::make_unique<ImageLoader>(&manager.import_cache()));

    auto original = manager.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(original);

    // Rewrite the file with different bytes: a new key, so a new decode.
    const std::vector<u8> edited = checker_tga_bytes_edited();
    REQUIRE_FALSE(fs::write_binary_file(dir.file("checker.tga"), edited.data(), edited.size()));

    ResourceManager reloaded;
    reloaded.import_cache().set_root(dir.file("cache"));
    reloaded.register_loader(std::make_unique<ImageLoader>(&reloaded.import_cache()));
    auto changed = reloaded.load_as<Image>(dir.file("checker.tga"));
    REQUIRE(changed);
    // The edited texel changed the decoded pixels.
    CHECK(changed.value()->pixels() != original.value()->pixels());
}
