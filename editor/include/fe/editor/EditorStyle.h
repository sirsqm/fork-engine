// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/EditorTheme.h"

#include <string_view>

struct ImFont;

namespace fe::editor {

// The fonts the editor's UI draws with, resolved once at startup.
//
// One struct so the sizes are decided in one place (EditorMetrics) and the
// panels name a role ("the mono font") rather than a size. The regular face is
// the default; a panel that wants the mono face pushes it for its own draws.
struct EditorFonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
    ImFont* mono = nullptr;
    // The mono face at the heading size, for a panel title that is a path or an
    // identifier and reads better monospaced.
    bool loaded = false;
};

// Loads the embedded fonts into ImGui's atlas at the sizes EditorMetrics asks
// for, and installs the regular face as the default. Must be called after
// ImGui::CreateContext() and before the first NewFrame(). Returns false when the
// atlas rejected every embedded face (which leaves ImGui's own default font in
// place, so the editor still runs). The font bytes are the embedded arrays, so
// nothing is read from disk and nothing is owned by the atlas.
[[nodiscard]] bool load_editor_fonts(EditorFonts& fonts, const EditorMetrics& metrics);

// Re-bakes the atlas at the sizes `metrics` asks for, after a density change.
//
// Clearing the atlas removes the font the current frame has bound, which ImGui
// documents as illegal and which aborts (an assertion inside ImTextureData).
// So this must be called *between* frames — after the previous frame's Render,
// before the next NewFrame — which is why it is a separate entry point rather
// than part of apply_editor_style(). EditorUi calls it from begin_frame().
[[nodiscard]] bool rebuild_editor_fonts(EditorFonts& fonts, const EditorMetrics& metrics);

// Applies `scheme` to ImGui's live style: every colour and measurement the editor
// controls. Called by EditorUi when the theme or density changes, so a switch is
// immediate. Kept out of EditorUi so the mapping from a role-named palette to
// ImGui's colour slots is one function that can be read on its own.
void apply_editor_style(const EditorScheme& scheme);

} // namespace fe::editor
