// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ProfilerPanelViewModel.h"

#include <algorithm>

namespace fe::editor {

void ProfilerPanelViewModel::capture(const Profiler& profiler) {
    // The panel profiles itself: the capture read is bracketed and counted, so
    // the read-out can report the cost of taking a read-out. FE_PROFILE_SCOPE is
    // a no-op when profiling is compiled out or no profiler is current, so this
    // adds nothing to a release build without instrumentation.
    FE_PROFILE_SCOPE("editor/profiler_panel/capture");

    const Result<FrameReport> report = profiler.frame_report();
    if (!report) {
        // No frame committed yet (the panel can be drawn before the loop has
        // completed one). Keep the last good snapshot rather than blanking, so
        // the panel does not flicker empty on startup.
        return;
    }

    capture(report.value());
    ++capture_count_;
}

void ProfilerPanelViewModel::capture(const FrameReport& frame) {
    total_ms_ = frame.total_ms;
    update_ms_ = frame.update_ms;
    render_ms_ = frame.render_ms;
    truncated_ = frame.dropped_names > 0;
    dropped_names_ = frame.dropped_names;

    scopes_.clear();
    scopes_.reserve(std::min<usize>(frame.scope_count, FrameReport::kMaxAggregates));
    for (u32 i = 0; i < frame.scope_count && i < FrameReport::kMaxAggregates; ++i) {
        const ScopeAggregate& scope = frame.scopes[i];
        scopes_.push_back(ScopeRow{scope.name, scope.total_ms, scope.count});
    }

    counters_.clear();
    counters_.reserve(std::min<usize>(frame.counter_count, FrameReport::kMaxCounters));
    for (u32 i = 0; i < frame.counter_count && i < FrameReport::kMaxCounters; ++i) {
        const CounterAggregate& counter = frame.counters[i];
        counters_.push_back(CounterRow{counter.name, counter.value});
    }

    has_frame_ = true;
}

void ProfilerPanelViewModel::clear() {
    has_frame_ = false;
    scopes_.clear();
    counters_.clear();
    total_ms_ = 0.0;
    update_ms_ = 0.0;
    render_ms_ = 0.0;
    truncated_ = false;
    dropped_names_ = 0;
}

} // namespace fe::editor
