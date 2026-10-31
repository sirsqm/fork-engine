// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/script/ScriptDiagnostics.h"

#include <utility>

namespace fe {

void ScriptDiagnostics::record(const Uuid& node_id, std::string_view node_name, std::string_view behaviour,
                               std::string_view message, u64 frame) {
    std::lock_guard lock(mutex_);
    Failure failure;
    failure.node_id = node_id;
    failure.node_name.assign(node_name.data(), node_name.size());
    failure.behaviour.assign(behaviour.data(), behaviour.size());
    failure.message.assign(message.data(), message.size());
    failure.frame = frame;
    failure.sequence = next_sequence_++;
    failures_.push_back(std::move(failure));
    while (failures_.size() > capacity_) {
        failures_.pop_front();
        ++dropped_;
    }
}

std::vector<ScriptDiagnostics::Failure> ScriptDiagnostics::snapshot() const {
    std::lock_guard lock(mutex_);
    return std::vector<Failure>(failures_.begin(), failures_.end());
}

usize ScriptDiagnostics::size() const {
    std::lock_guard lock(mutex_);
    return failures_.size();
}

u64 ScriptDiagnostics::newest_sequence() const {
    std::lock_guard lock(mutex_);
    return failures_.empty() ? 0 : failures_.back().sequence;
}

u64 ScriptDiagnostics::dropped_count() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

void ScriptDiagnostics::clear() {
    std::lock_guard lock(mutex_);
    failures_.clear();
    dropped_ = 0;
}

} // namespace fe
