// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/net/Message.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace fe::net {
namespace {

using json = nlohmann::json;

struct KindName {
    MessageKind kind;
    const char* name;
};

constexpr KindName kKindNames[] = {
    {MessageKind::Join, "join"},           {MessageKind::Leave, "leave"},
    {MessageKind::Operation, "operation"}, {MessageKind::PropertyEdit, "property_edit"},
    {MessageKind::Sync, "sync"},
};

} // namespace

const char* to_string(MessageKind kind) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "operation";
}

bool message_kind_from_string(std::string_view text, MessageKind& out) noexcept {
    for (const KindName& entry : kKindNames) {
        if (text == entry.name) {
            out = entry.kind;
            return true;
        }
    }
    return false;
}

std::string Message::to_json() const {
    json root;
    root["kind"] = net::to_string(kind);
    root["origin"] = origin.to_string();
    if (!target.is_nil()) {
        root["target"] = target.to_string();
    }
    root["seq"] = seq;
    if (kind == MessageKind::Operation || kind == MessageKind::PropertyEdit) {
        root["op"] = json::parse(serialization::to_json(op));
    }
    if (!payload.empty()) {
        root["payload"] = payload;
    }
    return root.dump();
}

Result<Message> Message::from_json(std::string_view text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        return Error(std::string("net: invalid message JSON: ") + e.what());
    }
    if (!root.is_object() || !root.contains("kind") || !root.at("kind").is_string()) {
        return Error("net: message has no \"kind\"");
    }

    Message message;
    if (!message_kind_from_string(root.at("kind").get<std::string>(), message.kind)) {
        return Error("net: unknown message kind");
    }
    if (!root.contains("origin") || !root.at("origin").is_string() ||
        !Uuid::try_parse(root.at("origin").get<std::string>(), message.origin)) {
        return Error("net: message has a malformed \"origin\"");
    }
    if (root.contains("target") && root.at("target").is_string()) {
        if (!Uuid::try_parse(root.at("target").get<std::string>(), message.target)) {
            return Error("net: message has a malformed \"target\"");
        }
    }
    message.seq = root.value("seq", static_cast<u64>(0));
    if (root.contains("payload") && root.at("payload").is_string()) {
        message.payload = root.at("payload").get<std::string>();
    }
    if (message.kind == MessageKind::Operation || message.kind == MessageKind::PropertyEdit) {
        if (!root.contains("op")) {
            return Error("net: an operation message has no \"op\"");
        }
        if (Error error = serialization::from_json(root.at("op").dump(), message.op); error) {
            return error;
        }
    }
    return message;
}

} // namespace fe::net
