// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the profiler panel's view model. The view model is the headless
// half of the panel: it turns a profiler's latest frame into rows and counters.
// These tests exercise that mapping with hand-built reports and with a real
// profiler, and assert only on structure and presence -- never on absolute
// timings, which depend on the machine.

#include <doctest/doctest.h>

#include "fe/core/Profiler.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/ProfilerPanelViewModel.h"

#include <string>

using namespace fe;
using namespace fe::editor;

namespace {

// Installs a profiler as the thread's current one for the lifetime of the test,
// and clears it again so a later test cannot reach a destroyed stack object.
// Mirrors the helper in test_profiler.cpp.
struct ScopedCurrent {
    explicit ScopedCurrent(Profiler& profiler) { Profiler::set_current(&profiler); }
    ~ScopedCurrent() { Profiler::set_current(nullptr); }
};

// A report with `scope_count` scopes and `counter_count` counters, filled with
// recognisable names and values. A plain helper so each test states only what it
// cares about.
FrameReport make_report(u32 scope_count, u32 counter_count, f64 total_ms) {
    FrameReport frame;
    frame.total_ms = total_ms;
    frame.update_ms = total_ms * 0.6;
    frame.render_ms = total_ms * 0.4;
    frame.scope_count = scope_count;
    frame.counter_count = counter_count;
    for (u32 i = 0; i < scope_count; ++i) {
        frame.scopes[i].name = "scope_" + std::to_string(i);
        frame.scopes[i].total_ms = static_cast<f64>(i + 1);
        frame.scopes[i].count = i + 1;
    }
    for (u32 i = 0; i < counter_count; ++i) {
        frame.counters[i].name = "counter_" + std::to_string(i);
        frame.counters[i].value = static_cast<i64>(i) * 10;
    }
    return frame;
}

} // namespace

TEST_CASE("capture maps a frame report into totals, scopes, and counters") {
    ProfilerPanelViewModel view_model;
    CHECK_FALSE(view_model.has_frame());

    view_model.capture(make_report(3, 2, 12.5));

    CHECK(view_model.has_frame());
    CHECK(view_model.total_ms() == doctest::Approx(12.5));
    CHECK(view_model.update_ms() == doctest::Approx(7.5));
    CHECK(view_model.render_ms() == doctest::Approx(5.0));

    REQUIRE(view_model.scopes().size() == 3);
    CHECK(view_model.scopes()[0].name == "scope_0");
    CHECK(view_model.scopes()[0].total_ms == doctest::Approx(1.0));
    CHECK(view_model.scopes()[0].count == 1);
    CHECK(view_model.scopes()[2].name == "scope_2");
    CHECK(view_model.scopes()[2].count == 3);

    REQUIRE(view_model.counters().size() == 2);
    CHECK(view_model.counters()[0].name == "counter_0");
    CHECK(view_model.counters()[0].value == 0);
    CHECK(view_model.counters()[1].value == 10);
}

TEST_CASE("capture preserves the order the report already sorted by cost") {
    ProfilerPanelViewModel view_model;

    // FrameReport sorts scopes by descending total_ms; the view model must not
    // reorder them, so the panel shows the biggest cost first.
    FrameReport frame = make_report(3, 0, 10.0);
    frame.scopes[0] = ScopeAggregate{"render", 9.0, 1};
    frame.scopes[1] = ScopeAggregate{"update", 6.0, 1};
    frame.scopes[2] = ScopeAggregate{"audio", 1.0, 1};

    view_model.capture(frame);

    REQUIRE(view_model.scopes().size() == 3);
    CHECK(view_model.scopes()[0].name == "render");
    CHECK(view_model.scopes()[1].name == "update");
    CHECK(view_model.scopes()[2].name == "audio");
}

TEST_CASE("an empty frame is captured as an empty, present snapshot") {
    ProfilerPanelViewModel view_model;

    view_model.capture(make_report(0, 0, 3.0));

    CHECK(view_model.has_frame());
    CHECK(view_model.scopes().empty());
    CHECK(view_model.counters().empty());
    CHECK_FALSE(view_model.truncated());
}

TEST_CASE("a truncated frame is surfaced, not hidden") {
    ProfilerPanelViewModel view_model;

    FrameReport frame = make_report(1, 0, 5.0);
    frame.dropped_names = 7;
    view_model.capture(frame);

    CHECK(view_model.truncated());
    CHECK(view_model.dropped_names() == 7);
}

TEST_CASE("clear returns the view model to its empty state") {
    ProfilerPanelViewModel view_model;
    view_model.capture(make_report(2, 1, 4.0));
    REQUIRE(view_model.has_frame());

    view_model.clear();

    CHECK_FALSE(view_model.has_frame());
    CHECK(view_model.scopes().empty());
    CHECK(view_model.counters().empty());
    CHECK(view_model.total_ms() == doctest::Approx(0.0));
    CHECK_FALSE(view_model.truncated());
}

TEST_CASE("capturing a profiler with no committed frame changes nothing") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    ProfilerPanelViewModel view_model;
    view_model.capture(profiler);

    CHECK_FALSE(view_model.has_frame());
    CHECK(view_model.capture_count() == 0);
}

TEST_CASE("capturing a profiler reads its latest committed frame") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope scope("work");
        profiler.add_counter("draws", 42);
    }
    REQUIRE(profiler.end_frame());

    ProfilerPanelViewModel view_model;
    view_model.capture(profiler);

    CHECK(view_model.has_frame());
    CHECK(view_model.capture_count() == 1);
    // The frame's wall-clock length is machine-dependent, so assert only that it
    // is a non-negative duration; the presence of the recorded names is the real
    // check.
    CHECK(view_model.total_ms() >= 0.0);

    bool found_work = false;
    for (const ProfilerPanelViewModel::ScopeRow& row : view_model.scopes()) {
        if (row.name == "work") {
            found_work = true;
            CHECK(row.count >= 1);
        }
    }
    CHECK(found_work);

    bool found_counter = false;
    for (const ProfilerPanelViewModel::CounterRow& row : view_model.counters()) {
        if (row.name == "draws") {
            found_counter = true;
            CHECK(row.value == 42);
        }
    }
    CHECK(found_counter);
}

TEST_CASE("a later capture replaces the previous snapshot") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope scope("first");
    }
    REQUIRE(profiler.end_frame());

    ProfilerPanelViewModel view_model;
    view_model.capture(profiler);
    REQUIRE(view_model.has_frame());

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope scope("second");
    }
    REQUIRE(profiler.end_frame());

    view_model.capture(profiler);

    CHECK(view_model.capture_count() == 2);
    for (const ProfilerPanelViewModel::ScopeRow& row : view_model.scopes()) {
        CHECK(row.name != "first");
    }
}

TEST_CASE("the editor context creates its profiler lazily and once") {
    EditorContext context;
    CHECK(context.profiler() == nullptr);

    Profiler& created = context.ensure_profiler();
    CHECK(context.profiler() == &created);

    // A second call returns the same object rather than replacing it, so the
    // panel's view of the profiler is stable across frames.
    Profiler& again = context.ensure_profiler();
    CHECK(&again == &created);
}
