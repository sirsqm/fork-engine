// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <array>
#include <chrono>
#include <string>
#include <string_view>

namespace fe {

// Forward declaration: Profiler::frame_report() returns one by value, and the
// full definition (which needs Profiler's capacities) follows the class.
struct FrameReport;

// A frame profiler: named CPU scopes and integer counters, recorded per frame
// into a fixed ring buffer.
//
// Design notes (see docs/adr/0022-profiler-and-frame-timing.md):
//   * Fixed capacity everywhere. Recording a scope or a counter never allocates
//     and never grows: the storage is a preallocated ring of frame samples.
//   * Scope and counter names are interned once into a fixed name table, so the
//     hot path works with small integer handles instead of strings.
//   * A frame is bracketed by begin_frame()/end_frame(). Scopes opened outside a
//     frame are ignored, so a stray scope cannot corrupt a sample.
//   * The "current" profiler is thread-local; ProfileScope attaches to it on
//     construction. With no current profiler set on the thread, a scope is a
//     no-op.
//
// Durations are milliseconds (f64). Time comes from std::chrono::steady_clock,
// which is monotonic, so a wall-clock adjustment cannot make a duration negative.
//
// The object is large by design (the ring is inline storage, roughly a quarter
// of a megabyte at the default capacities). Make it a long-lived member, or
// heap-allocate it; do not create one per frame or per scope.
class Profiler {
public:
    // Capacities. Deliberately modest: a frame that exceeds them is a signal in
    // itself, and overflows are counted rather than dropped silently.
    static constexpr usize kMaxFrames = 64; // ~1 s at 60 Hz
    static constexpr usize kMaxScopesPerFrame = 128;
    static constexpr usize kMaxCountersPerFrame = 32;
    static constexpr usize kMaxNames = 128;
    static constexpr usize kMaxNameLength = 63;
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

    // One recorded scope: which name, when it started, how long it ran, and how
    // deeply it was nested (0 is the outermost scope in a frame).
    struct ScopeSample {
        u32 name = kInvalidIndex;
        f64 start_ms = 0.0;
        f64 duration_ms = 0.0;
        u32 depth = 0;
    };

    // One counter accumulated within a frame.
    struct CounterSample {
        u32 name = kInvalidIndex;
        i64 value = 0;
    };

    // A single frame's worth of samples. The arrays are fixed; scope_count and
    // counter_count say how many entries are live.
    struct FrameSample {
        f64 begin_ms = 0.0;
        f64 end_ms = 0.0;
        u32 scope_count = 0;
        u32 counter_count = 0;
        u32 dropped_scopes = 0;
        u32 dropped_counters = 0;
        std::array<ScopeSample, kMaxScopesPerFrame> scopes{};
        std::array<CounterSample, kMaxCountersPerFrame> counters{};
    };

    // Aggregate duration statistics for one scope name across all committed
    // frames. Times are milliseconds.
    struct ScopeStats {
        f64 last_ms = 0.0;
        f64 min_ms = 0.0;
        f64 max_ms = 0.0;
        f64 avg_ms = 0.0;
        usize count = 0;
    };

    Profiler() noexcept = default;

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;
    Profiler(Profiler&&) = delete;
    Profiler& operator=(Profiler&&) = delete;

    // --- Thread-local current profiler -------------------------------------
    // ProfileScope reads this on construction. Set it to profile a region.
    [[nodiscard]] static Profiler* current() noexcept;
    static void set_current(Profiler* profiler) noexcept;

    // --- Frame boundary ----------------------------------------------------
    // Opens a new frame. Fails if a frame is already in progress.
    [[nodiscard]] Result<void> begin_frame();
    // Closes the current frame and commits it to the ring. Fails if no frame is
    // in progress.
    [[nodiscard]] Result<void> end_frame();
    [[nodiscard]] bool frame_in_progress() const noexcept { return in_frame_; }

