// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Log.h"
#include "fe/core/Types.h"

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// A bounded, in-memory log sink a tool can read back.
//
// `LogSink` has always been write-only: it is handed a level and a formatted
// message and keeps nothing, so the engine's own log went to stderr and no tool
// could show it to the person using the editor. A game author without a debugger
// attached -- the common case for an exported build -- then had nowhere to look
// when a script misbehaved, which is exactly the gap M7's debugging item names
// ("an in-engine logging/assertion surface a game author can use without a
// debugger").
//
// This sink keeps the most recent `capacity` records in memory, in order, so a
// console panel (or a crash report, or a test) can read what the engine logged.
// It is bounded on purpose: an unbounded log is a memory leak with a friendly
// name, and a console only ever shows the tail anyway.
//
// Every record carries a monotonically increasing `sequence`, assigned at write
// time and never reused. A reader that samples the buffer periodically compares
// the newest sequence it has seen against the newest still held, which is how it
// tells "nothing new" from "the buffer wrapped past me" without re-copying the
// whole buffer every frame.
//
// Threading: `write` is called from the logger, which may be called from any
// thread (a job can log); `snapshot`/`clear` are called from the UI thread. All
// access is guarded, so a job logging while the console redraws is safe.
class LogBuffer final : public LogSink {
public:
    struct Record {
        LogLevel level = LogLevel::Info;
        std::string message;
        u64 sequence = 0;
    };

    // The most records held at once. Older records are dropped once this many
    // are held, so memory is bounded by this and the message sizes.
    static constexpr usize kDefaultCapacity = 512;

    explicit LogBuffer(usize capacity = kDefaultCapacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    // --- LogSink -----------------------------------------------------------
    void write(LogLevel level, std::string_view message) override;

    // --- Reading -----------------------------------------------------------
    // The records held, oldest first. A copy, so the caller may keep it while the
    // buffer is written to.
    [[nodiscard]] std::vector<Record> snapshot() const;

    [[nodiscard]] usize size() const;

    // The sequence of the most recent record written, or 0 when empty. Never
    // reused, so it is the "what have I seen" marker a reader keeps.
    [[nodiscard]] u64 newest_sequence() const;

    // How many records have been dropped to stay within capacity. Non-zero means
    // a reader that looked away has missed records, which a console surfaces
    // rather than hiding.
    [[nodiscard]] u64 dropped_count() const;

    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<Record> records_;
    usize capacity_;
    u64 next_sequence_ = 1;
    u64 dropped_ = 0;
};

} // namespace fe
