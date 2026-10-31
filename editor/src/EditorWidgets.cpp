// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorWidgets.h"

#include "fe/editor/EditorTheme.h"

#include "imgui.h"

namespace fe::editor::ui {
namespace {

// The scheme the helpers draw with. Set by EditorUi when it applies one; the dark
// default means a helper called before the first apply still draws.
EditorPalette g_palette = EditorPalette::dark();
EditorMetrics g_metrics = EditorMetrics::for_density(EditorDensity::Comfortable);

[[nodiscard]] ImVec4 to_vec4(const Rgba& c) noexcept {
    return ImVec4{c.r, c.g, c.b, c.a};
}

} // namespace

void set_active_palette(const EditorPalette& palette) {
    g_palette = palette;
}

const EditorPalette& palette() {
    return g_palette;
}

void set_active_metrics(const EditorMetrics& metrics) {
    g_metrics = metrics;
}

const EditorMetrics& metrics() {
    return g_metrics;
}

void labelled(std::string_view label, std::string_view value, float label_column) {
    ImGui::TextDisabled("%.*s", static_cast<int>(label.size()), label.data());
    ImGui::SameLine(label_column);
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

void section_heading(std::string_view title) {
    ImGui::Dummy(ImVec2{0.0f, 4.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, to_vec4(g_palette.text_muted));
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2{0.0f, 2.0f});
}

void badge(std::string_view text, const Rgba& colour) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 text_size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = text_size.y + pad.y;
    const float width = text_size.x + pad.x * 2.0f;
    // The label is the background colour, so it stays legible on any accent.
    const ImU32 fg = ImGui::GetColorU32(to_vec4(g_palette.window_bg));
    draw->AddRectFilled(pos, ImVec2{pos.x + width, pos.y + height}, ImGui::GetColorU32(to_vec4(colour)), height * 0.5f);
    draw->AddText(ImVec2{pos.x + pad.x, pos.y + pad.y * 0.5f}, fg, text.data(), text.data() + text.size());
    ImGui::SetCursorScreenPos(ImVec2{pos.x + width, pos.y});
}

void empty_hint(std::string_view text) {
    const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > size.x) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size.x) * 0.5f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, to_vec4(g_palette.text_disabled));
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

} // namespace fe::editor::ui
