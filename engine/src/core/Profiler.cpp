// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/Profiler.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace fe {

namespace {

// The current profiler is per-thread: a scope records on the thread that set it,
// so a worker thread does not silently write into the frame the main thread owns.
thread_local Profiler* tls_current = nullptr;

} // namespace

Profiler* Profiler::current() noexcept {
    return tls_current;
}

void Profiler::set_current(Profiler* profiler) noexcept {
    tls_current = profiler;
}

f64 Profiler::now_ms() const noexcept {
    const auto delta = Clock::now() - start_;
    return std::chrono::duration<f64, std::milli>(delta).count();
}

u32 Profiler::intern(std::string_view name) noexcept {
    if (name.empty()) {
        return kInvalidIndex;
    }
    if (const u32 existing = find_name(name); existing != kInvalidIndex) {
        return existing;
    }
    if (name_count_ >= kMaxNames) {
        return kInvalidIndex;
    }

    const usize length = name.size() < kMaxNameLength ? name.size() : kMaxNameLength;
    auto& slot = names_[name_count_];
    for (usize i = 0; i < length; ++i) {
        slot[i] = name[i];
    }
    slot[length] = '\0';
    return name_count_++;
}

u32 Profiler::find_name(std::string_view name) const noexcept {
    for (u32 i = 0; i < name_count_; ++i) {
        const char* stored = names_[i].data();
        if (std::string_view(stored) == name) {
            return i;
        }
    }
    return kInvalidIndex;
}

std::string_view Profiler::name_of(u32 index) const noexcept {
    if (index >= name_count_) {
        return {};
    }
    return std::string_view(names_[index].data());
}

Result<void> Profiler::begin_frame() {
    if (in_frame_) {
        return Error("profiler: begin_frame called while a frame is already in progress");
    }

    FrameSample& sample = frames_[write_index_];
    sample = FrameSample{};
    sample.begin_ms = now_ms();

    in_frame_ = true;
    depth_ = 0;
    ++frame_generation_;
    return Result<void>{};
}

Result<void> Profiler::end_frame() {
    if (!in_frame_) {
        return Error("profiler: end_frame called with no frame in progress");
    }

    FrameSample& sample = frames_[write_index_];
    sample.end_ms = now_ms();

    in_frame_ = false;
    depth_ = 0;
    write_index_ = (write_index_ + 1) % static_cast<u32>(kMaxFrames);
    if (committed_ < kMaxFrames) {
        ++committed_;
    }
    return Result<void>{};
}

u32 Profiler::begin_scope(std::string_view name, u64 generation) noexcept {
    if (!in_frame_ || generation != frame_generation_) {
        return kInvalidIndex; // no frame, or the caller belongs to a different one
    }

    FrameSample& sample = frames_[write_index_];
    if (sample.scope_count >= kMaxScopesPerFrame) {
        ++sample.dropped_scopes;
        // Still count the depth so a nested scope that is recorded reports the
        // right nesting level.
        ++depth_;
        return kInvalidIndex;
    }

    const u32 id = intern(name);
    if (id == kInvalidIndex) {
        ++sample.dropped_scopes;
        ++depth_;
        return kInvalidIndex;
    }

    const u32 index = sample.scope_count++;
    ScopeSample& scope = sample.scopes[index];
    scope.name = id;
    scope.start_ms = now_ms();
    scope.duration_ms = 0.0;
    scope.depth = depth_;
    ++depth_;
    return index;
}

void Profiler::end_scope(u32 handle, u64 generation) noexcept {
    if (!in_frame_ || generation != frame_generation_) {
        return; // the scope outlived its frame, or there is no frame
    }
    if (depth_ > 0) {
        --depth_;
    }
    if (handle == kInvalidIndex) {
        return; // a dropped scope only contributed to the depth counter
    }

    FrameSample& sample = frames_[write_index_];
    if (handle >= sample.scope_count) {
        return;
    }
    ScopeSample& scope = sample.scopes[handle];
    scope.duration_ms = now_ms() - scope.start_ms;
    if (scope.duration_ms < 0.0) {
        scope.duration_ms = 0.0;
    }
}

void Profiler::add_counter(std::string_view name, i64 value) noexcept {
    if (!in_frame_) {
        return;
    }

    FrameSample& sample = frames_[write_index_];
    const u32 id = intern(name);
    if (id == kInvalidIndex) {
        ++sample.dropped_counters;
        return;
    }

    for (u32 i = 0; i < sample.counter_count; ++i) {
        if (sample.counters[i].name == id) {
            sample.counters[i].value += value;
            return;
        }
    }

    if (sample.counter_count >= kMaxCountersPerFrame) {
        ++sample.dropped_counters;
        return;
    }
    sample.counters[sample.counter_count++] = CounterSample{id, value};
}

const Profiler::FrameSample* Profiler::frame_from_latest(usize offset) const noexcept {
    if (offset >= committed_) {
        return nullptr;
    }
    const usize newest = (write_index_ + kMaxFrames - 1) % kMaxFrames;
    const usize index = (newest + kMaxFrames - offset) % kMaxFrames;
    return &frames_[index];
}

const Profiler::FrameSample* Profiler::in_progress_frame() const noexcept {
    if (!in_frame_) {
        return nullptr;
    }
    return &frames_[write_index_];
}

