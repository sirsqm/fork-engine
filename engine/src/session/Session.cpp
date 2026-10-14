// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/session/Session.h"

#include "fe/resources/ResourceManager.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace fe::session {
namespace {

using json = nlohmann::json;

[[nodiscard]] bool is_destructive(serialization::OperationKind kind) noexcept {
    return kind == serialization::OperationKind::DeleteNode || kind == serialization::OperationKind::RemoveComponent;
}

} // namespace

Session::Session(ResourceManager* resources) : resources_(resources) {
    replica_id_ = Uuid::generate();
}

void Session::drop_redo_tail() {
    if (undo_count_ == 0) {
        return;
    }
    log_.resize(log_.size() - undo_count_);
    inverses_.resize(log_.size());
    captures_.resize(log_.size());
    undo_count_ = 0;
}

Result<usize> Session::submit(serialization::Operation op) {
    drop_redo_tail();

    const usize index = log_.size();
    log_.push_back(LogEntry{std::move(op), replica_id_});
    inverses_.emplace_back();
    captures_.emplace_back();
    if (Error error = apply_forward(index); error) {
        // A failed apply leaves the log and the document inconsistent; drop the
        // entry rather than keep a committed edit that did not take.
        log_.pop_back();
        inverses_.pop_back();
        captures_.pop_back();
        return error;
    }
    return index;
}

Error Session::receive(serialization::Operation op, const ReplicaId& origin) {
    drop_redo_tail();

    const usize index = log_.size();
    log_.push_back(LogEntry{std::move(op), origin});
    inverses_.emplace_back();
    captures_.emplace_back();
    if (Error error = apply_forward(index); error) {
        log_.pop_back();
        inverses_.pop_back();
        captures_.pop_back();
        return error;
    }
    return Error::none();
}

Error Session::apply_forward(usize index) {
    const serialization::Operation& op = log_[index].op;

    if (is_destructive(op.kind)) {
        // The object itself is the inverse; hold it so undo can restore it.
        return serialization::apply(op, document_, resources_, &captures_[index]);
    }

    // Derive the inverse now, while the document still holds the old value, then
    // apply. This is the same ordering the editor's OperationCommand uses.
    Result<serialization::Operation> inv = serialization::inverse(op, document_);
    if (!inv) {
        return inv.error();
    }
    inverses_[index] = std::move(inv.value());
    return serialization::apply(op, document_, resources_, nullptr);
}

Error Session::replay_into(Scene& out, ResourceManager* resources) const {
    out.clear();
    for (const LogEntry& entry : log_) {
        // A replay has no undo stack, so a removed object is simply destroyed.
        if (Error error = serialization::apply(entry.op, out, resources, nullptr); error) {
            return error;
        }
    }
    return Error::none();
}

Error Session::undo() {
    if (!can_undo()) {
        return Error("session: nothing to undo");
    }
    const usize index = log_.size() - undo_count_ - 1;
    const serialization::Operation& op = log_[index].op;

    if (is_destructive(op.kind)) {
        if (Error error = serialization::restore(captures_[index], document_); error) {
            return error;
        }
    } else if (Error error = serialization::apply(inverses_[index], document_, resources_, nullptr); error) {
        return error;
    }
    ++undo_count_;
    return Error::none();
}

Error Session::redo() {
    if (!can_redo()) {
        return Error("session: nothing to redo");
    }
    // The stored inverse and capture already describe this entry; re-applying it
    // forward is the redo. The inverse is unchanged because undo restored the
    // state it was derived from.
    if (Error error = apply_forward(log_.size() - undo_count_); error) {
        return error;
    }
    --undo_count_;
    return Error::none();
}

std::string Session::to_json() const {
    json root;
    json entries = json::array();
    for (const LogEntry& entry : log_) {
        json item;
        item["origin"] = entry.origin.to_string();
        item["op"] = json::parse(serialization::to_json(entry.op));
        entries.push_back(std::move(item));
    }
    root["entries"] = std::move(entries);
    return root.dump();
}

Error Session::from_json(std::string_view text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        return Error(std::string("session: invalid JSON: ") + e.what());
    }
    if (!root.is_object() || !root.contains("entries") || !root.at("entries").is_array()) {
        return Error("session: log has no \"entries\" array");
    }

    std::vector<LogEntry> new_log;
    for (const json& item : root.at("entries")) {
        if (!item.is_object() || !item.contains("op")) {
            return Error("session: a log entry has no \"op\"");
        }
        LogEntry entry;
        if (Error error = serialization::from_json(item.at("op").dump(), entry.op); error) {
            return error;
        }
        if (item.contains("origin")) {
            if (!item.at("origin").is_string() ||
                !Uuid::try_parse(item.at("origin").get<std::string>(), entry.origin)) {
                return Error("session: a log entry has a malformed \"origin\"");
            }
        }
        new_log.push_back(std::move(entry));
    }

    // Adopt the parsed log, then rebuild the document by applying it forward.
    log_ = std::move(new_log);
    inverses_.assign(log_.size(), serialization::Operation{});
    captures_.clear();
    captures_.resize(log_.size());
    undo_count_ = 0;
    document_.clear();
    for (usize i = 0; i < log_.size(); ++i) {
        if (Error error = apply_forward(i); error) {
            return error;
        }
    }
    return Error::none();
}

} // namespace fe::session
