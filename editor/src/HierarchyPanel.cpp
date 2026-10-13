// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/HierarchyPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/HierarchyViewModel.h"
#include "fe/scene/Node.h"

#include "imgui.h"

#include <cstring>
#include <string>

namespace fe::editor {

void HierarchyPanel::draw(EditorContext& context) {
    HierarchyViewModel& model = context.hierarchy();

    if (ImGui::Begin(name())) {
        // Keep the view model in step with the scene. The editor refreshes it
        // after structural changes, but a rebuild here guarantees that a panel
        // opened at any time shows the current tree without the caller having
        // remembered to refresh.
        model.rebuild(context.scene());

        if (ImGui::InputTextWithHint("##filter", "Search nodes", filter_buffer_.data(), filter_buffer_.size())) {
            model.set_filter(std::string(filter_buffer_.data()));
        }

        ImGui::Separator();

        for (const HierarchyViewModel::Row& row : model.rows()) {
            Node* node = row.node;
            if (node == nullptr) {
                continue;
            }

            ImGui::PushID(node);

            // Indent by depth; ImGui's tree nodes would double-indent, so the
            // expander arrow is a separate leaf control.
            if (row.depth > 0) {
                ImGui::Indent(static_cast<f32>(row.depth) * 12.0f);
            }

            if (row.has_children) {
                ImGui::SetNextItemOpen(row.expanded, ImGuiCond_Always);
                if (ImGui::ArrowButton("##expand", row.expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
                    model.toggle_expanded(node->id());
                }
                ImGui::SameLine();
            }

            const bool selected = model.is_selected(*node);
            if (ImGui::Selectable(node->name().c_str(), selected)) {
                model.select(node);
            }

            if (row.depth > 0) {
                ImGui::Unindent(static_cast<f32>(row.depth) * 12.0f);
            }

            // A tooltip with the node's type and component count is the closest
            // thing to detail this milestone offers: the property inspector is
            // deferred with reflection (ADR-0005).
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s (%zu components)", node->type_name(), node->component_count());
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        if (model.rows().empty()) {
            ImGui::TextDisabled("No nodes");
        }
    }
    ImGui::End();
}

} // namespace fe::editor