Result<Profiler::ScopeStats> Profiler::scope_stats(std::string_view name) const {
    const u32 id = find_name(name);
    if (id == kInvalidIndex) {
        return Error("profiler: no scope named '" + std::string(name) + "' has been recorded");
    }

    ScopeStats stats;
    bool seen = false;
    for (usize offset = 0; offset < committed_; ++offset) {
        const FrameSample* frame = frame_from_latest(offset);
        if (frame == nullptr) {
            continue;
        }
        for (u32 i = 0; i < frame->scope_count; ++i) {
            const ScopeSample& scope = frame->scopes[i];
            if (scope.name != id) {
                continue;
            }
            const f64 duration = scope.duration_ms;
            if (!seen) {
                stats.last_ms = duration; // the first hit is in the newest frame
                stats.min_ms = duration;
                stats.max_ms = duration;
                seen = true;
            } else {
                if (duration < stats.min_ms) {
                    stats.min_ms = duration;
                }
                if (duration > stats.max_ms) {
                    stats.max_ms = duration;
                }
            }
            stats.avg_ms += duration;
            ++stats.count;
        }
    }

    if (!seen) {
        return Error("profiler: scope '" + std::string(name) + "' has no samples in the ring");
    }
    stats.avg_ms /= static_cast<f64>(stats.count);
    return stats;
}

Result<i64> Profiler::counter_value(std::string_view name, usize frame_offset) const {
    const u32 id = find_name(name);
    if (id == kInvalidIndex) {
        return Error("profiler: no counter named '" + std::string(name) + "' has been recorded");
    }
    const FrameSample* frame = frame_from_latest(frame_offset);
    if (frame == nullptr) {
        return Error("profiler: frame offset is out of range");
    }
    for (u32 i = 0; i < frame->counter_count; ++i) {
        if (frame->counters[i].name == id) {
            return frame->counters[i].value;
        }
    }
    return Error("profiler: counter '" + std::string(name) + "' is not present in that frame");
}

Result<FrameReport> Profiler::frame_report(usize frame_offset) const {
    const FrameSample* frame = frame_from_latest(frame_offset);
    if (frame == nullptr) {
        return Error("profiler: frame offset is out of range");
    }

    FrameReport report;
    report.total_ms = frame->end_ms - frame->begin_ms;
    if (report.total_ms < 0.0) {
        report.total_ms = 0.0;
    }
    report.sample_count = frame->scope_count;
    report.dropped_scopes = frame->dropped_scopes;
    report.dropped_counters = frame->dropped_counters;

    // Fold every sample into its name's aggregate, in first-seen order. The
    // sample count per frame is bounded (kMaxScopesPerFrame), so this is a short
    // O(n^2) over distinct names, not a per-frame hot path -- it runs when a
    // caller asks for a report, not while recording.
    for (u32 i = 0; i < frame->scope_count; ++i) {
        const ScopeSample& sample = frame->scopes[i];
        const std::string_view name = name_of(sample.name);
        if (name.empty()) {
            continue;
        }
        ScopeAggregate* slot = nullptr;
        for (u32 j = 0; j < report.scope_count; ++j) {
            if (report.scopes[j].name == name) {
                slot = &report.scopes[j];
                break;
            }
        }
        if (slot == nullptr) {
            if (report.scope_count >= FrameReport::kMaxAggregates) {
                ++report.dropped_names;
                continue;
            }
            slot = &report.scopes[report.scope_count++];
            slot->name.assign(name);
        }
        slot->total_ms += sample.duration_ms;
        ++slot->count;
    }

    // The two regions the runtime brackets, pulled out of the breakdown for the
    // summary line. Absent when the caller did not instrument them.
    for (u32 i = 0; i < report.scope_count; ++i) {
        if (report.scopes[i].name == "frame/update") {
            report.update_ms = report.scopes[i].total_ms;
        } else if (report.scopes[i].name == "frame/render") {
            report.render_ms = report.scopes[i].total_ms;
        }
    }

    // Biggest cost first, so a reader (or the editor panel) sees the hot spot at
    // the top. Ties keep insertion order.
    std::stable_sort(report.scopes.begin(), report.scopes.begin() + report.scope_count,
                     [](const ScopeAggregate& a, const ScopeAggregate& b) { return a.total_ms > b.total_ms; });

    for (u32 i = 0; i < frame->counter_count && report.counter_count < FrameReport::kMaxCounters; ++i) {
        const CounterSample& counter = frame->counters[i];
        const std::string_view name = name_of(counter.name);
        if (name.empty()) {
            continue;
        }
        report.counters[report.counter_count].name.assign(name);
        report.counters[report.counter_count].value = counter.value;
        ++report.counter_count;
    }

    return report;
}

namespace {

// Fixed-precision milliseconds with a trailing "ms", without pulling in
// <sstream> or <format> for one call site.
std::string format_ms(f64 value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3fms", value);
    return std::string(buffer);
}

} // namespace

std::string FrameReport::summary() const {
    std::string out = "frame ";
    out += format_ms(total_ms);
    out += " | update ";
    out += format_ms(update_ms);
    out += " | render ";
    out += format_ms(render_ms);
    out += " | " + std::to_string(sample_count) + " scopes | " + std::to_string(counter_count) + " counters";
    if (dropped_scopes > 0 || dropped_counters > 0) {
        out += " | dropped " + std::to_string(dropped_scopes) + " scopes, " + std::to_string(dropped_counters) +
               " counters";
    }
    return out;
}

std::string FrameReport::to_string() const {
    std::string out = summary();
    out += '\n';
    for (u32 i = 0; i < scope_count; ++i) {
        out += "  ";
        out += scopes[i].name;
        out += ' ';
        out += format_ms(scopes[i].total_ms);
        if (scopes[i].count > 1) {
            out += " (x" + std::to_string(scopes[i].count) + ")";
        }
        out += '\n';
    }
    for (u32 i = 0; i < counter_count; ++i) {
        out += "  #";
        out += counters[i].name;
        out += " = " + std::to_string(counters[i].value) + '\n';
    }
    return out;
}

} // namespace fe
