// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/resources/ResourceManager.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace fe;

namespace {

// A minimal in-memory resource used to test the manager's caching, dispatch, and
// release behaviour without touching the filesystem or the GPU.
class FakeResource final : public Resource {
public:
    static constexpr const char* kTypeName = "FakeResource";

    explicit FakeResource(std::string path, int resource_tag) : Resource(std::move(path)), tag(resource_tag) {}

    [[nodiscard]] const char* type_name() const noexcept override { return kTypeName; }

    int tag;
};

// Counts how many times load() was invoked, so tests can prove the cache is
// actually consulted rather than re-loading.
class FakeLoader final : public ResourceLoader {
public:
    explicit FakeLoader(std::string scheme_name, int& load_counter)
        : scheme_(std::move(scheme_name)), counter_(&load_counter) {}

    [[nodiscard]] std::string_view scheme() const noexcept override { return scheme_; }
    [[nodiscard]] bool supports(std::string_view extension) const noexcept override { return extension == ".fake"; }

    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override {
        ++(*counter_);
        return std::static_pointer_cast<Resource>(std::make_shared<FakeResource>(std::string(path), *counter_));
    }

private:
    std::string scheme_;
    int* counter_;
};

} // namespace

TEST_CASE("load dispatches to a loader by extension and caches the result") {
    int load_count = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", load_count));

    auto first = manager.load_as<FakeResource>("hero.fake");
    REQUIRE(first);
    CHECK(first.value()->tag == 1);
    CHECK(load_count == 1);

    // Second load of the same path must hit the cache.
    auto second = manager.load_as<FakeResource>("hero.fake");
    REQUIRE(second);
    CHECK((second.value() == first.value()));
    CHECK(load_count == 1);
}

TEST_CASE("the cache key includes the scheme") {
    int default_loads = 0;
    int tex_loads = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", default_loads));
    manager.register_loader(std::make_unique<FakeLoader>("tex", tex_loads));

    auto image = manager.load_as<FakeResource>("hero.fake");
    auto texture = manager.load_as<FakeResource>("tex:hero.fake");

    REQUIRE(image);
    REQUIRE(texture);
    // Different schemes are different resources even for the same file.
    CHECK((image.value() != texture.value()));
    CHECK(default_loads == 1);
    CHECK(tex_loads == 1);
    CHECK(manager.cached_count() == 2);
}

TEST_CASE("a released resource is reloaded on the next request") {
    int load_count = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", load_count));

    {
        auto resource = manager.load_as<FakeResource>("thing.fake");
        REQUIRE(resource);
        CHECK(load_count == 1);
    } // The last reference goes away here.

    auto reloaded = manager.load_as<FakeResource>("thing.fake");
    REQUIRE(reloaded);
    CHECK(load_count == 2);

    manager.collect_garbage();
    CHECK(manager.loaded_count() == 1);
}

TEST_CASE("an unknown extension produces a clear error") {
    ResourceManager manager;
    auto result = manager.load("mystery.unknown");
    CHECK_FALSE(result);
    CHECK(result.error().message().find(".unknown") != std::string::npos);
}

TEST_CASE("an unknown scheme produces a clear error") {
    int load_count = 0;
    ResourceManager manager;
    manager.register_loader(std::make_unique<FakeLoader>("", load_count));

    auto result = manager.load("bogus:hero.fake");
    CHECK_FALSE(result);
    CHECK(result.error().message().find("bogus") != std::string::npos);
}
