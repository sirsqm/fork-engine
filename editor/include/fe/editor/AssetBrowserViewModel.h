// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/resources/AssetId.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

class ResourceManager;

namespace editor {

// Pure, headless view model for the asset browser panel.
//
// The panel's rendering is a thin projection of this type, exactly as the
// hierarchy panel is a projection of `HierarchyViewModel`: the view model
// decides which assets are listed and how they are sorted and labelled, and the
// ImGui code only draws them. That split is what makes the browser's behaviour
// testable without a window or a GPU.
//
// It is a read-only client of the resource system: it lists what the
// `AssetDatabase` has indexed (the sidecars that survived the last scan) and
// formats GUID references. It never imports, moves, or deletes anything. The
// GUIDs come from the database, so a file move is reflected on the next rescan
// without the browser knowing about paths.
class AssetBrowserViewModel {
public:
    // One listed asset. `path` is relative to the asset root and uses forward
    // slashes (the database's convention); `name` is the file name; `directory`
    // is the containing folder, or "." for a root-level asset.
    struct Entry {
        AssetId id;
        std::string path;
        std::string name;
        std::string directory;
        std::string scheme;
    };

    AssetBrowserViewModel() = default;

    // Rebuilds the listing from `resources`' asset database. A null manager, a
    // manager with no database root, or an empty database all produce an empty
    // listing rather than an error: "no assets yet" is a normal editor state.
    void rebuild(const ResourceManager* resources);

    // Rebuilds the listing from an explicit set of entries. Used by tests and by
    // callers that already have the assets in hand; it does not touch a
    // ResourceManager.
    void set_entries(std::vector<Entry> entries);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] usize entry_count() const noexcept { return entries_.size(); }

    // --- Filtering ---------------------------------------------------------
    // Filters by a case-insensitive substring of the asset's path. An empty
    // filter shows everything. The full listing is retained, so changing the
    // filter never needs a rescan.
    void set_filter(std::string filter);
    [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

    // The entries that pass the current filter, in sorted order.
    [[nodiscard]] const std::vector<Entry>& visible_entries() const noexcept { return visible_; }

    // --- Selection ---------------------------------------------------------
    // Selection is stored by id, not by pointer, so it survives a rescan.
    void select(const AssetId& id) noexcept;
    void clear_selection() noexcept;
    [[nodiscard]] bool has_selection() const noexcept { return !selected_id_.is_nil(); }
    [[nodiscard]] const AssetId& selected_id() const noexcept { return selected_id_; }

    // The selected entry, or nullptr when nothing is selected or the selected
    // asset is no longer in the database. Filtering does not clear the
    // selection, so the detail view and the copyable reference stay available
    // while a filter hides other assets.
    [[nodiscard]] const Entry* selected_entry() const noexcept;

    // The selected asset's stable reference ("guid:<uuid>"), or "" when nothing
    // is selected. This is the string the browser offers for drag-and-drop and
    // the inspector.
    [[nodiscard]] std::string selected_reference() const;

private:
    // Recomputes `visible_` from `entries_`, `filter_`, and the sort order, and
    // drops a selection whose asset is gone.
    void apply_view();

    std::vector<Entry> entries_;
    std::vector<Entry> visible_;
    std::string filter_;
    AssetId selected_id_{};
};

} // namespace editor
} // namespace fe
