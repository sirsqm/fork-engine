// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/Profiler.h"

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

u32 Profiler::begin_scope(std::string_view name) noexcept {
    if (!in_frame_) {
        return kInvalidIndex;
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

} // namespace fe