    // --- Recording (used by ProfileScope and by systems) -------------------
    // Opens a scope in the current frame and returns a handle for end_scope, or
    // kInvalidIndex when the scope is not recorded (no frame in progress, or the
    // frame is full). Never allocates.
    //
    // `generation` is the caller's view of frame_generation(). It is compared
    // against the profiler's at the moment of the call: a stale caller (one that
    // captured the generation before this frame began, or after it ended) is
    // refused, so it can never open a scope inside a frame it does not belong
    // to. A caller that has no generation to offer (a test opening a scope by
    // hand) may pass the current frame_generation().
    [[nodiscard]] u32 begin_scope(std::string_view name, u64 generation) noexcept;
    // True when `generation` still identifies the frame currently in progress.
    // ProfileScope uses this to skip the (very cheap) begin_scope call entirely
    // when there is nothing to record, which is the common case when no frame is
    // running or the profiler was swapped out mid-frame.
    [[nodiscard]] bool scope_active(u64 generation) const noexcept {
        return in_frame_ && generation == frame_generation_;
    }
    // Closes the scope opened with `handle` and records its duration. The
    // `generation` is the value frame_generation() had when the scope opened: if
    // it no longer matches, the scope outlived its frame and is ignored, so a
    // scope can never write into a later frame's sample.
    void end_scope(u32 handle, u64 generation) noexcept;
    // Monotonic counter incremented at each begin_frame(). Identifies a frame.
    [[nodiscard]] u64 frame_generation() const noexcept { return frame_generation_; }

    // Adds to a counter in the current frame, creating it on first use. Never
    // allocates. Ignored when there is no frame in progress.
    void add_counter(std::string_view name, i64 value = 1) noexcept;

    // --- Queries -----------------------------------------------------------
    // Number of completed frames available in the ring.
    [[nodiscard]] usize frame_count() const noexcept { return committed_; }
    // offset 0 is the most recent completed frame. Returns nullptr when out of
    // range (including when no frame has been committed yet).
    [[nodiscard]] const FrameSample* frame_from_latest(usize offset) const noexcept;
    // The frame currently being recorded, or nullptr when none is in progress.
    [[nodiscard]] const FrameSample* in_progress_frame() const noexcept;

    // Aggregate duration stats for a named scope over all committed frames.
    // Fails if the name was never recorded.
    [[nodiscard]] Result<ScopeStats> scope_stats(std::string_view name) const;

    // Accumulated value of a named counter in a completed frame (offset 0 is the
    // most recent). Fails if the counter name is unknown or absent from that
    // frame, or if the offset is out of range.
    [[nodiscard]] Result<i64> counter_value(std::string_view name, usize frame_offset = 0) const;

    // The interned name for a handle, or an empty view for kInvalidIndex.
    [[nodiscard]] std::string_view name_of(u32 index) const noexcept;

    // Copies the most recent completed frame into a self-contained snapshot
    // (frame offset 0 is the most recent; offset 1 the one before). The snapshot
    // owns its data and its names, so it stays valid as the ring advances -- a
    // caller can hand it to a UI or a log without holding the profiler.
    //
    // Aggregation happens here, in one walk of the frame's samples, so the
    // snapshot is ready to print. Fails if the offset is out of range (no frame
    // has been committed, or the offset is older than the ring).
    [[nodiscard]] Result<FrameReport> frame_report(usize frame_offset = 0) const;

private:
    using Clock = std::chrono::steady_clock;

    // Interns a name, adding it to the table on first sight. Returns
    // kInvalidIndex for an empty name or when the table is full.
    [[nodiscard]] u32 intern(std::string_view name) noexcept;
    // Looks a name up without adding it.
    [[nodiscard]] u32 find_name(std::string_view name) const noexcept;
    [[nodiscard]] f64 now_ms() const noexcept;

    std::array<std::array<char, kMaxNameLength + 1>, kMaxNames> names_{};
    u32 name_count_ = 0;

    std::array<FrameSample, kMaxFrames> frames_{};
    u32 write_index_ = 0; // the frame being filled, or the next to fill
    usize committed_ = 0; // completed frames available in the ring
    bool in_frame_ = false;
    u32 depth_ = 0;
    u64 frame_generation_ = 0;

    Clock::time_point start_{Clock::now()};
};

// RAII scope: records a named duration from construction to destruction on the
// thread's current profiler. A no-op when there is no current profiler, when no
// frame is in progress, or when the frame is full.
//
// Cost when disabled or idle: one thread-local read and one branch. When a frame
// is in progress, one now() sample on open and one on close. No allocation, ever.
// Correct across early returns: the destructor runs on every exit path.
class ProfileScope {
public:
    explicit ProfileScope(std::string_view name) noexcept
        : profiler_(Profiler::current()), handle_(Profiler::kInvalidIndex), generation_(0) {
        if (profiler_ == nullptr) {
            return;
        }
        // Capture the generation first; then only open the scope if that
        // generation still matches the frame in progress. If a frame begins or
        // ends between the two reads, the scope is simply not recorded -- it can
        // never land in the wrong frame.
        generation_ = profiler_->frame_generation();
        if (profiler_->scope_active(generation_)) {
            handle_ = profiler_->begin_scope(name, generation_);
        }
    }

