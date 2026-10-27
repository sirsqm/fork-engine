// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the writable user-data storage seam (ADR-0096).
//
// The seam is exercised through its two real backends and its null one. The
// desktop backend is pointed at a temporary directory (Desc::root_override), so
// the tests never touch the developer's real home directory; the null backend
// proves the contract with no OS; and the engine is run with an application id to
// prove the store is wired into the frame context.

#include <doctest/doctest.h>

#include "fe/platform/UserData.h"
#include "fe/runtime/Application.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"
#include "fe/systems/System.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace fe;

namespace {

// A unique temporary directory per test case, removed on destruction. Same shape
// as the filesystem tests use, so the two suites clean up the same way.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() /
                ("fe_userdata_test_" + name + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

[[nodiscard]] UserData::Desc desktop_desc(const TempDir& dir) {
    UserData::Desc desc;
    desc.app_id = "test_app";
    desc.root_override = dir.str();
    return desc;
}

// A system that records the store the context handed it and writes one key, so a
// test can prove the store reaches gameplay through FrameContext.
class SavingSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "SavingSystem"; }

    void update(const FrameContext& context, Scene& /*scene*/) override {
        user_data = context.user_data;
        if (user_data != nullptr) {
            wrote_ok = user_data->write("from_system", std::string_view("hello")).ok();
        }
    }

    UserData* user_data = nullptr;
    bool wrote_ok = false;
};

// An application that saves through a system on its first frame.
class SavingApp final : public Application {
public:
    explicit SavingApp(Engine::Config config) : Application(config) {}

    SavingSystem* system = nullptr;

protected:
    Error on_start() override {
        system = schedule().add_system<SavingSystem>();
        return Error::none();
    }
};

} // namespace

TEST_CASE("a key is a name, not a path") {
    // The rule the seam rests on: a key can never name a path, so a store can
    // turn one into a filename without escaping its directory.
    CHECK(UserData::is_valid_key("best_score"));
    CHECK(UserData::is_valid_key("a.b-c_1"));
    CHECK_FALSE(UserData::is_valid_key(""));
    CHECK_FALSE(UserData::is_valid_key("."));
    CHECK_FALSE(UserData::is_valid_key(".."));
    CHECK_FALSE(UserData::is_valid_key("../escape"));
    CHECK_FALSE(UserData::is_valid_key("a/b"));
    CHECK_FALSE(UserData::is_valid_key("a\\b"));
    CHECK_FALSE(UserData::is_valid_key("with space"));
    // A NUL is not `[A-Za-z0-9._-]`; the length-carrying view is what proves it
    // (a bare literal would stop at the NUL and look like "nul").
    CHECK_FALSE(UserData::is_valid_key(std::string_view("nul\0byte", 8)));
}

TEST_CASE("the null store round-trips a value within the process") {
    UserData::Desc desc;
    desc.backend = UserData::Desc::Backend::Null;
    auto store = UserData::create(desc);
    REQUIRE(store != nullptr);
    CHECK(std::string_view(store->backend_name()) == "null");
    CHECK(store->root().empty());

    CHECK_FALSE(store->contains("score"));
    // A missing key is an ordinary result, not a crash: a first run has no save.
    auto missing = store->read_text("score");
    CHECK_FALSE(missing.ok());

    CHECK(store->write("score", std::string_view("1234")).ok());
    CHECK(store->contains("score"));
    auto value = store->read_text("score");
    REQUIRE(value.ok());
    CHECK(value.value() == "1234");

    // Overwrite replaces rather than appends.
    CHECK(store->write("score", std::string_view("99")).ok());
    CHECK(store->read_text("score").value() == "99");

    CHECK(store->remove("score").ok());
    CHECK_FALSE(store->contains("score"));
    // Removing a key that is already gone still satisfies "the key is gone".
    CHECK(store->remove("score").ok());
}

TEST_CASE("the null store keeps binary values byte-exact") {
    UserData::Desc desc;
    desc.backend = UserData::Desc::Backend::Null;
    auto store = UserData::create(desc);

    const std::vector<u8> bytes{0x00, 0x01, 0xff, 0x7f, 0x80, 0x0a, 0x00};
    CHECK(store->write("blob", bytes.data(), bytes.size()).ok());
    auto read = store->read("blob");
    REQUIRE(read.ok());
    CHECK(read.value() == bytes);
}

