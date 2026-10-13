// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::editor {

class EditorContext;

// A dockable editor panel.
//
// A panel is a small object that draws itself every frame from editor state and
// returns nothing: immediate mode means the UI is a projection of the model, so
// a panel is a pure "read the model, emit widgets" function with a lifetime only
// for the state the user is mid-edit on (a text buffer, say).
//
// `draw()` must only be called between ImGui::NewFrame() and ImGui::Render();
// the ImGui context is owned by `EditorUi`, not by the panel. This is the same
// arrangement as the docking host: the toolkit lifecycle lives in one place and
// panels stay free of it.
class Panel {
public:
    virtual ~Panel() = default;

    // Stable identity used as the ImGui window name and the docking key. Must
    // be a compile-time constant with a lifetime longer than the panel.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    // Emits this panel's widgets for one frame.
    virtual void draw(EditorContext& context) = 0;
};

} // namespace fe::editor
