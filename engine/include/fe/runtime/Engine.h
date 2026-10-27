// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Log.h"
#include "fe/core/Types.h"
#include "fe/platform/InputState.h"
#include "fe/platform/UserData.h"
#include "fe/platform/Window.h"
#include "fe/resources/ResourceManager.h"
#include "fe/rhi/Device.h"

#include <functional>
#include <memory>
#include <string>

namespace fe {

namespace renderer2d {
class Renderer2D;
}

namespace renderer3d {
class Renderer3D;
}

namespace audio {
class AudioSystem;
} // namespace audio

// Owns the engine's long-lived subsystems and their lifetimes.
//
// The Engine is the single place that knows the order in which subsystems are
// created and destroyed:
//
//   window  ->  device  ->  resource manager (with loaders)  ->  renderers
//
// Creation order matters because the resource manager needs a device to create
// GPU textures, and the device needs a current context from the window.
// Destruction happens in reverse. Concentrating this here means gameplay code
// and the future editor never have to reason about it.
//
// The Engine does not run a frame loop; that is Application's job. Keeping the
// two apart lets the editor embed an Engine and drive frames itself (for
// play-in-editor) without inheriting the standalone loop.
class Engine {
public:
    struct Config {
        WindowDesc window{};
        // Minimum log severity. Defaults to Info so a shipped game is quiet but
        // still reports warnings and errors.
        fe::LogLevel log_level = fe::LogLevel::Info;
        // Root of the content-addressed import cache. Empty (the default) leaves
        // the cache inert, so a run behaves exactly as it did before the cache
        // existed. A project sets it to a disposable directory (for example
        // "<project>/.fork/cache"). See docs/adr/0029-import-cache.md.
        std::string cache_root;
        // When the window has no graphics context (WindowBackend::Null), create
        // the null RHI device and the renderers so the render path -- extraction,
        // batching, and draw submission -- runs headless instead of being
        // skipped. Off by default: a plain headless run creates no device, which
        // is the ADR-0041 behaviour. Nothing is rasterised, so this proves
        // submission, not pixels. See docs/adr/0042-null-rhi-device.md.
        bool headless_render = false;
        // A fixed simulation step, in seconds. Zero (the default) steps by the
        // measured wall-clock delta, which is what a game wants. A positive value
        // makes every frame advance by exactly that much regardless of how long
        // the frame took, so a run is reproducible: the same scene and the same
        // input produce the same state on any machine, at any frame rate. It is
        // what a verification run and a headless capture need, and what a game
        // that wants a stable simulation would pin. See docs/adr/0079.
        f32 fixed_delta_time = 0.0f;
        // The id the per-user data store is named after, e.g. "breakout". Empty
        // (the default) leaves the store in memory and persistent nowhere, so a
        // run that never set an id cannot write to a shared location by accident.
        // A game that saves sets it. See docs/adr/0096-writable-user-data-storage.md.
        std::string app_id;
        // Overrides where the per-user data store lives. Empty (the default)
        // resolves the OS's conventional per-user directory under `app_id`. A
        // test sets it to a temporary directory, and a portable install sets it
        // beside the game so a save travels with it.
        std::string user_data_root;
    };

    // Declared but not defined here: the unique_ptr members need their types
    // complete wherever they are destroyed, and the audio type is only
    // forward-declared in this header. Defining the special members out of line
    // keeps the header free of the audio module.
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Brings up every subsystem. Returns an Error describing the first failure.
    [[nodiscard]] Error initialize(const Config& config);

    void shutdown();

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    // True when the engine came up with a graphics context and a device. False
    // for a headless run (a null window). When false, device(), renderer_2d()
    // and renderer_3d() must not be called: they have no object to return, and
    // code that renders must branch on this rather than assume a surface. This
    // is the one place the runtime asks "is there a surface?", and it is honest:
    // a null window cannot render. See
    // docs/adr/0041-platform-contract-and-null-backend.md.
    [[nodiscard]] bool has_graphics() const noexcept { return has_graphics_; }

    // True when the engine came up with a device and renderers but no OS
    // surface: a headless run with Engine::Config::headless_render set. The
    // render path executes against the null RHI device, so device(),
    // renderer_2d() and renderer_3d() are valid, but nothing reaches a screen
    // and read_pixels() yields no pixels. has_graphics() and
    // headless_render() are never both true. See
    // docs/adr/0042-null-rhi-device.md.
    [[nodiscard]] bool headless_render() const noexcept { return headless_render_; }

    [[nodiscard]] Window& window() noexcept { return *window_; }
    [[nodiscard]] const Window& window() const noexcept { return *window_; }

    // The frame's input snapshot, owned by the window. A gameplay system reaches
    // it through FrameContext::input rather than through the window directly, so
    // a system never depends on the platform layer.
    [[nodiscard]] InputState& input() noexcept { return window_->input(); }
    [[nodiscard]] const InputState& input() const noexcept { return window_->input(); }

    // Requires has_graphics(). A headless engine has no device to return.
    [[nodiscard]] rhi::Device& device() noexcept { return *device_; }
    [[nodiscard]] const rhi::Device& device() const noexcept { return *device_; }

