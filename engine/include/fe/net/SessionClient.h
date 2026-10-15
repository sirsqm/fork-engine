// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/net/Message.h"
#include "fe/net/Transport.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/Operation.h"
#include "fe/session/Session.h"

#include <vector>

namespace fe {

class ResourceManager;

namespace net {

// A replica's view of a collaborative session: a local document and the
// server-ordered log it has applied, plus the transport that carries its edits.
//
// The client applies **only** what the server sends, in the server's order. It
// does not apply its own edits optimistically: a submitted edit goes to the
// server and comes back in the server's position, and only then is it applied.
// That is what makes two clients converge without any client-side
// reconciliation — there is a single order, and every replica walks it.
//
// The cost is a round trip before an edit is visible locally. Applying
// optimistically and reconciling is the next step (ADR-0019 C6), and is
// deliberately not attempted here: reconciliation is the hard part, and the
// server-ordered path is the floor it will be built on.
//
// See docs/adr/0049-net-transport-and-session-server.md.
class SessionClient {
public:
    // `transport` is owned by the caller and must outlive the client.
    SessionClient(const session::ReplicaId& replica, Transport& transport, ResourceManager* resources = nullptr);

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    [[nodiscard]] const session::ReplicaId& replica_id() const noexcept { return session_.replica_id(); }
    [[nodiscard]] const Scene& document() const noexcept { return session_.document(); }
    [[nodiscard]] const std::vector<session::LogEntry>& log() const noexcept { return session_.log(); }
    [[nodiscard]] usize applied() const noexcept { return session_.log_size(); }

    // Announces this replica to the server. The server replies with a `Sync`
    // carrying the authoritative log; call `poll` to receive it.
    [[nodiscard]] Error join();

    // Tells the server this replica is leaving.
    [[nodiscard]] Error leave();

    // Submits `op` to the server. The edit is not applied locally until the
    // server echoes it; call `poll` to apply it. A `SetProperty` is sent as a
    // `PropertyEdit` so the server can apply property policy without inspecting
    // the operation kind.
    [[nodiscard]] Error edit(const serialization::Operation& op);

    // Applies every message that has arrived from the server. A `Sync` replaces
    // the local document with the server's log; an `Operation`/`PropertyEdit` is
    // appended in the server's order. Returns the first error, if any.
    [[nodiscard]] Error poll();

    // The number of messages applied by the last `poll`, for tests.
    [[nodiscard]] usize last_applied() const noexcept { return last_applied_; }

private:
    session::Session session_;
    Transport& transport_;
    usize last_applied_ = 0;
};

} // namespace net
} // namespace fe
