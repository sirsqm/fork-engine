// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/Application.h"

#include "fe/core/Log.h"
#include "fe/platform/InputActions.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/scene/Camera2D.h"

namespace fe {

Application::Application(Engine::Config config) : engine_(), config_(config), scene_() {}

Application::~Application() = default;

int Application::run() {
    if (Error error = initialize(); error) {
        // initialize() logged the specific failure; a failed start needs no
        // shutdown, so the run ends here.
        return 1;
    }

    while (step_frame()) {
    }

    shutdown();
    return 0;
}

Error Application::initialize() {
    if (initialized_) {
        return Error("Application::initialize called twice");
    }

    if (Error error = engine_.initialize(config_); error) {
        FE_LOG_CRITICAL("failed to start engine: %s", error.message().c_str());
        return error;
    }

    if (Error error = on_start(); error) {
        FE_LOG_CRITICAL("failed to start application: %s", error.message().c_str());
        scene_.clear();
        engine_.shutdown();
        return error;
    }

    // Create the frame's arena once, before the loop, and keep it for the run.
    // It is sized from the config; a capacity of 0 leaves it null, so systems see
    // a null arena and the run behaves exactly as it did before the arena
    // existed. One allocation for the whole run, not one per frame (ADR-0051).
    if (frame_arena_bytes_ > 0) {
        frame_arena_ = std::make_unique<FrameArena>(frame_arena_bytes_);
        FE_LOG_INFO("frame arena: %zu bytes", frame_arena_bytes_);
    }

    // Create the run's job system. With worker_count_ == 0 it is single-threaded
    // and every job runs inline on the frame thread, so a run that did not ask
    // for workers behaves exactly as it did before the pool existed. The pool is
    // joined in shutdown(), so no job outlives the frame loop (ADR-0017,
    // ADR-0068).
    jobs_ = std::make_unique<JobSystem>(worker_count_);
    FE_LOG_INFO("job system: %u worker threads", jobs_->worker_count());

    FE_LOG_INFO("entering main loop (%s)", headless_ ? "headless" : "interactive");

    // Install the application's profiler on this thread for the whole run, so
    // the frame loop and every system that records through FE_PROFILE_SCOPE land
    // in the same ring. Cleared in shutdown() so a later test cannot reach a
    // destroyed Application. (Recording is per-thread: a worker thread would set
    // its own.)
    if (profiling_enabled_) {
        Profiler::set_current(&profiler_);
    }

    frames_stepped_ = 0;
    initialized_ = true;
    return Error::none();
}

bool Application::step_frame() {
    if (!initialized_) {
        return false;
    }

    // The same stop conditions run()'s loop tests at the top of each iteration,
    // in the same order: an explicit exit, then the headless frame count, then
    // the window. Reaching one means the loop is done and the caller must stop.
    if (exit_requested_) {
        return false;
    }
    if (headless_ && headless_frames_ > 0 && frames_stepped_ >= headless_frames_) {
        return false;
    }
    if (!headless_ && engine_.window().should_close()) {
        return false;
    }

    // Bracket the whole frame so the report has a total to compare the parts
    // against. When profiling is off this is a no-op, and begin_frame() is
    // skipped, so no frame is committed.
    const bool recording = profiling_enabled_ && profiler_.begin_frame();
    const ProfileScope frame_scope("frame");

    // If the platform recorded any page-visibility transitions — a backgrounded
    // browser tab — tell the engine about each, before it reads the clock. Drain
    // the whole queue: no frame runs while the page is hidden, so a tab that was
    // backgrounded and restored between two frames recorded *two* transitions
    // (hidden, then visible) and the engine must see the hidden one to know the
    // span was not simulation time (docs/adr/0082). A desktop window records
    // none, so this loop does not run there.
    bool page_hidden = false;
    while (engine_.window().take_page_visibility_change(page_hidden)) {
        engine_.notify_page_visibility(!page_hidden);
    }

    engine_.tick();

    // Ordering: advance the input edges, then let the OS deliver this frame's
    // events into that fresh state.
    engine_.window().input().begin_frame();
    engine_.window().poll_events();

    // Evaluate the gameplay actions from that snapshot, before any hook or system
    // reads them, so both the subclass's on_update() and its systems see the same
    // one-frame edges InputState just produced. The viewport a touch is
    // normalised against is the framebuffer size, which is the space the
    // backend's touch positions are in.
    {
        Vec2 viewport = action_viewport_;
        if (viewport.x <= 0.0f || viewport.y <= 0.0f) {
            viewport = Vec2{static_cast<f32>(engine_.window().framebuffer_width()),
                            static_cast<f32>(engine_.window().framebuffer_height())};
        }
        actions_.update(engine_.window().input(), viewport);
    }

    {
        const ProfileScope update_scope("frame/update");
        on_update(engine_.delta_time());

        // Run the user's systems for this frame. This is the whole execution
        // model: after the per-frame hook, before rendering, in the order the
        // systems were added. Each system is timed by the schedule.
        //
        // Reset the frame arena first: everything a system allocated from it
        // last frame is dead by definition (nothing may outlive the frame), so
        // the whole buffer is reclaimed in one pointer move before this frame's
        // systems allocate. Resetting before the systems run, not after, means a
        // system can allocate and the last system's data is still valid while it
        // runs.
        if (frame_arena_ != nullptr) {
            frame_arena_->reset();
        }
        FrameContext frame;
        frame.delta_time = engine_.delta_time();
        frame.elapsed_time = engine_.elapsed_time();
        frame.resources = &engine_.resources();
        frame.input = &engine_.input();
        frame.actions = &actions_;
        frame.arena = frame_arena_.get();
        frame.jobs = jobs_.get();
        schedule_.update(frame, scene_);
    }

    {
        const ProfileScope render_scope("frame/render");
        // A plain headless engine has no device or renderer, so there is nothing
        // to draw: the frame still advances the clock, the input edges and the
        // systems. A headless engine that asked to render has a null RHI device,
        // so the render path (extraction, batching, draw submission) runs and
        // only rasterisation is absent. Both cases are explicit here rather than
        // a no-op device pretending to render.
        if (engine_.has_graphics() || engine_.headless_render()) {
            render_frame();
            // Presents the back buffer. With a real window this flips the
            // surface; with the null backend it is a no-op.
            engine_.device().end_frame();
        }
    }

    if (recording) {
        if (Result<void> ended = profiler_.end_frame(); !ended) {
            FE_LOG_WARN("profiler: end_frame failed: %s", ended.error().message().c_str());
        }
    }

    ++frames_stepped_;
    return true;
}

void Application::shutdown() {
    if (!initialized_) {
        return;
    }
    initialized_ = false;

    if (profiling_enabled_) {
        Profiler::set_current(nullptr);
    }

    on_stop();

    // Release the frame arena with the loop. It holds only trivially
    // destructible frame data, so it needs no scene/device ordering like the
    // scene does below; it just must not outlive the run it belongs to.
    frame_arena_.reset();

    // Join the pool after the loop and before the scene is torn down. A worker
    // never touches the scene (a job extracts into the system's own data), so
    // this ordering is not load-bearing the way the arena's is; it is here so no
    // thread is alive past the point the frame loop stops driving it.
    jobs_.reset();

    // Release the scene *before* the graphics device and window. Scene
    // components can own GPU-backed resources (a Sprite2D holds a Texture2D),
    // and their destructors call into the device, which requires a live GL
    // context. Tearing the context down first would make those releases
    // undefined behaviour.
    scene_.clear();
    engine_.shutdown();
}

Result<FrameReport> Application::last_frame_report(usize frame_offset) const {
    if (!profiling_enabled_) {
        return Error("Application: profiling is disabled");
    }
    return profiler_.frame_report(frame_offset);
}

void Application::render_frame() {
    const u32 width = engine_.window().framebuffer_width();
    const u32 height = engine_.window().framebuffer_height();

    // Determine the clear colour from the active camera, so the engine does not
    // impose a background on the scene. Uses the renderer's own camera
    // selection so the clear colour and the projection always agree.
    rhi::ClearColor clear{};
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(scene_); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    engine_.device().begin_frame(width, height, clear);

    // Prefer the arena-backed path when the frame has an arena: the 2D pass then
    // builds its extraction and cull scratch with no heap traffic. With the arena
    // disabled (frame_arena_ == nullptr) the heap path runs, which is the
    // documented opt-out and the pre-arena behaviour (ADR-0054).
    Result<usize> result =
        frame_arena_ != nullptr
            ? engine_.renderer_2d().render(engine_.device(), scene_, width, height, *frame_arena_, jobs_.get())
            : engine_.renderer_2d().render(engine_.device(), scene_, width, height, jobs_.get());
    if (!result) {
        FE_LOG_ERROR("render failed: %s", result.error().message().c_str());
        last_sprite_count_ = 0;
    } else {
        last_sprite_count_ = result.value();
    }

    on_render(engine_.delta_time());
}

} // namespace fe
