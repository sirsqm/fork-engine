// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/serialization/Operation.h"

#include <string>
#include <string_view>

namespace fe::net {

// What a message is. Structural operations and property edits share the
// `Operation` payload; the split is kept so the server can order them by
// different rules (structural edits in arrival order, property edits
// last-writer-wins) without inspecting the operation kind itself.
enum class MessageKind : u8 {
    Join,         // a client announces its replica id
    Leave,        // a client departs
    Operation,    // a structural or general edit
    PropertyEdit, // a single-property write, subject to last-writer-wins
    Sync,         // the authoritative log, sent to a joining client
};

[[nodiscard]] const char* to_string(MessageKind kind) noexcept;
[[nodiscard]] bool message_kind_from_string(std::string_view text, MessageKind& out) noexcept;

// One message on the wire. Deliberately a plain value with a JSON form, so a
// loopback transport, a test, and (later) a WebSocket all carry the same bytes.
//
// `origin` is the authoring replica. `seq` is the position the server assigned,
// the global order a client applies in. Neither is guessed locally: only the
// server orders, and every client applies the server's order, which is what
// makes two clients converge.
struct Message {
    MessageKind kind = MessageKind::Operation;
    // The authoring replica.
    Uuid origin{};
    // The destination. Nil means broadcast (every connected replica).
    Uuid target{};
    // The server-assigned global order. Zero before the server accepts it.
    u64 seq = 0;
    serialization::Operation op{};
    // Sync only: the authoritative log as JSON (the `Session::to_json` form).
    std::string payload;

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static Result<Message> from_json(std::string_view text);
};

} // namespace fe::net
