// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// A minimal example that runs the engine's frame loop with no display and no
// graphics context: the null platform backend.
//
// It exists to make "the engine runs on a machine with no GPU and no X server"
// a thing you can execute, not just a claim. Run it on a headless CI runner or
// a dedicated server:
//
//   fork_headless            # 60 frames
//   fork_headless 5          # five frames
//   fork_headless --render 5 # also run the render path (null RHI device)
//
// What it proves, in one process:
//   * Window::create() with WindowBackend::Null needs no OS surface.
//   * Engine::initialize() comes up with has_graphics() == false and no device.
//   * Application's loop still advances the clock, feeds input edges, and runs
//     the schedule, so gameplay systems execute with no rendering.
//   * With --render, the render path (extraction, batching, draw submission)
//     runs against the null RHI device and the renderer reports its draw calls,
//     even though nothing is rasterised (ADR-0042).
//
// It deliberately uses no textures and no assets: the point is the platform
// layer, not the content pipeline. See
// docs/adr/0041-platform-contract-and-null-backend.md and
// docs/adr/0042-null-rhi-device.md.

#include "fe/core/Log.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Device.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/systems/SceneSchedule.h"
#include "fe/systems/System.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

namespace {

// A gameplay system with no dependency on rendering: it counts the frames it
// has seen and moves its target node. If this runs, the schedule ran, which is
// the whole claim of the headless mode.
class SpinSystem final : public fe::System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "SpinSystem"; }

    void update(const fe::FrameContext& context, fe::Scene& scene) override {
        ++frames_;
        fe::Node* node = scene.find_node_by_name("Spinner");
        if (node != nullptr) {
            node->transform().rotation.z += 1.0f * context.delta_time;
        }
    }

    [[nodiscard]] fe::u64 frames() const noexcept { return frames_; }

private:
    fe::u64 frames_ = 0;
};

class HeadlessApp final : public fe::Application {
public:
    HeadlessApp(fe::Engine::Config config, bool render) : fe::Application(config), render_(render) {}

protected:
    fe::Error on_start() override {
        // A camera so the scene has the shape a real one would; it is never used
        // to render here, but a headless server still hosts gameplay state.
        fe::Node* camera = scene().create_node("Camera");
        camera->add_component<fe::Camera2D>();

        // A sprite with no texture: the component exists and is valid (a null
        // texture draws nothing), which is exactly the state a headless server
        // is in - the data is present, the drawing is not.
        fe::Node* spinner = scene().create_node("Spinner");
        auto* sprite = spinner->add_component<fe::Sprite2D>();

        if (render_) {
            // Give the sprite a texture built on the null device, so the 2D pass
            // has geometry to extract, batch, and submit. The texture is inert;
            // it exists so the render path has something to draw.
            const fe::u8 pixel[4] = {255, 255, 255, 255};
            fe::rhi::TextureDesc desc;
            desc.name = "headless_sprite";
            desc.width = 1;
            desc.height = 1;
            desc.pixels = pixel;
            auto texture = engine().device().create_texture(desc);
            if (!texture) {
                return texture.error();
            }
            sprite->size = fe::Vec2{1.0f, 1.0f};
            sprite->texture = std::make_shared<fe::Texture2D>("tex:headless.png", texture.value());
        }

        spin_ = schedule().add_system<SpinSystem>();
        return fe::Error::none();
    }

    void on_stop() override {
        // Machine-checkable: the frame loop and the systems ran. A CI job greps
        // these lines.
        FE_LOG_INFO("headless: %llu frames, SpinSystem ran %llu times", static_cast<unsigned long long>(frame_count_),
                    static_cast<unsigned long long>(spin_ != nullptr ? spin_->frames() : 0));
        FE_LOG_INFO("headless: has_graphics=%s headless_render=%s", engine().has_graphics() ? "true" : "false",
                    engine().headless_render() ? "true" : "false");
        if (engine().headless_render()) {
            // The renderer's own counter: the same value an interactive run
            // reports. It proves submission, not pixels.
            FE_LOG_INFO("headless: render submitted %u draw call(s) with no surface",
                        engine().renderer_2d().last_draw_calls());
        }
    }

    void on_update(fe::f32) override { ++frame_count_; }

private:
    SpinSystem* spin_ = nullptr;
    bool render_ = false;
    fe::u64 frame_count_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    fe::u32 frames = 60;
    fe::LogLevel log_level = fe::LogLevel::Info;
    bool render = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--verbose") {
            log_level = fe::LogLevel::Debug;
        } else if (arg == "--quiet") {
            log_level = fe::LogLevel::Warn;
        } else if (arg == "--render") {
            render = true;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: fork_headless [frames] [--render] [--verbose] [--quiet]");
            return 0;
        } else if (arg[0] != '-') {
            frames = static_cast<fe::u32>(std::strtoul(argv[i], nullptr, 10));
        }
    }

    fe::Engine::Config config;
    config.log_level = log_level;
    // The whole point: no OS window, no GL context.
    config.window.backend = fe::WindowBackend::Null;
    config.window.title = "Fork Engine Headless";
    config.window.width = 1280;
    config.window.height = 720;
    config.window.visible = false;
    config.window.vsync = false;
    // With --render the render path runs against the null RHI device.
    config.headless_render = render;

    HeadlessApp app(config, render);
    app.set_headless(frames);
    return app.run();
}
