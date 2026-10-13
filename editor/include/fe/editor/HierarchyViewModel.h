// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fe::editor {

// Pure, headless view model for the scene hierarchy panel.
//
// The panel's rendering is a thin projection of this type: the view model
// decides which rows exist, which are expanded, and which are selected, and the
// ImGui code only draws them. Keeping the decision logic here means the
// hierarchy behaves the same and is testable without a window or a graphics
// context.
//
// It depends only on the public `Scene`/`Node` API (identity, name, children,
// type_name, component count). It never mutates the scene: selection and
// expansion are view state, not scene state. That keeps the editor a read-only
// client of the data model until edit commands (a later milestone) are added.
class HierarchyViewModel {
public:
    // One visible row, in depth-first pre-order. `depth` is the node's distance
    // from the scene root, used to indent the row.
    struct Row {
        Node* node = nullptr;
        u32 depth = 0;
        bool has_children = false;
        bool expanded = false;
        bool selected = false;
    };

    HierarchyViewModel() = default;

    // Rebuilds the visible rows from `scene`. A node's children are included
    // only when the node is expanded, so this is what makes the tree fold.
    //
    // Selection is stored by Uuid rather than by pointer so it survives the
    // flattening and a scene reload: an id that no longer exists simply selects
    // nothing until the matching node is found again.
    void rebuild(const Scene& scene);

    [[nodiscard]] const std::vector<Row>& rows() const noexcept { return rows_; }
    [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }

    // --- Selection ---------------------------------------------------------
    void select(Node* node) noexcept;
    void clear_selection() noexcept;
    [[nodiscard]] bool is_selected(const Node& node) const noexcept;
    [[nodiscard]] const Uuid& selected_id() const noexcept { return selected_id_; }
    [[nodiscard]] bool has_selection() const noexcept { return !selected_id_.is_nil(); }

    // The selected node's name, or "" when nothing is selected. The panel shows
    // this as its summary; tests assert on it without touching ImGui.
    [[nodiscard]] std::string selected_name() const noexcept;

    [[nodiscard]] Node* selected_node() const noexcept { return selected_node_; }

    // --- Expansion ---------------------------------------------------------
    void set_expanded(const Uuid& id, bool expanded);
    void toggle_expanded(const Uuid& id);
    [[nodiscard]] bool is_expanded(const Uuid& id) const noexcept;

    // --- Search ------------------------------------------------------------
    // Filters rows by a case-insensitive substring of the node name. An empty
    // filter shows everything. A filtered row is visible without its ancestors,
    // which keeps a deep match reachable without expanding the whole tree.
    void set_filter(std::string filter);
    [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

private:
    void visit(const Node& node, u32 depth);

    std::vector<Row> rows_;
    // Nodes default to expanded, so only the ones the user folded away need
    // recording. This keeps a freshly loaded scene fully visible.
    std::unordered_set<Uuid> collapsed_;
    std::string filter_;
    Uuid selected_id_{};
    Node* selected_node_ = nullptr;
};

} // namespace fe::editor
