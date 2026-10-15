// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Profiler.h"
#include "fe/core/Types.h"

#include <string>
#include <vector>

namespace fe::editor {

// Pure, headless view model for the profiler panel.
//
// The panel's rendering is a thin projection of this type: the view model turns
// a profiler's most recent frame into the rows and counters the panel draws, and
// the ImGui code only emits widgets. Keeping the decision logic here means the
// panel's content is testable without a window, a GPU, or ImGui -- the same
// arrangement the hierarchy and asset-browser view models use.
//
// It is a read-only client of the profiler. `capture` takes the profiler by
// const reference and reads its latest committed frame through the public
// `frame_report` surface; it never touches the profiler's ring directly. It does
// not decide *when* to sample -- the host owns the frame loop and calls
// `capture` once per frame -- and it does not format for a human beyond the
// plain `name`/`total_ms` fields, so a test can assert on the data rather than on
// presentation.
class ProfilerPanelViewModel {
public:
    // One scope-name row, in the panel's display order (descending total time,
    // which is the order `FrameReport` already uses). The strings are owned, so
    // a snapshot is safe to keep after the profiler's ring has advanced.
    struct ScopeRow {
        std::string name;
        f64 total_ms = 0.0;
        u32 count = 0;
    };

    // One counter row, in the order the frame recorded them.
    struct CounterRow {
        std::string name;
        i64 value = 0;
    };

    ProfilerPanelViewModel() = default;

    // Replaces the snapshot with `profiler`'s most recent committed frame.
    //
    // The read is bracketed with a named scope, so the panel's own cost shows up
    // in the profiler's breakdown (the panel is a consumer of the profiler it
    // displays). If no frame has been committed yet, the last good snapshot is
    // kept rather than blanked, so the panel does not flicker empty on startup.
    void capture(const Profiler& profiler);

    // Replaces the snapshot with `frame`. This is the pure reshaping step --
    // no profiler, no scope, no clock -- split out so the mapping from a
    // `FrameReport` to the panel's rows can be tested with a hand-built report.
    void capture(const FrameReport& frame);

    // Drops the snapshot, so the panel shows the empty state.
    void clear();

    // True once a frame has been captured. False before the first capture, when
    // the profiler has not committed a frame yet, or after clear().
    [[nodiscard]] bool has_frame() const noexcept { return has_frame_; }

    [[nodiscard]] const std::vector<ScopeRow>& scopes() const noexcept { return scopes_; }
    [[nodiscard]] const std::vector<CounterRow>& counters() const noexcept { return counters_; }

    // --- Frame totals ------------------------------------------------------
    [[nodiscard]] f64 total_ms() const noexcept { return total_ms_; }
    [[nodiscard]] f64 update_ms() const noexcept { return update_ms_; }
    [[nodiscard]] f64 render_ms() const noexcept { return render_ms_; }

    // --- Health flags ------------------------------------------------------
    // The frame capacity overflowed, so the snapshot is incomplete. Surfaced
    // rather than hidden: a truncated frame is a signal about the workload, not
    // a rendering detail.
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    // The number of scope names the frame could not hold (0 when not truncated).
    [[nodiscard]] u32 dropped_names() const noexcept { return dropped_names_; }

    // --- Own cost ----------------------------------------------------------
    // How many times the panel has captured a frame. The capture itself is
    // bracketed with a named scope, so its cost appears in the profiler's own
    // breakdown (the panel is a consumer of the profiler it displays); this
    // counter is just how many samples it has taken.
    [[nodiscard]] u64 capture_count() const noexcept { return capture_count_; }

private:
    bool has_frame_ = false;
    std::vector<ScopeRow> scopes_;
    std::vector<CounterRow> counters_;
    f64 total_ms_ = 0.0;
    f64 update_ms_ = 0.0;
    f64 render_ms_ = 0.0;
    bool truncated_ = false;
    u32 dropped_names_ = 0;
    u64 capture_count_ = 0;
};

} // namespace fe::editor
