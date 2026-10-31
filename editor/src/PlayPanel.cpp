// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/PlayPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorWidgets.h"
#include "fe/editor/PlaySession.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace fe::editor {

void PlayPanel::draw(EditorContext& context) {
    const PlaySession& play = context.play();

    if (ImGui::Begin(name())) {
        // The transport, repeated here so the panel is usable on its own (the
        // toolbar is not the only place to start a play). A filled button marks
        // the primary action, matching the toolbar.
        const Rgba& accent = ui::palette().accent;
        if (play.is_playing()) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4{ui::palette().danger.r, ui::palette().danger.g, ui::palette().danger.b, 1.0f});
            if (ImGui::Button("Stop")) {
                context.request_stop_play();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ui::badge("PLAYING", ui::palette().success);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{accent.r, accent.g, accent.b, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ui::palette().window_bg.r, ui::palette().window_bg.g,
                                                        ui::palette().window_bg.b, 1.0f});
            if (ImGui::Button("Play")) {
                context.request_start_play();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ui::badge("Stopped", ui::palette().text_disabled);
        }

        ui::section_heading("Scene");
        ui::labelled("Name", context.scene().name());
        ui::labelled("Path", context.scene_path().empty() ? "(unsaved)" : context.scene_path());
        ui::labelled("Nodes", std::to_string(context.scene().node_count()));

        ui::section_heading("Play");
        ui::labelled("Frames", std::to_string(play.frames_played()));
        ui::labelled("Sprites", std::to_string(play.last_sprite_count()));
        // The play clock, formatted to two places.
        char time[32];
        std::snprintf(time, sizeof(time), "%.2f s", static_cast<double>(play.play_time()));
        ui::labelled("Time", time);

        ui::section_heading("Selection");
        if (context.hierarchy().has_selection()) {
            ui::labelled("Selected", context.hierarchy().selected_name());
        } else {
            ui::empty_hint("Nothing selected");
        }
    }
    ImGui::End();
}

} // namespace fe::editor
