// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ProfilerPanel.h"

#include "fe/editor/EditorContext.h"

#include "imgui.h"

namespace fe::editor {

void ProfilerPanel::draw(EditorContext& context) {
    const Profiler* profiler = context.profiler();

    if (ImGui::Begin(name())) {
        // Capture the latest frame unless the user paused the view. Reading is
        // the view model's job; the panel only decides whether to read.
        if (!paused_ && profiler != nullptr) {
            view_model_.capture(*profiler);
        }

        if (ImGui::Button(paused_ ? "Resume" : "Pause")) {
            paused_ = !paused_;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset view")) {
            view_model_.clear();
            paused_ = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("captures: %llu", static_cast<unsigned long long>(view_model_.capture_count()));

        if (profiler == nullptr) {
            ImGui::SeparatorText("Frame");
            ImGui::TextDisabled("No profiler attached");
        } else if (!view_model_.has_frame()) {
            ImGui::SeparatorText("Frame");
            ImGui::TextDisabled("Waiting for the first completed frame");
        } else {
            ImGui::SeparatorText("Frame");
            ImGui::Text("Total:  %6.3f ms", view_model_.total_ms());
            ImGui::Text("Update: %6.3f ms", view_model_.update_ms());
            ImGui::Text("Render: %6.3f ms", view_model_.render_ms());

            if (view_model_.truncated()) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%u scope name(s) dropped",
                                   view_model_.dropped_names());
            }

            const f64 total = view_model_.total_ms();
            ImGui::SeparatorText("Scopes");
            if (view_model_.scopes().empty()) {
                ImGui::TextDisabled("None recorded");
            }
            for (const ProfilerPanelViewModel::ScopeRow& row : view_model_.scopes()) {
                const f64 share = total > 0.0 ? (row.total_ms / total) * 100.0 : 0.0;
                ImGui::Text("%s", row.name.c_str());
                ImGui::SameLine(220.0f);
                ImGui::Text("%7.3f ms  %5.1f%%  x%u", row.total_ms, share, row.count);
            }

            ImGui::SeparatorText("Counters");
            if (view_model_.counters().empty()) {
                ImGui::TextDisabled("None recorded");
            }
            for (const ProfilerPanelViewModel::CounterRow& row : view_model_.counters()) {
                ImGui::Text("%s", row.name.c_str());
                ImGui::SameLine(220.0f);
                ImGui::Text("%lld", static_cast<long long>(row.value));
            }
        }
    }
    ImGui::End();
}

} // namespace fe::editor
