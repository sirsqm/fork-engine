// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe::editor {

// Pure, headless view model for the editor's file dialog.
//
// The editor needs to open and save scenes and projects from the UI, which means
// browsing the filesystem. That browsing - which directory is shown, which
// entries pass the filter, what the chosen path is, and how a typed file name is
// completed - is logic, not drawing, so it lives here and is tested without a
// window. The ImGui panel (`FileDialogPanel`) is a thin projection of this type,
// the same split the hierarchy and asset browser use.
//
// The model is deliberately small and opinionated. It browses *directories* one
// level at a time (via `fs::list_directory`), offers a text filter, and tracks a
// "current file name" for the save case. It performs no filesystem writes; the
// caller performs the open or save with the path the model reports.
//
// Paths are kept as the caller supplied them, except that a chosen path is
// always the shown directory joined with an entry name, so it is a real path a
// loader or saver can open. The model never calls `std::filesystem` directly; it
// goes through `fe::fs`, so the platform path rules live in one place.
class FileDialogViewModel {
public:
    // What the dialog is for. It changes the title, which verbs are offered
    // ("Open" versus "Save"), and whether an existing file is required.
    enum class Mode {
        OpenFile,
        SaveFile,
    };

    // One row the panel can draw. Directories are navigable; files are
    // selectable, and a file whose extension matches the dialog's filter is
    // highlighted as "selectable for this dialog" (the panel greys the rest).
    struct Row {
        std::string name;
        bool is_directory = false;
        // True when this row is a file the dialog would accept (its extension
        // matches `extension_filter()`, or the filter is empty).
        bool matches_filter = false;
    };

    FileDialogViewModel() = default;

    // --- Lifecycle ---------------------------------------------------------
    // Opens the dialog on `directory` with an optional starting file name (the
    // save case) and a title. `directory` is the directory to browse; when it is
    // empty or not a directory, the current working directory is used, so the
    // dialog always shows something. `extension_filter` is a lower-case extension
    // including the dot (".fescene"); an empty filter accepts every file.
    void open(Mode mode, std::string directory, std::string file_name = {}, std::string extension_filter = {});

    // Closes the dialog. A closed dialog is inert: navigation and confirmation
    // do nothing until it is opened again.
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::string& extension_filter() const noexcept { return extension_filter_; }

    // --- Navigation --------------------------------------------------------
    // The directory currently shown. Never empty while the dialog is open.
    [[nodiscard]] const std::string& directory() const noexcept { return directory_; }

    // The rows for the current directory, after filtering. Directories sort
    // before files and each group by name.
    [[nodiscard]] const std::vector<Row>& rows() const noexcept { return rows_; }

    // Enters `name` as a subdirectory of the current directory. Returns false
    // (and changes nothing) when the dialog is closed, `name` is empty, or the
    // result is not a directory. The current file name is cleared, because it
    // named a file in the old directory.
    bool enter(std::string_view name);

    // Moves to the parent of the current directory. Returns false at a root (a
    // parent that is empty or equal to the current directory). The current file
    // name is cleared.
    bool go_up();

    // Sets the current file name (the save target, or the open selection). The
    // name is taken verbatim; it may contain a path separator, in which case the
    // caller's join still resolves it.
    void set_file_name(std::string name) { file_name_ = std::move(name); }
    [[nodiscard]] const std::string& file_name() const noexcept { return file_name_; }

    // Sets the text filter applied to the rows. An empty filter shows every
    // entry. Case-insensitive substring match on the entry name.
    void set_filter(std::string filter);
    [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

    // --- Confirmation ------------------------------------------------------
    // The path the dialog would confirm right now: the current directory joined
    // with the current file name. Empty when there is no file name. This is what
    // "Open"/"Save" acts on.
    [[nodiscard]] std::string selected_path() const;

    // True when the dialog can be confirmed: it is open and a path is selected.
    // In OpenFile mode the path must also exist (you cannot open what is not
    // there); in SaveFile mode a new name is allowed.
    [[nodiscard]] bool can_confirm() const;

    // True when the model changed the directory, filter, file name, or open
    // state since the last call to `clear_changed()`. The panel uses this to
    // rebuild its cached text buffers only when it must, and a test uses it to
    // assert that a no-op navigation reports no change.
    [[nodiscard]] bool changed() const noexcept { return changed_; }
    void clear_changed() noexcept { changed_ = false; }

    // For tests: force the shown directory without the directory existing (so a
    // non-existent start directory can be exercised without touching the disk).
    void set_directory_for_test(std::string directory);

private:
    // Recomputes `rows_` from `directory_` and `filter_`, via fs::list_directory.
    void refresh_rows();

    Mode mode_ = Mode::OpenFile;
    bool open_ = false;
    bool changed_ = false;
    std::string directory_;
    std::string file_name_;
    std::string filter_;
    std::string extension_filter_;
    std::vector<Row> rows_;
};

} // namespace fe::editor
