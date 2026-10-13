// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The runtime integration, end to end: a real Application embeds an Engine,
// runs a fixed number of headless frames, and the profiler reports those frames.
// This is what proves the profiler is fed by the actual frame loop rather than
// only by hand-written test scopes. It needs a GL context, so it lives with the
// graphics tests and skips cleanly when none is available.

#include <doctest/doctest.h>

#include "fe/core/Profiler.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/systems/System.h"

#include <string>
#include <string_view>

using namespace fe;

namespace {

// A system that does a little work so it shows up in the frame breakdown.
class SpinSystem final : public System {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "SpinSystem"; }
    void update(const FrameContext&, Scene&) override {
        volatile u64 acc = 0;
        for (u64 i = 0; i < 1000; ++i) {
            acc += i;
        }
        (void)acc;
    }
};

// A tiny application that sets up a camera, one system, and a fixed headless
// frame count. It captures the last frame report in on_stop(), while the engine
// (and the profiler) are still alive.
class ProbeApp final : public Application {
public:
    ProbeApp(u32 frames, bool profiling) : Application(make_config()) {
        set_headless(frames);
        set_profiling(profiling);
    }

    Result<FrameReport> report{Error("not run")};
    bool started = false;

protected:
    Error on_start() override {
        started = true;
        auto* camera_node = scene().create_node("Camera");
        auto* camera = camera_node->add_component<Camera2D>();
        camera->primary = true;
        schedule().add_system<SpinSystem>();
        return Error::none();
    }

    void on_stop() override { report = last_frame_report(); }

private:
    static Engine::Config make_config() {
        Engine::Config config;
        config.window.title = "fork engine profiler runtime test";
        config.window.width = 64;
        config.window.height = 64;
        config.window.visible = false;
        config.window.vsync = false;
        return config;
    }
};

} // namespace

TEST_CASE("the runtime frame loop feeds the profiler and yields a report") {
    // A GL context may be unavailable on a machine with no display; Application
    // then fails to initialise and run() returns non-zero. Report and pass, the
    // same convention the renderer tests use, so ctest stays green.
    ProbeApp app(4, true);
    const int code = app.run();
    if (!app.started) {
        MESSAGE("graphics context unavailable; runtime profiler test skipped");
        return;
    }
    REQUIRE(code == 0);

    // The loop committed one frame per iteration.
    CHECK(app.profiler().frame_count() == 4);

    const Result<FrameReport> report = app.report;
    REQUIRE(report);
    const FrameReport& r = report.value();

    // The frame was bracketed by the runtime, not by the test.
    CHECK(r.total_ms >= 0.0);
    CHECK(r.update_ms >= 0.0);
    CHECK(r.render_ms >= 0.0);

    // The two top-level regions and the per-system sample are all present,
    // proving the loop and the schedule both fed the profiler.
    bool saw_update = false;
    bool saw_render = false;
    bool saw_system = false;
    for (u32 i = 0; i < r.scope_count; ++i) {
        saw_update = saw_update || r.scopes[i].name == "frame/update";
        saw_render = saw_render || r.scopes[i].name == "frame/render";
        saw_system = saw_system || r.scopes[i].name == "system/SpinSystem";
    }
    CHECK(saw_update);
    CHECK(saw_render);
    CHECK(saw_system);

    // The breakdown is a snapshot the caller can print, headless.
    CHECK(r.summary().find("frame ") == 0);
}

TEST_CASE("an application with profiling disabled commits no frames") {
    ProbeApp app(3, false);
    const int code = app.run();
    if (!app.started) {
        MESSAGE("graphics context unavailable; runtime profiler test skipped");
        return;
    }
    REQUIRE(code == 0);
    CHECK(app.profiler().frame_count() == 0);
    CHECK_FALSE(app.report); // last_frame_report() reports that profiling is off
}
