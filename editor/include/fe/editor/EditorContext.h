// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Profiler.h"
#include "fe/editor/Command.h"
#include "fe/editor/Commands.h"
#include "fe/editor/HierarchyViewModel.h"
#include "fe/editor/PlaySession.h"
#include "fe/editor/ProjectSettings.h"
#include "fe/editor/ScriptCatalog.h"
#include "fe/scene/Scene.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

class ResourceManager;

namespace editor {

// A request from the UI for the editor host to act on. The panels only read
// state and set requests; the host (which owns the embedded Engine) performs
// them. This keeps `EditorContext` free of the engine and makes the UI's
// intentions explicit and testable.
enum class PlayRequest {
    None,
    Start,
    Stop,
};

// Which renderer the viewport panel presents. A scene may hold both 2D and 3D
// content (a HUD over a 3D world), so this is a view choice rather than a
// property of the document. `Auto` picks 3D when the scene has a 3D camera and
// 2D otherwise, so opening a 3D scene shows it without a click.
enum class ViewportMode {
    Auto,
    TwoD,
    ThreeD,
};

// A document-level request from the menu. Like `PlayRequest`, the UI only sets
// it; the host - which owns the embedded Engine and the window - performs it
// between frames, so document replacement and window control never happen from
// inside a panel's draw. The host decides which of these need a file dialog or a
// confirmation first.
enum class FileRequest {
    None,
    // Replace the document with a fresh example scene.
    NewScene,
    // Browse for a scene to open.
    OpenScene,
    // Save to the current path, or browse for one when the document has none.
    SaveScene,
    // Always browse for a path, even when the document has one.
    SaveSceneAs,
    // Re-read the current scene from disk, discarding in-memory edits.
    ReloadScene,
    // Browse for a project file to open.
    OpenProject,
    // Close the editor, after a confirmation when there are unsaved changes.
    Quit,
    // The user answered the "discard unsaved changes?" prompt affirmatively; the
    // host performs the action held in the context's pending action.
    ConfirmDiscard,
    // The user declined it; the host clears the pending action and does nothing.
    CancelDiscard,
};

// The editor's document: the authored scene plus the view state and play
// session that operate on it.
//
// This is the headless heart of the editor shell. It owns no window and no UI
// toolkit, so its behaviour - loading a scene file, saving one, folding the
// hierarchy, starting and stopping play - is fully testable without a display.
// The ImGui panels and the shell are thin shells over this type.
//
// It is a client of `scene` and `serialization` through their public APIs only
// (docs/ARCHITECTURE.md section 8). It never reaches into a node's private
// state, and it does not depend on the reflection module.
class EditorContext {
public:
    EditorContext();

    // Registers the copy factories for the built-in component types
    // (Camera2D, Sprite2D) on the play session, so play-in-editor sees them.
    void register_default_component_factories();

    // --- Scripts -----------------------------------------------------------
    // The authored scene's gameplay scripts: which behaviour is bound to which
    // node, and the catalogue the inspector offers. Scripts are not components
    // (ADR-0044), so they live here rather than in the scene, and the scene
    // serializer persists them through this catalog's host and registry
    // (ADR-0063).
    [[nodiscard]] ScriptCatalog& scripts() noexcept { return scripts_; }
    [[nodiscard]] const ScriptCatalog& scripts() const noexcept { return scripts_; }

    // Polls the loaded gameplay modules for a rebuilt image and hot-reloads each
    // one that changed, rebinding the nodes it scripted to fresh code (ADR-0066).
    // Returns the ids that were reloaded, so the caller can log them. This is the
    // editor's per-frame "did a module get rebuilt" step: poll-driven, so the
    // loop decides how often to ask, exactly as the resource manager's change
    // check does (ADR-0046). Empty when nothing changed. A reload that fails is
    // logged and the module dropped, not thrown.
    //
    // `settle_polls` debounces a build (ADR-0067): a module is reloaded only once
    // its write time has held for that many consecutive polls, so a link still in
    // progress is not read as a finished module. The host passes 2 - one frame of
    // latency - so the common "image is being written" window is skipped.
    [[nodiscard]] std::vector<std::string> poll_script_modules(u32 settle_polls = 1);

