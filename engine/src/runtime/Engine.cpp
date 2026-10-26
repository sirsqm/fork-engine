// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/Engine.h"

#include "fe/runtime/ComponentRegistration.h"

#include "fe/audio/AudioSystem.h"
#include "fe/audio/WavLoader.h"
#include "fe/core/Assert.h"
#include "fe/core/Log.h"
#include "fe/mesh/GltfLoader.h"
#include "fe/mesh/ObjLoader.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/resources/FontLoader.h"
#include "fe/resources/ImageLoader.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/version/Version.h"

#include <chrono>

namespace fe {
namespace {

[[nodiscard]] f64 now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<f64>(clock::now().time_since_epoch()).count();
}

// A frame longer than this is treated as a stall rather than gameplay time.
constexpr f32 kMaxDeltaTime = 0.25f;

} // namespace

Engine::Engine() = default;

Engine::~Engine() {
    shutdown();
}

void Engine::set_time_source(std::function<f64()> source) {
    if (source) {
        time_source_ = std::move(source);
    } else {
        time_source_ = nullptr;
    }
}

void Engine::notify_page_visibility(bool visible) noexcept {
    if (visible) {
        // A hidden span ended. The next tick must not charge the whole span to
        // one frame: rebaseline the clock so it advances by the frame's own
        // duration. page_hidden_ guards against a redundant visible report (a
        // page that was never hidden) needlessly resetting the clock.
        if (page_hidden_) {
            rebaseline_clock_ = true;
        }
        page_hidden_ = false;
    } else {
        page_hidden_ = true;
    }
}

std::unique_ptr<audio::AudioSystem> Engine::take_audio_system() noexcept {
    return std::move(audio_system_);
}

Error Engine::initialize(const Config& config) {
    if (initialized_) {
        return Error("Engine::initialize called twice");
    }

    log::set_min_level(config.log_level);
    FE_LOG_INFO("fork engine v%s starting", version::kEngineVersionString);

    // Make the engine's component types visible to the serializer, the
    // inspector, and any scene loaded later. Idempotent.
    runtime::register_all_component_types();

    auto window = Window::create(config.window);
    if (!window) {
        return window.error();
    }
    window_ = std::move(window.value());
    FE_LOG_INFO("window created (%ux%u, backend %s)", window_->width(), window_->height(), window_->backend_name());

    // A null window has no graphics context. Two honest outcomes: a plain
    // headless run skips the graphics stack entirely, and a headless run that
    // asked to render builds a null RHI device so the render path still
    // executes. Nothing above the platform layer that renders branches on which
    // of the two it is; it checks has_graphics()/headless_render(). See
    // docs/adr/0041-platform-contract-and-null-backend.md and
    // docs/adr/0042-null-rhi-device.md.
    has_graphics_ = window_->has_context();
    headless_render_ = !has_graphics_ && config.headless_render;
    fixed_delta_time_ = config.fixed_delta_time > 0.0f ? config.fixed_delta_time : 0.0f;
    if (fixed_delta_time_ > 0.0f) {
        FE_LOG_INFO("fixed simulation step: %.4f s", static_cast<double>(fixed_delta_time_));
    }
    const bool wants_device = has_graphics_ || headless_render_;

    if (has_graphics_) {
        auto device = rhi::create_device(*window_);
        if (!device) {
            // Release the window before reporting, so a failed start leaves no
            // half-initialised state behind.
            window_.reset();
            return device.error();
        }
        device_ = std::move(device.value());
    } else if (headless_render_) {
        auto device = rhi::create_null_device();
        if (!device) {
            window_.reset();
            return device.error();
        }
        device_ = std::move(device.value());
        FE_LOG_INFO("headless render: null RHI device, no OS surface (nothing is rasterised)");
    } else {
        FE_LOG_INFO("headless run: no graphics context, device and renderers are skipped");
    }

    resources_ = std::make_unique<ResourceManager>();
    // Opt into the import cache only when a root is configured. The cache is
    // inert otherwise, so the loader's cache check is a no-op and the load path
    // is byte-for-byte the pre-cache behaviour.
    if (!config.cache_root.empty()) {
        resources_->import_cache().set_root(config.cache_root);
        FE_LOG_INFO("import cache root '%s'", resources_->import_cache().root().c_str());
    }
    resources_->register_loader(std::make_unique<ImageLoader>(&resources_->import_cache()));
    if (wants_device) {
        // The GPU texture loader needs a device to upload through. A run with no
        // device registers only the CPU image loader, so an asset can still be
        // decoded and inspected with no context.
        resources_->register_loader(std::make_unique<Texture2DLoader>(*device_, *resources_));
        // A font resolves its atlas through the texture loader, so it registers
        // here rather than in the device-free path.
        resources_->register_loader(std::make_unique<FontLoader>(*resources_));
    }
    resources_->register_loader(std::make_unique<ObjLoader>());
    resources_->register_loader(std::make_unique<GltfLoader>());

    if (wants_device) {
        renderer_2d_ = std::make_unique<renderer2d::Renderer2D>();
        if (Error error = renderer_2d_->initialize(*device_); error) {
            device_.reset();
            window_.reset();
            return error;
        }

        renderer_3d_ = std::make_unique<renderer3d::Renderer3D>();
        if (Error error = renderer_3d_->initialize(*device_); error) {
            renderer_2d_.reset();
            device_.reset();
            window_.reset();
            return error;
        }
    }

    // Register the decoded-audio loader so "wav:path.wav" resolves to an
    // AudioClip through the ordinary resource path. The device itself is not
    // opened here: audio output is opt-in, so a machine with no sound card still
    // initialises. A caller (the sandbox, a game, the editor) opens a device and
    // attaches it to the audio system.
    resources_->register_loader(std::make_unique<audio::WavLoader>());
    audio_system_ = std::make_unique<audio::AudioSystem>(nullptr);

    last_tick_time_ = time_source_ ? time_source_() : now_seconds();
    initialized_ = true;
    FE_LOG_INFO("engine initialised");
    return Error::none();
}

