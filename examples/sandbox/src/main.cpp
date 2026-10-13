// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The Fork Engine sandbox.
//
// A deliberately small program that exercises every layer of the foundation
// built in milestone 1:
//
//   * platform  - creates a window and reads input
//   * resources - loads and caches a texture (and its decoded CPU image)
//   * scene     - builds a node hierarchy with a camera and sprites
//   * serialization - saves the scene to JSON and loads it back
//   * renderer2d - batches the sprites and draws them
//   * runtime   - runs the frame loop
//
// Usage:
//   sandbox                  run interactively (needs a display)
//   sandbox --headless N     render N frames with no window presentation
//   sandbox --scene PATH     load a scene instead of building the demo one
//   sandbox --save-scene P   save the built scene to P and exit
//
// Controls (interactive):
//   W A S D / arrows   move the camera
//   Q / E              zoom out / in
//   Escape             quit

#include "fe/core/Log.h"
#include "fe/resources/Image.h"
#include "fe/resources/Texture2D.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/SceneSerializer.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kTexturePath = "tex:assets/textures/checker.png";

// A tiny demo application. It builds a scene in on_start(), moves the camera in
// on_update(), and lets the base class render it.
class Sandbox final : public fe::Application {
public:
    Sandbox(fe::Engine::Config config, std::string scene_to_load, std::string scene_to_save)
        : fe::Application(config), scene_to_load_(std::move(scene_to_load)),
          scene_to_save_(std::move(scene_to_save)) {}

protected:
    fe::Error on_start() override {
        if (!scene_to_load_.empty()) {
            if (fe::Error error = fe::serialization::scene::load(scene_to_load_, scene(), &engine().resources());
                error) {
                return error;
            }
            FE_LOG_INFO("loaded scene from '%s' (%zu nodes)", scene_to_load_.c_str(), scene().node_count());
        } else {
            if (fe::Error error = build_demo_scene(); error) {
                return error;
            }
        }

        if (!scene_to_save_.empty()) {
            if (fe::Error error = fe::serialization::scene::save(scene(), scene_to_save_); error) {
                return error;
            }
            FE_LOG_INFO("saved scene to '%s'", scene_to_save_.c_str());
            // Saving is the whole job in this mode.
            request_exit();
        }

        return fe::Error::none();
    }

    void on_update(fe::f32 delta_time) override {
        fe::Camera2D* camera = camera_;
        if (camera == nullptr || camera->node() == nullptr) {
            return;
        }

        const fe::InputState& input = engine().window().input();
        constexpr fe::f32 kMoveSpeed = 240.0f; // World units per second.

        fe::Vec3 position = camera->node()->transform().position;
        if (input.is_key_down(fe::Key::W) || input.is_key_down(fe::Key::Up)) {
            position.y += kMoveSpeed * delta_time;
        }
        if (input.is_key_down(fe::Key::S) || input.is_key_down(fe::Key::Down)) {
            position.y -= kMoveSpeed * delta_time;
        }
        if (input.is_key_down(fe::Key::A) || input.is_key_down(fe::Key::Left)) {
            position.x -= kMoveSpeed * delta_time;
        }
        if (input.is_key_down(fe::Key::D) || input.is_key_down(fe::Key::Right)) {
            position.x += kMoveSpeed * delta_time;
        }
        camera->node()->transform().position = position;

        if (input.is_key_down(fe::Key::Q)) {
            camera->zoom *= (1.0f - 1.5f * delta_time);
        }
        if (input.is_key_down(fe::Key::E)) {
            camera->zoom *= (1.0f + 1.5f * delta_time);
        }
        camera->zoom = std::clamp(camera->zoom, 0.1f, 8.0f);

        if (input.is_key_down(fe::Key::Escape)) {
            request_exit();
        }
    }