    // --- Document ----------------------------------------------------------
    [[nodiscard]] Scene& scene() noexcept { return scene_; }
    [[nodiscard]] const Scene& scene() const noexcept { return scene_; }

    [[nodiscard]] HierarchyViewModel& hierarchy() noexcept { return hierarchy_; }
    [[nodiscard]] const HierarchyViewModel& hierarchy() const noexcept { return hierarchy_; }

    [[nodiscard]] PlaySession& play() noexcept { return play_; }
    [[nodiscard]] const PlaySession& play() const noexcept { return play_; }

    // The path the current scene was loaded from or last saved to; "" for a new
    // scene that has never been persisted.
    [[nodiscard]] const std::string& scene_path() const noexcept { return scene_path_; }

    // --- Unsaved-changes tracking -----------------------------------------
    // True when the document has been edited since it was last created, loaded,
    // or saved. Every edit that goes through the command stack - a component
    // property, a script property, a structural command, an undo or a redo -
    // marks it; new_scene, load_scene, save_scene, and reload_scene clear it.
    //
    // This is what lets the editor ask "discard unsaved changes?" before a New,
    // an Open, or a quit, and what a future "modified" marker in the title bar
    // reads. It is deliberately conservative: a false positive (dirty when the
    // file happens to match) is harmless, a false negative loses work.
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }
    void clear_dirty() noexcept { dirty_ = false; }
    void mark_dirty() noexcept { dirty_ = true; }

    // Replaces the document with a newly built example scene (a camera and a
    // few sprites). `resources` may be null, in which case sprites are created
    // without textures. Clears the path and the hierarchy.
    [[nodiscard]] Error new_scene(ResourceManager* resources = nullptr);

    // Loads a scene file into the document. `resources` may be null. On failure
    // the existing document is left untouched.
    [[nodiscard]] Error load_scene(std::string_view path, ResourceManager* resources = nullptr);

    // Re-reads the current scene from its file, discarding in-memory edits. This
    // is the editor's "revert" (and the file-watch reload): it is load_scene on
    // the current path, so it clears the undo history and the dirty flag. Returns
    // an Error when the document has never been saved (no path to re-read).
    [[nodiscard]] Error reload_scene(ResourceManager* resources = nullptr);

    // Saves the current scene to `path` (or to the current path when `path` is
    // empty). On success `scene_path()` becomes the saved path.
    [[nodiscard]] Error save_scene(std::string_view path);

    // Rebuilds the hierarchy view model from the current scene. Called after a
    // load or a structural change.
    void refresh_hierarchy();

    // The resource manager the inspector resolves resource references against
    // (for example a sprite's texture). Null when the document has no owning
    // engine, which is the headless test case: resource fields then show their
    // current key but are not editable. The host points this at the embedded
    // engine's manager once the engine is up.
    [[nodiscard]] ResourceManager* resources() const noexcept { return resources_; }
    void set_resources(ResourceManager* resources) noexcept { resources_ = resources; }

    // --- Project -----------------------------------------------------------
    // The project's asset root: the directory the asset database indexes and
    // that asset references resolve against. Empty means the editor has no
    // project open, in which case the asset browser is empty and only path
    // references (not GUID references) resolve.
    //
    // This is a projection of the loaded project's settings (see below), kept as
    // a plain accessor because it is what the asset browser and the serializer
    // consult. A real project is a file the editor opens; that file is the
    // source of truth and this is its effect.
    [[nodiscard]] const std::string& project_root() const noexcept { return project_root_; }

    // Sets the project's asset root and rescans the asset database through
    // `resources`. A null manager or an empty root clears the index. Rescanning
    // on every set keeps the database and this field in step; the scan is cheap
    // (a directory walk) and only happens when the root changes.
    void set_project_root(std::string root, ResourceManager* resources = nullptr);

