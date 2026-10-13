// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/Profiler.h"
#include "fe/scene/Scene.h"
#include "fe/systems/SceneSchedule.h"
#include "fe/systems/System.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

using namespace fe;

namespace fe_test {
// Defined in test_profiler_disabled.cpp, which is compiled with FE_PROFILING=0.
// Calling it proves the macros expand to nothing and record no samples.
usize run_disabled_macros();
} // namespace fe_test

namespace {

// Installs a profiler as the thread's current one for the lifetime of the test,
// and clears it again so a later test cannot reach a destroyed stack object.
struct ScopedCurrent {
    explicit ScopedCurrent(Profiler& profiler) { Profiler::set_current(&profiler); }
    ~ScopedCurrent() { Profiler::set_current(nullptr); }
};

// Busy-waits until steady_clock advances. This guarantees a strictly positive
// measured interval without depending on timer magnitude, so the assertion
// cannot flake on a fast or coarse clock.
void spin_until_clock_advances() {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() == start) {
    }
}

const Profiler::ScopeSample* find_scope(const Profiler& profiler, const Profiler::FrameSample& frame,
                                        std::string_view name) {
    for (u32 i = 0; i < frame.scope_count; ++i) {
        if (profiler.name_of(frame.scopes[i].name) == name) {
            return &frame.scopes[i];
        }
    }
    return nullptr;
}

// A named system whose update body uses FE_PROFILE_FUNCTION, so the test can
// assert the macro names the sample after the enclosing function.
void the_function_under_test() {
    FE_PROFILE_FUNCTION();
    spin_until_clock_advances();
}

// A system that just yields, so the schedule's per-system scope is the only
// thing recorded. Named so the schedule labels it "system/<name>".
class NamedSystem final : public System {
public:
    explicit NamedSystem(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    void update(const FrameContext&, Scene&) override { spin_until_clock_advances(); }

private:
    std::string name_;
};

} // namespace

TEST_CASE("a scope records a positive duration") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope scope("work");
        spin_until_clock_advances();
    }
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    CHECK(frame->scope_count == 1);

    const Profiler::ScopeSample* scope = find_scope(profiler, *frame, "work");
    REQUIRE(scope != nullptr);
    CHECK(scope->duration_ms > 0.0);
    CHECK(scope->depth == 0);
}

TEST_CASE("nesting gives a parent duration at least as long as its child") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope parent("parent");
        spin_until_clock_advances();
        {
            ProfileScope child("child");
            spin_until_clock_advances();
        }
    }
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);

    const Profiler::ScopeSample* parent = find_scope(profiler, *frame, "parent");
    const Profiler::ScopeSample* child = find_scope(profiler, *frame, "child");
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);

    CHECK(parent->duration_ms >= child->duration_ms);
    CHECK(parent->depth == 0);
    CHECK(child->depth == 1);
}

TEST_CASE("the ring buffer keeps the newest frames and never overflows") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    constexpr usize kFrames = Profiler::kMaxFrames + 10;
    for (usize i = 0; i < kFrames; ++i) {
        REQUIRE(profiler.begin_frame());
        profiler.add_counter("frame_index", static_cast<i64>(i));
        {
            ProfileScope scope("tick");
        }
        REQUIRE(profiler.end_frame());
    }

    CHECK(profiler.frame_count() == Profiler::kMaxFrames);
    CHECK(profiler.frame_from_latest(0) != nullptr);
    CHECK(profiler.frame_from_latest(Profiler::kMaxFrames - 1) != nullptr);
    CHECK(profiler.frame_from_latest(Profiler::kMaxFrames) == nullptr);

    // The newest frame is the last one written; the oldest retained frame is
    // kMaxFrames - 1 behind it.
    const Result<i64> newest = profiler.counter_value("frame_index", 0);
    REQUIRE(newest);
    CHECK(newest.value() == static_cast<i64>(kFrames - 1));

    const Result<i64> oldest = profiler.counter_value("frame_index", Profiler::kMaxFrames - 1);
    REQUIRE(oldest);
    CHECK(oldest.value() == static_cast<i64>(kFrames - Profiler::kMaxFrames));
}

