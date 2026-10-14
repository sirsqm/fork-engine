// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for resource change detection and reload (ADR-0046): the ResourceManager
// remembers the file each cached resource was loaded from and its write time,
// poll_changed() reports the ones whose file changed, and reload() re-loads a
// named one and hands the caller the fresh instance to adopt. Headless -- no GPU,
// no window; a fake loader stands in for a real one so a reload is observable as
// a replaced instance rather than decoded pixels.
#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/resources/AssetId.h"
#include "fe/resources/ResourceManager.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace fe;

namespace {

// A minimal in-memory resource whose `tag` identifies the load that produced it,
// so a test can tell a fresh instance from a cached one.
class FakeResource final : public Resource {
public:
    static constexpr const char* kTypeName = "FakeResource";

    explicit FakeResource(std::string path, int resource_tag) : Resource(std::move(path)), tag(resource_tag) {}

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    int tag;
};

// Counts loads and tags each resource with the running count. It does not read
// the file, so it succeeds even for a missing path; that is what most tests want
// (they drive the cache, not the decoder).
class FakeLoader final : public ResourceLoader {
public:
    explicit FakeLoader(std::string scheme_name, int& load_counter)
        : scheme_(std::move(scheme_name)), counter_(&load_counter) {}

    [[nodiscard]] std::string_view scheme() const noexcept override { return scheme_; }
    [[nodiscard]] bool supports(std::string_view extension) const noexcept override { return extension == ".blob"; }

    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override {
        ++(*counter_);
        return std::static_pointer_cast<Resource>(std::make_shared<FakeResource>(std::string(path), *counter_));
    }

private:
    std::string scheme_;
    int* counter_;
};

// A loader that reads its bytes through the source, so a missing file is a real
// load failure - the case the reload path must report without discarding the
// instance it already has.
class FileBackedFakeLoader final : public ResourceLoader {
public:
    explicit FileBackedFakeLoader(int& load_counter) : counter_(&load_counter) {}

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override { return extension == ".blob"; }

    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override {
        auto bytes = read_bytes(path);
        if (!bytes) {
            return bytes.error();
        }
        ++(*counter_);
        return std::static_pointer_cast<Resource>(std::make_shared<FakeResource>(std::string(path), *counter_));
    }

private:
    int* counter_;
};

// A byte source that is not the filesystem, standing in for a mounted pack. A
// resource loaded through it has no loose file and must never be reloaded.
class MemorySource final : public ResourceSource {
public:
    [[nodiscard]] Result<std::vector<u8>> read_binary(std::string_view path) const override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) {
            return Error("missing");
        }
        return std::vector<u8>(it->second.begin(), it->second.end());
    }

    [[nodiscard]] Result<std::string> read_text(std::string_view path) const override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) {
            return Error("missing");
        }
        return it->second;
    }

    [[nodiscard]] bool exists(std::string_view path) const override { return files_.count(std::string(path)) > 0; }

private:
    std::unordered_map<std::string, std::string> files_;
};

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_reload_test_" + name + "_" + std::to_string(counter_++));
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

// Moves a file's write time forward, so a change check sees a difference
// regardless of the filesystem's timestamp resolution. This is what "the file
// changed on disk" means to the reload check.
void touch_forward(const std::string& path) {
    const auto now = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, now + std::chrono::seconds(10));
}

} // namespace

TEST_CASE("fs::last_write_time_ns is 0 for a missing file and changes for a real one") {
    TempDir dir("stamp");
    const std::string path = dir.file("thing.blob");
    CHECK(fs::last_write_time_ns(path) == 0);

    REQUIRE_FALSE(fs::write_text_file(path, "hello"));
    const u64 before = fs::last_write_time_ns(path);
    REQUIRE(before != 0);

    touch_forward(path);
    CHECK(fs::last_write_time_ns(path) != before);
}

