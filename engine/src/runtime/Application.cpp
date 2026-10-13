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

    FE_LOG_INFO("entering main loop (%s)", headless_ ? "headless" : "interactive");

    u32 frame_index = 0;
    while (!exit_requested_) {
        if (headless_ && headless_frames_ > 0 && frame_index >= headless_frames_) {
            break;
        }
        if (!headless_ && engine_.window().should_close()) {
            break;
        }

        engine_.tick();

        // Ordering: advance the input edges, then let the OS deliver this
        // frame's events into that fresh state.
        engine_.window().input().begin_frame();
        engine_.window().poll_events();

        on_update(engine_.delta_time());
        render_frame();

        // Presents the back buffer (or, headless, simply ends the frame). The
        // frame was fully rendered either way, so the pipeline is exercised.
        engine_.device().end_frame();

        ++frame_index;
    }

    on_stop();

    // Release the scene *before* the graphics device and window. Scene
    // components can own GPU-backed resources (a Sprite2D holds a Texture2D),
    // and their destructors call into the device, which requires a live GL
    // context. Tearing the context down first would make those releases
    // undefined behaviour.
    scene_.clear();
    engine_.shutdown();
    return 0;
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