    // The loaded project settings. Default-constructed until a project file is
    // opened, so `project().asset_root()` is the default ("assets") and
    // `project().name()` is empty.
    [[nodiscard]] ProjectSettings& project() noexcept { return project_; }
    [[nodiscard]] const ProjectSettings& project() const noexcept { return project_; }

    // The path the current project was loaded from or saved to; "" for a project
    // that has never been persisted (including "no project").
    [[nodiscard]] const std::string& project_path() const noexcept { return project_path_; }

    // Opens a project file: reads and validates its settings, adopts its
    // directory, and points the asset root at the database through `resources`
    // (which rescans it). The current document is left alone - opening a project
    // does not by itself load a scene; the host does that from the settings. On
    // failure nothing changes.
    [[nodiscard]] Error load_project(std::string_view path, ResourceManager* resources = nullptr);

    // Writes the current settings to `path` (or to the current project path when
    // `path` is empty). On success `project_path()` becomes the saved path.
    [[nodiscard]] Error save_project(std::string_view path = {});

    // --- Play control ------------------------------------------------------
    // Records that the user asked to start or stop playing. The host consumes it
    // with take_play_request() in the same frame; nothing happens until it does.
    void request_start_play() noexcept { play_request_ = PlayRequest::Start; }
    void request_stop_play() noexcept { play_request_ = PlayRequest::Stop; }

    [[nodiscard]] PlayRequest play_request() const noexcept { return play_request_; }

    // Returns the pending request and clears it.
    [[nodiscard]] PlayRequest take_play_request() noexcept {
        const PlayRequest request = play_request_;
        play_request_ = PlayRequest::None;
        return request;
    }

    // --- File control ------------------------------------------------------
    // Records a document-level request from the menu. The host consumes it with
    // take_file_request() in the same frame. Requests that need a file dialog or
    // a confirmation are the host's to resolve, not the menu's, so the menu stays
    // a pure emitter of intent.
    void request_file(FileRequest request) noexcept { file_request_ = request; }

    [[nodiscard]] FileRequest file_request() const noexcept { return file_request_; }

    // Returns the pending request and clears it.
    [[nodiscard]] FileRequest take_file_request() noexcept {
        const FileRequest request = file_request_;
        file_request_ = FileRequest::None;
        return request;
    }

    // --- Confirmation prompt ----------------------------------------------
    // A yes/no question the UI must show before a destructive action (replacing
    // the document, quitting with unsaved changes). The host asks; the confirm
    // panel draws it; the answer comes back as ConfirmDiscard/CancelDiscard
    // through the file-request channel, so the host performs the action and the
    // panel stays a pure reader/emitter.
    void ask_confirm(std::string message) {
        confirm_message_ = std::move(message);
        confirming_ = true;
    }
    [[nodiscard]] bool is_confirming() const noexcept { return confirming_; }
    [[nodiscard]] const std::string& confirm_message() const noexcept { return confirm_message_; }
    void clear_confirm() noexcept {
        confirming_ = false;
        confirm_message_.clear();
    }

    // --- Editing -----------------------------------------------------------
    // The undo/redo history. Every scene edit made through the editor goes
    // through here, so it is reversible; see docs/adr/0015-editor-undo-redo.md.
    [[nodiscard]] CommandStack& commands() noexcept { return commands_; }
    [[nodiscard]] const CommandStack& commands() const noexcept { return commands_; }

    // Executes `command` against the scene and refreshes the hierarchy, which
    // every structural command can change. Returns true when the command was
    // applied. Coalescing is handled by the stack.
    bool execute(std::unique_ptr<Command> command);

    // Routes an edit of a gameplay script's property through the command stack
    // as a serializable `SetScriptProperty` operation, so it is undoable and
    // coalesces with the previous edit of the same field exactly as a component
    // property does. `path` is the property's path within the behaviour's
    // described properties; `behaviour` is the behaviour the node is expected to
    // run, recorded so the operation is a no-op if the node's script changes.
    //
    // The script host is reached through this context's `ScriptCatalog`, which
    // implements the `serialization::ScriptTarget` seam, so the operation model
    // stays independent of the script module. Returns true when the command was
    // executed.
    bool edit_script_property(const Uuid& node_id, const std::string& behaviour, const std::vector<std::string>& path,
                              const reflect::Value& value);

