// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

namespace fe::editor {

// A colour, as linear-ish sRGB bytes in 0..1. Deliberately not ImVec4: the
// palette is the editor's, not the toolkit's, so it is testable without an ImGui
// context and survives a change of UI toolkit. EditorUi converts it.
struct Rgba {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;

    [[nodiscard]] constexpr Rgba with_alpha(f32 alpha) const noexcept { return Rgba{r, g, b, alpha}; }
};

// The editor's colour scheme, named by role rather than by widget.
//
// The palette is a value, so it is one source of truth for both the dark and the
// light scheme and can be checked (contrast, ordering) without drawing anything.
// EditorUi maps each role onto the toolkit's colour slots; a panel never names a
// colour, so re-theming the editor is a change here and nowhere else.
//
// Roles are chosen so a widget's colour follows from what it *is*:
//   * window_bg      - a panel's body, the largest surface on screen
//   * surface        - chrome above the body: menu bar, title bars, tab bars
//   * surface_raised - an element that sits *on* the body and should lift: an
//                      input frame, a button, a popup
//   * surface_sunken - a recessed well: an empty viewport, a code area
//   * border / border_strong - hairlines and emphasised outlines
//   * text / text_muted / text_disabled - the three legibility tiers
//   * accent (+ hovered/active) - the one interactive highlight colour
//   * selection_bg   - a selected row or list item
//   * danger / success - the two semantic signals (an error, a running play)
struct EditorPalette {
    Rgba window_bg;
    Rgba surface;
    Rgba surface_raised;
    Rgba surface_sunken;
    Rgba border;
    Rgba border_strong;
    Rgba text;
    Rgba text_muted;
    Rgba text_disabled;
    Rgba accent;
    Rgba accent_hovered;
    Rgba accent_active;
    Rgba selection_bg;
    Rgba danger;
    Rgba success;

    // The dark scheme: a cool, near-neutral graphite with a warm amber accent.
    // Amber is the engine's own signature (the viewport selection outline is the
    // same hue), so the highlight is recognisable as *this* editor rather than
    // one more blue.
    [[nodiscard]] static constexpr EditorPalette dark() noexcept {
        return EditorPalette{
            .window_bg = {0.118f, 0.129f, 0.153f},      // #1E2127
            .surface = {0.149f, 0.165f, 0.196f},        // #262A32
            .surface_raised = {0.180f, 0.200f, 0.239f}, // #2E333D
            .surface_sunken = {0.086f, 0.094f, 0.114f}, // #16181D
            .border = {0.227f, 0.255f, 0.314f},         // #3A4150
            .border_strong = {0.290f, 0.325f, 0.392f},  // #4A5364
            .text = {0.894f, 0.906f, 0.925f},           // #E4E7EC
            .text_muted = {0.604f, 0.639f, 0.698f},     // #9AA3B2
            .text_disabled = {0.420f, 0.447f, 0.502f},  // #6B7280
            .accent = {0.902f, 0.635f, 0.235f},         // #E6A23C
            .accent_hovered = {0.941f, 0.690f, 0.314f}, // #F0B050
            .accent_active = {0.839f, 0.573f, 0.173f},  // #D6922C
            .selection_bg = {0.227f, 0.298f, 0.416f},   // #3A4C6A
            .danger = {0.910f, 0.416f, 0.416f},         // #E86A6A
            .success = {0.424f, 0.784f, 0.518f},        // #6CC884
        };
    }

    // The light scheme: a soft off-white rather than pure white, so panels
    // separate without a hard border, with a deepened amber that keeps the
    // accent legible on light.
    [[nodiscard]] static constexpr EditorPalette light() noexcept {
        return EditorPalette{
            .window_bg = {0.957f, 0.961f, 0.969f},      // #F4F5F7
            .surface = {0.914f, 0.922f, 0.937f},        // #E9EBEF
            .surface_raised = {1.000f, 1.000f, 1.000f}, // #FFFFFF
            .surface_sunken = {0.875f, 0.886f, 0.906f}, // #DFE2E7
            .border = {0.808f, 0.824f, 0.855f},         // #CED2DA
            .border_strong = {0.690f, 0.714f, 0.761f},  // #B0B6C2
            .text = {0.110f, 0.122f, 0.149f},           // #1C1F26
            .text_muted = {0.345f, 0.373f, 0.424f},     // #585F6C
            .text_disabled = {0.549f, 0.573f, 0.620f},  // #8C929E
            .accent = {0.690f, 0.439f, 0.078f},         // #B07014
            .accent_hovered = {0.769f, 0.510f, 0.125f}, // #C48220
            .accent_active = {0.588f, 0.369f, 0.063f},  // #965E10
            .selection_bg = {0.808f, 0.871f, 0.973f},   // #CEDEF8
            .danger = {0.745f, 0.235f, 0.235f},         // #BE3C3C
            .success = {0.157f, 0.510f, 0.275f},        // #288246
        };
    }

    // The palette for a scheme, so a caller with an EditorTheme does not branch.
    [[nodiscard]] static constexpr EditorPalette for_dark(bool dark) noexcept {
        return dark ? EditorPalette::dark() : EditorPalette::light();
    }
};

// WCAG relative luminance of an sRGB colour, for the contrast checks below. Not
// constexpr: std::pow is not constexpr until C++26, and the value is only ever
// needed at run time (a test), never in a constant expression.
[[nodiscard]] f32 relative_luminance(const Rgba& colour) noexcept;

// The WCAG contrast ratio between two opaque colours, in 1..21. 4.5 is the AA
// threshold for body text and 3.0 for large text and UI boundaries; the palette
// tests hold the editor to those. Alpha is ignored: the palette's text roles are
// opaque, and a translucent surface is composited before it is drawn.
[[nodiscard]] f32 contrast_ratio(const Rgba& a, const Rgba& b) noexcept;

} // namespace fe::editor
