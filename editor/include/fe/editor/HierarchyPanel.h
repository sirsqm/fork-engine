// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Uuid.h"
#include "fe/editor/Panel.h"

#include <array>

namespace fe {

class Node;

namespace editor {

// The scene hierarchy panel: a tree of the scene's nodes, and the editor's
// structural editing surface (add, rename, delete, reparent).
//
// The panel is a thin projection of `HierarchyViewModel`: the view model decides
// which rows are visible, expanded, and selected, and this class only emits
// widgets for them. That split is what makes the hierarchy's behaviour testable
// without a window (see `HierarchyViewModel`) and keeps the ImGui code free of
// tree logic.
//
// Every edit is expressed as a `Command` and handed to `EditorContext::execute`,
// so all of them are undoable and none mutates the scene directly
// (docs/adr/0011-editor-undo-redo.md). Editing is refused while play is running.
//
// It reads the scene through the public `Scene`/`Node` API only. No reflection,
// no private access.
class HierarchyPanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Hierarchy"; }

    void draw(EditorContext& context) override;

private:
    // Draws the context menu for the row of `node` (add child, rename, delete).
    void draw_row_context_menu(EditorContext& context, Node& node);

    // Handles drag-and-drop reparenting: a row is both a drag source and a drop
    // target, and a drop makes the dragged node a child of the dropped-on node.
    void draw_row_drag_drop(EditorContext& context, Node& node);

    // Commits the in-progress rename, if any, as a RenameNodeCommand.
    void commit_rename(EditorContext& context);

    // Begins an inline rename of `node`.
    void begin_rename(const Node& node);

    std::array<char, 128> filter_buffer_{};

    // Inline rename state. The node is referenced by id, so a scene change
    // between frames cannot leave a dangling pointer.
    bool renaming_ = false;
    Uuid rename_id_{};
    std::array<char, 128> rename_buffer_{};
};

} // namespace editor
} // namespace fe
