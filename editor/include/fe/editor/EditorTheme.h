// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/editor/EditorPalette.h"

namespace fe::editor {

// The editor's colour scheme.
//
// Dark is the default, the convention for a tool that shows rendered scenes (a
// bright chrome around a viewport skews how the content reads). Light is for a
// bright room or a projector. The choice is view state, not a property of the
// document, so it is not persisted in the project file.
//
// It lives in its own header rather than in EditorContext.h so the UI toolkit
// (EditorUi) can name the type without including the whole document - the same
// split that keeps the panels a thin projection of the model.
//
// It also picks the window icon's ink: the mark is drawn white on the dark
// chrome and black on the light, so it stays legible in the title bar.
enum class EditorTheme {
    Dark,
    Light,
};

// How tightly the UI is packed. A dense layout fits more rows on screen (a large
// hierarchy, a long profiler list); a comfortable one is easier to hit with a
// pointer and easier to read. This is view state too: it is a property of the
// person, not the project.
enum class EditorDensity {
    Compact,
    Comfortable,
};

// The measurements a scheme draws with, in logical pixels. Kept beside the
// palette because the two together are "the look", and both derive from the
// theme and the density rather than being scattered as literals in the panels.
struct EditorMetrics {
    // The UI font size. The body size; headings are this scaled up.
    f32 font_size = 15.0f;
    // The size of the monospaced font, a little smaller so a path or a number
    // column does not shout.
    f32 mono_font_size = 13.5f;
    // The size of a section heading (a panel's "Transform", a menu title).
    f32 heading_size = 16.5f;

    // Corner radii. A modern tool is softly rounded, not square.
    f32 window_rounding = 8.0f;
    f32 frame_rounding = 6.0f;
    f32 popup_rounding = 8.0f;
    f32 tab_rounding = 6.0f;
    f32 scrollbar_rounding = 8.0f;
    f32 grab_rounding = 8.0f;

    // Padding inside a window (panel body) and inside a framed control.
    f32 window_padding_x = 12.0f;
    f32 window_padding_y = 10.0f;
    f32 frame_padding_x = 10.0f;
    f32 frame_padding_y = 6.0f;

    // Gaps between items and within a composite item.
    f32 item_spacing_x = 8.0f;
    f32 item_spacing_y = 7.0f;
    f32 item_inner_spacing_x = 8.0f;
    f32 item_inner_spacing_y = 5.0f;

    // The indent per hierarchy depth level.
    f32 indent_spacing = 18.0f;

    // Borders. A hairline window border separates a panel from the background
    // without a heavy frame; frames carry no border of their own.
    f32 window_border = 1.0f;
    f32 frame_border = 0.0f;
    f32 popup_border = 1.0f;

    // The height of the main menu bar, driven by its frame padding.
    f32 menu_bar_padding_y = 7.0f;

    // Scrollbar and grab sizes.
    f32 scrollbar_size = 12.0f;
    f32 grab_min_size = 14.0f;

    // Tab bar: a minimum tab width and the height of the accent overline that
    // marks the selected tab.
    f32 tab_min_width = 68.0f;
    f32 tab_overline = 2.0f;

    // The alpha a disabled widget is drawn at.
    f32 disabled_alpha = 0.45f;

    // Derives the metrics for a density. Comfortable is the default; compact
    // shrinks the vertical rhythm (paddings and spacing) but keeps the type
    // size, so it fits more rows without becoming harder to read.
    [[nodiscard]] static constexpr EditorMetrics for_density(EditorDensity density) noexcept {
        EditorMetrics metrics;
        if (density == EditorDensity::Compact) {
            metrics.font_size = 14.0f;
            metrics.mono_font_size = 12.5f;
            metrics.heading_size = 15.0f;
            metrics.window_padding_x = 9.0f;
            metrics.window_padding_y = 7.0f;
            metrics.frame_padding_x = 8.0f;
            metrics.frame_padding_y = 4.0f;
            metrics.item_spacing_x = 6.0f;
            metrics.item_spacing_y = 4.0f;
            metrics.item_inner_spacing_x = 6.0f;
            metrics.item_inner_spacing_y = 4.0f;
            metrics.indent_spacing = 15.0f;
            metrics.menu_bar_padding_y = 5.0f;
            metrics.scrollbar_size = 10.0f;
            metrics.tab_min_width = 58.0f;
        }
        return metrics;
    }
};

// Everything a scheme needs to draw: the colours and the measurements. A value,
// so EditorUi can hold "the scheme currently applied" and re-apply only when it
// changes, and so a test can build one without a toolkit.
struct EditorScheme {
    EditorPalette palette;
    EditorMetrics metrics;

    [[nodiscard]] static constexpr EditorScheme for_theme(EditorTheme theme, EditorDensity density) noexcept {
        return EditorScheme{
            .palette = EditorPalette::for_dark(theme == EditorTheme::Dark),
            .metrics = EditorMetrics::for_density(density),
        };
    }
};

} // namespace fe::editor
