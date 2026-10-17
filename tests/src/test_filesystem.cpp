// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"

#include <filesystem>
#include <string>

using namespace fe;

namespace {

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_fs_test_" + name + "_" + std::to_string(counter_++));
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

} // namespace

TEST_CASE("text files round-trip through write and read") {
    TempDir dir("roundtrip");
    const std::string path = dir.file("hello.txt");

    REQUIRE_FALSE(fs::write_text_file(path, "fork engine\n"));
    CHECK(fs::exists(path));

    auto contents = fs::read_text_file(path);
    REQUIRE(contents);
    CHECK(contents.value() == "fork engine\n");
}

TEST_CASE("binary files preserve arbitrary bytes") {
    TempDir dir("binary");
    const std::string path = dir.file("blob.bin");

    const std::vector<u8> data = {0x00, 0x01, 0xFF, 0x7F, 0x80, 0x00};
    REQUIRE_FALSE(fs::write_binary_file(path, data.data(), data.size()));

    auto read = fs::read_binary_file(path);
    REQUIRE(read);
    CHECK(read.value() == data);
}

TEST_CASE("write creates missing parent directories") {
    TempDir dir("parents");
    const std::string path = dir.file("a/b/c/deep.txt");

    REQUIRE_FALSE(fs::write_text_file(path, "nested"));
    CHECK(fs::exists(path));
}

TEST_CASE("reading a missing file returns an error rather than throwing") {
    TempDir dir("missing");
    auto result = fs::read_text_file(dir.file("does_not_exist.txt"));
    CHECK_FALSE(result);
    CHECK_FALSE(result.error().message().empty());
}

TEST_CASE("path helpers behave consistently") {
    CHECK(fs::filename("a/b/c.png") == "c.png");
    CHECK(fs::filename("c.png") == "c.png");
    CHECK(fs::parent_path("a/b/c.png") == "a/b");
    CHECK(fs::parent_path("c.png").empty());

    CHECK(fs::extension("hero.PNG") == ".png");
    CHECK(fs::extension("noext").empty());
    CHECK(fs::extension("archive.tar.gz") == ".gz");

    CHECK(fs::join("assets", "textures") == "assets/textures");
    CHECK(fs::join("assets/", "textures") == "assets/textures");
    CHECK(fs::join("", "textures") == "textures");

    CHECK(fs::normalize("a/./b//c.png") == "a/b/c.png");
    CHECK(fs::normalize("a\\b\\c.png") == "a/b/c.png");
    CHECK(fs::normalize("./a/b") == "a/b");
    CHECK(fs::normalize("a/b/") == "a/b/");
    CHECK(fs::normalize("/abs//path") == "/abs/path");
    CHECK(fs::normalize("already/clean.png") == "already/clean.png");
}

TEST_CASE("relative_to strips a containing prefix and leaves the rest alone") {
    CHECK(fs::relative_to("/proj", "/proj/assets") == "assets");
    CHECK(fs::relative_to("/proj", "/proj/scenes/level.json") == "scenes/level.json");
    // A prefix that only matches mid-segment is not a containing directory.
    CHECK(fs::relative_to("/proj", "/project/assets") == "/project/assets");
    // A path outside the base is returned unchanged, so an absolute path
    // elsewhere on the machine survives a round trip.
    CHECK(fs::relative_to("/proj", "/other/assets") == "/other/assets");
    // The base itself, and separator/style differences, normalize first.
    CHECK(fs::relative_to("/proj", "/proj") == "/proj");
    CHECK(fs::relative_to("/proj/", "/proj//assets/") == "assets/");
    CHECK(fs::relative_to("", "assets") == "assets");
}

TEST_CASE("is_absolute follows the host platform's path syntax") {
    // Build the absolute example through std::filesystem so the expectation is
    // the platform's, not a POSIX path asserted on Windows.
    const std::string absolute = (std::filesystem::temp_directory_path() / "assets").string();
    CHECK(fs::is_absolute(absolute));
    CHECK_FALSE(fs::is_absolute("assets"));
    CHECK_FALSE(fs::is_absolute("assets/textures"));
    CHECK_FALSE(fs::is_absolute(""));
#ifdef _WIN32
    CHECK(fs::is_absolute("C:/projects/demo"));
#else
    CHECK(fs::is_absolute("/usr/local/assets"));
#endif
}

TEST_CASE("copy_file copies bytes and carries the executable bit") {
    TempDir dir("copy");
    const std::string source = dir.file("runtime");
    REQUIRE_FALSE(fs::write_binary_file(source, "ELF-ish", 7));

    // Make the source executable, then copy it: a copied runtime must stay one,
    // which is what an export step that copies the player depends on.
    std::filesystem::permissions(source, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

    const std::string destination = dir.file("out/runtime");
    REQUIRE_FALSE(fs::copy_file(source, destination));

    auto bytes = fs::read_binary_file(destination);
    REQUIRE(bytes);
    CHECK(std::string(bytes.value().begin(), bytes.value().end()) == "ELF-ish");

    const auto perms = std::filesystem::status(destination).permissions();
    CHECK((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);

    // A missing source is an error, not an empty destination.
    CHECK(fs::copy_file(dir.file("absent"), dir.file("out/absent")));
}

TEST_CASE("copy_file overwrites an existing destination") {
    TempDir dir("copy_overwrite");
    const std::string source = dir.file("new");
    const std::string destination = dir.file("old");
    REQUIRE_FALSE(fs::write_binary_file(source, "new", 3));
    REQUIRE_FALSE(fs::write_binary_file(destination, "old-longer", 10));

    REQUIRE_FALSE(fs::copy_file(source, destination));

    auto bytes = fs::read_binary_file(destination);
    REQUIRE(bytes);
    CHECK(std::string(bytes.value().begin(), bytes.value().end()) == "new");
}