TEST_CASE("a malformed key is refused rather than written") {
    UserData::Desc desc;
    desc.backend = UserData::Desc::Backend::Null;
    auto store = UserData::create(desc);

    CHECK_FALSE(store->write("../escape", std::string_view("x")).ok());
    CHECK_FALSE(store->read("a/b").ok());
    CHECK_FALSE(store->contains(".."));
    CHECK_FALSE(store->remove("a b").ok());
    // Nothing was written under any name.
    CHECK(store->keys().empty());
}

TEST_CASE("keys are listed sorted") {
    UserData::Desc desc;
    desc.backend = UserData::Desc::Backend::Null;
    auto store = UserData::create(desc);
    for (const char* key : {"zulu", "alpha", "mike"}) {
        CHECK(store->write(key, std::string_view("v")).ok());
    }
    CHECK(store->keys() == std::vector<std::string>{"alpha", "mike", "zulu"});
}

TEST_CASE("the desktop store persists to a real directory and reads it back") {
    TempDir dir("desktop");
    auto store = UserData::create(desktop_desc(dir));
    REQUIRE(store != nullptr);
    CHECK(std::string_view(store->backend_name()) == "desktop");
    CHECK_FALSE(store->root().empty());

    CHECK(store->write("best_score", std::string_view("4242")).ok());

    // A *second* store opened on the same directory sees the value: this is what
    // "persists across a reload" means, modelled as a fresh handle.
    auto reopened = UserData::create(desktop_desc(dir));
    REQUIRE(reopened != nullptr);
    auto value = reopened->read_text("best_score");
    REQUIRE(value.ok());
    CHECK(value.value() == "4242");
    CHECK(reopened->keys() == std::vector<std::string>{"best_score"});

    // Binary survives too: the desktop path must not go through text encoding.
    const std::vector<u8> bytes{0x00, 0xff, 0x10};
    CHECK(reopened->write("blob", bytes.data(), bytes.size()).ok());
    auto read = UserData::create(desktop_desc(dir))->read("blob");
    REQUIRE(read.ok());
    CHECK(read.value() == bytes);

    CHECK(reopened->remove("best_score").ok());
    CHECK_FALSE(UserData::create(desktop_desc(dir))->contains("best_score"));
}

TEST_CASE("the desktop store does not list a file it did not write") {
    TempDir dir("foreign");
    std::filesystem::create_directories(dir.str());
    // A file that is not `<key>.dat` is not a key, even though it is in the
    // store's directory.
    std::filesystem::create_directories(std::filesystem::path(dir.str()) / "subdir");
    { std::ofstream(dir.str() + "/notes.txt") << "hi"; }

    auto store = UserData::create(desktop_desc(dir));
    REQUIRE(store != nullptr);
    CHECK(store->write("real", std::string_view("v")).ok());
    CHECK(store->keys() == std::vector<std::string>{"real"});
}

TEST_CASE("a store with no application id persists nothing") {
    // The safe default: a run that never named itself cannot write to a shared
    // location, so it gets the in-memory store.
    UserData::Desc desc;
    desc.app_id.clear();
    auto store = UserData::create(desc);
    REQUIRE(store != nullptr);
    CHECK(std::string_view(store->backend_name()) == "null");
}

TEST_CASE("the engine hands a store to the frame context") {
    TempDir dir("engine");
    Engine::Config config;
    config.window.backend = WindowBackend::Null;
    config.app_id = "test_app";
    config.user_data_root = dir.str();

    SavingApp app(config);
    REQUIRE(app.initialize().ok());

    // The engine's own store is the persistent backend, because an id was given.
    CHECK(std::string_view(app.engine().user_data().backend_name()) == "desktop");

    REQUIRE(app.system != nullptr);
    CHECK(app.step_frame());
    CHECK(app.system->user_data == &app.engine().user_data());
    CHECK(app.system->wrote_ok);

    // What the system saved is readable through the engine's store and, crucially,
    // through a *fresh* store on the same directory -- the reload.
    CHECK(app.engine().user_data().read_text("from_system").value() == "hello");
    app.shutdown();

    auto reopened = UserData::create(desktop_desc(dir));
    REQUIRE(reopened != nullptr);
    CHECK(reopened->read_text("from_system").value() == "hello");
}

TEST_CASE("a headless engine with no application id gets the null store") {
    Engine::Config config;
    config.window.backend = WindowBackend::Null;

    Application app(config);
    REQUIRE(app.initialize().ok());
    CHECK(std::string_view(app.engine().user_data().backend_name()) == "null");
    // A write still succeeds; it simply does not persist.
    CHECK(app.engine().user_data().write("k", std::string_view("v")).ok());
    app.shutdown();
}
