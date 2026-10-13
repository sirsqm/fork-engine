// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/editor/HierarchyViewModel.h"
#include "fe/editor/PlaySession.h"
#include "fe/scene/Scene.h"

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

private:
    Scene scene_;
    HierarchyViewModel hierarchy_;
    PlaySession play_;
    std::string scene_path_;
    PlayRequest play_request_ = PlayRequest::None;
};

} // namespace editor
} // namespace fe