    ~ProfileScope() {
        if (profiler_ != nullptr) {
            profiler_->end_scope(handle_, generation_);
        }
    }

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
    ProfileScope(ProfileScope&&) = delete;
    ProfileScope& operator=(ProfileScope&&) = delete;

private:
    Profiler* profiler_;
    u32 handle_;
    u64 generation_;
};

// One scope name's contribution to a frame: the total duration recorded under
// that name (a name can appear more than once, e.g. a system that runs twice)
// and how many samples made it up.
struct ScopeAggregate {
    std::string name;
    f64 total_ms = 0.0;
    u32 count = 0;
};

// One counter's value in a frame. Owns its name (unlike the profiler's internal
// CounterSample, which stores an interned handle), so the snapshot is
// self-contained.
struct CounterAggregate {
    std::string name;
    i64 value = 0;
};

// A self-contained snapshot of one completed frame.
//
// This is the read-only surface a caller (a test, CI, a log, or the editor's
// profiler panel) consumes. It is a value: it owns its name strings and its
// arrays, so it is independent of the profiler's ring and safe to keep, copy,
// and pass across threads. `total_ms` is the wall-clock length of the frame;
// `update_ms` and `render_ms` are the two top-level regions the runtime
// brackets, and `scopes` is the per-name breakdown of the whole frame.
struct FrameReport {
    f64 total_ms = 0.0;     // wall-clock length of the frame (end - begin)
    f64 update_ms = 0.0;    // the "frame/update" region (hooks + systems)
    f64 render_ms = 0.0;    // the "frame/render" region
    u32 sample_count = 0;   // scope samples recorded this frame
    u32 dropped_scopes = 0; // samples the frame capacity could not hold
    u32 dropped_counters = 0;

    // Per-name totals, sorted by descending total_ms so the biggest cost is
    // first. Fixed capacity: a frame with more distinct names than this is
    // truncated (counted by dropped_names).
    static constexpr usize kMaxAggregates = Profiler::kMaxScopesPerFrame;
    std::array<ScopeAggregate, kMaxAggregates> scopes{};
    u32 scope_count = 0; // live entries in scopes
    u32 dropped_names = 0;

    // The counter values recorded this frame (name + accumulated value).
    static constexpr usize kMaxCounters = Profiler::kMaxCountersPerFrame;
    std::array<CounterAggregate, kMaxCounters> counters{};
    u32 counter_count = 0;

    // A short, stable, single-line summary of the frame, e.g.
    //   "frame 1.23ms | update 0.90ms | render 0.30ms | 12 scopes | 2 counters"
    // Intended for logs, tests, and CI. Deterministic given the snapshot.
    [[nodiscard]] std::string summary() const;

    // The full breakdown as text, one line per scope (descending cost) and one
    // per counter, prefixed by summary(). For a human or a CI artifact.
    [[nodiscard]] std::string to_string() const;
};

// --- Instrumentation macros ------------------------------------------------
//
// FE_PROFILE_SCOPE("name")    opens a named timing sample for the enclosing
//                             block and closes it on scope exit (RAII).
// FE_PROFILE_FUNCTION()       the same, named after the current function.
//
// When FE_PROFILING is 0 the macros expand to nothing: no object, no call, no
// name string, no unused-variable warning. Set FE_PROFILING=0 to compile the
// instrumentation out entirely (see docs/adr/0024-profiler-instrumentation.md).
#ifndef FE_PROFILING
#define FE_PROFILING 1
#endif

#if FE_PROFILING
// Two-level concat so __COUNTER__ is expanded before pasting (a single ## would
// paste the literal token). __COUNTER__ is unique per use, so two scopes on one
// line still get distinct variables.
#define FE_PROFILE_CONCAT_(a, b) a##b
#define FE_PROFILE_CONCAT(a, b) FE_PROFILE_CONCAT_(a, b)
#define FE_PROFILE_SCOPE(name) ::fe::ProfileScope FE_PROFILE_CONCAT(fe_profile_scope_, __COUNTER__)(name)
#define FE_PROFILE_FUNCTION() FE_PROFILE_SCOPE(__func__)
#else
#define FE_PROFILE_SCOPE(name) ((void)0)
#define FE_PROFILE_FUNCTION() ((void)0)
#endif

} // namespace fe
