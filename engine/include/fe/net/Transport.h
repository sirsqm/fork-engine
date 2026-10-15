// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/net/Message.h"

#include <functional>
#include <vector>

namespace fe::net {

// The seam between "the collaborative session" and "how bytes move". A
// transport connects one replica to a peer (a server, or another replica) and
// moves `Message`s. It knows nothing about operations, ordering, or scenes:
// those live above it. That is what lets the same session client run over an
// in-process loopback in tests and a WebSocket in production, unchanged.
//
// The interface is deliberately synchronous and single-threaded: a caller
// `poll`s to drain what has arrived and `send`s to hand a message over. A
// production transport may buffer internally, but it never calls back on another
// thread. This keeps the whole session deterministic and testable, and matches
// the engine's rule that the RHI device (and, here, the session) is
// single-threaded.
class Transport {
public:
    virtual ~Transport() = default;

    // Hands `message` to the peer. Returns an error if the transport is closed.
    [[nodiscard]] virtual Error send(const Message& message) = 0;

    // Drains every message that has arrived since the last poll into `out`
    // (appended, not cleared). Returns an error if the transport is closed.
    [[nodiscard]] virtual Error poll(std::vector<Message>& out) = 0;

    [[nodiscard]] virtual bool connected() const noexcept = 0;

    // The replica on the far end, when the transport models a point-to-point
    // link. A loopback client faces the hub, not one peer, so this is nil; a
    // point-to-point transport returns the peer it dialed.
    [[nodiscard]] virtual Uuid peer() const noexcept { return Uuid{}; }
};

// A handler the server calls to deliver a message to one connected replica. The
// loopback network uses it to route a server's reply into the right client.
using DeliveryFn = std::function<void(const Message&)>;

} // namespace fe::net
