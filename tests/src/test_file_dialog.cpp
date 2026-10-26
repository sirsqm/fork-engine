// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the file dialog's view model: the headless browsing logic behind
// the editor's Open/Save dialogs. These touch the filesystem through a real
// temporary directory, so the listing, filtering, navigation, and confirm rules
// are exercised as they are in the editor, not against a stub.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/editor/FileDialogViewModel.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace fe;
using namespace fe::editor;

namespace {

// A throwaway directory tree removed on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ = std::filesystem::temp_directory_path() / ("fe_file_dialog_" + tag + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string str() const { return path_.string(); }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

    void make_dir(const std::string& name) const { std::filesystem::create_directories(path_ / name); }
    void make_file(const std::string& name) const {
        std::filesystem::create_directories((path_ / name).parent_path());
        std::ofstream(path_ / name, std::ios::binary) << "x";
    }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

} // namespace

TEST_CASE("a directory listing shows folders before files, both sorted") {
    TempDir dir("listing");
    dir.make_file("zeta.fescene");
    dir.make_file("alpha.fescene");
    dir.make_dir("scenes");
    dir.make_dir("assets");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");
    REQUIRE(model.is_open());

    const auto& rows = model.rows();
    REQUIRE(rows.size() == 4);
    // Directories first (assets, scenes), then files (alpha, zeta).
    CHECK(rows[0].name == "assets");
    CHECK(rows[0].is_directory);
    CHECK(rows[1].name == "scenes");
    CHECK(rows[1].is_directory);
    CHECK(rows[2].name == "alpha.fescene");
    CHECK_FALSE(rows[2].is_directory);
    CHECK(rows[3].name == "zeta.fescene");
}

TEST_CASE("the filter narrows the listing without losing it") {
    TempDir dir("filter");
    dir.make_file("level_one.fescene");
    dir.make_file("level_two.fescene");
    dir.make_file("notes.txt");
    dir.make_dir("levels");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");

    model.set_filter("one");
    CHECK(model.rows().size() == 1);
    CHECK(model.rows().front().name == "level_one.fescene");

    // Case-insensitive: both level files and the "levels" directory match.
    model.set_filter("LEVEL");
    CHECK(model.rows().size() == 3);

    // Clearing restores everything.
    model.set_filter("");
    CHECK(model.rows().size() == 4);
}

TEST_CASE("only files matching the extension filter are marked selectable") {
    TempDir dir("ext");
    dir.make_file("level.fescene");
    dir.make_file("readme.txt");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");

    bool saw_scene = false;
    bool saw_text = false;
    for (const auto& row : model.rows()) {
        if (row.name == "level.fescene") {
            saw_scene = true;
            CHECK(row.matches_filter);
        }
        if (row.name == "readme.txt") {
            saw_text = true;
            CHECK_FALSE(row.matches_filter);
        }
    }
    CHECK(saw_scene);
    CHECK(saw_text);
}

TEST_CASE("navigating into a subdirectory and back up changes the listing") {
    TempDir dir("nav");
    dir.make_file("top.fescene");
    dir.make_dir("sub");
    dir.make_file("sub/inner.fescene");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");
    const usize top_count = model.rows().size();

    CHECK(model.enter("sub"));
    CHECK(fs::normalize(model.directory()) == fs::normalize(dir.file("sub")));
    CHECK(model.rows().size() == 1);
    CHECK(model.rows().front().name == "inner.fescene");

    CHECK(model.go_up());
    CHECK(fs::normalize(model.directory()) == fs::normalize(dir.str()));
    CHECK(model.rows().size() == top_count);
}

TEST_CASE("entering a file or a missing name does nothing") {
    TempDir dir("enter_bad");
    dir.make_file("file.fescene");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");
    const std::string before = model.directory();

    CHECK_FALSE(model.enter("file.fescene"));
    CHECK_FALSE(model.enter("no_such_dir"));
    CHECK_FALSE(model.enter(""));
    CHECK(fs::normalize(model.directory()) == fs::normalize(before));
}

TEST_CASE("the selected path joins the directory and the file name") {
    TempDir dir("selected");
    dir.make_file("level.fescene");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");
    CHECK(model.selected_path().empty());

    model.set_file_name("level.fescene");
    CHECK(fs::normalize(model.selected_path()) == fs::normalize(dir.file("level.fescene")));
}

TEST_CASE("confirm rules differ between open and save") {
    TempDir dir("confirm");
    dir.make_file("exists.fescene");

    SUBCASE("opening requires an existing file") {
        FileDialogViewModel model;
        model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");

        model.set_file_name("missing.fescene");
        CHECK_FALSE(model.can_confirm());

        model.set_file_name("exists.fescene");
        CHECK(model.can_confirm());
    }

    SUBCASE("saving allows a new name") {
        FileDialogViewModel model;
        model.open(FileDialogViewModel::Mode::SaveFile, dir.str(), "brand_new.fescene", ".fescene");
        CHECK(model.can_confirm());
        CHECK(fs::normalize(model.selected_path()) == fs::normalize(dir.file("brand_new.fescene")));
    }
}

TEST_CASE("a closed dialog is inert") {
    TempDir dir("closed");
    dir.make_dir("sub");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");
    model.close();

    CHECK_FALSE(model.is_open());
    CHECK(model.rows().empty());
    CHECK_FALSE(model.enter("sub"));
    CHECK_FALSE(model.go_up());
    model.set_file_name("x.fescene");
    CHECK_FALSE(model.can_confirm());
}

TEST_CASE("a non-existent start directory falls back rather than failing") {
    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, "/no/such/directory/at/all", {}, ".fescene");
    // The dialog opened anyway, browsing something, so it is never blank.
    CHECK(model.is_open());
    CHECK_FALSE(model.directory().empty());
}

TEST_CASE("a change flag reports navigation so the UI can resync") {
    TempDir dir("changed");
    dir.make_dir("sub");

    FileDialogViewModel model;
    model.open(FileDialogViewModel::Mode::OpenFile, dir.str(), {}, ".fescene");
    CHECK(model.changed());
    model.clear_changed();
    CHECK_FALSE(model.changed());

    // A no-op navigation reports no change; a real one does.
    CHECK_FALSE(model.enter("not_here"));
    CHECK_FALSE(model.changed());
    CHECK(model.enter("sub"));
    CHECK(model.changed());
}
