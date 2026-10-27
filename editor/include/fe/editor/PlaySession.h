// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fe {

namespace audio {
class AudioSystem;
class AudioDevice;
} // namespace audio

} // namespace fe

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

    // Out of line because the class owns unique_ptrs to forward-declared audio
    // types: the default constructor must be able to destroy members if a later
    // member's constructor throws, and the deleting destructor must be
    // instantiated where those types are complete.
    PlaySession();
    ~PlaySession();

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
    //
    // On the first `start()` this registers the default systems
    // (`SpriteAnimationSystem` and `Physics2DSystem`) on the schedule, so
    // play-in-editor runs real gameplay rather than only rendering. A caller
    // that wants a custom set may clear `schedule()` and add its own; later
    // `start()` calls leave whatever is registered in place, so editor-added
    // systems survive a play/stop cycle.
    [[nodiscard]] Error start(const Scene& authored_scene);

    // Advances the play simulation by one fixed step, without an engine or a
    // GPU: runs the schedule's systems over the play scene with `context`. This
    // is what makes play testable headlessly and deterministically - a test
    // passes a known `delta_time` and asserts on the resulting scene state. The
    // engine-driven frame (tick) calls it with the engine's own clock, then
    // renders. Fails if not playing.
    [[nodiscard]] Error advance(const FrameContext& context);

    // Advances `engine`'s clock, runs the schedule's systems over the play
    // scene, then draws one frame of the play scene into the engine's current
    // framebuffer. Returns the number of sprites drawn (a value of 0 is valid).
    // Fails if not playing.
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

    // The schedule of systems that runs over the play scene each tick. The
    // editor may add systems here to make play-in-editor exercise gameplay the
    // runtime would otherwise only get from `Application`. Owned by the session;
    // the reference is stable for the session's lifetime.
    [[nodiscard]] SceneSchedule& schedule() noexcept { return schedule_; }
    [[nodiscard]] const SceneSchedule& schedule() const noexcept { return schedule_; }

    [[nodiscard]] u32 frames_played() const noexcept { return frames_played_; }
    [[nodiscard]] usize last_sprite_count() const noexcept { return last_sprite_count_; }
    [[nodiscard]] f64 play_time() const noexcept { return play_time_; }

    // Enables audio for play mode: adopts `engine`'s audio system and the
    // camera-listener system onto the schedule, and opens an output device if
    // the platform has one. Idempotent, so the host may call it once at startup.
    //
    // Separate from `start()` so a headless test that only wants the copy or the
    // physics path never opens a sound device, and so audio outlives a
    // play/stop cycle: the systems live on the schedule, which `stop()` does not
    // clear. A device that will not open is not an error -- the mixer runs
    // silently, which is what a CI machine with no sound card does, and what a
    // browser does until the first user gesture (ADR-0081).
    void enable_audio(Engine& engine);

    // The first unregistered component type seen during the last copy, or ""
    // if every component was copied. Surfaced so the editor can warn once.
    // Mutable because `copy_scene` is const: the copy must not modify the
    // session, but recording what it could not reproduce is diagnostic output,
    // not session state.
    [[nodiscard]] const std::string& last_copy_warning() const noexcept { return last_copy_warning_; }

private:
    enum class State { Stopped, Playing };

    void copy_components_of(const Node& source_node, Node& destination_node) const;

    // Registers the default gameplay systems on `schedule_` if they are not
    // already present. Idempotent, so it can run on every start() without
    // duplicating systems or discarding editor-added ones.
    void register_default_systems();

    struct FactoryEntry {
        std::string type_name;
        ComponentFactory factory;
    };

    State state_ = State::Stopped;
    Scene play_scene_{};
    // The audio device is declared before the schedule so it is destroyed
    // *after* it: the schedule owns the AudioSystem, which holds a raw pointer to
    // the device, so the device must outlive the system that plays into it.
    std::unique_ptr<audio::AudioDevice> audio_device_;
    SceneSchedule schedule_{};
    std::vector<FactoryEntry> factories_;
    mutable std::string last_copy_warning_;
    audio::AudioSystem* audio_system_ = nullptr;
    // The adopted audio system, held until register_default_systems() moves it
    // onto the schedule on the first start().
    std::unique_ptr<audio::AudioSystem> audio_system_owned_;
    bool audio_enabled_ = false;
    u32 frames_played_ = 0;
    usize last_sprite_count_ = 0;
    f64 play_time_ = 0.0;
};

} // namespace fe::editor
