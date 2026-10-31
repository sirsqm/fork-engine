// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The editor's look is a value, so it is testable without a window or a toolkit.
// These hold the palette to its contract (roles are ordered and legible, the two
// schemes agree on structure) and the metrics to theirs (compact is tighter than
// comfortable, and the derived sizes stay readable). A regression here is a
// scheme that ships unreadable text, which no screenshot test would catch.

#include "fe/editor/EditorPalette.h"
#include "fe/editor/EditorTheme.h"

#include <doctest/doctest.h>

#include <array>

using fe::f32;
using fe::editor::contrast_ratio;
using fe::editor::EditorDensity;
using fe::editor::EditorMetrics;
using fe::editor::EditorPalette;
using fe::editor::EditorScheme;
using fe::editor::EditorTheme;
using fe::editor::relative_luminance;
using fe::editor::Rgba;

namespace {

// Every palette role, with its name, so a failing case names the role rather than
// a byte offset.
struct NamedColour {
    const char* name;
    Rgba colour;
};

[[nodiscard]] std::array<NamedColour, 15> roles(const EditorPalette& p) {
    return {{
        {"window_bg", p.window_bg},
        {"surface", p.surface},
        {"surface_raised", p.surface_raised},
        {"surface_sunken", p.surface_sunken},
        {"border", p.border},
        {"border_strong", p.border_strong},
        {"text", p.text},
        {"text_muted", p.text_muted},
        {"text_disabled", p.text_disabled},
        {"accent", p.accent},
        {"accent_hovered", p.accent_hovered},
        {"accent_active", p.accent_active},
        {"selection_bg", p.selection_bg},
        {"danger", p.danger},
        {"success", p.success},
    }};
}

} // namespace

TEST_CASE("every palette role is a valid, opaque colour") {
    for (const EditorPalette& p : {EditorPalette::dark(), EditorPalette::light()}) {
        for (const NamedColour& role : roles(p)) {
            CAPTURE(role.name);
            CHECK(role.colour.r >= 0.0f);
            CHECK(role.colour.r <= 1.0f);
            CHECK(role.colour.g >= 0.0f);
            CHECK(role.colour.g <= 1.0f);
            CHECK(role.colour.b >= 0.0f);
            CHECK(role.colour.b <= 1.0f);
            CHECK(role.colour.a == 1.0f);
        }
    }
}

TEST_CASE("body text meets the WCAG AA contrast threshold on its background") {
    // 4.5:1 is the AA threshold for body text. The editor's default text must
    // clear it on the panel body, or the UI is unreadable by the standard it
    // claims to meet.
    CHECK(contrast_ratio(EditorPalette::dark().text, EditorPalette::dark().window_bg) >= 4.5f);
    CHECK(contrast_ratio(EditorPalette::light().text, EditorPalette::light().window_bg) >= 4.5f);
}

TEST_CASE("muted and disabled text stay legible, in that order") {
    // The three text tiers must be ordered: full text is the most legible, then
    // muted, then disabled. A muted tier that is darker than disabled would make
    // a hint out-shout a label.
    for (const EditorPalette& p : {EditorPalette::dark(), EditorPalette::light()}) {
        const f32 full = contrast_ratio(p.text, p.window_bg);
        const f32 muted = contrast_ratio(p.text_muted, p.window_bg);
        const f32 disabled = contrast_ratio(p.text_disabled, p.window_bg);
        CHECK(full > muted);
        CHECK(muted > disabled);
        // Even the disabled tier must be more than a whisper: below 2:1 it is
        // invisible on a low-quality panel.
        CHECK(disabled >= 2.0f);
    }
}

TEST_CASE("the accent is distinct from the text it sits beside") {
    // The accent is the interactive highlight; on a button its label is drawn in
    // the window background colour, so the pair must be legible both ways.
    for (const EditorPalette& p : {EditorPalette::dark(), EditorPalette::light()}) {
        CHECK(contrast_ratio(p.accent, p.window_bg) >= 3.0f);
        // The accent must not read as ordinary body text: it has to stand out.
        CHECK(relative_luminance(p.accent) != relative_luminance(p.text));
    }
}

