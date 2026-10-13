// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/Engine.h"

#include "fe/core/Log.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/renderer2d/Renderer2D.h"
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

Engine::~Engine() {
    shutdown();
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

    auto window = Window::create(config.window);
    if (!window) {
        return window.error();
    }
    window_ = std::move(window.value());
    FE_LOG_INFO("window created (%ux%u, backend %s)", window_->width(), window_->height(), Window::backend_name());

    auto device = rhi::create_device(*window_);
    if (!device) {
        // Release the window before reporting, so a failed start leaves no
        // half-initialised state behind.
        window_.reset();
        return device.error();
    }
    device_ = std::move(device.value());

    resources_ = std::make_unique<ResourceManager>();
    resources_->register_loader(std::make_unique<ImageLoader>());
    resources_->register_loader(std::make_unique<Texture2DLoader>(*device_, *resources_));

    renderer_2d_ = std::make_unique<renderer2d::Renderer2D>();
    if (Error error = renderer_2d_->initialize(*device_); error) {
        device_.reset();
        window_.reset();
        return error;
    }

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