TEST_CASE("counters accumulate within a frame") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    profiler.add_counter("draws");
    profiler.add_counter("draws");
    profiler.add_counter("draws", 5);
    profiler.add_counter("instances", 42);
    REQUIRE(profiler.end_frame());

    const Result<i64> draws = profiler.counter_value("draws");
    REQUIRE(draws);
    CHECK(draws.value() == 7);

    const Result<i64> instances = profiler.counter_value("instances");
    REQUIRE(instances);
    CHECK(instances.value() == 42);

    // A counter that is unknown, or absent from the frame, is an expected
    // failure, not a crash.
    CHECK_FALSE(profiler.counter_value("unknown"));
}

TEST_CASE("scope statistics match the recorded frames") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    constexpr usize kFrames = 5;
    for (usize i = 0; i < kFrames; ++i) {
        REQUIRE(profiler.begin_frame());
        {
            ProfileScope scope("t");
            for (usize spin = 0; spin <= i; ++spin) {
                spin_until_clock_advances();
            }
        }
        REQUIRE(profiler.end_frame());
    }

    const Result<Profiler::ScopeStats> stats = profiler.scope_stats("t");
    REQUIRE(stats);
    CHECK(stats.value().count == kFrames);

    // Recompute the aggregate from the ring; the accessor must agree.
    f64 min_ms = 0.0;
    f64 max_ms = 0.0;
    f64 sum_ms = 0.0;
    usize count = 0;
    f64 newest_ms = 0.0;
    for (usize offset = 0; offset < profiler.frame_count(); ++offset) {
        const Profiler::FrameSample* frame = profiler.frame_from_latest(offset);
        REQUIRE(frame != nullptr);
        const Profiler::ScopeSample* scope = find_scope(profiler, *frame, "t");
        REQUIRE(scope != nullptr);
        if (count == 0) {
            min_ms = scope->duration_ms;
            max_ms = scope->duration_ms;
            newest_ms = scope->duration_ms;
        } else {
            min_ms = std::min(min_ms, scope->duration_ms);
            max_ms = std::max(max_ms, scope->duration_ms);
        }
        sum_ms += scope->duration_ms;
        ++count;
    }

    CHECK(stats.value().min_ms == doctest::Approx(min_ms));
    CHECK(stats.value().max_ms == doctest::Approx(max_ms));
    CHECK(stats.value().avg_ms == doctest::Approx(sum_ms / static_cast<f64>(count)));
    CHECK(stats.value().last_ms == doctest::Approx(newest_ms));
    CHECK(stats.value().min_ms <= stats.value().avg_ms);
    CHECK(stats.value().avg_ms <= stats.value().max_ms);
}

TEST_CASE("a never-started profiler answers queries safely") {
    Profiler profiler;

    CHECK(profiler.frame_count() == 0);
    CHECK(profiler.frame_from_latest(0) == nullptr);
    CHECK(profiler.in_progress_frame() == nullptr);
    CHECK(profiler.name_of(Profiler::kInvalidIndex).empty());

    CHECK_FALSE(profiler.scope_stats("anything"));
    CHECK_FALSE(profiler.counter_value("anything"));

    // end_frame without a matching begin_frame is an expected failure.
    CHECK_FALSE(profiler.end_frame());

    // A scope with no current profiler is a no-op.
    Profiler::set_current(nullptr);
    {
        ProfileScope scope("ignored");
    }
    CHECK(profiler.frame_count() == 0);
}