    [[nodiscard]] ResourceManager& resources() noexcept { return *resources_; }
    [[nodiscard]] const ResourceManager& resources() const noexcept { return *resources_; }

    // The store a game saves its own data to (best score, settings). It always
    // exists once the engine is initialized: a run that set no `app_id`, or one
    // whose backend could not open, gets an in-memory store that persists
    // nothing rather than a null pointer, so a game that saves runs unchanged
    // and only loses the persistence. Check backend_name() == "null" to learn
    // whether a save will survive. See
    // docs/adr/0096-writable-user-data-storage.md.
    [[nodiscard]] UserData& user_data() noexcept { return *user_data_; }
    [[nodiscard]] const UserData& user_data() const noexcept { return *user_data_; }

    // Requires has_graphics().
    [[nodiscard]] renderer2d::Renderer2D& renderer_2d() noexcept { return *renderer_2d_; }
    [[nodiscard]] const renderer2d::Renderer2D& renderer_2d() const noexcept { return *renderer_2d_; }

    // Requires has_graphics().
    [[nodiscard]] renderer3d::Renderer3D& renderer_3d() noexcept { return *renderer_3d_; }
    [[nodiscard]] const renderer3d::Renderer3D& renderer_3d() const noexcept { return *renderer_3d_; }

    // The engine's audio system. It is created here so audio is ready as soon as
    // the engine is, but it is not registered with a schedule: the schedule
    // belongs to whoever owns the scene (Application, the sandbox, the editor),
    // and it adopts this system with take_audio_system() so the system runs in
    // the frame loop. The system starts with no device attached and runs
    // silently; a caller that wants sound opens a device (see
    // audio::create_audio_device) and attaches it with AudioSystem::set_device.
    // Audio output is deliberately not part of engine startup, so a machine with
    // no sound card still runs everything else.
    [[nodiscard]] audio::AudioSystem* audio_system() noexcept { return audio_system_.get(); }
    [[nodiscard]] const audio::AudioSystem* audio_system() const noexcept { return audio_system_.get(); }

    // Transfers the audio system to the caller, who normally hands it to a
    // SceneSchedule. After this the engine no longer owns it and audio_system()
    // returns nullptr. Defined out of line because the unique_ptr's deleter
    // needs AudioSystem complete and this header only forward-declares it.
    [[nodiscard]] std::unique_ptr<audio::AudioSystem> take_audio_system() noexcept;

    // Seconds elapsed since the previous frame, clamped to a sane maximum so a
    // long stall (a debugger breakpoint, a window drag) does not produce a huge
    // simulation step. When Config::fixed_delta_time is set this is that value,
    // so a run advances by a fixed step rather than by how long the frame took.
    [[nodiscard]] f32 delta_time() const noexcept { return delta_time_; }

    // Seconds elapsed since initialize().
    [[nodiscard]] f64 elapsed_time() const noexcept { return elapsed_time_; }

    // Advances the clock. Called once per frame by Application.
    void tick();

    // --- Page visibility (web) ---------------------------------------------
    // A backgrounded browser tab stops delivering animation frames, so the wall
    // clock keeps running while the simulation does not. The frame that follows
    // the tab becoming visible again would otherwise see the whole hidden span
    // as one delta. notify_page_visibility() lets the host report the browser's
    // visibilitychange event so tick() can rebaseline the clock on the first
    // frame after the tab returns, bounding the step to a single frame
    // (ADR-0082). The web backend records the event and Application forwards it;
    // a host that calls tick() directly reports it itself.
    //
    // A desktop run never calls this, so its clock is unchanged.
    void notify_page_visibility(bool visible) noexcept;

    // The wall-clock source tick() measures the delta from, in seconds. Defaults
    // to a steady clock. A host that already knows the frame time -- a browser's
    // requestAnimationFrame timestamp, a replay, a test -- can supply it here so
    // the delta comes from the host rather than from a second measurement.
    void set_time_source(std::function<f64()> source);

private:
    bool initialized_ = false;
    bool has_graphics_ = false;
    bool headless_render_ = false;
    std::unique_ptr<Window> window_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<ResourceManager> resources_;
    std::unique_ptr<UserData> user_data_;
    std::unique_ptr<renderer2d::Renderer2D> renderer_2d_;
    std::unique_ptr<renderer3d::Renderer3D> renderer_3d_;
    std::unique_ptr<audio::AudioSystem> audio_system_;

    f32 delta_time_ = 0.0f;
    f32 fixed_delta_time_ = 0.0f;
    f64 elapsed_time_ = 0.0;
    f64 last_tick_time_ = 0.0;
    // The wall-clock source; a steady clock unless a host replaced it.
    std::function<f64()> time_source_;
    // True when the host reported the page hidden (a backgrounded tab). The
    // next visible report rebaselines the clock so the hidden span is not one
    // huge delta. A desktop run leaves both false and the clock is untouched.
    bool page_hidden_ = false;
    bool rebaseline_clock_ = false;
};

} // namespace fe
