// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The transport and session server (docs/adr/0049-net-transport-and-session-
// server.md). An in-process loopback moves messages between a server and two
// clients; the server orders every accepted edit and broadcasts it, so the
// clients converge by walking one order. Headless: no sockets, no threads, no
// GPU.

#include <doctest/doctest.h>

#include "fe/net/LoopbackNetwork.h"
#include "fe/net/Message.h"
#include "fe/net/SessionClient.h"
#include "fe/net/SessionServer.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/Operation.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/session/Session.h"

#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace fe;
using fe::net::LoopbackNetwork;
using fe::net::Message;
using fe::net::MessageKind;
using fe::net::SessionClient;
using fe::net::SessionServer;
using fe::net::Transport;
using serialization::Operation;
using serialization::OperationKind;

namespace {

Operation make_create(const Uuid& node_id, const Uuid& parent_id, std::string name, usize index = 0) {
    Operation op;
    op.kind = OperationKind::CreateNode;
    op.node_id = node_id;
    op.parent_id = parent_id;
    op.name = std::move(name);
    op.index = index;
    return op;
}

Operation make_add_component(const Uuid& node_id, const Uuid& component_id, std::string type, usize index = 0) {
    Operation op;
    op.kind = OperationKind::AddComponent;
    op.node_id = node_id;
    op.component_id = component_id;
    op.name = std::move(type);
    op.index = index;
    return op;
}

Operation make_set_layer(const Uuid& node_id, const Uuid& component_id, i32 value) {
    Operation op;
    op.kind = OperationKind::SetProperty;
    op.node_id = node_id;
    op.component_id = component_id;
    op.path = {"layer"};
    op.value = value;
    return op;
}

Operation make_rename(const Uuid& node_id, std::string name) {
    Operation op;
    op.kind = OperationKind::RenameNode;
    op.node_id = node_id;
    op.name = std::move(name);
    return op;
}

Operation make_delete(const Uuid& node_id) {
    Operation op;
    op.kind = OperationKind::DeleteNode;
    op.node_id = node_id;
    return op;
}

// The document as the file format writes it, for a byte-for-byte convergence
// comparison (ids are stable, so two converged replicas match exactly).
std::string document_json(const Scene& scene) {
    const Result<std::string> text = serialization::scene::to_json(scene);
    REQUIRE(text);
    return text.value();
}

// A client and its transport, owned together so the transport outlives the
// client and the network outlives both.
struct Client {
    std::unique_ptr<Transport> transport;
    std::unique_ptr<SessionClient> client;
};

// Builds a server and two joined clients over one loopback network, with an
// empty authoritative log. The returned clients have already applied their
// join sync.
struct Fixture {
    LoopbackNetwork network;
    SessionServer server;
    Uuid replica_a = Uuid::generate();
    Uuid replica_b = Uuid::generate();
    Client a;
    Client b;

    void start() {
        server.attach(network);
        a.transport = network.connect(replica_a);
        b.transport = network.connect(replica_b);
        a.client = std::make_unique<SessionClient>(replica_a, *a.transport);
        b.client = std::make_unique<SessionClient>(replica_b, *b.transport);
        REQUIRE_FALSE(a.client->join());
        REQUIRE_FALSE(b.client->join());
        network.flush();
        REQUIRE_FALSE(a.client->poll());
        REQUIRE_FALSE(b.client->poll());
    }

    // Sends `op` from `from`, then settles the whole network and lets both
    // clients apply whatever the server ordered.
    void edit(Client& from, const Operation& op) {
        REQUIRE_FALSE(from.client->edit(op));
        network.flush();
        REQUIRE_FALSE(a.client->poll());
        REQUIRE_FALSE(b.client->poll());
    }
};

} // namespace

