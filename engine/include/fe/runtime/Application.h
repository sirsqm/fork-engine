// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/FrameArena.h"
#include "fe/core/Profiler.h"
#include "fe/jobs/JobSystem.h"
#include "fe/platform/InputActions.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Scene.h"
#include "fe/systems/SceneSchedule.h"

#include <memory>
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
    //
    // This is the desktop driver: initialize(), step until the loop is done,
    // shutdown(). A host that owns the frame schedule -- a browser page calling
    // requestAnimationFrame, a test stepping by hand -- uses those three
    // primitives directly instead of run(), which is why they are public.
    [[nodiscard]] int run();

    // --- Driving the loop yourself -----------------------------------------
    // Brings the engine up, runs on_start(), creates the frame arena and the
    // job system, and installs the profiler: everything run() does before its
    // first frame. Returns the first failure and leaves nothing half-started, so
    // a failed start needs no shutdown(). Calling it twice is an error.
    [[nodiscard]] Error initialize();

    // Advances exactly one frame -- the same order of work run() performs
    // (profiler bracket, clock, input edges, events, update hooks and systems,
    // render, present) -- and returns true. Returns false when the loop is
    // finished for the same reasons run() stops: an exit was requested, a
    // window asked to close, or a headless run reached its frame count. A caller
    // that gets false must not step again.
    bool step_frame();

    // Tears the run down in the order run() used: on_stop(), then the arena, the
    // job system, the scene, and the engine. Safe after initialize(); a second
    // call, or a call after a failed initialize(), is a no-op.
    void shutdown();

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

    // --- Per-frame arena ---------------------------------------------------
    // The application owns one FrameArena and resets it once per frame, before
    // the systems run, then hands it to every system through FrameContext::arena.
    // Systems build their transient data (scratch lists, extraction arrays) from
    // it instead of the heap, which removes the per-frame malloc/free churn
    // ADR-0017 names (ADR-0051, ADR-0053).
    //
    // The arena is created in run(), sized from the config, and destroyed with
    // the application; before run() and after it returns, arena() is null. The
    // default capacity is generous enough that a typical frame never overflows;
    // a system that allocates a large, data-dependent amount should still treat a
    // null from FrameArena::allocate as an expected, recoverable condition.
    //
    // Disable it with set_frame_arena_bytes(0) for a run that wants the exact
    // pre-arena behaviour (systems get a null arena).
    void set_frame_arena_bytes(usize bytes) noexcept { frame_arena_bytes_ = bytes; }

    // The frame's arena, or null when it is disabled or the loop has not started.
    // A system reaches this through FrameContext::arena; this accessor is for the
    // application itself and for tests.
    [[nodiscard]] FrameArena* arena() noexcept { return frame_arena_.get(); }
    [[nodiscard]] const FrameArena* arena() const noexcept { return frame_arena_.get(); }

    // --- Job system --------------------------------------------------------
    // The application owns one JobSystem and hands it to every system through
    // FrameContext::jobs, so a system that can parallelise its work has a pool to
    // use without owning one (ADR-0017, ADR-0068).
    //
    // It is single-threaded by default (every job runs inline on the calling
    // thread), so adding it to the frame loop changes no behaviour until a worker
    // count is asked for. Set the count before run(); it is fixed for the run, so
    // a system never sees the pool change size underneath it. The pool is created
    // in run() and joined when run() returns, so no job outlives the loop.
    void set_worker_count(u32 workers) noexcept { worker_count_ = workers; }
    [[nodiscard]] u32 worker_count() const noexcept { return worker_count_; }

    // The run's job system, or null before run() starts and after it returns.
    // A system reaches it through FrameContext::jobs.
    [[nodiscard]] JobSystem* jobs() noexcept { return jobs_.get(); }
    [[nodiscard]] const JobSystem* jobs() const noexcept { return jobs_.get(); }

    // --- Gameplay actions --------------------------------------------------
    // The application owns one ActionMap and re-evaluates it once per frame from
    // the input snapshot, then hands it to every system through
    // FrameContext::actions. A subclass declares its actions and bindings in
    // on_start() -- `actions().add("MoveLeft")` and the `bind_*` calls -- and its
    // systems then ask about actions rather than raw keys, which is what lets one
    // system serve a keyboard and a touchscreen (docs/PLAN.md,
    // `touch-input-actions`).
    //
    // Actions are evaluated before the systems run and after the backend has
    // delivered this frame's events, so a system sees the same one-frame edges
    // InputState provides.
    [[nodiscard]] ActionMap& actions() noexcept { return actions_; }
    [[nodiscard]] const ActionMap& actions() const noexcept { return actions_; }

    // The size a touch position is normalised against when the action map is
    // evaluated. Defaults to the window's framebuffer size, which is what a
    // touch position is in; override it only for a host that renders to a
    // surface of its own (the editor's viewport, for instance).
    void set_action_viewport(Vec2 viewport) noexcept { action_viewport_ = viewport; }

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
    // One arena for the frame, created in run() and reset before the systems run.
    // Null before run() starts and after it returns.
    std::unique_ptr<FrameArena> frame_arena_;
    // The run's job system, created in run() and joined when it returns. Null
    // before and after. Single-threaded when worker_count_ is 0.
    std::unique_ptr<JobSystem> jobs_;
    u32 worker_count_ = 0;
    // Default capacity for the per-frame arena. 1 MiB holds the transient data of
    // a frame (scratch lists, extraction arrays) with room to spare; a frame that
    // needs more has grown past what a bump allocator should serve and should say
    // so by overflowing, which the caller sees as a null from allocate().
    static constexpr usize kDefaultFrameArenaBytes = 1u << 20;
    usize frame_arena_bytes_ = kDefaultFrameArenaBytes;
    bool profiling_enabled_ = true;
    bool exit_requested_ = false;
    bool headless_ = false;
    // True between initialize() and shutdown(). step_frame() and shutdown() are
    // no-ops outside that window, so a caller that never started the loop cannot
    // advance or tear down a run that does not exist.
    bool initialized_ = false;
    // Frames completed since initialize(). This is the headless stop counter: it
    // replaces run()'s local frame_index now that the loop body lives in
    // step_frame().
    u32 frames_stepped_ = 0;
    u32 headless_frames_ = 0;
    usize last_sprite_count_ = 0;
    // The gameplay action map, re-evaluated each frame from the input snapshot.
    // Owned here rather than by the window so a host that drives frames itself
    // (a browser page, a test) gets the same action state the desktop loop does.
    ActionMap actions_;
    // Zero means "use the window's framebuffer size"; see set_action_viewport().
    Vec2 action_viewport_{};
};

} // namespace fe