void Engine::shutdown() {
    if (!initialized_ && window_ == nullptr) {
        return;
    }

    // Reverse creation order. GPU resources must be released while the device
    // is still alive, and the device while the context is still current.
    audio_system_.reset();
    renderer_3d_.reset();
    renderer_2d_.reset();
    if (resources_ != nullptr) {
        resources_->collect_garbage();
        resources_.reset();
    }
    device_.reset();
    window_.reset();

    initialized_ = false;
    // Clear the surface flags with the lifetime they describe: after shutdown
    // there is no device and no surface, so has_graphics()/headless_render()
    // must not keep reporting the state of a torn-down engine.
    has_graphics_ = false;
    headless_render_ = false;
    FE_LOG_INFO("engine shut down");
}

void Engine::tick() {
    const f64 now = time_source_ ? time_source_() : now_seconds();
    f64 elapsed = now - last_tick_time_;
    last_tick_time_ = now;

    // First frame after a backgrounded page became visible again: the span
    // since the last frame is the hidden time, not gameplay time, so it is
    // discarded rather than charged to one frame. Rebaselining to zero bounds
    // the step; every later frame measures normally.
    if (rebaseline_clock_) {
        rebaseline_clock_ = false;
        elapsed = 0.0;
    }

    // A fixed step makes the simulation advance by that much on every frame,
    // whatever the frame actually took. elapsed_time() then measures simulated
    // time, not wall-clock time, which is the point: a run that pinned the step
    // is reproducible, and one that did not is unchanged.
    const f64 step = fixed_delta_time_ > 0.0f ? static_cast<f64>(fixed_delta_time_) : elapsed;
    elapsed_time_ += step;

    const auto frame = static_cast<f32>(step);
    delta_time_ = frame > kMaxDeltaTime ? kMaxDeltaTime : frame;
}

} // namespace fe
