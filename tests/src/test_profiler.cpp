// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/Profiler.h"

#include <algorithm>
#include <chrono>
#include <string_view>

using namespace fe;

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