    // Routes an edit of a gameplay script's *resource reference* through the
    // command stack as a serializable `SetScriptResource` operation. A resource
    // is written by resolving a key through the manager rather than by assigning
    // a value, so it has its own operation; this is the script counterpart of the
    // component resource edit. `path` is the reference's path within the
    // behaviour's described properties. Returns true when the command executed.
    bool edit_script_resource(const Uuid& node_id, const std::string& behaviour, const std::vector<std::string>& path,
                              const std::string& reference);

    // Undoes/redoes the last command and refreshes the hierarchy. Returns false
    // when there is nothing to undo/redo. Both are refused while play is
    // running, because play shows a copy and an edit to the hidden authored
    // scene would be invisible.
    bool undo();
    bool redo();

    [[nodiscard]] bool can_undo() const noexcept { return !play_.is_playing() && commands_.can_undo(); }
    [[nodiscard]] bool can_redo() const noexcept { return !play_.is_playing() && commands_.can_redo(); }

    // True when the scene may be edited: not while playing.
    [[nodiscard]] bool is_editable() const noexcept { return !play_.is_playing(); }

    // --- Viewport mode -----------------------------------------------------
    // Which of the two renderers the viewport panel shows. A scene may hold both
    // 2D and 3D content (a HUD over a 3D world), so the mode is a view choice,
    // not a property of the document; it lives here so the toolbar and the panel
    // read one source of truth. `Auto` picks 3D when the scene has a 3D camera
    // and 2D otherwise, which is what makes opening a 3D scene show it without a
    // click.
    [[nodiscard]] ViewportMode viewport_mode() const noexcept { return viewport_mode_; }
    void set_viewport_mode(ViewportMode mode) noexcept { viewport_mode_ = mode; }

    // Ends the current coalescing group, so the next property edit is a new undo
    // step. The panels call this when an edit gesture finishes.
    void break_undo_coalescing() noexcept { commands_.break_coalescing(); }

    // --- Profiling ---------------------------------------------------------
    // The editor's profiler, owned here so the profiler panel has a stable
    // source and the editor does not have to reach into the embedded engine for
    // one. It is created lazily by ensure_profiler(), because the object is
    // large (the ring is inline storage) and a host that never shows the panel
    // should not pay for it.
    //
    // It is not a member by value: `Profiler` is non-movable, and a
    // `std::optional` holds it in place while still letting the editor have no
    // profiler until one is asked for. The host installs it as the thread's
    // current profiler for the duration of each frame, so scopes recorded by the
    // editor's own frame are captured.
    [[nodiscard]] Profiler* profiler() noexcept { return profiler_ ? &*profiler_ : nullptr; }
    [[nodiscard]] const Profiler* profiler() const noexcept { return profiler_ ? &*profiler_ : nullptr; }

    // Creates the profiler on first use and returns it. Idempotent.
    Profiler& ensure_profiler() {
        if (!profiler_) {
            profiler_.emplace();
        }
        return *profiler_;
    }

private:
    Scene scene_;
    HierarchyViewModel hierarchy_;
    PlaySession play_;
    ScriptCatalog scripts_;
    CommandStack commands_;
    std::string scene_path_;
    bool dirty_ = false;
    ProjectSettings project_;
    std::string project_path_;
    std::string project_root_;
    ResourceManager* resources_ = nullptr;
    PlayRequest play_request_ = PlayRequest::None;
    FileRequest file_request_ = FileRequest::None;
    bool confirming_ = false;
    std::string confirm_message_;
    std::optional<Profiler> profiler_;
    ViewportMode viewport_mode_ = ViewportMode::Auto;
};

} // namespace editor
} // namespace fe
