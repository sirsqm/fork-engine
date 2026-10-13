# ADR-0019: Collaborative editing — operations, a replicated log, an authority

- **Status** — Proposed
- **Supersedes** — none
- **Superseded by** — none

## Context

Several people should be able to open the same project and edit it at the same
time, see each other's changes live, see each other's cursors and selections,
and never run a manual merge. This is a scene editor with real-time
multiplayer, not a file-sync tool.

The difficulty is not the network. It is that a scene is a *shared mutable
structure*: nodes and components with identity, parent/child relationships,
ordered sibling lists, and typed properties. Concurrent edits to such a
structure must converge — every replica must end up with the same scene — or
collaborators silently diverge.

The engine is already unusually well prepared for this:

- Nodes have a stable `Uuid` (`scene`).
- Serialization and the inspector are both driven by reflection
  ([ADR-0005](0005-reflection-and-serialization.md)), so a property edit is
  already addressable as `(component, property path, value)` with no per-type
  code.
- Every editor edit already flows through a command stack rather than mutating
  the scene directly ([ADR-0015](0015-editor-undo-redo.md)).

What is missing is stable identity for *components* (only nodes have it) and an
edit representation that can be transmitted and replayed.

## Decision

**Model editing as discrete, serializable operations over stably identified
objects; replicate an ordered operation log through an authoritative session;
apply property edits last-writer-wins per property; layer presence on top; do
the whole thing locally and headlessly before any network exists.**

### Identity

- Components gain stable ids, so an operation can address the same component on
  every replica and survive serialization, reimport, and undo. Node identity
  (`Uuid`) already exists and is reused.

### Operations

- An `Operation` is the serializable form of an edit: create node, delete node,
  reparent, reorder, add component, remove component, set property. A property
  operation is `(entity id, component id, property path, value)`.
- The existing `Command` stack is generalised so local edits are expressed as
  operations. Undo becomes log-based: undo is an inverse operation, redo is the
  original.
- Operations are the *only* way the document changes. Panels already build
  commands; they now build operations.

### The replicated log

- A `Session` holds a document and an ordered, append-only log of operations.
  Every replica applies the same log in the same order and converges.
- An **authoritative session server** assigns sequence numbers, serialises
  structural operations (so two people cannot create conflicting structure
  simultaneously), and resolves concurrent property edits by last-writer-wins
  per property.
- Edits apply **optimistically** locally and are reconciled against the server's
  order, so the editor never blocks on the network.

### Presence

- Cursors, selections, and per-user viewport cameras are broadcast through the
  session as ephemeral messages, not part of the document log.

### Assets

- A content-addressed asset store; a replica that lacks an asset the scene
  references fetches it, with dedup and delta transfer.

### Sequencing

- C1–C3 are **local and headless**: component identity, the operation model, and
  the replicated log with two in-process replicas. No network. This also makes
  undo/redo more robust, so it earns its keep even if collaboration slips.
- C4 adds a transport abstraction (in-process loopback for tests, WebSocket in
  production) and the server. Loopback is what makes the whole thing testable
  without a network.

## Consequences

- The editor's edit path becomes an operation log. That is a deeper change than
  it sounds — it touches how the document mutates and how undo works — but it
  is also a strict improvement to undo/redo, which is why it lands early and
  locally.
- Component identity is now load-bearing. Ids must be stable across
  serialization and reimport; a test that they survive a round-trip is part of
  C1.
- Convergence is testable: two in-process replicas fed the same interleaved log
  must produce identical scenes, and a fuzz test interleaves random operations.
- The engine grows a `net` module and a session server. That is new operational
  surface (a server to run), which is why the transport is abstracted and the
  local case works with no server at all.
- Last-writer-wins per property is simple and predictable but coarse: two people
  editing different fields of the same component are fine, but the same field
  resolves to whoever wrote last. Node-level locking for structural edits is the
  fallback if this proves insufficient; property CRDTs are a further fallback,
  adopted only against a concrete failure.

## Alternatives considered

- **File-based sync (a shared folder, Git, a cloud drive).** Rejected: it is
  manual-merge collaboration, which is exactly what the goal excludes.
- **A CRDT for the whole scene from day one.** Deferred: full-scene CRDTs are
  complex, and much of the difficulty (ordered sibling lists, structural moves)
  is avoided by ordering structural operations through an authority. CRDTs are
  kept as a property-level fallback, not the foundation.
- **Peer-to-peer, no authority.** Rejected: without an ordering authority,
  structural conflicts (two creates, a move and a delete) need a coordination
  protocol as complex as a server, with worse debuggability.
- **Editing the live game (a running session) rather than the document.**
  Rejected for now: the goal is collaborative *authoring*. Live-edit is a
  separate, later capability with different consistency needs.
- **A third-party multiplayer/OT library.** Considered; deferred until the local
  log exists, so the choice is made against real requirements rather than
  guessed. The operation model is deliberately small enough to own.
