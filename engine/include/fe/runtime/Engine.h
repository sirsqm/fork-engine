// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Log.h"
#include "fe/core/Types.h"
#include "fe/platform/Window.h"
#include "fe/resources/ResourceManager.h"
#include "fe/rhi/Device.h"

#include <memory>
#include <string>

namespace fe {

namespace renderer2d {
class Renderer2D;
}

namespace renderer3d {
class Renderer3D;
}

// Owns the engine's long-lived subsystems and their lifetimes.
//
// The Engine is the single place that knows the order in which subsystems are
// created and destroyed:
//
//   window  ->  device  ->  resource manager (with loaders)  ->  renderers
//
// Creation order matters because the resource manager needs a device to create
// GPU textures, and the device needs a current context from the window.
// Destruction happens in reverse. Concentrating this here means gameplay code
// and the future editor never have to reason about it.
//
// The Engine does not run a frame loop; that is Application's job. Keeping the
// two apart lets the editor embed an Engine and drive frames itself (for
// play-in-editor) without inheriting the standalone loop.
class Engine {
public:
    struct Config {
        WindowDesc window{};
        // Minimum log severity. Defaults to Info so a shipped game is quiet but
        // still reports warnings and errors.
        fe::LogLevel log_level = fe::LogLevel::Info;
    };

    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Brings up every subsystem. Returns an Error describing the first failure.
    [[nodiscard]] Error initialize(const Config& config);

    void shutdown();

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    [[nodiscard]] Window& window() noexcept { return *window_; }
    [[nodiscard]] const Window& window() const noexcept { return *window_; }

    [[nodiscard]] rhi::Device& device() noexcept { return *device_; }
    [[nodiscard]] const rhi::Device& device() const noexcept { return *device_; }

    [[nodiscard]] ResourceManager& resources() noexcept { return *resources_; }
    [[nodiscard]] const ResourceManager& resources() const noexcept { return *resources_; }

    [[nodiscard]] renderer2d::Renderer2D& renderer_2d() noexcept { return *renderer_2d_; }
    [[nodiscard]] const renderer2d::Renderer2D& renderer_2d() const noexcept { return *renderer_2d_; }

    [[nodiscard]] renderer3d::Renderer3D& renderer_3d() noexcept { return *renderer_3d_; }
    [[nodiscard]] const renderer3d::Renderer3D& renderer_3d() const noexcept { return *renderer_3d_; }

    // Seconds elapsed since the previous frame, clamped to a sane maximum so a
    // long stall (a debugger breakpoint, a window drag) does not produce a huge
    // simulation step.
    [[nodiscard]] f32 delta_time() const noexcept { return delta_time_; }

    // Seconds elapsed since initialize().
    [[nodiscard]] f64 elapsed_time() const noexcept { return elapsed_time_; }

    // Advances the clock. Called once per frame by Application.
    void tick();

private:
    bool initialized_ = false;
    std::unique_ptr<Window> window_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<ResourceManager> resources_;
    std::unique_ptr<renderer2d::Renderer2D> renderer_2d_;
    std::unique_ptr<renderer3d::Renderer3D> renderer_3d_;

    f32 delta_time_ = 0.0f;
    f64 elapsed_time_ = 0.0;
    f64 last_tick_time_ = 0.0;
};

} // namespace fe
