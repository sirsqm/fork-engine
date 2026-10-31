// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

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

} // namespace fe::editor
