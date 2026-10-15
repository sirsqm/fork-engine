// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/Application.h"

#include "fe/core/Log.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/scene/Camera2D.h"

namespace fe {

Application::Application(Engine::Config config) : engine_(), config_(config), scene_() {}

Application::~Application() = default;

int Application::run() {
    if (Error error = engine_.initialize(config_); error) {
        FE_LOG_CRITICAL("failed to start engine: %s", error.message().c_str());
        return 1;
    }

    if (Error error = on_start(); error) {
        FE_LOG_CRITICAL("failed to start application: %s", error.message().c_str());
        scene_.clear();
        engine_.shutdown();
        return 1;
    }

    // Create the frame's arena once, before the loop, and keep it for the run.
    // It is sized from the config; a capacity of 0 leaves it null, so systems see
    // a null arena and the run behaves exactly as it did before the arena
    // existed. One allocation for the whole run, not one per frame (ADR-0051).
    if (frame_arena_bytes_ > 0) {
        frame_arena_ = std::make_unique<FrameArena>(frame_arena_bytes_);
        FE_LOG_INFO("frame arena: %zu bytes", frame_arena_bytes_);
    }

    FE_LOG_INFO("entering main loop (%s)", headless_ ? "headless" : "interactive");

    // Install the application's profiler on this thread for the whole run, so
    // the frame loop and every system that records through FE_PROFILE_SCOPE land
    // in the same ring. Cleared on exit so a later test cannot reach a destroyed
    // Application. (Recording is per-thread: a worker thread would set its own.)
    if (profiling_enabled_) {
        Profiler::set_current(&profiler_);
    }

    u32 frame_index = 0;
    while (!exit_requested_) {
        if (headless_ && headless_frames_ > 0 && frame_index >= headless_frames_) {
            break;
        }
        if (!headless_ && engine_.window().should_close()) {
            break;
        }

        // Bracket the whole frame so the report has a total to compare the parts
        // against. When profiling is off this is a no-op, and begin_frame() is
        // skipped, so no frame is committed.
        const bool recording = profiling_enabled_ && profiler_.begin_frame();
        const ProfileScope frame_scope("frame");

        engine_.tick();

        // Ordering: advance the input edges, then let the OS deliver this
        // frame's events into that fresh state.
        engine_.window().input().begin_frame();
        engine_.window().poll_events();

        {
            const ProfileScope update_scope("frame/update");
            on_update(engine_.delta_time());

            // Run the user's systems for this frame. This is the whole execution
            // model: after the per-frame hook, before rendering, in the order the
            // systems were added. Each system is timed by the schedule.
            //
            // Reset the frame arena first: everything a system allocated from it
            // last frame is dead by definition (nothing may outlive the frame), so
            // the whole buffer is reclaimed in one pointer move before this
            // frame's systems allocate. Resetting before the systems run, not
            // after, means a system can allocate and the last system's data is
            // still valid while it runs.
            if (frame_arena_ != nullptr) {
                frame_arena_->reset();
            }
            FrameContext frame;
            frame.delta_time = engine_.delta_time();
            frame.elapsed_time = engine_.elapsed_time();
            frame.resources = &engine_.resources();
            frame.input = &engine_.input();
            frame.arena = frame_arena_.get();
            schedule_.update(frame, scene_);
        }

        {
            const ProfileScope render_scope("frame/render");
            // A plain headless engine has no device or renderer, so there is
            // nothing to draw: the frame still advances the clock, the input
            // edges and the systems. A headless engine that asked to render has
            // a null RHI device, so the render path (extraction, batching, draw
            // submission) runs and only rasterisation is absent. Both cases are
            // explicit here rather than a no-op device pretending to render.
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

        ++frame_index;
    }

    if (profiling_enabled_) {
        Profiler::set_current(nullptr);
    }

    on_stop();

    // Release the frame arena with the loop. It holds only trivially
    // destructible frame data, so it needs no scene/device ordering like the
    // scene does below; it just must not outlive the run it belongs to.
    frame_arena_.reset();

    // Release the scene *before* the graphics device and window. Scene
    // components can own GPU-backed resources (a Sprite2D holds a Texture2D),
    // and their destructors call into the device, which requires a live GL
    // context. Tearing the context down first would make those releases
    // undefined behaviour.
    scene_.clear();
    engine_.shutdown();
    return 0;
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

    auto result = engine_.renderer_2d().render(engine_.device(), scene_, width, height);
    if (!result) {
        FE_LOG_ERROR("render failed: %s", result.error().message().c_str());
        last_sprite_count_ = 0;
    } else {
        last_sprite_count_ = result.value();
    }

    on_render(engine_.delta_time());
}

} // namespace fe
