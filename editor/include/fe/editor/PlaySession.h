// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <functional>
#include <string>
#include <vector>

namespace fe::editor {

// Play-in-editor: runs a *copy* of the authored scene against the editor's own
// embedded `Engine`, so the game sees the real runtime while the editor keeps
// the authored scene untouched for when play stops.
//
// This is the piece ADR-0001 and docs/ARCHITECTURE.md section 6 anticipate: the
// editor embeds an `Engine` and drives frames itself, so the viewport and
// play-in-editor share the exact runtime a shipped game uses, with no second
// code path and no fork of `Application`. `PlaySession::tick()` performs the
// engine tick and the 2D pass into the editor's window; the editor's loop owns
// event polling and presentation. No second window or GL context is created,
// which keeps context ownership simple and the editor's UI toolkit bound to the
// one context it knows about.
//
// Isolation: the play scene is a deep copy of the authored scene, made through
// a component registry so any component type can participate without this class
// knowing its concrete type. This copy lives in the editor, not in `scene`,
// because it needs to know how to clone components and `scene` is deliberately
// pure data (ADR-0002).
//
// Known limitation (M2): components that reference GPU resources (a `Sprite2D`
// texture) are shared by pointer in the copy, not reloaded. That is correct for
// rendering and matches the engine's `shared_ptr` resource ownership
// (ADR-0004), but a play session that mutated its own assets would alias the
// authored ones. Full play-world separation is deferred until the asset
// pipeline lands.
class PlaySession {
public:
    // Clones one component from `source_node` onto `destination_node`. The
    // registry is a list of these so `PlaySession` stays component-agnostic.
    using ComponentFactory = std::function<void(const Node& source_node, Node& destination_node)>;

    PlaySession() = default;

    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    // Registers a factory for one component type. Registering a factory for a
    // type that already has one replaces it. Only registered types are copied
    // into the play scene; an unregistered component type is reported by name
    // through `last_copy_warning()` rather than being silently dropped.
    void register_component_factory(std::string type_name, ComponentFactory factory);

    // Copies `authored_scene` into `out` using the registered factories without
    // touching any engine. Exposed so the copy can be tested headlessly and
    // reused by a viewport. `out` is cleared first.
    void copy_scene(const Scene& authored_scene, Scene& out) const;

    // Enters the `Playing` state and copies `authored_scene` into the play
    // scene. Fails if already playing. The caller passes the engine whose frames
    // will drive the play scene to `tick()`.
    [[nodiscard]] Error start(const Scene& authored_scene);

    // Advances `engine`'s clock and draws one frame of the play scene into the
    // engine's current framebuffer. Returns the number of sprites drawn (a value
    // of 0 is valid). Fails if not playing.
    //
    // The caller owns the frame: it must have called Device::begin_frame() and
    // must call Device::end_frame() afterwards. This lets the editor draw its
    // ImGui panels over the rendered scene in the same frame.
    [[nodiscard]] Result<usize> tick(Engine& engine);

    // Leaves the play state and releases the play scene. The authored scene was
    // never touched, so the editor resumes editing it exactly as it was.
    void stop();

    [[nodiscard]] bool is_playing() const noexcept { return state_ == State::Playing; }

    // The live play scene. Read-only: the editor must not author into it while
    // playing.
    [[nodiscard]] const Scene& play_scene() const noexcept { return play_scene_; }

    [[nodiscard]] u32 frames_played() const noexcept { return frames_played_; }
    [[nodiscard]] usize last_sprite_count() const noexcept { return last_sprite_count_; }
    [[nodiscard]] f64 play_time() const noexcept { return play_time_; }

    // The first unregistered component type seen during the last copy, or ""
    // if every component was copied. Surfaced so the editor can warn once.
    // Mutable because `copy_scene` is const: the copy must not modify the
    // session, but recording what it could not reproduce is diagnostic output,
    // not session state.
    [[nodiscard]] const std::string& last_copy_warning() const noexcept { return last_copy_warning_; }

private:
    enum class State { Stopped, Playing };

    void copy_components_of(const Node& source_node, Node& destination_node) const;

    struct FactoryEntry {
        std::string type_name;
        ComponentFactory factory;
    };

    State state_ = State::Stopped;
    Scene play_scene_{};
    std::vector<FactoryEntry> factories_;
    mutable std::string last_copy_warning_;
    u32 frames_played_ = 0;
    usize last_sprite_count_ = 0;
    f64 play_time_ = 0.0;
};

} // namespace fe::editor
