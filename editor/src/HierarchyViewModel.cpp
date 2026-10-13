// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/HierarchyViewModel.h"

#include <algorithm>
#include <cctype>

namespace fe::editor {
namespace {

[[nodiscard]] char to_lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool contains_case_insensitive(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](char a, char b) { return to_lower(a) == to_lower(b); });
    return it != haystack.end();
}

} // namespace

void HierarchyViewModel::rebuild(const Scene& scene) {
    rows_.clear();

    // Selection and expansion are keyed by identity, so re-resolving the
    // selected pointer against the current scene keeps it valid across a scene
    // reload without storing a dangling pointer. If the node is gone, the
    // selection is dropped rather than left pointing at a freed node.
    if (has_selection()) {
        selected_node_ = scene.find_node(selected_id_);
        if (selected_node_ == nullptr) {
            selected_id_ = Uuid{};
        }
    } else {
        selected_node_ = nullptr;
    }

    for (const auto& root : scene.roots()) {
        visit(*root, 0);
    }
}

void HierarchyViewModel::visit(const Node& node, u32 depth) {
    const bool filtering = !filter_.empty();
    const bool name_matches = contains_case_insensitive(node.name(), filter_);

    // While filtering, a node is shown when its name matches. Ancestors of a
    // match are not forced into view; the match itself is enough to reach it.
    if (!filtering || name_matches) {
        Row row;
        row.node = const_cast<Node*>(&node);
        row.depth = depth;
        row.has_children = node.child_count() > 0;
        row.expanded = is_expanded(node.id());
        row.selected = node.id() == selected_id_;
        rows_.push_back(row);
    }

    // Descend when the node is expanded, or unconditionally while filtering
    // (a match can occur anywhere below).
    if (filtering || is_expanded(node.id())) {
        for (const auto& child : node.children()) {
            visit(*child, depth + 1);
        }
    }
}

void HierarchyViewModel::select(Node* node) noexcept {
    if (node == nullptr) {
        clear_selection();
        return;
    }
    selected_id_ = node->id();
    selected_node_ = node;
}

void HierarchyViewModel::clear_selection() noexcept {
    selected_id_ = Uuid{};
    selected_node_ = nullptr;
}

bool HierarchyViewModel::is_selected(const Node& node) const noexcept {
    return !selected_id_.is_nil() && node.id() == selected_id_;
}

std::string HierarchyViewModel::selected_name() const noexcept {
    return selected_node_ != nullptr ? selected_node_->name() : std::string{};
}

void HierarchyViewModel::set_expanded(const Uuid& id, bool expanded) {
    if (expanded) {
        collapsed_.erase(id);
    } else {
        collapsed_.insert(id);
    }
}

void HierarchyViewModel::toggle_expanded(const Uuid& id) {
    set_expanded(id, !is_expanded(id));
}

bool HierarchyViewModel::is_expanded(const Uuid& id) const noexcept {
    // Nodes default to expanded so a freshly loaded scene shows its top level
    // immediately; the collapsed set records only what the user folded away.
    if (collapsed_.contains(id)) {
        return false;
    }
    return true;
}

void HierarchyViewModel::set_filter(std::string filter) {
    filter_ = std::move(filter);
}

} // namespace fe::editor
