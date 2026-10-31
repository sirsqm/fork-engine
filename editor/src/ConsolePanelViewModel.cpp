// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ConsolePanelViewModel.h"

#include <utility>

namespace fe::editor {

void ConsolePanelViewModel::push(Entry entry) {
    entries_.push_back(std::move(entry));
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
}

usize ConsolePanelViewModel::poll(const LogBuffer& log) {
    const std::vector<LogBuffer::Record> records = log.snapshot();
    if (records.empty()) {
        return 0;
    }
    const u64 newest = records.back().sequence;
    if (newest == last_log_sequence_) {
        return 0; // nothing new
    }

    // A hole between what the reader last saw and the oldest record still held
    // means the buffer dropped records before the reader looked. The size of
    // that hole is the honest "N records you missed"; the largest ever seen is
    // kept so the flag does not flicker. Comparing sequences rather than the
    // buffer's cumulative drop count is what keeps this from firing when the
    // reader is caught up and only already-seen records were evicted.
    const u64 oldest = records.front().sequence;
    if (oldest > last_log_sequence_ + 1) {
        const u64 gap = oldest - last_log_sequence_ - 1;
        log_truncated_ = true;
        if (gap > log_dropped_) {
            log_dropped_ = gap;
        }
    }

    usize added = 0;
    for (const LogBuffer::Record& record : records) {
        if (record.sequence <= last_log_sequence_) {
            continue;
        }
        push(Entry{record.level, record.message, false, record.sequence});
        last_log_sequence_ = record.sequence;
        ++added;
    }
    return added;
}

usize ConsolePanelViewModel::poll(const ScriptDiagnostics& diagnostics) {
    usize added = 0;
    for (const ScriptDiagnostics::Failure& failure : diagnostics.snapshot()) {
        if (failure.sequence <= last_failure_sequence_) {
            continue;
        }
        std::string message = failure.behaviour;
        message += " on '";
        message += failure.node_name.empty() ? "<unnamed>" : failure.node_name;
        message += "' failed: ";
        message += failure.message;
        push(Entry{LogLevel::Error, std::move(message), true, failure.sequence});
        last_failure_sequence_ = failure.sequence;
        ++added;
    }
    return added;
}

std::vector<ConsolePanelViewModel::Entry> ConsolePanelViewModel::visible() const {
    std::vector<Entry> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        if (entry.level >= filter_) {
            out.push_back(entry);
        }
    }
    return out;
}

bool ConsolePanelViewModel::has_hidden() const {
    for (const Entry& entry : entries_) {
        if (entry.level < filter_) {
            return true;
        }
    }
    return false;
}

usize ConsolePanelViewModel::hidden_count() const {
    usize count = 0;
    for (const Entry& entry : entries_) {
        if (entry.level < filter_) {
            ++count;
        }
    }
    return count;
}

void ConsolePanelViewModel::clear() {
    entries_.clear();
    last_log_sequence_ = 0;
    last_failure_sequence_ = 0;
    log_truncated_ = false;
    log_dropped_ = 0;
}

} // namespace fe::editor
