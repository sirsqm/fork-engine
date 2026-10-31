// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/EditorPalette.h"
#include "fe/editor/EditorTheme.h"

#include <string_view>

// Small, reusable pieces of editor chrome. They live in one header so every
// panel draws the same shape: a label/value row, a status badge, a section
// heading. A panel that reaches for these instead of assembling its own spacing
// is what makes the editor look like one program rather than a set of panels.
//
// These are immediate-mode helpers: each draws on the current window and returns
// nothing. They name palette roles, never raw colours, so they follow the theme.

namespace fe::editor::ui {

// Records the palette currently on screen, so the helpers below and any panel can
// name a role without reaching into the toolkit. EditorUi calls this whenever it
// applies a scheme; a panel never sets it. The alternative - threading the scheme
// through every helper call - would put the same argument on every row and hide
// the one line that actually changes it.
void set_active_palette(const EditorPalette& palette);

// The palette currently on screen. Valid after the first frame's style apply;
// before that it is the dark default, so a helper called early still draws.
[[nodiscard]] const EditorPalette& palette();

// Records and returns the metrics (paddings, indent, font sizes) currently on
// screen, for the same reason as the palette: a panel reads a measurement by role
// rather than carrying a literal.
void set_active_metrics(const EditorMetrics& metrics);
[[nodiscard]] const EditorMetrics& metrics();

// A label/value row: the label muted, the value in the normal colour, the value
// starting at `label_column` so a stack of rows lines up as a table. The shape
// most panels want, so the alignment is consistent rather than per-panel.
void labelled(std::string_view label, std::string_view value, float label_column = 92.0f);

// A section heading inside a panel: a semibold label with a hairline rule under
// it, the grouping device a properties panel uses between "Transform" and
// "Sprite". Uses the editor's heading font and the palette's muted colour.
void section_heading(std::string_view title);

// A small pill badge: a filled rounded rectangle with a label. For a state that
// matters (a running play, unsaved changes) so it reads at a glance.
void badge(std::string_view text, const Rgba& colour);

// A muted hint line, for an empty state ("No nodes", "Waiting for a frame").
// Centred horizontally in the available width so an empty panel does not read as
// a top-left fragment.
void empty_hint(std::string_view text);

} // namespace fe::editor::ui
