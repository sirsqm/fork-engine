// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/net/LoopbackNetwork.h"

#include <iterator>
#include <utility>

namespace fe::net {

void LoopbackNetwork::set_server_handler(DeliveryFn handler) {
    server_handler_ = std::move(handler);
}

std::unique_ptr<Transport> LoopbackNetwork::connect(const Uuid& replica) {
    Client& client = clients_[replica];
    client.replica = replica;
    return std::make_unique<LoopbackTransport>(*this, replica);
}

void LoopbackNetwork::send_to(const Uuid& target, const Message& message) {
    auto it = clients_.find(target);
    if (it == clients_.end()) {
        // The replica has left; a message to a gone peer is dropped, not an
        // error. The network is fine, the peer is not there.
        return;
    }
    it->second.inbox.push_back(message);
}

void LoopbackNetwork::broadcast(const Message& message, const Uuid& except) {
    for (auto& [replica, client] : clients_) {
        if (replica == except || !client.joined) {
            continue;
        }
        client.inbox.push_back(message);
    }
}

void LoopbackNetwork::flush() {
    if (flushing_) {
        // A handler that re-enters flush (it should not) would recurse; the
        // guard lets the outer loop drain everything instead.
        return;
    }
    flushing_ = true;
    while (!server_inbox_.empty()) {
        Message message = std::move(server_inbox_.front());
        server_inbox_.erase(server_inbox_.begin());
        if (server_handler_) {
            server_handler_(message);
        }
    }
    flushing_ = false;
}

std::vector<Uuid> LoopbackNetwork::connected() const {
    std::vector<Uuid> out;
    out.reserve(clients_.size());
    for (const auto& [replica, client] : clients_) {
        if (client.joined) {
            out.push_back(replica);
        }
    }
    return out;
}

Error LoopbackNetwork::client_send(const Uuid& from, const Message& message) {
    auto it = clients_.find(from);
    if (it == clients_.end()) {
        return Error("net: send from an unknown replica");
    }
    // Join/Leave are the network's business: it tracks membership so broadcast
    // and connected() need no protocol of their own. The origin is stamped here
    // so a client cannot claim to be another replica.
    if (message.kind == MessageKind::Join) {
        it->second.joined = true;
    } else if (message.kind == MessageKind::Leave) {
        it->second.joined = false;
    }
    Message copy = message;
    copy.origin = from;
    server_inbox_.push_back(std::move(copy));
    return Error::none();
}

Error LoopbackNetwork::client_poll(const Uuid& from, std::vector<Message>& out) {
    auto it = clients_.find(from);
    if (it == clients_.end()) {
        return Error("net: poll from an unknown replica");
    }
    out.insert(out.end(), std::make_move_iterator(it->second.inbox.begin()),
               std::make_move_iterator(it->second.inbox.end()));
    it->second.inbox.clear();
    return Error::none();
}

void LoopbackNetwork::mark_left(const Uuid& replica) {
    auto it = clients_.find(replica);
    if (it != clients_.end()) {
        it->second.joined = false;
    }
}

LoopbackTransport::LoopbackTransport(LoopbackNetwork& network, Uuid replica) : network_(network), replica_(replica) {}

LoopbackTransport::~LoopbackTransport() {
    network_.mark_left(replica_);
}

Error LoopbackTransport::send(const Message& message) {
    if (!connected_) {
        return Error("net: send on a closed transport");
    }
    return network_.client_send(replica_, message);
}

Error LoopbackTransport::poll(std::vector<Message>& out) {
    if (!connected_) {
        return Error("net: poll on a closed transport");
    }
    return network_.client_poll(replica_, out);
}

Uuid LoopbackTransport::peer() const noexcept {
    return Uuid{};
}

} // namespace fe::net
