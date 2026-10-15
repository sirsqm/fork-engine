// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/net/SessionServer.h"

#include <utility>

namespace fe::net {

void SessionServer::attach(LoopbackNetwork& network) {
    network_ = &network;
    network.set_server_handler([this](const Message& message) { on_message(message); });
}

void SessionServer::on_message(const Message& message) {
    switch (message.kind) {
    case MessageKind::Join:
        // A joining client starts from the server's state: send it the whole
        // log so it does not have to replay history op by op.
        if (network_ != nullptr) {
            Message sync;
            sync.kind = MessageKind::Sync;
            sync.origin = message.origin;
            sync.target = message.origin;
            sync.payload = session_.to_json();
            network_->send_to(message.origin, sync);
        }
        break;
    case MessageKind::Leave:
        break;
    case MessageKind::Operation:
    case MessageKind::PropertyEdit: {
        // The server is the only writer of the authoritative log, so it assigns
        // the global order here: an accepted edit becomes the next entry, and
        // every client (the author included) applies it in this position. That
        // is what makes convergence structural rather than a client-side repair.
        const Result<usize> index = session_.submit(message.op);
        if (!index) {
            // The edit would not apply to the authoritative document (a stale
            // target, say). Drop it: a bad edit must not become committed state
            // or be broadcast as if it had succeeded.
            ++rejected_;
            return;
        }
        if (network_ != nullptr) {
            Message out = message;
            out.seq = static_cast<u64>(index.value());
            // Fan out to everyone, including the author, so all replicas apply
            // the identical server order.
            network_->broadcast(out);
        }
        break;
    }
    case MessageKind::Sync:
        // Servers do not receive Sync; a client does. Ignore rather than loop.
        break;
    }
}

} // namespace fe::net
