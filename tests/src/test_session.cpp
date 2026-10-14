// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The local replicated log (docs/adr/0048-replicated-operation-log.md). A
// session holds a document and an ordered log of operations; two replicas fed
// the same operations in the same order converge, undo is the inverse of a log
// entry, and the whole log round-trips through JSON. Headless: no window, no
// network, no GPU.

#include <doctest/doctest.h>

#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Registry.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/Operation.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/session/Session.h"

#include <ostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace fe;
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

Operation make_set_int(const Uuid& node_id, const Uuid& component_id, std::string property, i64 value) {
    Operation op;
    op.kind = OperationKind::SetProperty;
    op.node_id = node_id;
    op.component_id = component_id;
    op.path = {std::move(property)};
    op.value = value;
    return op;
}

Operation make_delete(const Uuid& node_id) {
    Operation op;
    op.kind = OperationKind::DeleteNode;
    op.node_id = node_id;
    return op;
}

// The document as the file format would write it, for a byte-for-byte
// convergence comparison (ids are stable, so two converged replicas match).
std::string document_json(const Scene& scene) {
    const Result<std::string> text = serialization::scene::to_json(scene);
    REQUIRE(text);
    return text.value();
}

// A submitted edit in the fuzz loop is expected to be valid; report it as a
// check rather than ignoring the nodiscard result.
bool session_ok(const Result<usize>& result) {
    if (!result) {
        MESSAGE("unexpected submit failure: ", result.error().message());
    }
    return static_cast<bool>(result);
}

} // namespace

TEST_CASE("a submitted operation is logged, attributed, and applied") {
    reflect::register_builtin_types();
    session::Session session;

    const Uuid id = Uuid::generate();
    const Result<usize> index = session.submit(make_create(id, {}, "Hero"));
    REQUIRE(index);
    CHECK(index.value() == 0);
    CHECK(session.log_size() == 1);
    REQUIRE(session.log().size() == 1);
    CHECK(session.log()[0].origin == session.replica_id());
    CHECK(session.log()[0].op.kind == OperationKind::CreateNode);

    CHECK(session.document().find_node(id) != nullptr);
    CHECK(session.document().root_count() == 1);
}

TEST_CASE("a received operation is attributed to its origin, not the receiver") {
    reflect::register_builtin_types();
    session::Session a;
    session::Session b;

    const Uuid id = Uuid::generate();
    REQUIRE(a.submit(make_create(id, {}, "Hero")));
    REQUIRE_FALSE(b.receive(a.log()[0].op, a.log()[0].origin));

    REQUIRE(b.log_size() == 1);
    CHECK(b.log()[0].origin == a.replica_id());
    CHECK(b.log()[0].origin != b.replica_id());
    CHECK(b.document().find_node(id) != nullptr);
}

TEST_CASE("two replicas fed the same operations in the same order converge") {
    reflect::register_builtin_types();
    session::Session a;
    session::Session b;

    const Uuid root = Uuid::generate();
    const Uuid child = Uuid::generate();
    const Uuid sprite = Uuid::generate();

    REQUIRE(a.submit(make_create(root, {}, "Root")));
    REQUIRE(a.submit(make_create(child, root, "Child")));
    REQUIRE(a.submit(make_add_component(child, sprite, "Sprite2D")));
    REQUIRE(a.submit(make_set_int(child, sprite, "layer", 3)));

    // b receives every entry in order, exactly as a peer would.
    for (const session::LogEntry& entry : a.log()) {
        REQUIRE_FALSE(b.receive(entry.op, entry.origin));
    }

    CHECK(a.log_size() == b.log_size());
    CHECK(document_json(a.document()) == document_json(b.document()));
}

TEST_CASE("a log replayed into a fresh document reconstructs it") {
    reflect::register_builtin_types();
    session::Session session;

    const Uuid root = Uuid::generate();
    REQUIRE(session.submit(make_create(root, {}, "Root")));
    REQUIRE(session.submit(make_create(Uuid::generate(), root, "Child")));

    Scene replayed;
    REQUIRE_FALSE(session.replay_into(replayed));
    CHECK(document_json(replayed) == document_json(session.document()));
}

TEST_CASE("undo inverts the last entry and redo re-applies it") {
    reflect::register_builtin_types();
    session::Session session;

    const Uuid node = Uuid::generate();
    const Uuid sprite = Uuid::generate();
    REQUIRE(session.submit(make_create(node, {}, "Node")));
    REQUIRE(session.submit(make_add_component(node, sprite, "Sprite2D")));
    REQUIRE(session.submit(make_set_int(node, sprite, "layer", 7)));

    REQUIRE(session.document().find_node(node) != nullptr);
    REQUIRE(session.document().find_component(sprite) != nullptr);

    // Undo the property write: the inverse restores the value the document held.
    REQUIRE_FALSE(session.undo());
    Component* component = session.document().find_component(sprite);
    REQUIRE(component != nullptr);
    CHECK(static_cast<Sprite2D*>(component)->layer == 0);

    // Undo the add: the component is gone, but the create is still applied.
    REQUIRE_FALSE(session.undo());
    CHECK(session.document().find_component(sprite) == nullptr);
    CHECK(session.can_undo());

    // Redo re-applies the add, then the property write.
    REQUIRE_FALSE(session.redo());
    component = session.document().find_component(sprite);
    REQUIRE(component != nullptr);
    REQUIRE_FALSE(session.redo());
    CHECK(static_cast<Sprite2D*>(session.document().find_component(sprite))->layer == 7);
}

