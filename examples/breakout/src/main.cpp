// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Breakout — the M2.5 vertical slice.
//
// One small, complete game, built entirely on the engine's public API, exactly
// as an external project would build it. It is deliberately the integration test
// the roadmap asks for: in one artifact it exercises the scene graph, the
// resource manager and its texture/audio loaders, 2D physics (colliders, rigid
// bodies, contact events), the audio mixer, scene serialization, input, and the
// systems layer.
//
// The gameplay rules live in BreakoutState (breakout_state.h), a pure class with
// no engine dependency, so they are unit-tested directly. The field layout and
// the system wiring live in breakout_scene.h, shared with the tests so the tests
// prove the shipped wiring, not a copy. This file is only the application shell:
// it loads or builds the scene, opens the audio device, drives the loop, and
// reports a machine-checkable result.
//
// Usage:
//   fork_breakout                 play interactively (needs a display)
//   fork_breakout --headless N    run N frames with no window; autoplay drives
//                                 the ball so a run reaches win/lose and exits
//   fork_breakout --save-scene P  write the built scene to P and exit
//
// Controls (interactive):
//   Left/Right or A/D   move the paddle
//   Space / Enter       launch the ball
//   R                   restart after a win or a loss
//   Escape              quit

#include "breakout_scene.h"
#include "breakout_state.h"
#include "breakout_systems.h"

#include "fe/audio/AudioBackend.h"
#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioSystem.h"
#include "fe/core/Log.h"
#include "fe/platform/InputState.h"
#include "fe/resources/Resource.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/SceneSchedule.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

// The authored scene, loaded over the code-built one when present, and the cue.
constexpr const char* kScenePath = "examples/breakout/scenes/breakout.fescene";
constexpr const char* kBeepPath = "wav:examples/breakout/assets/audio/beep.wav";

// Loads the field's textures. A missing texture is not fatal: the field is built
// anyway and simply draws blank, which is what a headless run wants.
breakout::SceneAssets load_assets(fe::ResourceManager& resources) {
    breakout::SceneAssets assets;
    const auto load = [&resources](const char* path) -> std::shared_ptr<fe::Resource> {
        auto texture = resources.load(path);
        if (!texture) {
            FE_LOG_WARN("breakout: could not load '%s' (%s)", path, texture.error().message().c_str());
            return nullptr;
        }
        return texture.value();
    };
    assets.wall = load("examples/breakout/assets/textures/wall.png");
    assets.paddle = load("examples/breakout/assets/textures/paddle.png");
    assets.ball = load("examples/breakout/assets/textures/ball.png");
    assets.brick = load("examples/breakout/assets/textures/brick.png");
    return assets;
}

class BreakoutApp final : public fe::Application {
public:
    BreakoutApp(fe::Engine::Config config, bool headless, std::string scene_to_save)
        : fe::Application(config), headless_(headless), scene_to_save_(std::move(scene_to_save)) {}

protected:
    fe::Error on_start() override {
        // Build the canonical field first, then (unless we are only regenerating
        // the scene file) load the authored document over the top of it. The
        // document is a serialization of exactly this layout, so both paths give
        // the same game; loading it is what exercises the real scene-load path.
        breakout::build_breakout_scene(scene(), load_assets(engine().resources()));

        if (!scene_to_save_.empty()) {
            if (fe::Error error = fe::serialization::scene::save(scene(), scene_to_save_, &engine().resources());
                error) {
                return error;
            }
            FE_LOG_INFO("breakout: saved the built scene to '%s'", scene_to_save_.c_str());
            request_exit();
            return fe::Error::none();
        }

        if (fe::Error error = fe::serialization::scene::load(kScenePath, scene(), &engine().resources()); error) {
            FE_LOG_WARN("breakout: %s; using the scene built in code", error.message().c_str());
        } else {
            FE_LOG_INFO("breakout: loaded '%s' (%zu nodes)", kScenePath, scene().node_count());
        }

        if (scene().find_node_by_name(breakout::names::kBall) == nullptr ||
            scene().find_node_by_name(breakout::names::kPaddle) == nullptr) {
            return fe::Error("breakout: the scene is missing a 'Ball' or 'Paddle' node");
        }

        // Declare the game's actions before installing the systems, so the
        // systems get handles to bind to. The application evaluates the map each
        // frame and hands it to the systems through FrameContext, which is what
        // lets a touch and a key drive the identical gameplay code.
        const breakout::BreakoutActions bindings = breakout::declare_breakout_actions(actions());

        // The physics pointer the helper returns is only needed by tests; here
        // the systems already hold it.
        static_cast<void>(breakout::install_breakout_systems(schedule(), state_, headless_, bindings));
        adopt_audio_system();
        attach_sound();

        FE_LOG_INFO("breakout: field %dx%d, %d bricks, %d lives",
                    static_cast<int>(breakout::config::kFieldHalfWidth * 2.0f),
                    static_cast<int>(breakout::config::kFieldHalfHeight * 2.0f), state_.bricks_total(), state_.lives());
        return fe::Error::none();
    }

