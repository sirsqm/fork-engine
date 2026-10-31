// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/LogBuffer.h"
#include "fe/core/Types.h"
#include "fe/script/ScriptDiagnostics.h"

#include <deque>
#include <string>
#include <vector>

namespace fe::editor {

// Pure, headless view model for the console panel.
//
// The panel's rendering is a thin projection of this type, the same arrangement
// the hierarchy, asset-browser, and profiler view models use: the decision logic
// (which records to show, which levels pass the filter, whether the buffer has
// wrapped past the reader) lives here and is testable without a window or ImGui.
//
// Two sources are merged, because a game author debugging a script cares about
// both and they arrive by different routes:
//
//   * The engine log (`LogBuffer`), which carries everything the engine and the
//     gameplay code logged.
//   * The script host's contained failures (`ScriptDiagnostics`), which are
//     already in the log but are surfaced separately and structured, because a
//     disabled script is the single most useful thing to see and it is recorded
//     once rather than scrolling past.
//
// The view model keeps a bounded ring of its own, so a long run does not grow
// the panel's memory. It is fed incrementally: `poll` copies only records newer
// than the last one it saw, so a steady-state frame copies nothing, and it
// reports when the source's own buffer wrapped past the reader (records were
// dropped before the reader looked) rather than silently showing a gap.
class ConsolePanelViewModel {
public:
    struct Entry {
        LogLevel level = LogLevel::Info;
        std::string message;
        // True when this entry is a contained script failure rather than a plain
        // log record, so the panel can mark it.
        bool script_failure = false;
        u64 sequence = 0;
    };

    static constexpr usize kDefaultCapacity = 1024;

    explicit ConsolePanelViewModel(usize capacity = kDefaultCapacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // Copies any log records newer than the last poll into the ring. Returns how
    // many were added. Cheap when there is nothing new.
    usize poll(const LogBuffer& log);

    // Copies any contained script failures newer than the last poll into the
    // ring. Returns how many were added.
    usize poll(const ScriptDiagnostics& diagnostics);

    // The entries held, oldest first.
    [[nodiscard]] const std::deque<Entry>& entries() const noexcept { return entries_; }

    // The entries that pass the current level filter, oldest first. `filter()`
    // defaults to Info, so Trace/Debug are hidden until asked for.
    [[nodiscard]] std::vector<Entry> visible() const;

    // --- Filter ------------------------------------------------------------
    // The minimum level shown. Set to Trace to see everything.
    [[nodiscard]] LogLevel filter() const noexcept { return filter_; }
    void set_filter(LogLevel level) noexcept { filter_ = level; }

    // True when the filter hides at least one held entry, so the panel can say
    // "N hidden" rather than leaving the user to wonder.
    [[nodiscard]] bool has_hidden() const;

    [[nodiscard]] usize hidden_count() const;

    // --- Wrapping ----------------------------------------------------------
    // True when the log dropped records the reader never saw. Surfaced rather
    // than hidden: a gap the panel cannot show is a fact about the run.
    [[nodiscard]] bool log_truncated() const noexcept { return log_truncated_; }

    // The largest gap the reader has missed: records the log dropped before the
    // reader looked. Zero when it has kept up.
    [[nodiscard]] u64 log_dropped() const noexcept { return log_dropped_; }

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

    void clear();

private:
    void push(Entry entry);

    usize capacity_;
    std::deque<Entry> entries_;
    LogLevel filter_ = LogLevel::Info;
    u64 last_log_sequence_ = 0;
    u64 last_failure_sequence_ = 0;
    bool log_truncated_ = false;
    u64 log_dropped_ = 0;
};

} // namespace fe::editor
