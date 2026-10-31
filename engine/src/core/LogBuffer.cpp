// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/LogBuffer.h"

#include <utility>

namespace fe {

void LogBuffer::write(LogLevel level, std::string_view message) {
    std::lock_guard lock(mutex_);
    Record record;
    record.level = level;
    record.message.assign(message.data(), message.size());
    record.sequence = next_sequence_++;
    records_.push_back(std::move(record));
    while (records_.size() > capacity_) {
        records_.pop_front();
        ++dropped_;
    }
}

std::vector<LogBuffer::Record> LogBuffer::snapshot() const {
    std::lock_guard lock(mutex_);
    return std::vector<Record>(records_.begin(), records_.end());
}

usize LogBuffer::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

u64 LogBuffer::newest_sequence() const {
    std::lock_guard lock(mutex_);
    return records_.empty() ? 0 : records_.back().sequence;
}

u64 LogBuffer::dropped_count() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

void LogBuffer::clear() {
    std::lock_guard lock(mutex_);
    records_.clear();
    dropped_ = 0;
}

} // namespace fe
