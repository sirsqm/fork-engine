// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorHost.h"

#include "fe/core/Log.h"
#include "fe/editor/HierarchyPanel.h"
#include "fe/editor/PlayPanel.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/version/Version.h"

#include <memory>

namespace fe::editor {

EditorHost::EditorHost(Config config) : config_(std::move(config)) {}

EditorHost::~EditorHost() = default;

int EditorHost::run() {
    if (Error error = engine_.initialize(config_.engine); error) {
        FE_LOG_CRITICAL("editor: failed to start engine: %s", error.message().c_str());
        return 1;
    }

    // The document and its copy factories exist independently of the engine,
    // but must be registered before the first play request.
    context_.register_default_component_factories();

    if (!config_.scene_to_open.empty()) {
        if (Error error = context_.load_scene(config_.scene_to_open, &engine_.resources()); error) {
            FE_LOG_ERROR("editor: could not open '%s': %s", config_.scene_to_open.c_str(), error.message().c_str());
            if (Error fallback = context_.new_scene(&engine_.resources()); fallback) {
                FE_LOG_ERROR("editor: could not build the example scene: %s", fallback.message().c_str());
            }
        }
    } else {
        if (Error error = context_.new_scene(&engine_.resources()); error) {
            FE_LOG_ERROR("editor: could not build the example scene: %s", error.message().c_str());
        }
    }

    if (Error error = ui_.initialize(engine_.window()); error) {
        FE_LOG_CRITICAL("editor: failed to initialise the UI: %s", error.message().c_str());
        // Release the document before the engine, as the runtime does.
        context_.scene().clear();
        engine_.shutdown();
        return 1;
    }
    ui_.add_panel(std::make_unique<HierarchyPanel>());
    ui_.add_panel(std::make_unique<PlayPanel>());

    FE_LOG_INFO("editor: ready (fork engine v%s)", version::kEngineVersionString);

    u32 frame = 0;
    while (!engine_.window().should_close()) {
        engine_.window().input().begin_frame();
        engine_.window().poll_events();

        handle_play_request();

        // One frame: clear once, draw the scene, then the UI over it. When
        // playing, the scene drawn is the play copy, and the engine's clock is
        // advanced by PlaySession::tick; otherwise the authored scene is drawn
        // and no simulation time passes.
        ui_.begin_frame();
        ui_.draw_panels(context_);

        if (context_.play().is_playing()) {
            // tick() begins the frame and draws the play scene; end_frame() is
            // called by the host after the UI is rendered.
            auto result = context_.play().tick(engine_);
            if (!result) {
                FE_LOG_ERROR("editor: play render failed: %s", result.error().message().c_str());
            }
        } else {
            render_scene(context_.scene());
        }

        ui_.end_frame();
        engine_.device().end_frame();

        ++frame;
        if (config_.max_frames != 0 && frame >= config_.max_frames) {
            FE_LOG_INFO("editor: reached the frame limit (%u frames), stopping", frame);
            break;
        }
    }

    ui_.shutdown();
    // Release the play scene, then the authored scene, then the engine. Scene
    // tears down in reverse of the runtime's documented order.
    context_.play().stop();
    context_.scene().clear();
    engine_.shutdown();
    return 0;
}

void EditorHost::handle_play_request() {
    switch (context_.take_play_request()) {
    case PlayRequest::None:
        return;
    case PlayRequest::Start: {
        if (context_.play().is_playing()) {
            return;
        }
        if (Error error = context_.play().start(context_.scene()); error) {
            FE_LOG_ERROR("editor: could not start play: %s", error.message().c_str());
            return;
        }
        // Play frames are driven by the runtime, so the hierarchy keeps showing
        // the authored scene; selection points at nodes the user still owns.
        return;
    }
    case PlayRequest::Stop:
        context_.play().stop();
        return;
    }
}

void EditorHost::render_scene(const Scene& scene) {
    const u32 width = engine_.window().framebuffer_width();
    const u32 height = engine_.window().framebuffer_height();

    rhi::ClearColor clear{};
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(scene); camera != nullptr) {
        clear = rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }

    engine_.device().begin_frame(width, height, clear);
    auto result = engine_.renderer_2d().render(engine_.device(), scene, width, height);
    if (!result) {
        FE_LOG_ERROR("editor: render failed: %s", result.error().message().c_str());
    }
}

} // namespace fe::editor
