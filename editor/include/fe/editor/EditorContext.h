// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/editor/Command.h"
#include "fe/editor/Commands.h"
#include "fe/editor/HierarchyViewModel.h"
#include "fe/editor/PlaySession.h"
#include "fe/scene/Scene.h"

#include <memory>
#include <string>
#include <string_view>

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

    // Replaces the document with a newly built example scene (a camera and a
    // few sprites). `resources` may be null, in which case sprites are created
    // without textures. Clears the path and the hierarchy.
    [[nodiscard]] Error new_scene(ResourceManager* resources = nullptr);

    // Loads a scene file into the document. `resources` may be null. On failure
    // the existing document is left untouched.
    [[nodiscard]] Error load_scene(std::string_view path, ResourceManager* resources = nullptr);

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
    // This is the first slice of "project settings": a real project is a file
    // the editor opens, but the only setting that changes engine behaviour today
    // is where the assets live, so that is what this carries. It is deliberately
    // on the document, not on the engine: the engine's resource manager is
    // reusable and is told the root here, so the same manager serves the editor
    // and, later, a running game.
    [[nodiscard]] const std::string& project_root() const noexcept { return project_root_; }

    // Sets the project's asset root and rescans the asset database through
    // `resources`. A null manager or an empty root clears the index. Rescanning
    // on every set keeps the database and this field in step; the scan is cheap
    // (a directory walk) and only happens when the root changes.
    void set_project_root(std::string root, ResourceManager* resources = nullptr);

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

    // --- Editing -----------------------------------------------------------
    // The undo/redo history. Every scene edit made through the editor goes
    // through here, so it is reversible; see docs/adr/0015-editor-undo-redo.md.
    [[nodiscard]] CommandStack& commands() noexcept { return commands_; }
    [[nodiscard]] const CommandStack& commands() const noexcept { return commands_; }

    // Executes `command` against the scene and refreshes the hierarchy, which
    // every structural command can change. Returns true when the command was
    // applied. Coalescing is handled by the stack.
    bool execute(std::unique_ptr<Command> command);

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

    // Ends the current coalescing group, so the next property edit is a new undo
    // step. The panels call this when an edit gesture finishes.
    void break_undo_coalescing() noexcept { commands_.break_coalescing(); }

private:
    Scene scene_;
    HierarchyViewModel hierarchy_;
    PlaySession play_;
    CommandStack commands_;
    std::string scene_path_;
    std::string project_root_;
    ResourceManager* resources_ = nullptr;
    PlayRequest play_request_ = PlayRequest::None;
};

} // namespace editor
} // namespace fe
