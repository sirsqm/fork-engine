// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorHost.h"

#include "fe/core/Log.h"
#include "fe/editor/AssetBrowserPanel.h"
#include "fe/editor/HierarchyPanel.h"
#include "fe/editor/InspectorPanel.h"
#include "fe/editor/PlayPanel.h"
#include "fe/editor/ProfilerPanel.h"
#include "fe/editor/ViewportPanel.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/version/Version.h"

#include <memory>

namespace fe::editor {

EditorHost::EditorHost(Config config) : config_(std::move(config)) {}

EditorHost::~EditorHost() = default;

rhi::RenderTarget* EditorHost::viewport_target() const noexcept {
    return viewport_panel_ != nullptr ? viewport_panel_->target() : nullptr;
}

Error EditorHost::initialize() {
    if (Error error = engine_.initialize(config_.engine); error) {
        FE_LOG_CRITICAL("editor: failed to start engine: %s", error.message().c_str());
        return error;
    }

    // The document and its copy factories exist independently of the engine,
    // but must be registered before the first play request.
    context_.register_default_component_factories();
    // Point the inspector's resource fields at the embedded engine's manager.
    context_.set_resources(&engine_.resources());

    // Load the gameplay modules before any scene opens, so a scene that records
    // a behaviour by name can attach it during deserialization (ADR-0061). A
    // module that will not load is reported but not fatal: the editor still
    // opens, and the inspector simply does not offer its behaviours.
    for (const std::string& module : config_.modules) {
        if (Error error = context_.scripts().load_module(module); error) {
            FE_LOG_ERROR("editor: could not load module '%s': %s", module.c_str(), error.message().c_str());
        } else {
            FE_LOG_INFO("editor: loaded module '%s'", module.c_str());
        }
    }

    // Establish the project before any scene is loaded, so GUID references
    // inside the scene resolve during deserialization (the asset database must
    // be indexed first). A project file supplies the asset root and a startup
    // scene; without one, an explicit --project root is honoured. Both are
    // no-ops that leave the browser empty when neither is given.
    std::string scene_to_open = config_.scene_to_open;
    if (!config_.project_file.empty()) {
        if (Error error = context_.load_project(config_.project_file, &engine_.resources()); error) {
            FE_LOG_ERROR("editor: could not open project '%s': %s", config_.project_file.c_str(),
                         error.message().c_str());
        } else if (scene_to_open.empty()) {
            scene_to_open = context_.project().resolved_startup_scene();
        }
    } else if (!config_.project_root.empty()) {
        context_.set_project_root(config_.project_root, &engine_.resources());
    }

    if (!scene_to_open.empty()) {
        if (Error error = context_.load_scene(scene_to_open, &engine_.resources()); error) {
            FE_LOG_ERROR("editor: could not open '%s': %s", scene_to_open.c_str(), error.message().c_str());
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
        // The UI failed, but the engine is up. Let shutdown() unwind in order.
        initialized_ = true;
        shutdown();
        return error;
    }

    ui_.add_panel(std::make_unique<HierarchyPanel>());
    ui_.add_panel(std::make_unique<InspectorPanel>());
    ui_.add_panel(std::make_unique<AssetBrowserPanel>());
    ui_.add_panel(std::make_unique<PlayPanel>());
    ui_.add_panel(std::make_unique<ProfilerPanel>());

    // The profiler panel always reads a profiler, so create it up front. It is
    // created here rather than by the panel so the panel stays a pure reader and
    // the object's lifetime is the document's.
    context_.ensure_profiler();

    // The viewport renders through the embedded engine's device and 2D renderer,
    // and must release its off-screen target before the device shuts down.
    auto viewport = std::make_unique<ViewportPanel>();
    viewport->initialize(engine_.device(), engine_.renderer_2d());
    viewport_panel_ = viewport.get();
    ui_.add_panel(std::move(viewport));

    initialized_ = true;
    FE_LOG_INFO("editor: ready (fork engine v%s)", version::kEngineVersionString);
    return Error::none();
}

bool EditorHost::should_close() const {
    // A non-zero frame limit with a hidden window is the headless smoke test,
    // where nothing can close the window for us.
    if (config_.max_frames != 0 && frame_ >= config_.max_frames) {
        return true;
    }
    return engine_.window().should_close();
}

void EditorHost::step() {
    engine_.window().input().begin_frame();
    engine_.window().poll_events();

    handle_play_request();

    // Hot-reload any gameplay module whose image was rebuilt on disk, before the
    // frame runs, so the frame that follows executes the new code (ADR-0066).
    // Poll-driven: the check is a stat per module, and a module that changed is
    // reloaded and its nodes rebound. Scripts are attached and detached between
    // frames, never from inside a script's update(), which is the host contract.
    // The reloaded ids are logged by the poll itself; the host has no further
    // use for them here.
    //
    // Two settled polls before a reload (ADR-0067): a build writes a module image
    // over a short window (GNU ld truncates and rewrites the file in place), so a
    // single-poll trigger can hand reload_module() a half-written image, which
    // fails to load and - by ADR-0061's policy - drops the module, losing the
    // bindings. Waiting for the write time to hold across two polls costs one
    // frame and skips that window.
    (void)context_.poll_script_modules(2);

    // Install the editor's profiler for the frame, so the scopes the UI and the
    // panels record land in it. The profiler panel reads the *previous* committed
    // frame while drawing, which is the honest read-out: the frame being
    // recorded is not finished yet.
    Profiler* profiler = context_.profiler();
    Profiler* previous_profiler = Profiler::current();
    if (profiler != nullptr) {
        Profiler::set_current(profiler);
        if (Result<void> begun = profiler->begin_frame(); !begun) {
            // A frame was already in progress (should not happen in this loop);
            // record nothing rather than corrupt the ring.
            FE_LOG_WARN("editor: profiler begin_frame failed: %s", begun.error().message().c_str());
        }
    }

    // One frame: clear once, draw the scene, then the UI over it. When playing,
    // the scene drawn is the play copy, and the engine's clock is advanced by
    // PlaySession::tick; otherwise the authored scene is drawn and no simulation
    // time passes.
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

    if (profiler != nullptr) {
        if (Result<void> ended = profiler->end_frame(); !ended) {
            FE_LOG_WARN("editor: profiler end_frame failed: %s", ended.error().message().c_str());
        }
        Profiler::set_current(previous_profiler);
    }

    ++frame_;
}

void EditorHost::shutdown() {
    if (!initialized_) {
        return;
    }
    initialized_ = false;

    // Release the viewport's off-screen target before the engine goes away: the
    // target owns GPU objects whose destructors call into the device. This must
    // happen before ui_.shutdown(), which destroys the panel that owns it.
    if (viewport_panel_ != nullptr) {
        viewport_panel_->shutdown();
        viewport_panel_ = nullptr;
    }
    if (ui_.is_initialized()) {
        ui_.shutdown();
    }
    // Release the play scene, then the authored scene, then the engine. Scene
    // teardown runs in reverse of the runtime's documented order so components
    // that own GPU-backed resources are gone before the device.
    context_.play().stop();
    context_.scene().clear();
    engine_.shutdown();
}

int EditorHost::run() {
    if (Error error = initialize(); error) {
        return 1;
    }

    while (!should_close()) {
        step();
    }

    if (config_.max_frames != 0 && frame_ >= config_.max_frames) {
        FE_LOG_INFO("editor: reached the frame limit (%u frames), stopping", frame_);
    }

    shutdown();
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
