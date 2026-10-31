// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ProfilerPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorWidgets.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace fe::editor {
namespace {

// A thin horizontal bar, drawn under a scope row, whose width is `fraction` of
// the available width. A bar is what makes the profiler's numbers scannable: the
// eye finds the widest one without reading every figure. The accent hue is used
// for the largest cost so the single worst scope stands out.
void share_bar(f32 fraction, bool emphasise) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 4.0f;
    const Rgba colour = emphasise ? ui::palette().accent : ui::palette().border_strong;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2{pos.x + width, pos.y + height},
                        ImGui::GetColorU32(ImVec4{ui::palette().surface_sunken.r, ui::palette().surface_sunken.g,
                                                  ui::palette().surface_sunken.b, 1.0f}),
                        height * 0.5f);
    draw->AddRectFilled(pos, ImVec2{pos.x + width * fraction, pos.y + height},
                        ImGui::GetColorU32(ImVec4{colour.r, colour.g, colour.b, 1.0f}), height * 0.5f);
    ImGui::Dummy(ImVec2{0.0f, height});
}

} // namespace

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
        ui::badge(paused_ ? "paused" : "live", paused_ ? ui::palette().text_disabled : ui::palette().success);

        if (profiler == nullptr) {
            ui::section_heading("Frame");
            ui::empty_hint("No profiler attached");
        } else if (!view_model_.has_frame()) {
            ui::section_heading("Frame");
            ui::empty_hint("Waiting for the first completed frame");
        } else {
            // The frame budget summary: the three phases as label/value rows, so
            // the numbers line up as a table rather than as free text.
            ui::section_heading("Frame");
            char total[32];
            char update[32];
            char render[32];
            std::snprintf(total, sizeof(total), "%6.3f ms", view_model_.total_ms());
            std::snprintf(update, sizeof(update), "%6.3f ms", view_model_.update_ms());
            std::snprintf(render, sizeof(render), "%6.3f ms", view_model_.render_ms());
            ui::labelled("Total", total, 72.0f);
            ui::labelled("Update", update, 72.0f);
            ui::labelled("Render", render, 72.0f);

            if (view_model_.truncated()) {
                ImGui::TextColored(ImVec4{ui::palette().danger.r, ui::palette().danger.g, ui::palette().danger.b, 1.0f},
                                   "%u scope name(s) dropped", view_model_.dropped_names());
            }

            const f64 frame_total = view_model_.total_ms();
            ui::section_heading("Scopes");
            if (view_model_.scopes().empty()) {
                ui::empty_hint("None recorded");
            }
            // The largest scope, so its bar is emphasised.
            f64 widest = 0.0;
            for (const ProfilerPanelViewModel::ScopeRow& row : view_model_.scopes()) {
                widest = row.total_ms > widest ? row.total_ms : widest;
            }
            for (const ProfilerPanelViewModel::ScopeRow& row : view_model_.scopes()) {
                const f64 share = frame_total > 0.0 ? (row.total_ms / frame_total) * 100.0 : 0.0;
                const f32 bar = frame_total > 0.0 ? static_cast<f32>(row.total_ms / frame_total) : 0.0f;
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::SameLine(200.0f);
                ImGui::TextDisabled("%7.3f ms  %5.1f%%  x%u", row.total_ms, share, row.count);
                share_bar(bar, row.total_ms >= widest);
            }

            ui::section_heading("Counters");
            if (view_model_.counters().empty()) {
                ui::empty_hint("None recorded");
            }
            for (const ProfilerPanelViewModel::CounterRow& row : view_model_.counters()) {
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::SameLine(200.0f);
                ImGui::TextDisabled("%lld", static_cast<long long>(row.value));
            }
        }
    }
    ImGui::End();
}

} // namespace fe::editor
