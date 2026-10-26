// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorUi.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Engine.h"

#include <string>
#include <vector>

namespace fe::editor {

class ViewportPanel;
class FileDialogPanel;

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
        // A project file to open at startup. Its settings (name, asset root,
        // startup scene) are adopted: the asset root is indexed before any scene
        // loads, and its startup scene opens unless `scene_to_open` overrides it.
        std::string project_file;
        // The project's asset root: the directory the asset browser lists and
        // that GUID asset references resolve against. Empty means "no project",
        // which leaves the browser empty and only path references working.
        // Ignored when `project_file` is set (the file's asset root wins).
        std::string project_root;
        // A scene file to open at startup; empty opens the project's startup
        // scene, or a built-in example scene when there is no project.
        std::string scene_to_open;
        // Gameplay module shared libraries to load before any scene opens, so a
        // scene that records a behaviour by name can attach it (ADR-0061). Each
        // is watched for a rebuild and hot-reloaded while the editor runs
        // (ADR-0066).
        std::vector<std::string> modules;
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

    // The file dialog panel, exposed for tests so a dialog can be opened and
    // confirmed headlessly through `FileDialogPanel::confirm`. Null before
    // initialize().
    [[nodiscard]] FileDialogPanel* file_dialog() const noexcept { return file_dialog_panel_; }

private:
    // Consumes a pending play request from the UI and performs it against the
    // embedded engine.
    void handle_play_request();

    // Consumes a pending document-level request from the menu and performs it:
    // opening the file dialog, saving directly, confirming a destructive action,
    // or closing the window. The split from `handle_play_request` keeps the two
    // request kinds independent.
    void handle_file_request();

    // Performs `request`, assuming any confirmation it needs has already been
    // given. Split out so the confirmation path can defer to it.
    void perform_file_request(FileRequest request);

    // True when `request` would discard unsaved work, so it needs the
    // confirmation prompt first.
    [[nodiscard]] bool needs_confirmation(FileRequest request) const;

    // The directory a file dialog should start in: the current scene's, then the
    // project's, then the working directory. Never empty.
    [[nodiscard]] std::string dialog_start_directory() const;

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
    // Borrowed from the panel set owned by `ui_`; the host opens and reads it when
    // a file request needs a dialog.
    FileDialogPanel* file_dialog_panel_ = nullptr;
    // A destructive action waiting on the confirmation prompt. Set when a request
    // needs confirmation; performed on ConfirmDiscard, dropped on CancelDiscard.
    FileRequest pending_file_request_ = FileRequest::None;
    bool initialized_ = false;
    u32 frame_ = 0;
};

} // namespace fe::editor