TEST_CASE("a message round-trips through JSON") {
    reflect::register_builtin_types();

    const Uuid origin = Uuid::generate();
    const Uuid target = Uuid::generate();
    const Uuid node = Uuid::generate();

    Message message;
    message.kind = MessageKind::PropertyEdit;
    message.origin = origin;
    message.target = target;
    message.seq = 42;
    message.op = make_rename(node, "Renamed");

    const std::string text = message.to_json();
    const Result<Message> parsed = Message::from_json(text);
    REQUIRE(parsed);

    CHECK(parsed.value().kind == MessageKind::PropertyEdit);
    CHECK((parsed.value().origin == origin));
    CHECK((parsed.value().target == target));
    CHECK(parsed.value().seq == 42);
    CHECK(parsed.value().op.kind == OperationKind::RenameNode);
    CHECK(parsed.value().op.name == "Renamed");
    CHECK((parsed.value().op.node_id == node));
}

TEST_CASE("a malformed message is rejected, not guessed") {
    CHECK_FALSE(Message::from_json("not json"));
    CHECK_FALSE(Message::from_json("{}"));
    CHECK_FALSE(Message::from_json(R"({"kind":"teleport","origin":"x"})"));
    // An operation message with no payload is an error.
    CHECK_FALSE(Message::from_json(R"({"kind":"operation","origin":"00000000-0000-0000-0000-000000000000"})"));
}

TEST_CASE("the loopback delivers a client message to the server and back") {
    LoopbackNetwork network;
    std::vector<Message> received;
    network.set_server_handler([&](const Message& message) {
        received.push_back(message);
        Message reply;
        reply.kind = MessageKind::Sync;
        reply.target = message.origin;
        network.send_to(message.origin, reply);
    });

    const Uuid replica = Uuid::generate();
    std::unique_ptr<Transport> transport = network.connect(replica);
    REQUIRE(transport->connected());

    Message ping;
    ping.kind = MessageKind::Join;
    REQUIRE_FALSE(transport->send(ping));
    network.flush();

    REQUIRE(received.size() == 1);
    CHECK(received.front().kind == MessageKind::Join);
    // The network stamps the origin, so a client cannot claim to be a peer.
    CHECK((received.front().origin == replica));

    std::vector<Message> inbox;
    REQUIRE_FALSE(transport->poll(inbox));
    REQUIRE(inbox.size() == 1);
    CHECK(inbox.front().kind == MessageKind::Sync);
}

TEST_CASE("a server orders edits and two clients converge") {
    reflect::register_builtin_types();
    Fixture fixture;
    fixture.start();

    const Uuid node = Uuid::generate();
    const Uuid sprite = Uuid::generate();

    fixture.edit(fixture.a, make_create(node, {}, "Node"));
    fixture.edit(fixture.b, make_add_component(node, sprite, "Sprite2D"));
    fixture.edit(fixture.a, make_set_layer(node, sprite, 5));
    fixture.edit(fixture.b, make_rename(node, "Renamed"));

    CHECK(document_json(fixture.a.client->document()) == document_json(fixture.b.client->document()));
    CHECK(document_json(fixture.server.document()) == document_json(fixture.a.client->document()));
    CHECK(fixture.a.client->applied() == 4);
    CHECK(fixture.b.client->applied() == 4);
}

TEST_CASE("concurrent property writes converge to the server's last writer") {
    reflect::register_builtin_types();
    Fixture fixture;
    fixture.start();

    const Uuid node = Uuid::generate();
    const Uuid sprite = Uuid::generate();
    fixture.edit(fixture.a, make_create(node, {}, "Node"));
    fixture.edit(fixture.a, make_add_component(node, sprite, "Sprite2D"));

    // Both replicas write the same property before either has seen the other's
    // write. The server applies them in arrival order (a, then b); every client
    // walks that order, so the last writer in server order wins everywhere.
    REQUIRE_FALSE(fixture.a.client->edit(make_set_layer(node, sprite, 5)));
    REQUIRE_FALSE(fixture.b.client->edit(make_set_layer(node, sprite, 9)));
    fixture.network.flush();
    REQUIRE_FALSE(fixture.a.client->poll());
    REQUIRE_FALSE(fixture.b.client->poll());

    const Component* component = fixture.a.client->document().find_component(sprite);
    REQUIRE(component != nullptr);
    CHECK(static_cast<const Sprite2D*>(component)->layer == 9);
    CHECK(document_json(fixture.a.client->document()) == document_json(fixture.b.client->document()));
    CHECK(document_json(fixture.server.document()) == document_json(fixture.a.client->document()));
}