TEST_CASE("frame boundaries reject mismatched calls") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    CHECK(profiler.frame_in_progress());
    // A second begin_frame without an end_frame is an expected failure.
    CHECK_FALSE(profiler.begin_frame());
    REQUIRE(profiler.end_frame());
    CHECK_FALSE(profiler.frame_in_progress());

    // A scope outside any frame is ignored rather than recorded.
    {
        ProfileScope scope("outside");
    }
    CHECK(profiler.frame_count() == 1);

    // A scope that outlives its frame must not write into a later frame.
    REQUIRE(profiler.begin_frame());
    auto* late = new ProfileScope("late"); // a valid handle in this frame
    REQUIRE(profiler.end_frame());
    REQUIRE(profiler.begin_frame()); // a new frame generation
    delete late;                     // destroying it here must be ignored
    REQUIRE(profiler.end_frame());
    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    CHECK(frame->scope_count == 0);
}

TEST_CASE("FE_PROFILE_SCOPE records one sample with the right name and a non-negative duration") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        FE_PROFILE_SCOPE("macro/one");
        spin_until_clock_advances();
    }
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    CHECK(frame->scope_count == 1);
    const Profiler::ScopeSample* scope = find_scope(profiler, *frame, "macro/one");
    REQUIRE(scope != nullptr);
    CHECK(scope->duration_ms >= 0.0);
    CHECK(scope->depth == 0);
}

TEST_CASE("FE_PROFILE_FUNCTION names the sample after the function") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    the_function_under_test();
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->scope_count == 1);
    CHECK(profiler.name_of(frame->scopes[0].name) == "the_function_under_test");
}

TEST_CASE("the macros nest: parent at depth 0, child at depth 1") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        FE_PROFILE_SCOPE("macro/parent");
        spin_until_clock_advances();
        {
            FE_PROFILE_SCOPE("macro/child");
            spin_until_clock_advances();
        }
    }
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    const Profiler::ScopeSample* parent = find_scope(profiler, *frame, "macro/parent");
    const Profiler::ScopeSample* child = find_scope(profiler, *frame, "macro/child");
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);
    CHECK(parent->depth == 0);
    CHECK(child->depth == 1);
    CHECK(parent->duration_ms >= child->duration_ms);
}

TEST_CASE("a scope closes on an early return") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    const auto early = [](bool bail) {
        FE_PROFILE_SCOPE("macro/early");
        if (bail) {
            return 1;
        }
        return 2;
    };

    REQUIRE(profiler.begin_frame());
    CHECK(early(true) == 1);
    REQUIRE(profiler.end_frame());
    CHECK(profiler.frame_from_latest(0)->scope_count == 1);

    REQUIRE(profiler.begin_frame());
    CHECK(early(false) == 2);
    REQUIRE(profiler.end_frame());
    CHECK(profiler.frame_from_latest(0)->scope_count == 1);
}

TEST_CASE("two scopes on one line get distinct variables") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        FE_PROFILE_SCOPE("macro/a");
        FE_PROFILE_SCOPE("macro/b");
    }
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    CHECK(frame->scope_count == 2);
    CHECK(find_scope(profiler, *frame, "macro/a") != nullptr);
    CHECK(find_scope(profiler, *frame, "macro/b") != nullptr);
}

TEST_CASE("FE_PROFILING=0 expands the macros to nothing and records no samples") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    CHECK(fe_test::run_disabled_macros() == 0); // the disabled macros are no-ops
    REQUIRE(profiler.end_frame());
    CHECK(profiler.frame_from_latest(0)->scope_count == 0);
}

