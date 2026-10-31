// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ConsolePanel.h"

#include "fe/editor/EditorContext.h"

#include "imgui.h"

namespace fe::editor {

void ConsolePanel::draw(EditorContext& context) {
    if (ImGui::Begin(name())) {
        // Poll the two sources each frame. Polling copies only what is new, so a
        // quiet frame costs a comparison; the script host's diagnostics are read
        // through the editor's script catalog, which owns the host.
        const LogBuffer& log = context.log_buffer();
        view_model_.poll(log);
        view_model_.poll(context.scripts().host().diagnostics());

        if (ImGui::Button("Clear")) {
            view_model_.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &auto_scroll_);

        // Level filter. A combo rather than a slider so the choice is a name.
        static const LogLevel kLevels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                                           LogLevel::Warn,  LogLevel::Error, LogLevel::Critical};
        static const char* kLevelNames[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical"};
        int current = 2;
        for (int i = 0; i < 6; ++i) {
            if (kLevels[i] == view_model_.filter()) {
                current = i;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("Level", &current, kLevelNames, 6)) {
            view_model_.set_filter(kLevels[current]);
        }

        if (view_model_.log_truncated()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(%llu record(s) dropped)",
                               static_cast<unsigned long long>(view_model_.log_dropped()));
        }
        if (view_model_.has_hidden()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu hidden)", view_model_.hidden_count());
        }

        ImGui::Separator();

        if (ImGui::BeginChild("##console_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            const std::vector<ConsolePanelViewModel::Entry> rows = view_model_.visible();
            for (const ConsolePanelViewModel::Entry& row : rows) {
                const char* level_name = to_string(row.level);
                ImVec4 colour(0.8f, 0.8f, 0.8f, 1.0f);
                if (row.script_failure) {
                    colour = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                } else if (row.level >= LogLevel::Error) {
                    colour = ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
                } else if (row.level == LogLevel::Warn) {
                    colour = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                ImGui::TextUnformatted(level_name);
                ImGui::SameLine();
                ImGui::TextUnformatted(row.message.c_str());
                ImGui::PopStyleColor();
            }

            if (auto_scroll_ && !rows.empty() && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace fe::editor