TEST_CASE("a late joiner syncs the whole authoritative log") {
    reflect::register_builtin_types();
    Fixture fixture;
    fixture.start();

    const Uuid node = Uuid::generate();
    const Uuid sprite = Uuid::generate();
    fixture.edit(fixture.a, make_create(node, {}, "Node"));
    fixture.edit(fixture.a, make_add_component(node, sprite, "Sprite2D"));
    fixture.edit(fixture.a, make_set_layer(node, sprite, 3));

    // A third replica joins after the fact. It receives one Sync carrying the
    // log and rebuilds the document from it, rather than replaying history.
    const Uuid replica_c = Uuid::generate();
    std::unique_ptr<Transport> transport_c = fixture.network.connect(replica_c);
    SessionClient client_c(replica_c, *transport_c);
    REQUIRE_FALSE(client_c.join());
    fixture.network.flush();
    REQUIRE_FALSE(client_c.poll());

    CHECK(client_c.applied() == 3);
    CHECK(document_json(client_c.document()) == document_json(fixture.server.document()));
}

TEST_CASE("the server drops an edit that would not apply") {
    reflect::register_builtin_types();
    Fixture fixture;
    fixture.start();

    const usize before = fixture.server.log_size();
    // A rename of a node that does not exist cannot apply. The server must not
    // commit it, must not broadcast it, and must count it as rejected.
    REQUIRE_FALSE(fixture.a.client->edit(make_rename(Uuid::generate(), "Ghost")));
    fixture.network.flush();
    REQUIRE_FALSE(fixture.a.client->poll());

    CHECK(fixture.server.log_size() == before);
    CHECK(fixture.server.rejected() == 1);
    CHECK(fixture.a.client->applied() == before);
}

TEST_CASE("two clients interleaving random edits converge") {
    reflect::register_builtin_types();
    Fixture fixture;
    fixture.start();

    std::mt19937 rng(20261014);
    std::vector<Uuid> nodes;
    std::vector<Uuid> components;
    nodes.reserve(16);
    components.reserve(16);

    // A deterministic script of valid edits, each sent from an alternating
    // client. Because all sends precede one flush, the server sees them in
    // script order and both clients walk that same order.
    constexpr int kSteps = 300;
    for (int step = 0; step < kSteps; ++step) {
        Client& from = (rng() % 2 == 0) ? fixture.a : fixture.b;
        const int choice = static_cast<int>(rng() % 100);

        if (choice < 25 || nodes.empty()) {
            const Uuid node = Uuid::generate();
            const Uuid parent = nodes.empty() ? Uuid{} : nodes[rng() % nodes.size()];
            nodes.push_back(node);
            REQUIRE_FALSE(from.client->edit(make_create(node, parent, "Node" + std::to_string(step))));
        } else if (choice < 45) {
            const Uuid node = nodes[rng() % nodes.size()];
            const Uuid sprite = Uuid::generate();
            components.push_back(sprite);
            REQUIRE_FALSE(from.client->edit(make_add_component(node, sprite, "Sprite2D")));
        } else if (choice < 75 && !components.empty()) {
            const Uuid sprite = components[rng() % components.size()];
            const Uuid node = nodes[rng() % nodes.size()];
            REQUIRE_FALSE(from.client->edit(make_set_layer(node, sprite, static_cast<i32>(rng() % 32))));
        } else if (choice < 90) {
            const Uuid node = nodes[rng() % nodes.size()];
            REQUIRE_FALSE(from.client->edit(make_rename(node, "N" + std::to_string(step))));
        } else if (nodes.size() > 1) {
            const Uuid node = nodes[rng() % nodes.size()];
            REQUIRE_FALSE(from.client->edit(make_delete(node)));
        }

        fixture.network.flush();
        REQUIRE_FALSE(fixture.a.client->poll());
        REQUIRE_FALSE(fixture.b.client->poll());
    }

    CHECK(fixture.a.client->applied() == fixture.server.log_size());
    CHECK(fixture.b.client->applied() == fixture.server.log_size());
    CHECK(document_json(fixture.a.client->document()) == document_json(fixture.b.client->document()));
    CHECK(document_json(fixture.server.document()) == document_json(fixture.a.client->document()));
}
