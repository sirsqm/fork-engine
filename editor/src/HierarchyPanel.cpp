// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/HierarchyPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorWidgets.h"
#include "fe/editor/HierarchyViewModel.h"
#include "fe/editor/OperationCommand.h"
#include "fe/scene/Node.h"
#include "fe/serialization/Operation.h"

#include "imgui.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace fe::editor {
namespace {

// Builds an operation and pushes it onto the undo stack through the adapter, so
// every hierarchy edit is the serializable operation a log would carry
// (docs/adr/0047-local-operation-model.md).
bool commit_operation(EditorContext& context, serialization::Operation op) {
    return context.execute(std::make_unique<OperationCommand>(std::move(op)));
}

// The payload type for a hierarchy drag: the dragged node's id as its canonical
// string. An id (not a pointer) survives the frames between drag start and drop.
constexpr const char* kNodeDragPayload = "FE_NODE_ID";

} // namespace

void HierarchyPanel::begin_rename(const Node& node) {
    renaming_ = true;
    rename_id_ = node.id();
    rename_buffer_.fill('\0');
    const std::string& name = node.name();
    const usize length = name.size() < rename_buffer_.size() - 1 ? name.size() : rename_buffer_.size() - 1;
    std::memcpy(rename_buffer_.data(), name.data(), length);
}

void HierarchyPanel::commit_rename(EditorContext& context) {
    if (!renaming_) {
        return;
    }
    renaming_ = false;

    Node* node = context.scene().find_node(rename_id_);
    rename_id_ = Uuid{};
    if (node == nullptr) {
        return;
    }
    const std::string new_name = rename_buffer_.data();
    // An empty or unchanged name is not an edit; skip it so it does not pollute
    // the undo history.
    if (new_name.empty() || new_name == node->name()) {
        return;
    }
    serialization::Operation op;
    op.kind = serialization::OperationKind::RenameNode;
    op.node_id = node->id();
    op.name = new_name;
    commit_operation(context, std::move(op));
}

void HierarchyPanel::draw_row_context_menu(EditorContext& context, Node& node) {
    if (ImGui::BeginPopupContextItem("##row_menu")) {
        const bool editable = context.is_editable();

        if (ImGui::MenuItem("Add Child Node", nullptr, false, editable)) {
            const Uuid node_id = Uuid::generate();
            serialization::Operation op;
            op.kind = serialization::OperationKind::CreateNode;
            op.node_id = node_id;
            op.parent_id = node.id();
            op.index = node.child_count();
            op.name = "Node";
            if (commit_operation(context, std::move(op))) {
                // Select the new node so the user can immediately rename it.
                if (Node* created = context.scene().find_node(node_id); created != nullptr) {
                    context.hierarchy().select(created);
                }
            }
        }
        if (ImGui::MenuItem("Rename", "F2", false, editable)) {
            begin_rename(node);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del", false, editable)) {
            serialization::Operation op;
            op.kind = serialization::OperationKind::DeleteNode;
            op.node_id = node.id();
            commit_operation(context, std::move(op));
        }
        ImGui::EndPopup();
    }
}

void HierarchyPanel::draw_row_drag_drop(EditorContext& context, Node& node) {
    if (!context.is_editable()) {
        return;
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const std::string id = node.id().to_string();
        ImGui::SetDragDropPayload(kNodeDragPayload, id.data(), id.size());
        ImGui::TextUnformatted(node.name().c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodeDragPayload)) {
            const std::string text(static_cast<const char*>(payload->Data), static_cast<usize>(payload->DataSize));
            Uuid dragged_id{};
            if (Uuid::try_parse(text, dragged_id)) {
                Node* dragged = context.scene().find_node(dragged_id);
                // Refuse a drop onto itself or into its own subtree: that would
                // create a cycle. The engine's add_child would assert, so this
                // is a guard, not an optimisation.
                if (dragged != nullptr && dragged != &node && !dragged->is_ancestor_of(node)) {
                    serialization::Operation op;
                    op.kind = serialization::OperationKind::ReparentNode;
                    op.node_id = dragged->id();
                    op.parent_id = node.id();
                    op.index = node.child_count();
                    commit_operation(context, std::move(op));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void HierarchyPanel::draw(EditorContext& context) {
    HierarchyViewModel& model = context.hierarchy();

    if (ImGui::Begin(name())) {
        model.rebuild(context.scene());

        // The filter gets a full-width field with a hint, and the node count sits
        // beside it, so the panel says how much it is showing.
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##filter", "Search nodes", filter_buffer_.data(), filter_buffer_.size())) {
            model.set_filter(std::string(filter_buffer_.data()));
        }

        // Delete removes the selection. Handled at panel level so it works
        // whether or not a row is hovered. Refused while a text field has focus
        // (renaming) so typing "delete" does not delete the node.
        if (context.is_editable() && !renaming_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (Node* selected = model.selected_node(); selected != nullptr) {
                serialization::Operation op;
                op.kind = serialization::OperationKind::DeleteNode;
                op.node_id = selected->id();
                commit_operation(context, std::move(op));
            }
        }

        for (const HierarchyViewModel::Row& row : model.rows()) {
            Node* node = row.node;
            if (node == nullptr) {
                continue;
            }

            ImGui::PushID(node);

            if (row.depth > 0) {
                ImGui::Indent(static_cast<f32>(row.depth) * ui::metrics().indent_spacing);
            }

            if (row.has_children) {
                ImGui::SetNextItemOpen(row.expanded, ImGuiCond_Always);
                if (ImGui::ArrowButton("##expand", row.expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
                    model.toggle_expanded(node->id());
                }
                ImGui::SameLine();
            }

            const bool selected = model.is_selected(*node);
            if (renaming_ && node->id() == rename_id_) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##rename", rename_buffer_.data(), rename_buffer_.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    commit_rename(context);
                }
            } else {
                // A row is a full-width selectable, so the selection highlight
                // spans the row rather than hugging the label.
                if (ImGui::Selectable(node->name().c_str(), selected, ImGuiSelectableFlags_None)) {
                    model.select(node);
                }
                // The view model rebuilt this frame, so `node` is still valid;
                // start a rename from the menu or F2 once the row is selected.
                if (selected && context.is_editable() && !renaming_ && ImGui::IsKeyPressed(ImGuiKey_F2)) {
                    begin_rename(*node);
                }
                draw_row_drag_drop(context, *node);
                draw_row_context_menu(context, *node);
            }

            if (row.depth > 0) {
                ImGui::Unindent(static_cast<f32>(row.depth) * ui::metrics().indent_spacing);
            }

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(node->name().c_str());
                ImGui::Separator();
                ImGui::TextDisabled("%s", node->type_name());
                ImGui::TextDisabled("%zu component%s", node->component_count(),
                                    node->component_count() == 1 ? "" : "s");
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        if (model.rows().empty()) {
            ui::empty_hint(model.has_filter() ? "No matching nodes" : "No nodes in this scene");
            ImGui::Dummy(ImVec2{0.0f, 2.0f});
            ImGui::TextDisabled("Right-click to add a node");
        }

        // Right-clicking empty space adds a scene root.
        if (ImGui::BeginPopupContextWindow("##empty_menu", ImGuiPopupFlags_NoOpenOverItems) && context.is_editable()) {
            if (ImGui::MenuItem("Add Root Node")) {
                serialization::Operation op;
                op.kind = serialization::OperationKind::CreateNode;
                op.node_id = Uuid::generate();
                op.index = context.scene().root_count();
                op.name = "Node";
                commit_operation(context, std::move(op));
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

} // namespace fe::editor
