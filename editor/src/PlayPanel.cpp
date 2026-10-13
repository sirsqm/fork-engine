// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/PlayPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/PlaySession.h"

#include "imgui.h"

namespace fe::editor {

void PlayPanel::draw(EditorContext& context) {
    const PlaySession& play = context.play();

    if (ImGui::Begin(name())) {
        if (play.is_playing()) {
            if (ImGui::Button("Stop")) {
                context.request_stop_play();
            }
        } else {
            if (ImGui::Button("Play")) {
                context.request_start_play();
            }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(play.is_playing() ? "Playing" : "Stopped");

        ImGui::SeparatorText("Scene");
        ImGui::Text("Name: %s", context.scene().name().c_str());
        ImGui::Text("Nodes: %zu", context.scene().node_count());
        ImGui::Text("Path: %s", context.scene_path().empty() ? "(unsaved)" : context.scene_path().c_str());

        ImGui::SeparatorText("Play");
        ImGui::Text("Frames: %u", play.frames_played());
        ImGui::Text("Sprites: %zu", play.last_sprite_count());
        ImGui::Text("Time: %.2fs", play.play_time());

        if (context.hierarchy().has_selection()) {
            ImGui::SeparatorText("Selection");
            ImGui::Text("Selected: %s", context.hierarchy().selected_name().c_str());
        } else {
            ImGui::SeparatorText("Selection");
            ImGui::TextDisabled("Nothing selected");
        }
    }
    ImGui::End();
}

} // namespace fe::editor
