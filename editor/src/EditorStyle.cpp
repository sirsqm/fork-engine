// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorStyle.h"

#include "fe/editor/EditorFontData.h"

#include "imgui.h"

#include <cstddef>

namespace fe::editor {
namespace {

[[nodiscard]] ImVec4 to_vec4(const Rgba& c) noexcept {
    return ImVec4{c.r, c.g, c.b, c.a};
}

// ImGui's colour slots take a packed u32, and the style stores ImVec4. Setting
// through the slot array keeps one code path for every colour.
void set(ImGuiCol slot, const Rgba& colour) noexcept {
    ImGui::GetStyle().Colors[slot] = to_vec4(colour);
}

// Adds one embedded face at `size`. The bytes are the embedded arrays, which
// outlive the atlas, so FontDataOwnedByAtlas is false and nothing is freed. The
// oversampling is left at ImGui's default (auto), which is what keeps the glyphs
// crisp at a non-integer size.
[[nodiscard]] ImFont* add_face(const fonts::EmbeddedFont& font, float size) noexcept {
    if (font.data == nullptr || font.size == 0) {
        return nullptr;
    }
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 2;
    config.OversampleV = 1;
    // A small rasterizer boost makes the light-on-dark glyphs read as the same
    // weight as dark-on-light; ImGui recommends it for a small font.
    config.RasterizerMultiply = 1.06f;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(font.data),
                                                      static_cast<int>(font.size), size, &config);
}

} // namespace

bool load_editor_fonts(EditorFonts& fonts, const EditorMetrics& metrics) {
    const auto& embedded = fonts::kEmbeddedFonts;
    if (fonts::kEmbeddedFontCount < 3) {
        return false;
    }

    fonts.regular = add_face(embedded[0], metrics.font_size);
    fonts.semibold = add_face(embedded[1], metrics.heading_size);
    fonts.mono = add_face(embedded[2], metrics.mono_font_size);

    // A single missing face is not fatal: the atlas keeps the ones that loaded
    // and ImGui falls back to the first for a null. The editor is still usable,
    // so this reports success when at least the regular face resolved.
    if (fonts.regular != nullptr) {
        ImGui::GetIO().FontDefault = fonts.regular;
        // The default size the style will use on the first frame. ImGui otherwise
        // defaults FontSizeBase to the face's baked size, which is the same value
        // here, but setting it explicitly keeps the two from drifting.
        ImGui::GetStyle().FontSizeBase = metrics.font_size;
    }
    if (fonts.semibold == nullptr) {
        fonts.semibold = fonts.regular;
    }
    if (fonts.mono == nullptr) {
        fonts.mono = fonts.regular;
    }
    fonts.loaded = fonts.regular != nullptr;
    return fonts.loaded;
}

bool rebuild_editor_fonts(EditorFonts& fonts, const EditorMetrics& metrics) {
    // A full Clear() drops every baked face and its texture. It is only legal
    // between frames, so the caller owns that ordering (see the header).
    ImGui::GetIO().Fonts->Clear();
    return load_editor_fonts(fonts, metrics);
}