TEST_CASE("the frame report aggregates per-name totals, sorted by cost") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope update("frame/update");
        spin_until_clock_advances();
        spin_until_clock_advances();
        {
            ProfileScope a("system/A");
            spin_until_clock_advances();
        }
        {
            ProfileScope b("system/B");
        }
    }
    {
        ProfileScope render("frame/render");
        spin_until_clock_advances();
    }
    profiler.add_counter("draws", 3);
    profiler.add_counter("draws", 4);
    REQUIRE(profiler.end_frame());

    const Result<FrameReport> report = profiler.frame_report();
    REQUIRE(report);
    const FrameReport& r = report.value();

    // Structure: one entry per distinct name, none dropped, counters summed.
    CHECK(r.scope_count == 4);
    CHECK(r.dropped_names == 0);
    CHECK(r.sample_count == 4);
    CHECK(r.counter_count == 1);
    CHECK(r.counters[0].name == "draws");
    CHECK(r.counters[0].value == 7);

    // Sorted by descending total cost: the outer update region is the largest.
    CHECK(r.scopes[0].name == "frame/update");
    for (u32 i = 1; i < r.scope_count; ++i) {
        CHECK(r.scopes[i - 1].total_ms >= r.scopes[i].total_ms);
    }

    // The named regions are lifted out for the summary.
    CHECK(r.update_ms == doctest::Approx(r.scopes[0].total_ms));
    CHECK(r.render_ms >= 0.0);

    // Sum consistency within tolerance: the sum of the two bracketed regions is
    // close to the frame total, but not exactly equal. The frame spans work
    // outside them (clock tick, event poll, present) and the frame scope also
    // measures its own open/close. Allow 5% plus 1ms of absolute slack for a
    // very short frame. No wall-clock threshold is asserted.
    CHECK(r.update_ms + r.render_ms >= r.total_ms * 0.95 - 1.0);
    CHECK(r.update_ms <= r.total_ms);
    CHECK(r.render_ms <= r.total_ms);
}

TEST_CASE("the report summary is a stable, parseable single line") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope update("frame/update");
        ProfileScope render("frame/render");
    }
    profiler.add_counter("draws", 9);
    REQUIRE(profiler.end_frame());

    const FrameReport report = profiler.frame_report().value();
    const std::string summary = report.summary();
    CHECK(summary.find("frame ") == 0);
    CHECK(summary.find("update ") != std::string::npos);
    CHECK(summary.find("render ") != std::string::npos);
    CHECK(summary.find("2 scopes") != std::string::npos);
    CHECK(summary.find("1 counters") != std::string::npos);

    const std::string detail = report.to_string();
    CHECK(detail.find("frame/update") != std::string::npos);
    CHECK(detail.find("#draws = 9") != std::string::npos);
}

TEST_CASE("a report is a self-contained snapshot, independent of the ring") {
    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    {
        ProfileScope scope("frame/update");
        spin_until_clock_advances();
    }
    REQUIRE(profiler.end_frame());

    const FrameReport snapshot = profiler.frame_report().value();
    const f64 captured = snapshot.update_ms;
    const std::string name = snapshot.scopes[0].name;

    // Advance the ring well past the snapshot; the snapshot must not change.
    for (usize i = 0; i < Profiler::kMaxFrames + 3; ++i) {
        REQUIRE(profiler.begin_frame());
        {
            ProfileScope scope("other");
        }
        REQUIRE(profiler.end_frame());
    }

    CHECK(snapshot.update_ms == captured);
    CHECK(snapshot.scopes[0].name == name);
    CHECK(snapshot.scope_count == 1);
}

TEST_CASE("the schedule times each system by name") {
    Scene scene;
    SceneSchedule schedule;
    schedule.add_system<NamedSystem>("Alpha");
    schedule.add_system<NamedSystem>("Beta");

    Profiler profiler;
    ScopedCurrent current(profiler);

    REQUIRE(profiler.begin_frame());
    FrameContext context;
    schedule.update(context, scene);
    REQUIRE(profiler.end_frame());

    const Profiler::FrameSample* frame = profiler.frame_from_latest(0);
    REQUIRE(frame != nullptr);
    CHECK(frame->scope_count == 2);
    CHECK(find_scope(profiler, *frame, "system/Alpha") != nullptr);
    CHECK(find_scope(profiler, *frame, "system/Beta") != nullptr);

    const FrameReport report = profiler.frame_report().value();
    CHECK(report.scope_count == 2);
}

TEST_CASE("a report from an empty or out-of-range frame fails cleanly") {
    Profiler profiler;
    CHECK_FALSE(profiler.frame_report());

    ScopedCurrent current(profiler);
    REQUIRE(profiler.begin_frame());
    REQUIRE(profiler.end_frame());
    CHECK(profiler.frame_report(0));       // the one committed frame
    CHECK_FALSE(profiler.frame_report(1)); // nothing older
}
