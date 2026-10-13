// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorUi.h"
#include "fe/runtime/Engine.h"

#include <string>

namespace fe::editor {

// The editor host: an embedded `Engine`, the document, and the ImGui UI, wired
// into one loop.
//
// This is the concrete realisation of "the editor is a client of the runtime,
// not a fork of it" (ADR-0001, docs/ARCHITECTURE.md section 8): the host embeds
// an `Engine` and drives frames itself, rather than reusing `Application`'s
// loop. One loop renders the scene (authored, or the play copy while playing)
// and then draws the ImGui panels over it, so play-in-editor shares the exact
// runtime a game would use.
//
// The host owns the ordering constraints that matter: the play scene and the
// authored scene are released before the engine shuts down, because scene
// components can own GPU-backed resources whose destructors call into the
// device (docs/ARCHITECTURE.md section 6).
class EditorHost {
public:
    struct Config {
        Engine::Config engine{};
        // A scene file to open at startup; empty opens a built-in example
        // scene instead.
        std::string scene_to_open;
        // Run at most this many frames, then exit. 0 means "until the window
        // closes". A non-zero limit with a hidden window is the headless smoke
        // test used by CI, where there is no display to close the window.
        u32 max_frames = 0;
    };

    explicit EditorHost(Config config);
    EditorHost() : EditorHost(Config{}) {}
    ~EditorHost();

    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    // Runs until the window closes. Returns the process exit code (0 on a clean
    // run). Never throws.
    [[nodiscard]] int run();

    [[nodiscard]] EditorContext& context() noexcept { return context_; }
    [[nodiscard]] Engine& engine() noexcept { return engine_; }

private:
    // Consumes a pending play request from the UI and performs it against the
    // embedded engine.
    void handle_play_request();

    // Draws `scene` into the engine's current framebuffer, choosing the clear
    // colour from the active camera exactly as the runtime does.
    void render_scene(const Scene& scene);

    Config config_;
    Engine engine_;
    EditorContext context_;
    EditorUi ui_;
};

} // namespace fe::editor