TEST_CASE("undoing a delete restores the node with its identity intact") {
    reflect::register_builtin_types();
    session::Session session;

    const Uuid root = Uuid::generate();
    const Uuid child = Uuid::generate();
    REQUIRE(session.submit(make_create(root, {}, "Root")));
    REQUIRE(session.submit(make_create(child, root, "Child")));
    REQUIRE(session.submit(make_delete(child)));

    CHECK(session.document().find_node(child) == nullptr);

    REQUIRE_FALSE(session.undo());
    Node* restored = session.document().find_node(child);
    REQUIRE(restored != nullptr);
    CHECK(restored->name() == "Child");
    REQUIRE(restored->parent() != nullptr);
    CHECK((restored->parent()->id() == root));
}

TEST_CASE("a new submit after an undo drops the redo tail") {
    reflect::register_builtin_types();
    session::Session session;

    const Uuid node = Uuid::generate();
    const Uuid sprite = Uuid::generate();
    REQUIRE(session.submit(make_create(node, {}, "Node")));
    REQUIRE(session.submit(make_add_component(node, sprite, "Sprite2D")));

    REQUIRE_FALSE(session.undo()); // the AddComponent is undone
    CHECK(session.can_redo());

    // A new edit branches: the undone AddComponent is gone for good. The edit
    // must be valid on the post-undo document, so rename the node.
    Operation rename;
    rename.kind = OperationKind::RenameNode;
    rename.node_id = node;
    rename.name = "Renamed";
    REQUIRE(session.submit(rename));
    CHECK_FALSE(session.can_redo());
    CHECK(session.log_size() == 2);
    CHECK(session.document().find_component(sprite) == nullptr); // the add was dropped
    CHECK(session.document().find_node(node)->name() == "Renamed");
}

TEST_CASE("a log round-trips through JSON and rebuilds the same document") {
    reflect::register_builtin_types();
    session::Session session;

    const Uuid root = Uuid::generate();
    const Uuid child = Uuid::generate();
    REQUIRE(session.submit(make_create(root, {}, "Root")));
    REQUIRE(session.submit(make_create(child, root, "Child", 0)));
    REQUIRE(session.submit(make_delete(child)));

    const std::string text = session.to_json();

    session::Session restored;
    REQUIRE_FALSE(restored.from_json(text));
    CHECK(restored.log_size() == session.log_size());
    CHECK(document_json(restored.document()) == document_json(session.document()));
    // Origins survive the round trip.
    CHECK(restored.log()[0].origin == session.replica_id());
}

TEST_CASE("the log reader rejects a malformed log rather than guessing") {
    reflect::register_builtin_types();
    session::Session session;

    CHECK(session.from_json("not json").message().find("invalid JSON") != std::string::npos);
    CHECK(session.from_json("{}").message().find("entries") != std::string::npos);
    CHECK(session.from_json(R"({"entries":[{"op":{"kind":"no_such_kind"}}]})").message().find("kind") !=
          std::string::npos);
}

TEST_CASE("undo and redo on an empty log are reported, not silent") {
    reflect::register_builtin_types();
    session::Session session;

    CHECK_FALSE(session.can_undo());
    CHECK_FALSE(session.can_redo());
    CHECK(session.undo());
    CHECK(session.redo());
}

TEST_CASE("a long random but valid edit sequence converges across replicas") {
    reflect::register_builtin_types();
    session::Session source;

    // A fixed seed keeps the fuzz deterministic, so a failure is reproducible.
    std::mt19937 rng(20261014u);
    std::vector<Uuid> live_nodes;
    std::vector<std::pair<Uuid, Uuid>> live_components; // (node, component)

    for (int step = 0; step < 300; ++step) {
        const unsigned choice = rng() % 5u;
        if (live_nodes.empty() || choice == 0u) {
            const Uuid id = Uuid::generate();
            if (source.submit(make_create(id, {}, "Node" + std::to_string(step)))) {
                live_nodes.push_back(id);
            }
        } else if (choice == 1u) {
            const Uuid node = live_nodes[rng() % live_nodes.size()];
            const Uuid component = Uuid::generate();
            if (source.submit(make_add_component(node, component, "Sprite2D"))) {
                live_components.emplace_back(node, component);
            }
        } else if (choice == 2u && !live_components.empty()) {
            const auto& [node, component] = live_components[rng() % live_components.size()];
            CHECK(session_ok(source.submit(make_set_int(node, component, "layer", static_cast<i64>(rng() % 16u)))));
        } else if (choice == 3u) {
            const Uuid node = live_nodes[rng() % live_nodes.size()];
            Operation rename;
            rename.kind = OperationKind::RenameNode;
            rename.node_id = node;
            rename.name = "R" + std::to_string(step);
            CHECK(session_ok(source.submit(std::move(rename))));
        } else if (live_nodes.size() > 1u) {
            const usize pick = rng() % live_nodes.size();
            const Uuid node = live_nodes[pick];
            if (source.submit(make_delete(node))) {
                live_nodes.erase(live_nodes.begin() + static_cast<std::ptrdiff_t>(pick));
                std::erase_if(live_components, [&](const auto& pair) { return pair.first == node; });
            }
        }
    }

    // A peer that receives every entry in order converges with the source.
    session::Session peer;
    for (const session::LogEntry& entry : source.log()) {
        REQUIRE_FALSE(peer.receive(entry.op, entry.origin));
    }
    CHECK(source.log_size() > 0);
    CHECK(document_json(source.document()) == document_json(peer.document()));

    // Replaying the log into a fresh document reconstructs the same one.
    Scene replayed;
    REQUIRE_FALSE(source.replay_into(replayed));
    CHECK(document_json(replayed) == document_json(source.document()));
}