void apply_editor_style(const EditorScheme& scheme) {
    const EditorPalette& p = scheme.palette;
    const EditorMetrics& m = scheme.metrics;
    ImGuiStyle& style = ImGui::GetStyle();

    // --- Shape -------------------------------------------------------------
    style.WindowRounding = m.window_rounding;
    style.ChildRounding = m.frame_rounding;
    style.PopupRounding = m.popup_rounding;
    style.FrameRounding = m.frame_rounding;
    style.GrabRounding = m.grab_rounding;
    style.ScrollbarRounding = m.scrollbar_rounding;
    style.TabRounding = m.tab_rounding;
    style.MenuItemRounding = m.frame_rounding;
    style.DragDropTargetRounding = m.frame_rounding;

    style.WindowBorderSize = m.window_border;
    style.ChildBorderSize = m.window_border;
    style.FrameBorderSize = m.frame_border;
    style.PopupBorderSize = m.popup_border;
    style.TabBarBorderSize = 0.0f;
    style.SeparatorSize = 1.0f;

    // --- Spacing -----------------------------------------------------------
    style.WindowPadding = ImVec2{m.window_padding_x, m.window_padding_y};
    style.FramePadding = ImVec2{m.frame_padding_x, m.frame_padding_y};
    style.ItemSpacing = ImVec2{m.item_spacing_x, m.item_spacing_y};
    style.ItemInnerSpacing = ImVec2{m.item_inner_spacing_x, m.item_inner_spacing_y};
    style.IndentSpacing = m.indent_spacing;
    style.CellPadding = ImVec2{m.frame_padding_x * 0.6f, m.frame_padding_y * 0.5f};
    style.ScrollbarSize = m.scrollbar_size;
    style.GrabMinSize = m.grab_min_size;
    style.DisabledAlpha = m.disabled_alpha;
    style.SeparatorTextPadding = ImVec2{m.window_padding_x, m.frame_padding_y * 0.5f};
    style.SeparatorTextBorderSize = 1.0f;

    // Tabs: a comfortable minimum so a two-letter tab is not a sliver, and the
    // overline is the accent bar that marks the selected tab.
    style.TabMinWidthBase = m.tab_min_width;
    style.TabBarOverlineSize = m.tab_overline;

    // Menus and buttons centre their text; a selectable stays left-aligned.
    style.ButtonTextAlign = ImVec2{0.5f, 0.5f};
    style.SelectableTextAlign = ImVec2{0.0f, 0.5f};
    style.WindowTitleAlign = ImVec2{0.0f, 0.5f};

    // --- Colour ------------------------------------------------------------
    // Every slot is set, including the ones a stock theme leaves at a default, so
    // switching themes never leaves a stale colour from the other scheme.
    set(ImGuiCol_Text, p.text);
    set(ImGuiCol_TextDisabled, p.text_disabled);
    set(ImGuiCol_WindowBg, p.window_bg);
    set(ImGuiCol_ChildBg, Rgba{0.0f, 0.0f, 0.0f, 0.0f});
    set(ImGuiCol_PopupBg, p.surface_raised);
    set(ImGuiCol_Border, p.border);
    set(ImGuiCol_BorderShadow, Rgba{0.0f, 0.0f, 0.0f, 0.0f});

    // Title bars are the panel chrome; the active one is the surface, the
    // inactive a shade toward the background so focus is legible.
    set(ImGuiCol_TitleBg, p.surface);
    set(ImGuiCol_TitleBgActive, p.surface);
    set(ImGuiCol_TitleBgCollapsed, p.surface);
    set(ImGuiCol_MenuBarBg, p.surface);

    set(ImGuiCol_ScrollbarBg, Rgba{0.0f, 0.0f, 0.0f, 0.0f});
    set(ImGuiCol_ScrollbarGrab, p.border_strong);
    set(ImGuiCol_ScrollbarGrabHovered, p.text_disabled);
    set(ImGuiCol_ScrollbarGrabActive, p.accent);

    // A framed control (input, combo, drag) is the raised surface, with the
    // accent showing only on the active edit.
    set(ImGuiCol_FrameBg, p.surface_raised);
    set(ImGuiCol_FrameBgHovered, p.surface_raised);
    set(ImGuiCol_FrameBgActive, p.surface_raised);

    set(ImGuiCol_CheckMark, p.accent);
    set(ImGuiCol_SliderGrab, p.accent);
    set(ImGuiCol_SliderGrabActive, p.accent_active);

    set(ImGuiCol_Button, p.surface_raised);
    set(ImGuiCol_ButtonHovered, p.border_strong);
    set(ImGuiCol_ButtonActive, p.accent_active);

    // Headers are rows: a tree node, a selectable, a collapsing header.
    set(ImGuiCol_Header, p.selection_bg);
    set(ImGuiCol_HeaderHovered, p.surface_raised);
    set(ImGuiCol_HeaderActive, p.selection_bg);

    set(ImGuiCol_Separator, p.border);
    set(ImGuiCol_SeparatorHovered, p.accent);
    set(ImGuiCol_SeparatorActive, p.accent_active);

    set(ImGuiCol_ResizeGrip, Rgba{0.0f, 0.0f, 0.0f, 0.0f});
    set(ImGuiCol_ResizeGripHovered, p.accent);
    set(ImGuiCol_ResizeGripActive, p.accent_active);

    // Tabs: an unselected tab is the body colour (it recedes into the panel), the
    // selected one is the raised surface with the accent overline.
    set(ImGuiCol_Tab, p.surface);
    set(ImGuiCol_TabHovered, p.surface_raised);
    set(ImGuiCol_TabSelected, p.surface_raised);
    set(ImGuiCol_TabSelectedOverline, p.accent);
    set(ImGuiCol_TabDimmed, p.surface);
    set(ImGuiCol_TabDimmedSelected, p.surface_raised);

    set(ImGuiCol_DockingPreview, p.accent.with_alpha(0.35f));
    set(ImGuiCol_DockingEmptyBg, p.surface_sunken);

    set(ImGuiCol_PlotLines, p.accent);
    set(ImGuiCol_PlotLinesHovered, p.accent_hovered);
    set(ImGuiCol_PlotHistogram, p.accent);
    set(ImGuiCol_PlotHistogramHovered, p.accent_hovered);

    set(ImGuiCol_TableHeaderBg, p.surface);
    set(ImGuiCol_TableBorderStrong, p.border_strong);
    set(ImGuiCol_TableBorderLight, p.border);
    set(ImGuiCol_TableRowBg, Rgba{0.0f, 0.0f, 0.0f, 0.0f});
    set(ImGuiCol_TableRowBgAlt, p.text.with_alpha(0.035f));

    set(ImGuiCol_TextLink, p.accent);
    set(ImGuiCol_TextSelectedBg, p.selection_bg);
    set(ImGuiCol_DragDropTarget, p.accent);
    set(ImGuiCol_NavCursor, p.accent);
    set(ImGuiCol_ModalWindowDimBg, Rgba{0.0f, 0.0f, 0.0f, 0.55f});
}

} // namespace fe::editor
