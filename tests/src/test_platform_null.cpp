// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the null/headless platform backend (ADR-0041).
//
// These run with no display and no GL context by design: that is the feature
// under test. The null window is exercised directly, and the engine and the
// application frame loop are run headless to prove the whole stack comes up and
// advances without a surface.

#include <doctest/doctest.h>

#include "fe/platform/InputState.h"
#include "fe/platform/Window.h"
#include "fe/runtime/Application.h"
#include "fe/runtime/Engine.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/systems/SceneSchedule.h"
#include "fe/systems/System.h"

#include <string_view>

using namespace fe;

namespace {

class CountSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "CountSystem"; }

    void update(const FrameContext&, Scene&) override { ++runs_; }

    [[nodiscard]] u64 runs() const noexcept { return runs_; }

private:
    u64 runs_ = 0;
};

// A minimal Application that records what the headless loop did, so a test can
// assert the frame loop ran without a display.
class HeadlessLoopApp final : public Application {
public:
    explicit HeadlessLoopApp(Engine::Config config) : Application(config) {}

    u64 updates = 0;
    bool had_graphics_at_stop = true;

protected:
    Error on_start() override {
        (void)scene().create_node("Node");
        schedule().add_system<CountSystem>();
        return Error::none();
    }

    void on_update(f32) override { ++updates; }

    void on_stop() override { had_graphics_at_stop = engine().has_graphics(); }
};

[[nodiscard]] Engine::Config null_config() {
    Engine::Config config;
    config.window.backend = WindowBackend::Null;
    config.window.width = 320;
    config.window.height = 200;
    config.log_level = LogLevel::Warn;
    return config;
}

} // namespace

TEST_CASE("the default window backend is the real one") {
    // A plain-data check: a WindowDesc{} must still mean "make me a real window",
    // so existing callers are unaffected by the null backend's arrival.
    CHECK(WindowDesc{}.backend == WindowBackend::GLFW);
}

TEST_CASE("the null window needs no display and reports no context") {
    WindowDesc desc;
    desc.backend = WindowBackend::Null;
    desc.width = 640;
    desc.height = 360;

    auto created = Window::create(desc);
    REQUIRE(created.ok());
    const Window& window = *created.value();

    CHECK(window.has_context() == false);
    CHECK(std::string_view(window.backend_name()) == "null");
    CHECK(window.native_handle() == nullptr);
    CHECK(window.get_proc_address("glClear") == nullptr);

    // The logical size is the framebuffer size: there is no scale factor and no
    // surface to differ from.
    CHECK(window.width() == 640);
    CHECK(window.height() == 360);
    CHECK(window.framebuffer_width() == 640);
    CHECK(window.framebuffer_height() == 360);
}

TEST_CASE("the null window closes only when asked") {
    WindowDesc desc;
    desc.backend = WindowBackend::Null;

    auto created = Window::create(desc);
    REQUIRE(created.ok());
    Window& window = *created.value();

    CHECK_FALSE(window.should_close());
    window.request_close();
    CHECK(window.should_close());
}

TEST_CASE("the null window's event and present calls are harmless no-ops") {
    WindowDesc desc;
    desc.backend = WindowBackend::Null;

    auto created = Window::create(desc);
    REQUIRE(created.ok());
    Window& window = *created.value();

    // Nothing to poll, nothing to present, no title to set, no swap interval to
    // honour: all must be safe to call so the frame loop does not branch on the
    // backend.
    window.poll_events();
    window.swap_buffers();
    window.set_title("ignored");
    window.set_vsync(true);
    window.input().begin_frame();
    window.poll_events();

    // No events are ever delivered, so the input snapshot stays empty.
    CHECK_FALSE(window.input().is_key_down(Key::W));
    CHECK_FALSE(window.input().is_mouse_down(MouseButton::Left));
}

TEST_CASE("a headless engine comes up without a graphics device") {
    Engine engine;
    REQUIRE(engine.initialize(null_config()).ok());

    CHECK(engine.is_initialized());
    CHECK(engine.has_graphics() == false);
    CHECK(std::string_view(engine.window().backend_name()) == "null");

    // Resources and audio still exist: only the graphics stack is skipped.
    CHECK(engine.resources().loaded_count() == 0);
    CHECK(engine.audio_system() != nullptr);

    // The clock still runs, so gameplay systems that use delta time behave.
    engine.tick();
    CHECK(engine.elapsed_time() >= 0.0);

    engine.shutdown();
    CHECK_FALSE(engine.is_initialized());
}

TEST_CASE("a headless application runs its loop and its systems with no display") {
    Engine::Config config = null_config();
    HeadlessLoopApp app(config);
    app.set_headless(5);

    CHECK(app.run() == 0);

    // The frame loop ran exactly the requested number of frames, and the
    // schedule ran a system each frame, with no window and no GL context.
    CHECK(app.updates == 5);
    const System* system = app.schedule().find("CountSystem");
    REQUIRE(system != nullptr);
    CHECK(static_cast<const CountSystem*>(system)->runs() == 5);

    // The engine never claimed a surface.
    CHECK(app.had_graphics_at_stop == false);
}
