// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/core/Uuid.h"

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// A record of the scripts that failed, kept where a tool can read it.
//
// A failing script is contained by `ScriptHost`: it is logged, disabled, and the
// frame keeps going (ADR-0044, ADR-0003). Logging alone is not enough for a game
// author, though. The failure is the one thing they most need to see, it happens
// once (the script is then disabled and never runs again), and the log line that
// carried it scrolls away. This type keeps the failures themselves -- which
// node, which behaviour, what went wrong, and how many frames it survived --
// so an editor panel or a test can read them without scraping stderr.
//
// It is deliberately small and dependency-free (no scene, no script header, no
// logger), so `ScriptHost` can own one without widening its own dependencies and
// a test can drive it directly.
//
// Bounded: the most recent `capacity` failures are kept. A script that fails on
// attach in a loop would otherwise grow this without limit, and a diagnostics
// view only ever shows the tail.
//
// Threading: `record` is called from the frame thread (the host's update), and
// `snapshot` from the UI thread. Access is guarded, matching `LogBuffer`.
class ScriptDiagnostics {
public:
    struct Failure {
        Uuid node_id;
        // The node's name at the time of failure, so the record is readable
        // after the node is gone.
        std::string node_name;
        // The behaviour's name (`Script::name()`).
        std::string behaviour;
        std::string message;
        // The frame the failure was recorded on, as reported by the host.
        u64 frame = 0;
        // Assigned at record time and never reused, so a reader that samples the
        // set periodically can copy only what is new. Mirrors LogBuffer.
        u64 sequence = 0;
    };

    static constexpr usize kDefaultCapacity = 64;

    explicit ScriptDiagnostics(usize capacity = kDefaultCapacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // Records a failure. The most recent are kept.
    void record(const Uuid& node_id, std::string_view node_name, std::string_view behaviour, std::string_view message,
                u64 frame);

    // The failures held, oldest first.
    [[nodiscard]] std::vector<Failure> snapshot() const;

    [[nodiscard]] usize size() const;

    // The sequence of the most recent failure recorded, or 0 when empty.
    [[nodiscard]] u64 newest_sequence() const;

    // How many failures have been dropped to stay within capacity.
    [[nodiscard]] u64 dropped_count() const;

    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<Failure> failures_;
    usize capacity_;
    u64 next_sequence_ = 1;
    u64 dropped_ = 0;
};

} // namespace fe