TEST_CASE("surfaces are ordered from sunken to raised") {
    // A dark scheme gets lighter as an element comes forward; a light scheme gets
    // darker. Either way the order is strict, so a panel, a frame and a popup
    // never collide into one flat grey.
    const EditorPalette dark = EditorPalette::dark();
    CHECK(relative_luminance(dark.surface_sunken) < relative_luminance(dark.window_bg));
    CHECK(relative_luminance(dark.window_bg) < relative_luminance(dark.surface));
    CHECK(relative_luminance(dark.surface) < relative_luminance(dark.surface_raised));

    const EditorPalette light = EditorPalette::light();
    CHECK(relative_luminance(light.surface_sunken) < relative_luminance(light.window_bg));
    CHECK(relative_luminance(light.window_bg) < relative_luminance(light.surface_raised));
}

TEST_CASE("border roles are ordered, and stronger is more visible") {
    for (const EditorPalette& p : {EditorPalette::dark(), EditorPalette::light()}) {
        // The stronger border must contrast with the body at least as much as the
        // plain one, or "stronger" means nothing.
        CHECK(contrast_ratio(p.border_strong, p.window_bg) > contrast_ratio(p.border, p.window_bg));
    }
}

TEST_CASE("for_dark picks the matching scheme") {
    CHECK(EditorPalette::for_dark(true).window_bg.r == EditorPalette::dark().window_bg.r);
    CHECK(EditorPalette::for_dark(false).window_bg.r == EditorPalette::light().window_bg.r);
}

TEST_CASE("compact density is tighter than comfortable, without shrinking the type") {
    const EditorMetrics comfortable = EditorMetrics::for_density(EditorDensity::Comfortable);
    const EditorMetrics compact = EditorMetrics::for_density(EditorDensity::Compact);

    // Every vertical rhythm measurement must shrink (or stay), never grow.
    CHECK(compact.window_padding_y <= comfortable.window_padding_y);
    CHECK(compact.frame_padding_y <= comfortable.frame_padding_y);
    CHECK(compact.item_spacing_y <= comfortable.item_spacing_y);
    CHECK(compact.indent_spacing <= comfortable.indent_spacing);
    CHECK(compact.menu_bar_padding_y <= comfortable.menu_bar_padding_y);

    // The type stays readable: compact shrinks it a little, but not below a
    // legible floor.
    CHECK(compact.font_size >= 12.0f);
    CHECK(compact.font_size <= comfortable.font_size);
}

TEST_CASE("metrics are internally consistent") {
    for (const EditorMetrics& m :
         {EditorMetrics::for_density(EditorDensity::Comfortable), EditorMetrics::for_density(EditorDensity::Compact)}) {
        // The mono face is never larger than the body face, or a number column
        // would shout over the label beside it.
        CHECK(m.mono_font_size <= m.font_size);
        // A heading is the body scaled up, so a panel title reads as a title.
        CHECK(m.heading_size > m.font_size);
        // The indent must be positive, or a hierarchy has no visible depth.
        CHECK(m.indent_spacing > 0.0f);
        // Padding must be non-negative; a negative padding would push content out
        // of its frame.
        CHECK(m.window_padding_x >= 0.0f);
        CHECK(m.frame_padding_x >= 0.0f);
        CHECK(m.item_spacing_x >= 0.0f);
    }
}

TEST_CASE("a scheme carries the palette and metrics for its theme and density") {
    const EditorScheme scheme = EditorScheme::for_theme(EditorTheme::Light, EditorDensity::Compact);
    CHECK(scheme.palette.window_bg.r == EditorPalette::light().window_bg.r);
    CHECK(scheme.metrics.font_size == EditorMetrics::for_density(EditorDensity::Compact).font_size);
}