    void on_update(fe::f32 /*delta_time*/) override {
        if (!headless_ && engine().input().was_key_pressed(fe::Key::Escape)) {
            request_exit();
        }

        // Restart: whoever owns the scene rebuilds it. The systems look their
        // nodes up by name each frame, so only the state and the scene change.
        if (state_.consume_restart()) {
            rebuild();
            FE_LOG_INFO("breakout: restarted");
        }

        if (headless_ && state_.is_over()) {
            request_exit();
        }
    }

    void on_stop() override {
        if (audio_system_ != nullptr) {
            // The machine-checkable proof the mixer ran and produced non-silent
            // audio, whether or not a device was opened and whether or not anyone
            // heard it. A CI run greps this line.
            FE_LOG_INFO("breakout: audio mixed %llu frames, peak %.4f",
                        static_cast<unsigned long long>(audio_system_->mixed_frames()),
                        static_cast<double>(audio_system_->peak_max()));
        }
        // The line a headless run greps for its outcome.
        FE_LOG_INFO("breakout: score=%d lives=%d result=%s", state_.score(), state_.lives(), state_.result_name());
    }

private:
    // Adopts the engine's audio system into the schedule (last, so a cue fired
    // this frame is mixed this frame) and opens the real output device if one is
    // available. A device that will not open is not an error: the mixer still
    // runs, silently, which is what a CI machine with no sound card does.
    void adopt_audio_system() {
        auto system = engine().take_audio_system();
        if (system == nullptr) {
            return;
        }
        audio_system_ = system.get();

        auto device = fe::audio::create_audio_device();
        if (!device) {
            FE_LOG_WARN("breakout: no audio backend (%s); mixing silently", device.error().message().c_str());
        } else if (auto opened = device.value()->open(48000, 2); !opened) {
            FE_LOG_WARN("breakout: could not open the audio device (%s); mixing silently",
                        opened.error().message().c_str());
        } else if (fe::Error error = device.value()->start(); error) {
            FE_LOG_WARN("breakout: could not start playback (%s); mixing silently", error.message().c_str());
        } else {
            audio_device_ = std::move(device.value());
            audio_system_->set_device(audio_device_.get());
            FE_LOG_INFO("breakout: opened %s backend at %u Hz", std::string(audio_device_->backend()).c_str(),
                        audio_device_->format().sample_rate);
        }

        schedule().add_system(std::move(system));
    }

    // Attaches the decoded cue to the scene's audio node. Only the clip is set
    // here: it is a live resource, not scene data.
    void attach_sound() {
        fe::Node* node = scene().find_node_by_name(breakout::names::kAudio);
        if (node == nullptr) {
            return;
        }
        auto* source = node->get_component<fe::audio::AudioSource>();
        if (source == nullptr) {
            return;
        }
        auto clip = engine().resources().load(kBeepPath);
        if (!clip) {
            FE_LOG_WARN("breakout: could not load '%s' (%s); the game runs silently", kBeepPath,
                        clip.error().message().c_str());
            return;
        }
        source->clip = clip.value();
    }

    // Rebuilds the field for a restart: clear the scene, build it again, reset
    // the rules. Rebuilding keeps one code path for "a fresh field", shared with
    // startup, rather than trying to un-break bricks.
    void rebuild() {
        scene().clear();
        breakout::build_breakout_scene(scene(), load_assets(engine().resources()));
        attach_sound();
        state_.reset();
    }

    breakout::BreakoutState state_{};
    bool headless_ = false;
    std::string scene_to_save_;

    // The device is owned here; the AudioSystem (owned by the schedule) borrows
    // it. Neither is touched after the loop ends, so the device being destroyed
    // first is safe. audio_system_ is a borrowed pointer used from on_stop().
    std::unique_ptr<fe::audio::AudioDevice> audio_device_;
    fe::audio::AudioSystem* audio_system_ = nullptr;
};

struct Options {
    bool headless = false;
    fe::u32 headless_frames = 0;
    std::string scene_to_save;
    fe::LogLevel log_level = fe::LogLevel::Info;
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") {
            options.headless = true;
            options.headless_frames = 1800;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.headless_frames = static_cast<fe::u32>(std::stoul(argv[++i]));
            }
        } else if (arg == "--save-scene" && i + 1 < argc) {
            options.scene_to_save = argv[++i];
        } else if (arg == "--verbose") {
            options.log_level = fe::LogLevel::Debug;
        } else if (arg == "--quiet") {
            options.log_level = fe::LogLevel::Warn;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: fork_breakout [--headless [frames]] [--save-scene PATH] [--verbose] [--quiet]");
            std::exit(0);
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    fe::Engine::Config config;
    config.window.title = "Fork Engine Breakout";
    config.window.width = 1280;
    config.window.height = 720;
    config.log_level = options.log_level;
    if (options.headless) {
        config.window.visible = false;
        config.window.vsync = false;
    }

    BreakoutApp app(config, options.headless, options.scene_to_save);
    if (options.headless) {
        app.set_headless(options.headless_frames);
    }
    return app.run();
}
