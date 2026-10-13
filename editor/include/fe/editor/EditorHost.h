// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorUi.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Engine.h"

#include <string>

namespace fe::editor {

class ViewportPanel;

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

    // Brings up the embedded engine, the document, and the UI, and registers the
    // panels. Returns an Error describing the first failure; a failed call
    // leaves the host shutdown-safe (calling shutdown() is still valid).
    [[nodiscard]] Error initialize();

    // Runs exactly one frame: poll events, consume a play request, draw the
    // scene, draw the panels, present. Requires initialize() to have succeeded.
    void step();

    // True when the window has been asked to close, i.e. the loop in run()
    // should stop.
    [[nodiscard]] bool should_close() const;

    // Releases the UI, the document, and the engine in the order their
    // destructors require. Safe to call more than once.
    void shutdown();

    // initialize(), then a frame loop until should_close() or the configured
    // frame limit, then shutdown(). Returns the process exit code (0 on a clean
    // run). Never throws. This is what the shell calls; step() exists so a test
    // can act between frames.
    [[nodiscard]] int run();

    [[nodiscard]] EditorContext& context() noexcept { return context_; }
    [[nodiscard]] Engine& engine() noexcept { return engine_; }

    // The viewport's off-screen render target after at least one frame, or null
    // before the first draw or if the viewport failed to allocate. Exposed for
    // the end-to-end test, which asserts the editor really renders the scene into
    // a panel-sized target rather than only the window.
    [[nodiscard]] rhi::RenderTarget* viewport_target() const noexcept;

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
    // Borrowed from the panel set owned by `ui_`; used to release the viewport's
    // GPU target before the device shuts down.
    ViewportPanel* viewport_panel_ = nullptr;
    bool initialized_ = false;
    u32 frame_ = 0;
};

} // namespace fe::editor
