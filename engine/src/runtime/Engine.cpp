// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/Engine.h"

#include "fe/audio/AudioSystem.h"
#include "fe/audio/AudioTypes.h"
#include "fe/audio/WavLoader.h"
#include "fe/core/Assert.h"
#include "fe/core/Log.h"
#include "fe/mesh/GltfLoader.h"
#include "fe/mesh/ObjLoader.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics3d/Physics3DTypes.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/resources/ImageLoader.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene3d/Scene3DTypes.h"
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
    reflect::register_builtin_types();
    reflect::register_physics_types();
    reflect::register_physics3d_types();
    reflect::register_scene3d_types();
    reflect::register_audio_types();

    auto window = Window::create(config.window);
    if (!window) {
        return window.error();
    }
    window_ = std::move(window.value());
    FE_LOG_INFO("window created (%ux%u, backend %s)", window_->width(), window_->height(), window_->backend_name());

    // A null window has no graphics context, so there is no device to create.
    // Everything above the platform layer that renders checks has_graphics();
    // the rest of the engine (systems, resources, audio, the frame loop) runs
    // unchanged. See docs/adr/0041-platform-contract-and-null-backend.md.
    has_graphics_ = window_->has_context();
    if (has_graphics_) {
        auto device = rhi::create_device(*window_);
        if (!device) {
            // Release the window before reporting, so a failed start leaves no
            // half-initialised state behind.
            window_.reset();
            return device.error();
        }
        device_ = std::move(device.value());
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
    if (has_graphics_) {
        // The GPU texture loader needs a device to upload through. Headless runs
        // register only the CPU image loader, so an asset can still be decoded
        // and inspected with no context (see the null RHI device in ADR-0041).
        resources_->register_loader(std::make_unique<Texture2DLoader>(*device_, *resources_));
    }
    resources_->register_loader(std::make_unique<ObjLoader>());
    resources_->register_loader(std::make_unique<GltfLoader>());

    if (has_graphics_) {
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

    last_tick_time_ = now_seconds();
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
    FE_LOG_INFO("engine shut down");
}

void Engine::tick() {
    const f64 now = now_seconds();
    const f64 elapsed = now - last_tick_time_;
    last_tick_time_ = now;
    elapsed_time_ += elapsed;

    const auto frame = static_cast<f32>(elapsed);
    delta_time_ = frame > kMaxDeltaTime ? kMaxDeltaTime : frame;
}

} // namespace fe
