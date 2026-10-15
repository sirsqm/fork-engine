// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/net/SessionClient.h"

#include <utility>
#include <vector>

namespace fe::net {

SessionClient::SessionClient(const session::ReplicaId& replica, Transport& transport, ResourceManager* resources)
    : session_(replica, resources), transport_(transport), resources_(resources) {}

Error SessionClient::join() {
    Message message;
    message.kind = MessageKind::Join;
    message.origin = session_.replica_id();
    return transport_.send(message);
}

Error SessionClient::leave() {
    Message message;
    message.kind = MessageKind::Leave;
    message.origin = session_.replica_id();
    return transport_.send(message);
}

Error SessionClient::edit(const serialization::Operation& op) {
    Message message;
    message.kind =
        op.kind == serialization::OperationKind::SetProperty ? MessageKind::PropertyEdit : MessageKind::Operation;
    message.origin = session_.replica_id();
    message.op = op;
    return transport_.send(message);
}

Error SessionClient::poll() {
    std::vector<Message> incoming;
    if (Error error = transport_.poll(incoming); error) {
        return error;
    }
    last_applied_ = 0;
    for (const Message& message : incoming) {
        switch (message.kind) {
        case MessageKind::Sync:
            if (Error error = session_.from_json(message.payload); error) {
                return error;
            }
            break;
        case MessageKind::Operation:
        case MessageKind::PropertyEdit:
            if (Error error = session_.receive(message.op, message.origin); error) {
                return error;
            }
            break;
        case MessageKind::Join:
        case MessageKind::Leave:
            break;
        }
        ++last_applied_;
    }
    return Error::none();
}

} // namespace fe::net
