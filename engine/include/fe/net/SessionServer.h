// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/net/LoopbackNetwork.h"
#include "fe/net/Message.h"
#include "fe/session/Session.h"

#include <vector>

namespace fe::net {

// The authoritative orderer for a collaborative session (ADR-0019 C4).
//
// Every accepted edit is given a global sequence number and applied to the
// server's own authoritative document; the server then broadcasts it to every
// client. Clients apply only what the server sends, in the server's order, so
// they converge by construction — no client-side reconciliation is needed.
//
// Last-writer-wins per property is a *consequence* of the single order, not a
// separate rule: the last write to a property in server order is the one that
// survives, because every client applies that same order. The `PropertyEdit`
// message kind exists so a later policy (coalescing, per-property vectors) can
// be added without changing the wire format.
//
// A joining client is sent a `Sync` message carrying the whole log, so it starts
// from the server's state rather than replaying history op by op.
class SessionServer {
public:
    SessionServer() = default;

    SessionServer(const SessionServer&) = delete;
    SessionServer& operator=(const SessionServer&) = delete;

    // Installs this server as the network's handler. After this, `flush()`
    // drives the server.
    void attach(LoopbackNetwork& network);

    // Handles one message from a client. Public so a test can drive the server
    // without a network; `attach` wires it to a `LoopbackNetwork`.
    void on_message(const Message& message);

    // The authoritative log, in the order the server assigned.
    [[nodiscard]] const std::vector<session::LogEntry>& log() const noexcept { return session_.log(); }
    [[nodiscard]] usize log_size() const noexcept { return session_.log_size(); }
    [[nodiscard]] u64 last_seq() const noexcept { return session_.log_size(); }

    // The server's own document. Tests compare a client's document to this.
    [[nodiscard]] const Scene& document() const noexcept { return session_.document(); }

    // The number of edits the server rejected (an operation that would not
    // apply). Kept as a counter so a test can assert a bad edit was dropped
    // rather than broadcast.
    [[nodiscard]] usize rejected() const noexcept { return rejected_; }

private:
    LoopbackNetwork* network_ = nullptr;
    session::Session session_;
    usize rejected_ = 0;
};

} // namespace fe::net
