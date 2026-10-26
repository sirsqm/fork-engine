// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/FileDialogViewModel.h"

#include "fe/core/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace fe::editor {
namespace {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

} // namespace

void FileDialogViewModel::open(Mode mode, std::string directory, std::string file_name, std::string extension_filter) {
    mode_ = mode;
    file_name_ = std::move(file_name);
    extension_filter_ = std::move(extension_filter);
    filter_.clear();
    // Fall back to the working directory so the dialog always shows something,
    // even when the caller's hint is empty or gone.
    if (directory.empty() || !fs::is_directory(directory)) {
        directory = ".";
    }
    directory_ = fs::normalize(directory);
    // normalize(".") strips the single "." segment down to "", which is not a
    // usable path; "." is the working directory and is what a listing needs.
    if (directory_.empty()) {
        directory_ = ".";
    }
    open_ = true;
    changed_ = true;
    refresh_rows();
}

void FileDialogViewModel::close() noexcept {
    open_ = false;
    changed_ = true;
    rows_.clear();
}

bool FileDialogViewModel::enter(std::string_view name) {
    if (!open_ || name.empty()) {
        return false;
    }
    const std::string candidate = fs::join(directory_, name);
    if (!fs::is_directory(candidate)) {
        return false;
    }
    directory_ = fs::normalize(candidate);
    // The old file name named a file in the old directory.
    file_name_.clear();
    changed_ = true;
    refresh_rows();
    return true;
}

bool FileDialogViewModel::go_up() {
    if (!open_) {
        return false;
    }
    const std::string parent = fs::parent_path(directory_);
    // A root ("/", "C:/") has no parent, or normalizes to itself; either way
    // there is nowhere to go.
    if (parent.empty() || parent == directory_) {
        return false;
    }
    directory_ = fs::normalize(parent);
    file_name_.clear();
    changed_ = true;
    refresh_rows();
    return true;
}

void FileDialogViewModel::set_filter(std::string filter) {
    if (filter_ == filter) {
        return;
    }
    filter_ = std::move(filter);
    changed_ = true;
    refresh_rows();
}

std::string FileDialogViewModel::selected_path() const {
    if (file_name_.empty()) {
        return {};
    }
    return fs::join(directory_, file_name_);
}

bool FileDialogViewModel::can_confirm() const {
    if (!open_) {
        return false;
    }
    const std::string path = selected_path();
    if (path.empty()) {
        return false;
    }
    if (mode_ == Mode::OpenFile) {
        // Opening requires an existing file; saving may name a new one.
        return fs::exists(path);
    }
    return true;
}

void FileDialogViewModel::set_directory_for_test(std::string directory) {
    directory_ = fs::normalize(std::move(directory));
    changed_ = true;
    refresh_rows();
}

void FileDialogViewModel::refresh_rows() {
    rows_.clear();
    for (const fs::DirectoryEntry& entry : fs::list_directory(directory_)) {
        // Hidden entries are noise in a dialog; the editor's own convention is
        // that a leading dot is internal (".fork", ".meta").
        if (!entry.name.empty() && entry.name.front() == '.') {
            continue;
        }
        if (!contains_ci(entry.name, filter_)) {
            continue;
        }
        Row row;
        row.name = entry.name;
        row.is_directory = entry.is_directory;
        row.matches_filter = entry.is_directory || extension_filter_.empty() ||
                             lower(fs::extension(entry.name)) == lower(extension_filter_);
        rows_.push_back(std::move(row));
    }
    // fs::list_directory already orders directories-first then by name, and the
    // filter is order-preserving, so `rows_` keeps that order.
}

} // namespace fe::editor
