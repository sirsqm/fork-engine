// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ConfirmPanel.h"

#include "fe/editor/EditorContext.h"

#include "imgui.h"

namespace fe::editor {

void ConfirmPanel::draw(EditorContext& context) {
    if (!context.is_confirming()) {
        return;
    }

    constexpr const char* kPopupId = "##fork_confirm";
    if (!ImGui::IsPopupOpen(kPopupId, ImGuiPopupFlags_None)) {
        const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup(kPopupId);
    }

    if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted(context.confirm_message().c_str());
    ImGui::Separator();

    // Enter confirms, Escape cancels, which is what a keyboard user expects of a
    // destructive prompt. The buttons carry the same meaning for a mouse user.
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if (ImGui::Button("Discard", ImVec2(120.0f, 0.0f)) || enter) {
        context.request_file(FileRequest::ConfirmDiscard);
        context.clear_confirm();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || escape) {
        context.request_file(FileRequest::CancelDiscard);
        context.clear_confirm();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace fe::editor
