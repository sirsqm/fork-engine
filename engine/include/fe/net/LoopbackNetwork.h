// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/net/Message.h"
#include "fe/net/Transport.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fe::net {

// An in-process, synchronous hub: every message a client sends is delivered to
// the registered server handler, and every message the server sends is routed
// back to the addressed client. No threads, no sockets, no timing — a `flush()`
// runs the queued deliveries to completion.
//
// This is the transport the collaborative tests run on (ADR-0019 C4: an
// in-process loopback for tests). Because it is deterministic, a test can
// interleave two clients' edits in an exact order and assert they converge,
// which is the property the network transport will inherit.
class LoopbackNetwork {
public:
    LoopbackNetwork() = default;

    LoopbackNetwork(const LoopbackNetwork&) = delete;
    LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;

    // The server end. Exactly one handler may be installed; a second call
    // replaces the first. The handler receives every client message and may
    // reply synchronously through `send_to`/`broadcast`.
    void set_server_handler(DeliveryFn handler);

    // Connects a new client identified by `replica`. Returns a transport the
    // client owns. The client announces itself with a Join (see
    // `SessionClient::join`); until then it is not a broadcast target.
    [[nodiscard]] std::unique_ptr<Transport> connect(const Uuid& replica);

    // --- Server side -------------------------------------------------------
    // Sends `message` to one connected replica. An unknown target is dropped
    // (a message to a replica that has left), which is not an error: the client
    // is gone, not the network.
    void send_to(const Uuid& target, const Message& message);

    // Sends `message` to every connected replica except `except` (nil sends to
    // all). Broadcast is how the server fans an accepted edit out.
    void broadcast(const Message& message, const Uuid& except = Uuid{});

    // Runs every queued delivery to completion. A `send` during `flush` queues
    // for the same flush, so one call settles the whole network.
    void flush();

    [[nodiscard]] std::vector<Uuid> connected() const;

private:
    friend class LoopbackTransport;

    struct Client {
        Uuid replica{};
        bool joined = false;
        std::vector<Message> inbox;
    };

    [[nodiscard]] Error client_send(const Uuid& from, const Message& message);
    [[nodiscard]] Error client_poll(const Uuid& from, std::vector<Message>& out);
    void mark_left(const Uuid& replica);

    DeliveryFn server_handler_;
    std::unordered_map<Uuid, Client> clients_;
    std::vector<Message> server_inbox_;
    bool flushing_ = false;
};

// One client's end of a loopback link. Owned by the client; the network owns the
// routing. Polling delivers whatever the server has addressed to this replica.
class LoopbackTransport final : public Transport {
public:
    LoopbackTransport(LoopbackNetwork& network, Uuid replica);
    ~LoopbackTransport() override;

    LoopbackTransport(const LoopbackTransport&) = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;

    [[nodiscard]] Error send(const Message& message) override;
    [[nodiscard]] Error poll(std::vector<Message>& out) override;
    [[nodiscard]] bool connected() const noexcept override { return connected_; }
    [[nodiscard]] Uuid peer() const noexcept override;

private:
    LoopbackNetwork& network_;
    Uuid replica_;
    bool connected_ = true;
};

} // namespace fe::net