TEST_CASE("poll_changed reports a changed file once and leaves an unchanged one alone") {
    TempDir dir("poll");
    const std::string changed = dir.file("changed.blob");
    const std::string stable = dir.file("stable.blob");
    REQUIRE_FALSE(fs::write_text_file(changed, "first"));
    REQUIRE_FALSE(fs::write_text_file(stable, "same"));

    int loads = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", loads));

    auto first = manager.load(changed);
    REQUIRE(first);
    auto stable_resource = manager.load(stable);
    REQUIRE(stable_resource);
    const int stable_tag = static_cast<FakeResource*>(stable_resource.value().get())->tag;
    CHECK(loads == 2);

    // Nothing changed yet: a poll is empty.
    CHECK(manager.poll_changed().empty());
    CHECK(loads == 2);

    touch_forward(changed);
    const std::vector<std::string> changed_refs = manager.poll_changed();
    REQUIRE(changed_refs.size() == 1);
    CHECK(changed_refs.front() == changed);

    // The change is reported once; a second poll before adopting it is empty.
    CHECK(manager.poll_changed().empty());
    CHECK(loads == 2);

    // Adopting the change yields a fresh instance; the old handle stays valid.
    auto reloaded = manager.reload(changed);
    REQUIRE(reloaded);
    CHECK(loads == 3);
    CHECK(reloaded.value() != first.value());
    CHECK(static_cast<FakeResource*>(reloaded.value().get())->tag == 3);

    // The unchanged file is the very same instance, not re-loaded.
    auto stable_again = manager.load(stable);
    REQUIRE(stable_again);
    CHECK(stable_again.value() == stable_resource.value());
    CHECK(static_cast<FakeResource*>(stable_again.value().get())->tag == stable_tag);
}

TEST_CASE("a released resource is dropped, not reported, by a poll") {
    TempDir dir("released");
    const std::string path = dir.file("gone.blob");
    REQUIRE_FALSE(fs::write_text_file(path, "bytes"));

    int loads = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", loads));

    {
        auto resource = manager.load(path);
        REQUIRE(resource);
        touch_forward(path);
    }
    // The only reference is gone, so there is nothing to refresh; the poll must
    // not resurrect it, and reload must refuse.
    CHECK(manager.poll_changed().empty());
    CHECK(loads == 1);
    CHECK_FALSE(manager.reload(path));

    // A later request loads it fresh.
    auto again = manager.load(path);
    REQUIRE(again);
    CHECK(loads == 2);
}

TEST_CASE("reload refuses a reference that is not loaded") {
    TempDir dir("unloaded");
    const std::string path = dir.file("thing.blob");
    REQUIRE_FALSE(fs::write_text_file(path, "bytes"));

    int loads = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", loads));

    const auto result = manager.reload(path);
    REQUIRE_FALSE(result);
    CHECK(loads == 0);
}

TEST_CASE("reload replaces the cached instance and reports a broken file without tearing down the old one") {
    TempDir dir("broken");
    const std::string path = dir.file("thing.blob");
    REQUIRE_FALSE(fs::write_text_file(path, "bytes"));

    int loads = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FileBackedFakeLoader>(loads));

    auto first = manager.load(path);
    REQUIRE(first);
    CHECK(loads == 1);

    // Deleting the file makes the next load fail; the working instance must stay
    // cached and usable.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    CHECK_FALSE(manager.reload(path));
    CHECK(loads == 1);

    auto still_there = manager.load(path);
    REQUIRE(still_there);
    CHECK(still_there.value() == first.value());
}

TEST_CASE("a guid-referenced asset is watched through its current path") {
    TempDir dir("guid");
    ResourceManager manager;
    int loads = 0;
    manager.register_loader(std::make_unique<FakeLoader>("tex", loads));

    manager.assets().set_root(dir.str());
    REQUIRE_FALSE(fs::write_text_file(dir.file("hero.blob"), "bytes"));
    auto imported = manager.assets().import("hero.blob", "tex");
    REQUIRE(imported);
    const AssetId id = imported.value().id;

    const std::string reference = to_reference(id);
    auto resource = manager.load(reference);
    REQUIRE(resource);
    CHECK(resource.value()->asset_id() == id);

    CHECK(manager.poll_changed().empty());

    touch_forward(dir.file("hero.blob"));
    const std::vector<std::string> changed_refs = manager.poll_changed();
    REQUIRE(changed_refs.size() == 1);
    CHECK(changed_refs.front() == reference);

    auto reloaded = manager.reload(reference);
    REQUIRE(reloaded);
    CHECK(reloaded.value() != resource.value());
    // The reload re-resolves the identity, so the asset id is unchanged.
    CHECK(reloaded.value()->asset_id() == id);
}

TEST_CASE("a resource from a mounted source has no loose file and is never reported") {
    ResourceManager manager;
    int loads = 0;
    manager.register_loader(std::make_unique<FakeLoader>("", loads));
    manager.mount(std::make_shared<MemorySource>());

    auto resource = manager.load("packed.blob");
    REQUIRE(resource);

    CHECK(manager.poll_changed().empty());
    CHECK(loads == 1);
}
