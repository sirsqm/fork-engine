// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/editor/Panel.h"

#include <memory>
#include <string>
#include <vector>

namespace fe {

class Window;

namespace editor {

class EditorContext;

// Owns the Dear ImGui context and the editor's panel set, and drives one UI
// frame.
//
// Responsibilities:
//   * Create and destroy the ImGui context and its GLFW + OpenGL3 backends.
//   * Provide the dock space: a single full-window host into which the panels
//     dock, plus a default layout the first time the editor runs.
//   * Own the panels and call them in order between NewFrame() and Render().
//
// Non-responsibilities: it does not own the window or the GL context (the
// embedded `Engine` does), does not poll events or present (the shell's loop
// does), and does not know what any panel draws. This keeps the toolkit's
// lifecycle in exactly one place, which is the property that makes the rest of
// the editor toolkit-agnostic (see docs/adr/0007-editor-ui-toolkit.md).
//
// The editor creates exactly one `EditorUi`, bound to the one GL context the
// embedded engine owns. Nothing here creates a second context.
class EditorUi {
public:
    EditorUi();
    ~EditorUi();

    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    // Creates the ImGui context and initialises the backends against `window`.
    // Requires `window` to have a current GL context. Returns an Error if the
    // context or a backend fails to initialise.
    [[nodiscard]] Error initialize(Window& window);

    void shutdown();

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // --- Panels ------------------------------------------------------------
    // Takes ownership of a panel. Panels are drawn in insertion order; order
    // only matters for the first-run default layout, after which the user's
    // saved dock layout wins.
    void add_panel(std::unique_ptr<Panel> panel);

    // --- Frame -------------------------------------------------------------
    // Begins an ImGui frame. Call once per frame after the OS events have been
    // polled, before any panel draws.
    void begin_frame();

    // Draws the dock space host and every registered panel. Call after
    // begin_frame().
    void draw_panels(EditorContext& context);

    // Finalises and renders the ImGui draw data onto the current framebuffer.
    // Call after draw_panels() and before Device::end_frame().
    void end_frame();

private:
    void draw_dock_space(unsigned int dockspace_id);

    // The top menu bar: play/stop control and the scene's identity. Kept here
    // rather than in a panel so it is always visible regardless of the dock
    // layout, the way a real editor's transport bar is.
    void draw_toolbar(EditorContext& context);

    bool initialized_ = false;
    std::vector<std::unique_ptr<Panel>> panels_;
    // Set once the default dock layout has been applied, so a user's rearranged
    // layout is not overwritten on every launch.
    bool default_layout_applied_ = false;
    std::string layout_ini_path_;
};

} // namespace editor
} // namespace fe
