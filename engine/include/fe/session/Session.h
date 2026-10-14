// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/Operation.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

class ResourceManager;

namespace session {

// The stable name of one replica in a session. Every operation carries the id of
// the replica that made it, so a log can be read back and attributed without
// asking the peer.
using ReplicaId = Uuid;

// One committed entry: an operation and the replica that authored it.
struct LogEntry {
    serialization::Operation op;
    ReplicaId origin{};
};

// A local, headless collaborative-editing session: a document plus an ordered,
// append-only log of operations.
//
// This is the third local step of ADR-0019 (after component identity, ADR-0045,
// and the operation model, ADR-0047). The point of the log is convergence: every
// replica that applies the same operations in the same order ends up with the
// same document, so two in-process replicas can be fed one interleaved sequence
// and compared. There is no network here; a transport and an authoritative
// server come later (the `net` module). The log earns its keep before any server
// exists because undo becomes log-based: undo is the inverse of an entry, redo
// is the entry again.
//
// The session owns its document. Operations name their objects by stable id, so
// a replica never has to translate a peer's pointers: the ids in the log are the
// ids in every document.
//
// See docs/adr/0048-replicated-operation-log.md.
class Session {
public:
    // `resources` may be null. It is used only to resolve resource references in
    // a SetResource operation, exactly as in apply(); a headless session passes
    // null and SetResource becomes a no-op.
    explicit Session(ResourceManager* resources = nullptr);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] Scene& document() noexcept { return document_; }
    [[nodiscard]] const Scene& document() const noexcept { return document_; }

    // The replica this session is. Operations submitted here are stamped with it.
    [[nodiscard]] const ReplicaId& replica_id() const noexcept { return replica_id_; }

    // --- The log -----------------------------------------------------------
    [[nodiscard]] const std::vector<LogEntry>& log() const noexcept { return log_; }
    [[nodiscard]] usize log_size() const noexcept { return log_.size(); }

    // Appends `op` to the log as this replica's edit and applies it to the
    // document. Returns the index the entry was given.
    //
    // Submitting discards any undone tail: a new edit after an undo is a new
    // branch, and the redo entries it replaced are dropped (the usual editor
    // rule). Applying optimistically is the local stand-in for the server's
    // ordering; reconciliation arrives with the transport.
    [[nodiscard]] Result<usize> submit(serialization::Operation op);

    // Appends an entry from another replica and applies it. This is what a
    // replica does when it receives a peer's operation; a log replayed through
    // this path converges with the sender. `origin` is the authoring replica.
    [[nodiscard]] Error receive(serialization::Operation op, const ReplicaId& origin);

    // Applies every entry in the log, in order, to a fresh document. Used to
    // prove convergence (replay into a second replica and compare) and to
    // reconstruct a document from a persisted log. `out` is cleared first.
    [[nodiscard]] Error replay_into(Scene& out, ResourceManager* resources = nullptr) const;

    // --- Undo/redo over the log -------------------------------------------
    // Undo is the inverse of the last applied entry, recomputed from the current
    // document; redo re-applies it. A destructive entry (DeleteNode,
    // RemoveComponent) is undone by restoring the object it captured, so its
    // identity survives the round trip.
    [[nodiscard]] bool can_undo() const noexcept { return undo_count_ < log_.size(); }
    [[nodiscard]] bool can_redo() const noexcept { return undo_count_ > 0; }
    [[nodiscard]] Error undo();
    [[nodiscard]] Error redo();

    // --- Persistence -------------------------------------------------------
    // The log as JSON: `{"entries":[{"origin":..,"op":{..}}, ..]}`. An operation
    // is the serializable form of an edit (ADR-0047), so the whole log
    // round-trips and can be handed to a transport.
    [[nodiscard]] std::string to_json() const;

    // Replaces the log and the document from a JSON log. The document is rebuilt
    // by replaying, so a session is a pure function of its log.
    [[nodiscard]] Error from_json(std::string_view text);

private:
    // Applies entry `index` forward, capturing the removed object for a
    // destructive entry and the document-derived inverse for the others.
    [[nodiscard]] Error apply_forward(usize index);

    // Drops any undone tail, so a new edit after an undo starts a new branch.
    void drop_redo_tail();

    Scene document_;
    std::vector<LogEntry> log_;
    // The inverse of each entry, derived from the document just before it was
    // applied (the moment the "previous" value still exists). Unused for a
    // destructive entry, whose undo restores the captured object instead.
    std::vector<serialization::Operation> inverses_;
    // Removed objects, aligned with `log_`. Empty for entries that remove
    // nothing; consumed when such an entry is undone.
    std::vector<serialization::AppliedObjects> captures_;
    // How many trailing entries are currently undone. The applied entries are
    // [0, log_.size() - undo_count_).
    usize undo_count_ = 0;
    ReplicaId replica_id_{};
    ResourceManager* resources_ = nullptr;
};

} // namespace session
} // namespace fe
