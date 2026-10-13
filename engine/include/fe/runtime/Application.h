// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Profiler.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Scene.h"
#include "fe/systems/SceneSchedule.h"

#include <string>

namespace fe {

// A runnable game: an Engine plus a Scene and a frame loop.
//
// Application is the thin layer between "the engine works" and "a game runs".
// It owns the standard loop:
//
//   tick clock -> poll events -> begin frame -> update -> render -> present
//
// Subclasses override a small set of hooks. The defaults do nothing, so a
// subclass only implements what it needs.
//
// This is deliberately not the editor's loop. The editor drives Engine frames
// itself so it can render a viewport without a window-owned swap chain; the two
// loops share the Engine, not each other.
class Application {
public:
    explicit Application(Engine::Config config = {});
    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Runs until the window closes or request_exit() is called. Returns the
    // process exit code (0 on a clean run). Never throws.
    [[nodiscard]] int run();

    void request_exit() noexcept { exit_requested_ = true; }

    [[nodiscard]] Engine& engine() noexcept { return engine_; }
    [[nodiscard]] Scene& scene() noexcept { return scene_; }
    [[nodiscard]] const Scene& scene() const noexcept { return scene_; }

    // The systems run once per frame, in registration order, after on_update()
    // and before rendering. A subclass adds its systems here -- typically in
    // on_start() -- and the base class drives them. The editor pairs a schedule
    // with its own scene the same way, which is what lets play-in-editor run the
    // user's systems through the identical path.
    [[nodiscard]] SceneSchedule& schedule() noexcept { return schedule_; }
    [[nodiscard]] const SceneSchedule& schedule() const noexcept { return schedule_; }

    [[nodiscard]] bool headless() const noexcept { return headless_; }

    // --- Profiling ---------------------------------------------------------
    // The application's profiler. The frame loop installs it as the thread's
    // current profiler for the duration of each frame and brackets the frame,
    // the update phase (hooks + systems), and the render phase with named
    // scopes, so a frame's breakdown is recorded without the caller adding a
    // macro. Systems are timed individually by the schedule ("system/<name>").
    //
    // Recording is cheap and allocation-free; it is on by default. Call
    // set_profiling(false) to skip begin_frame()/end_frame() and pay only the
    // (idle) scope checks -- useful when a host wants zero profiler activity.
    [[nodiscard]] Profiler& profiler() noexcept { return profiler_; }
    [[nodiscard]] const Profiler& profiler() const noexcept { return profiler_; }

    void set_profiling(bool enabled) noexcept { profiling_enabled_ = enabled; }
    [[nodiscard]] bool profiling_enabled() const noexcept { return profiling_enabled_; }

    // A snapshot of the most recent completed frame, or an error when the frame
    // loop has not completed a frame yet (or profiling is disabled). This is the
    // read-only surface a test, a log, or the editor reads; it does not expose
    // the ring.
    [[nodiscard]] Result<FrameReport> last_frame_report(usize frame_offset = 0) const;

    // A headless run initialises the engine, runs a fixed number of frames, and
    // exits without presenting. Used by automated tests and CI, where there is
    // no display. Frame count 0 means "run until the window closes".
    //
    // This is public because the entry point configures it before run(), and it
    // is orthogonal to the subclass hooks.
    void set_headless(u32 frame_count) noexcept {
        headless_ = true;
        headless_frames_ = frame_count;
    }

protected:
    // --- Hooks -------------------------------------------------------------
    // Called once, after the engine is up and before the first frame. Return an
    // Error to abort startup.
    virtual Error on_start() { return Error::none(); }

    // Called once per frame before rendering. `delta_time` is in seconds.
    virtual void on_update(f32 delta_time) { (void)delta_time; }

    // Called once per frame after the 2D pass, inside the frame.
    virtual void on_render(f32 delta_time) { (void)delta_time; }

    // Called once, after the loop ends and before the engine shuts down.
    virtual void on_stop() {}

    // --- Options -----------------------------------------------------------
    // Number of sprites drawn in the most recent frame, for statistics.
    [[nodiscard]] usize last_sprite_count() const noexcept { return last_sprite_count_; }

private:
    void render_frame();

    Engine engine_;
    Engine::Config config_;
    Scene scene_;
    SceneSchedule schedule_;
    Profiler profiler_;
    bool profiling_enabled_ = true;
    bool exit_requested_ = false;
    bool headless_ = false;
    u32 headless_frames_ = 0;
    usize last_sprite_count_ = 0;
};

} // namespace fe
