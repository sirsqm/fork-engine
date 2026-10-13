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
//   * systems   - animates a sprite and drops a physics crate
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

#include "fe/audio/AudioBackend.h"
#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioSystem.h"
#include "fe/audio/WavLoader.h"
#include "fe/core/Log.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/resources/Image.h"
#include "fe/resources/Texture2D.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/SpriteAnimator.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/SpriteAnimationSystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kTexturePath = "tex:assets/textures/checker.png";
constexpr const char* kAudioPath = "wav:assets/audio/beep.wav";

// A tiny demo application. It builds a scene in on_start(), moves the camera in
// on_update(), and lets the base class render it.
class Sandbox final : public fe::Application {
public:
    Sandbox(fe::Engine::Config config, std::string scene_to_load, std::string scene_to_save)
        : fe::Application(config), scene_to_load_(std::move(scene_to_load)), scene_to_save_(std::move(scene_to_save)) {}

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

        setup_audio();
        return fe::Error::none();
    }

    // Sets up the demo sound. This is the only place the sandbox touches audio,
    // and it is additive: the rest of the demo is unchanged.
    //
    // The engine's audio system is always adopted, so the mixer runs in the
    // frame loop and produces data even when no device can be opened. Opening
    // the real device is attempted separately, and a failure (a CI machine, a
    // sandbox with no sound card) only means the mixed frames are not heard -
    // they are still produced, and on_stop() still reports them.
    void setup_audio() {
        auto system = engine().take_audio_system();
        if (system == nullptr) {
            FE_LOG_WARN("audio: the engine had no audio system; skipping the demo sound");
            return;
        }
        audio_report_ = system.get();
        schedule().add_system(std::move(system));

        // Try the real backend. A device that will not open is not an error: the
        // mixer runs regardless, into no sink.
        auto device = fe::audio::create_audio_device();
        if (!device) {
            FE_LOG_WARN("audio: no output backend (%s); mixing silently", device.error().message().c_str());
        } else {
            auto opened = device.value()->open(48000, 2);
            if (!opened) {
                FE_LOG_WARN("audio: could not open the device (%s); mixing silently", opened.error().message().c_str());
            } else if (fe::Error error = device.value()->start(); error) {
                FE_LOG_WARN("audio: could not start playback (%s); mixing silently", error.message().c_str());
            } else {
                audio_device_ = std::move(device.value());
                audio_report_->set_device(audio_device_.get());
                FE_LOG_INFO("audio: opened %s backend at %u Hz, %u channels",
                            std::string(audio_device_->backend()).c_str(), audio_device_->format().sample_rate,
                            audio_device_->format().channels);
            }
        }

        auto clip = engine().resources().load(kAudioPath);
        if (!clip) {
            FE_LOG_WARN("audio: could not load '%s' (%s)", kAudioPath, clip.error().message().c_str());
            return;
        }

        fe::Node* node = scene().create_node("Beep");
        auto* source = node->add_component<fe::audio::AudioSource>();
        source->clip = clip.value();
        source->gain = 0.8f;
        source->loop = true; // so a short demo run is still audible
        FE_LOG_INFO("audio: playing '%s'", kAudioPath);
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
        if (crate_ != nullptr) {
            // The crate starts at y = 160 and falls. Headless frames run in
            // real time, so this reports where physics left it after however
            // long the run took - proof the system ran, not a resting position.
            FE_LOG_INFO("crate ended at (%.1f, %.1f)", static_cast<double>(crate_->transform().position.x),
                        static_cast<double>(crate_->transform().position.y));
        }
        if (audio_report_ != nullptr) {
            // The machine-checkable proof that audio data was produced, whether
            // or not a device was opened and whether or not anyone heard it. A
            // CI run greps this line to confirm the mixer ran and emitted
            // non-silent samples.
            FE_LOG_INFO("audio: mixed %llu frames, peak sample %.4f",
                        static_cast<unsigned long long>(audio_report_->mixed_frames()),
                        static_cast<double>(audio_report_->last_peak()));
        }
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
            node->transform().position = fe::Vec3{static_cast<fe::f32>(i) * 128.0f - 192.0f, 0.0f, 0.0f};
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

        // An animated sprite, to exercise the systems layer end to end. The
        // animator is a component like any other; adding it plus registering
        // the system is the whole setup. The sheet is the same checker texture
        // treated as a 4x4 grid of cells.
        //
        // This is the point of ADR-0008: gameplay is a system you add to the
        // schedule, not a method a node inherits.
        fe::Node* animated_node = scene().create_node("Animated");
        auto* animated_sprite = animated_node->add_component<fe::Sprite2D>();
        animated_sprite->texture = typed;
        animated_sprite->size = fe::Vec2{96.0f, 96.0f};
        auto* animator = animated_node->add_component<fe::SpriteAnimator>();
        animator->frames_x = 4;
        animator->frames_y = 4;
        animator->fps = 8.0f;
        animated_node->transform().position = fe::Vec3{-320.0f, 160.0f, 0.0f};
        schedule().add_system<fe::SpriteAnimationSystem>();

        // A physics demo: a static floor and a crate that falls onto it. The
        // floor is a collider with no rigid body (a static obstacle); the crate
        // is a collider plus a dynamic rigid body. Same two components, two
        // configurations - the composition model at work.
        fe::Node* floor = scene().create_node("Floor");
        auto* floor_collider = floor->add_component<fe::physics2d::Collider2D>();
        floor_collider->shape = fe::physics2d::ShapeType::Box;
        floor_collider->half_extent = fe::Vec2{480.0f, 16.0f};
        floor->transform().position = fe::Vec3{0.0f, -256.0f, 0.0f};
        auto* floor_sprite = floor->add_component<fe::Sprite2D>();
        floor_sprite->texture = typed;
        floor_sprite->size = fe::Vec2{960.0f, 32.0f};
        floor_sprite->color = fe::Vec4{0.4f, 0.4f, 0.5f, 1.0f};

        fe::Node* crate = scene().create_node("Crate");
        auto* crate_collider = crate->add_component<fe::physics2d::Collider2D>();
        crate_collider->shape = fe::physics2d::ShapeType::Box;
        crate_collider->half_extent = fe::Vec2{48.0f, 48.0f};
        crate_collider->restitution = 0.3f;
        auto* crate_body = crate->add_component<fe::physics2d::RigidBody2D>();
        crate_body->mass = 1.0f;
        crate_body->velocity = fe::Vec2{120.0f, 0.0f};
        crate->transform().position = fe::Vec3{-400.0f, 160.0f, 0.0f};
        auto* crate_sprite = crate->add_component<fe::Sprite2D>();
        crate_sprite->texture = typed;
        crate_sprite->size = fe::Vec2{96.0f, 96.0f};
        crate_sprite->color = fe::Vec4{1.0f, 0.7f, 0.4f, 1.0f};
        crate_ = crate;
        // The engine is unit-agnostic: physics works in whatever units the
        // scene uses. This demo is authored at roughly pixel scale (a crate is
        // 96 units), so gravity is scaled to match; at a 1-unit-per-metre scale
        // the default -9.81 would be correct and this line would be omitted.
        auto* physics = schedule().add_system<fe::physics2d::Physics2DSystem>();
        physics->gravity = fe::Vec2{0.0f, -980.0f};

        FE_LOG_INFO("built demo scene: %zu nodes", scene().node_count());
        return fe::Error::none();
    }

    fe::Camera2D* camera_ = nullptr;
    fe::Node* crate_ = nullptr;
    // Owned by the sandbox; declared before the system that borrows it so the
    // system is destroyed first. The system itself is owned by the schedule;
    // audio_report_ is a borrowed pointer to it for reporting.
    std::unique_ptr<fe::audio::AudioDevice> audio_device_;
    fe::audio::AudioSystem* audio_report_ = nullptr;
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
