// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/AssetBrowserViewModel.h"

#include "fe/resources/AssetDatabase.h"
#include "fe/resources/ImportSettings.h"
#include "fe/resources/ResourceManager.h"

#include <algorithm>
#include <cctype>

namespace fe::editor {
namespace {

[[nodiscard]] char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Case-insensitive substring test. The filter is a human-typed search, so
// matching "Hero" against "hero.png" is the expected behaviour.
[[nodiscard]] bool contains_case_insensitive(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (usize i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (usize j = 0; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool less_case_insensitive(std::string_view a, std::string_view b) noexcept {
    const usize common = std::min(a.size(), b.size());
    for (usize i = 0; i < common; ++i) {
        const char la = lower(a[i]);
        const char lb = lower(b[i]);
        if (la != lb) {
            return la < lb;
        }
    }
    // A prefix sorts before a longer string; ties fall back to byte order so the
    // listing is deterministic when two paths differ only in case.
    if (a.size() != b.size()) {
        return a.size() < b.size();
    }
    return a < b;
}

// The file name, i.e. everything after the last '/'.
[[nodiscard]] std::string file_name_of(std::string_view path) {
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string(path) : std::string(path.substr(slash + 1));
}

// The containing directory, or "." for a root-level asset.
[[nodiscard]] std::string directory_of(std::string_view path) {
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string(".") : std::string(path.substr(0, slash));
}

} // namespace

void AssetBrowserViewModel::rebuild(const ResourceManager* resources) {
    entries_.clear();
    if (resources != nullptr) {
        const AssetDatabase& database = resources->assets();
        entries_.reserve(database.asset_count());
        for (const auto& [id, asset] : database.entries()) {
            Entry entry;
            entry.id = id;
            entry.path = asset.path;
            entry.name = file_name_of(asset.path);
            entry.directory = directory_of(asset.path);
            entry.scheme = asset.scheme;
            entry.settings_kind = to_string(asset.settings.kind);
            entries_.push_back(std::move(entry));
        }
    }
    apply_view();
}

void AssetBrowserViewModel::set_entries(std::vector<Entry> entries) {
    entries_ = std::move(entries);
    apply_view();
}

void AssetBrowserViewModel::set_filter(std::string filter) {
    filter_ = std::move(filter);
    apply_view();
}

void AssetBrowserViewModel::select(const AssetId& id) noexcept {
    selected_id_ = id;
}

void AssetBrowserViewModel::clear_selection() noexcept {
    selected_id_ = AssetId{};
}

const AssetBrowserViewModel::Entry* AssetBrowserViewModel::selected_entry() const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.id == selected_id_) {
            return &entry;
        }
    }
    return nullptr;
}

std::string AssetBrowserViewModel::selected_reference() const {
    const Entry* entry = selected_entry();
    return entry == nullptr ? std::string{} : to_reference(entry->id);
}

void AssetBrowserViewModel::apply_view() {
    visible_.clear();
    visible_.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        if (contains_case_insensitive(entry.path, filter_)) {
            visible_.push_back(entry);
        }
    }
    std::sort(visible_.begin(), visible_.end(),
              [](const Entry& a, const Entry& b) { return less_case_insensitive(a.path, b.path); });

    // A selection whose asset was filtered out or rescanned away stays in the id
    // so it can come back; the browser does not silently drop the user's choice.
    // selected_entry() reports the entry regardless of the filter, so the detail
    // view and the copyable reference remain available.
}

} // namespace fe::editor
