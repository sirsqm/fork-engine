# ADR-0048: The replicated operation log, local only

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

[ADR-0019](0019-collaborative-editing.md) plans collaborative editing in stages.
The first two local steps have landed: components carry stable ids
([ADR-0045](0045-component-identity.md)), and an edit is a serializable,
id-addressed `Operation` with a document-derived inverse
([ADR-0047](0047-local-operation-model.md)). The third step is the log.

The reason to build it now, with no network in sight, is that it is what makes
convergence a property that can be *tested*. Two replicas that apply the same
operations in the same order must end up with the same document; that claim is
either checked by a test or it is a hope. Building the log locally, in process,
turns "will collaboration converge?" into a question a unit test answers today.

There is a second reason. Undo/redo currently reconstructs a removed object from
bespoke command classes. Once the document is a function of a log, undo is the
inverse of an entry and redo is the entry again, and that is a strict
improvement to undo/redo on its own — it earns its keep before any server exists.

## Decision

**A `Session` owns a document and an ordered, append-only log of operations.
Every replica that applies the same log in the same order converges. The log is
local and headless: no network, no server, no threads.**

### The log

- `fe::session::Session` holds a `Scene`, a `std::vector<LogEntry>`, and the id
  of the replica it is. A `LogEntry` is an `Operation` plus the `ReplicaId` that
  authored it, so a log can be read back and attributed without asking a peer.
- `submit(op)` appends an entry authored by this replica and applies it
  optimistically. `receive(op, origin)` appends a peer's entry and applies it —
  the path a replica uses when a peer's operation arrives. Both are the same
  append-and-apply; only the attribution differs.
- The document is a pure function of the log: `replay_into(scene)` applies the
  whole log to a fresh document, and `from_json` rebuilds the log's document the
  same way. There is no second source of truth to drift.

### Convergence, testably

- Two in-process `Session`s fed the same operations in the same order produce
  byte-identical documents (compared through the scene serializer, which writes
  stable ids). This is the test that stands in for the network until the network
  exists.
- The log round-trips through JSON (`{"entries":[{"origin":..,"op":..}]}`). An
  operation is already serializable (ADR-0047), so a log is serializable for
  free, which is what a transport will carry.

### Undo/redo over the log

- `undo()` inverts the last applied entry. For a property or structural edit the
  inverse is computed from the document **before** the entry is applied and
  stored alongside it — the same ordering `OperationCommand` uses, and the only
  ordering in which the "previous" value still exists.
- A destructive entry (`DeleteNode`, `RemoveComponent`) has no single-operation
  inverse: its inverse is the object it removed. `apply` hands that object to a
  capture sink (ADR-0047), the session holds it, and `undo` restores it with its
  identity intact. `redo` re-applies the entry forward.
- A new `submit` after an undo drops the undone tail: a new edit is a new branch,
  the usual editor rule. The log stays a linear sequence of applied edits, which
  is what a server will order.

### What is deliberately *not* here

- **No transport, no server, no presence.** Those are the next steps of
  ADR-0019. The log is the piece they are built on; adding them now would mean
  guessing at requirements the local log lets us measure instead.
- **No threads.** Ordering is whatever the caller submits; concurrency is the
  server's job, not the session's.
- **No last-writer-wins or CRDT policy.** With one writer (the local user) there
  is nothing to reconcile. The policy lands with the server, against a
  reproduced conflict, per ADR-0019.

## Consequences

- Convergence becomes a unit test rather than an aspiration. The same test
  extends naturally when a loopback transport arrives: the transport delivers
  entries and the session is unchanged.
- Undo/redo no longer depends on the command classes for a removed object; the
  session's capture is the mechanism. The editor's `CommandStack` still exists
  and still works; migrating the editor's history onto the log is a later,
  separate step (the log-based undo is proven here first, headless).
- A session is a pure function of its log, so a document can be reconstructed,
  diffed, and (later) replayed from a peer. That is the property the whole
  collaborative feature rests on.
- The `session` module depends on `scene` and `serialization` and on nothing
  above them. It is engine-side and headless, so it is testable with no window
  and no GPU, like the operation model before it.

## Alternatives considered

- **Build the transport first, the log as part of it.** Rejected: the log's
  correctness is independent of how bytes move, and proving it in process first
  means the transport is tested against a known-good log rather than being asked
  to make an unproven log correct.
- **Make the log a scene-wide CRDT immediately.** Rejected for the same reason
  ADR-0019 defers it: ordered structural operations through an authority avoid
  most of the difficulty, and a CRDT is adopted only against a reproduced
  failure.
- **Store the inverse as captured editor state in each entry.** Rejected: it
  duplicates state the document already holds, and it is exactly what ADR-0047
  removed from the command layer. Deriving the inverse from the document keeps
  one source of truth.
- **Journal to disk on every submit.** Deferred: the log round-trips through
  JSON already, so persistence is a call the caller makes, not something the
  session does implicitly on the edit path.
