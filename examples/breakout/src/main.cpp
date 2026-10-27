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
//   Space / Enter       start the game, launch the ball, continue after game over
//   P                   pause and resume
//   R                   restart the current game
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

// A browser page owns the frame schedule, so the web build cannot block in
// run()'s while loop: the module returns to the page and requestAnimationFrame
// calls back once per frame. See docs/adr/0071-web-first-2d-shipping-path.md.
#if defined(FE_PLATFORM_WEB)
#include <emscripten.h>
#endif

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
    // The HUD font. The scheme selects the font loader, so the same path the
    // metrics file lives at resolves to a `Font` rather than a bare file.
    assets.font = load("font:examples/breakout/assets/fonts/hud.fefont");
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

        // Open on the title, unless this is a headless autoplay run, which drives
        // a live game with no player and no menu (the menu would wait forever for
        // a press that never comes). This is the one place the title flow is a
        // choice, which is why it lives in the application and not the rules.
        if (!headless_) {
            state_.show_title();
        }

        FE_LOG_INFO("breakout: field %dx%d, %d bricks, %d lives",
                    static_cast<int>(breakout::config::kFieldHalfWidth * 2.0f),
                    static_cast<int>(breakout::config::kFieldHalfHeight * 2.0f), state_.bricks_total(), state_.lives());
        return fe::Error::none();
    }

    void on_update(fe::f32 /*delta_time*/) override {
        if (!headless_ && engine().input().was_key_pressed(fe::Key::Escape)) {
            request_exit();
        }

        // A browser keeps the audio context suspended until a gesture, so the
        // first frames are silent. Report the transition once in each direction,
        // so a web run's log tells "waiting for the user" apart from "audible".
        if (audio_device_ != nullptr && audio_device_->needs_user_gesture() != audio_gesture_reported_) {
            audio_gesture_reported_ = audio_device_->needs_user_gesture();
            FE_LOG_INFO(audio_gesture_reported_ ? "breakout: audio suspended until the first user gesture"
                                                : "breakout: audio resumed by a user gesture");
        }

        // Restart: whoever owns the scene rebuilds it. The systems look their
        // nodes up by name each frame, so only the state and the scene change.
        if (state_.consume_restart()) {
            rebuild();
            FE_LOG_INFO("breakout: restarted");
        }

        // A cleared board advances the level: the rules have already kept the
        // score and the lives and raised the ball's speed, so only the bricks the
        // last level broke need to come back. Rebuilding the field is what brings
        // them back; un-breaking them in place would be a second code path for the
        // same "fresh board".
        if (state_.consume_level_change()) {
            rebuild();
            FE_LOG_INFO("breakout: level %d of %d", state_.level(), state_.level_count());
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

    // Rebuilds the field: clear the scene, build it again. It does not touch the
    // rules, because each caller has already made the state transition it wants
    // (a restart resets, a level advance keeps the score and the lives).
    void rebuild() {
        scene().clear();
        breakout::build_breakout_scene(scene(), load_assets(engine().resources()));
        attach_sound();
    }

    breakout::BreakoutState state_{};
    bool headless_ = false;
    std::string scene_to_save_;

    // The device is owned here; the AudioSystem (owned by the schedule) borrows
    // it. Neither is touched after the loop ends, so the device being destroyed
    // first is safe. audio_system_ is a borrowed pointer used from on_stop().
    std::unique_ptr<fe::audio::AudioDevice> audio_device_;
    // Whether the last reported gesture state was "waiting for the user". Starts
    // false so a device that never needs a gesture logs nothing, and the first
    // suspended frame logs once.
    bool audio_gesture_reported_ = false;
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

#if defined(FE_PLATFORM_WEB)
// The application the browser's frame callback drives, and the callback itself.
// It is owned by a file-scope pointer, not a local in main(): a frame rate of 0
// tells Emscripten to return from main() and call back from
// requestAnimationFrame, so a stack-local application would be destroyed before
// the first frame ran and the callback would then step freed memory.
std::unique_ptr<BreakoutApp> web_app;

void web_frame(void* /*arg*/) {
    // step_frame() returns false once the run is over. Cancel the loop and tear
    // down there, so shutdown() runs exactly once and no frame follows it.
    if (web_app == nullptr || !web_app->step_frame()) {
        emscripten_cancel_main_loop();
        if (web_app != nullptr) {
            web_app->shutdown();
            web_app.reset();
        }
    }
}
#endif

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

#if defined(FE_PLATFORM_WEB)
    // Drive the loop from the page instead of blocking in run(): the engine's
    // initialize/step_frame/shutdown primitives exist for a host that owns the
    // frame schedule (see Application.h), and a browser is such a host.
    web_app = std::make_unique<BreakoutApp>(config, options.headless, options.scene_to_save);
    if (options.headless) {
        web_app->set_headless(options.headless_frames);
    }
    if (fe::Error error = web_app->initialize(); error) {
        FE_LOG_CRITICAL("breakout: %s", error.message().c_str());
        web_app.reset();
        return 1;
    }
    // Frame rate 0 means "as fast as the browser schedules", which is
    // requestAnimationFrame's own cadence; the callback above cancels the loop.
    emscripten_set_main_loop_arg(web_frame, nullptr, 0, 0);
    return 0;
#else
    BreakoutApp app(config, options.headless, options.scene_to_save);
    if (options.headless) {
        app.set_headless(options.headless_frames);
    }
    return app.run();
#endif
}