    void on_stop() override {
        // Report what the pipeline actually produced, so a headless CI run has a
        // machine-checkable signal that geometry reached the GPU.
        FE_LOG_INFO("rendered %zu sprites in the final frame", last_sprite_count());
    }

private:
    fe::Error build_demo_scene() {
        scene().set_name("Sandbox");

        // Camera.
        fe::Node* camera_node = scene().create_node("Camera");
        camera_ = camera_node->add_component<fe::Camera2D>();
        camera_->zoom = 1.0f;

        // Load the texture once; every sprite shares the same resource.
        auto texture = engine().resources().load(kTexturePath);
        if (!texture) {
            return texture.error();
        }
        auto typed = std::dynamic_pointer_cast<fe::Texture2D>(texture.value());
        if (!typed) {
            return fe::Error("expected the 'tex' scheme to produce a Texture2D");
        }

        // A row of sprites plus a child sprite, to demonstrate hierarchy and
        // batching (all five sprites share one texture, so they should collapse
        // into a single draw call).
        for (int i = 0; i < 4; ++i) {
            const std::string name = "Sprite" + std::to_string(i);
            fe::Node* node = scene().create_node(name);
            auto* sprite = node->add_component<fe::Sprite2D>();
            sprite->texture = typed;
            sprite->size = fe::Vec2{96.0f, 96.0f};
            sprite->color = fe::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            node->transform().position =
                fe::Vec3{static_cast<fe::f32>(i) * 128.0f - 192.0f, 0.0f, 0.0f};
            node->transform().rotation = fe::Vec3{0.0f, 0.0f, static_cast<fe::f32>(i) * 0.15f};
        }

        // A parent sprite with a child, so the hierarchy is exercised. The
        // child inherits the parent's transform.
        fe::Node* parent = scene().create_node("ParentSprite");
        auto* parent_sprite = parent->add_component<fe::Sprite2D>();
        parent_sprite->texture = typed;
        parent_sprite->size = fe::Vec2{128.0f, 128.0f};
        parent_sprite->color = fe::Vec4{1.0f, 1.0f, 0.6f, 1.0f};
        parent->transform().position = fe::Vec3{0.0f, 160.0f, 0.0f};

        fe::Node* child = parent->create_child<fe::Node>("ChildSprite");
        auto* child_sprite = child->add_component<fe::Sprite2D>();
        child_sprite->texture = typed;
        child_sprite->size = fe::Vec2{48.0f, 48.0f};
        child_sprite->color = fe::Vec4{0.6f, 1.0f, 0.6f, 1.0f};
        child->transform().position = fe::Vec3{80.0f, 0.0f, 0.0f};
        child->transform().rotation = fe::Vec3{0.0f, 0.0f, fe::kPi * 0.25f};

        FE_LOG_INFO("built demo scene: %zu nodes", scene().node_count());
        return fe::Error::none();
    }

    fe::Camera2D* camera_ = nullptr;
    std::string scene_to_load_;
    std::string scene_to_save_;
};

struct Options {
    bool headless = false;
    fe::u32 headless_frames = 0;
    std::string scene_to_load;
    std::string scene_to_save;
    fe::LogLevel log_level = fe::LogLevel::Info;
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") {
            options.headless = true;
            options.headless_frames = 60;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.headless_frames = static_cast<fe::u32>(std::stoul(argv[++i]));
            }
        } else if (arg == "--scene" && i + 1 < argc) {
            options.scene_to_load = argv[++i];
        } else if (arg == "--save-scene" && i + 1 < argc) {
            options.scene_to_save = argv[++i];
        } else if (arg == "--verbose") {
            options.log_level = fe::LogLevel::Debug;
        } else if (arg == "--quiet") {
            options.log_level = fe::LogLevel::Warn;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: sandbox [--headless [frames]] [--scene PATH] [--save-scene PATH] "
                      "[--verbose] [--quiet]");
            std::exit(0);
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    fe::Engine::Config config;
    config.window.title = "Fork Engine Sandbox";
    config.window.width = 1280;
    config.window.height = 720;
    config.log_level = options.log_level;
    if (options.headless) {
        // Keep the window hidden but still request a real GL context, so the
        // render path is identical to an interactive run.
        config.window.visible = false;
        config.window.vsync = false;
    }

    Sandbox app(config, options.scene_to_load, options.scene_to_save);
    if (options.headless) {
        app.set_headless(options.headless_frames);
    }

    return app.run();
}
