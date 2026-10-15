# ADR-0049: The transport and the authoritative session server

- **Status** — Accepted
- **Supersedes** — none
- **Superseded by** — none

## Context

[ADR-0019](0019-collaborative-editing.md) plans collaborative editing in stages.
The first three local steps have landed: components carry stable ids
([ADR-0045](0045-component-identity.md)), an edit is a serializable,
id-addressed `Operation` ([ADR-0047](0047-local-operation-model.md)), and a
`Session` holds a document plus an ordered log that two replicas converge on
([ADR-0048](0048-replicated-operation-log.md)). All of it is local: no bytes
move between replicas.

The fourth step is transport and an authority. The log proved that *if* every
replica applies the same operations in the same order, they converge. What it did
not settle is *who decides the order*. Two clients editing at once will each
apply their own edit first; without a single order they diverge, and no amount of
per-replica cleverness fixes it. Something has to be the arbiter.

ADR-0019 and STRATEGY.md (C4) already chose the shape: a `net` module with a
transport abstraction, an in-process loopback for tests and a WebSocket for
production, and a session server that orders structural operations and applies
last-writer-wins to property edits. This record lands the module and the server
on the loopback; the WebSocket backend is a separate change behind the same
interface.

## Decision

**A `net` module moves `Message`s over a `Transport`. An authoritative
`SessionServer` gives every accepted edit a global sequence number and
broadcasts it; every `SessionClient` applies only what the server sends, in the
server's order, so replicas converge by construction.**

### The transport seam

- `Transport` is the whole of the network contract: `send(message)`,
  `poll(out)`, `connected()`. It knows nothing about operations, ordering, or
  scenes — those live above it. That is what lets the same session code run over
  an in-process loopback in a test and a WebSocket in production, unchanged.
- The interface is synchronous and single-threaded: a caller `poll`s to drain
  what arrived and `send`s to hand a message over. A production transport may
  buffer internally, but it never calls back on another thread. This keeps the
  session deterministic and testable, and matches the engine's rule that the RHI
  device — and here the session — is single-threaded.
- `LoopbackNetwork` is the test transport: an in-process hub with one server
  handler and one inbox per connected replica. A `flush()` runs the queued
  deliveries to completion, so a test interleaves two clients' edits in an exact
  order and asserts convergence. It also tracks membership (a `Join`/`Leave`
  marks a replica connected/left), so `broadcast` and `connected()` need no
  protocol of their own, and it stamps the origin, so a client cannot claim to be
  a peer.

### The authority

- `SessionServer` owns a `Session` (its authoritative document and log). For each
  `Operation`/`PropertyEdit` it receives, it `submit`s the operation to that
  session, which assigns it the next log index. That index **is** the global
  order. The server then broadcasts the message, stamped with the index, to
  every replica — including the author.
- `SessionClient` applies **only** what the server sends, in the server's order.
  It does not apply its own edit optimistically: an edit goes to the server and
  comes back in the server's position, and only then is it applied. There is a
  single order and every replica walks it, so convergence is structural rather
  than a client-side repair.
- An edit that would not apply to the authoritative document (a stale target) is
  dropped and counted, not committed and not broadcast: a bad edit must not
  become committed state as if it had succeeded.
- A joining client is sent a `Sync` carrying the whole log, so it starts from the
  server's state rather than replaying history op by op. `Sync` reuses
  `Session::to_json`, so a client rebuilds its document the same way it would
  from a saved log.

### Last-writer-wins per property

Last-writer-wins is a *consequence* of the single order, not a separate rule:
the last write to a property in server order is the one that survives, because
every client applies that same order. The `PropertyEdit` message kind exists so
that a later policy — coalescing, per-property vectors, node-level locking
(ADR-0019 C6) — can be added without changing the wire format. The server does
not need to inspect the operation kind to order it; the kind is carried for the
policy that is not written yet.

### What is deliberately *not* here

- **No WebSocket backend.** The `Transport` interface is the deliverable; a
  socket backend is a second implementation behind it, and it is a separate
  change. Landing it now would mean testing the protocol against a real socket
  before the protocol is settled.
- **No optimistic local apply.** Applying locally and reconciling is the hard
  part of collaborative editing, and ADR-0019 defers reconciliation until a
  conflict is reproduced. The server-ordered path is the floor it will be built
  on, and it is correct on its own.
- **No presence, no asset sync.** Those are C5 and C7 of ADR-0019.
- **No threads in the transport.** Concurrency is a deployment concern; the
  session contract stays single-threaded.

## Consequences

- Two in-process clients editing concurrently converge, and a fuzz test that
  interleaves random operations from both confirms it. The property ADR-0048
  proved for a shared log is now proved for two clients over a transport.
- Every edit pays a round trip before it is visible locally. That is the price of
  a single order with no reconciliation, and it is the right first price: it is
  simple, it is correct, and the optimistic path can be layered on later without
  changing the log or the wire format.
- The `net` module depends only on `core`, `scene`, `serialization`, and
  `session`. It is engine-side and headless, so it is testable with no window, no
  GPU, and no socket — like the session before it.
- A production WebSocket backend is now a bounded piece of work: implement
  `Transport`, and the server and client are unchanged.

## Alternatives considered

- **Peer-to-peer with no authority.** Rejected: without a single order, two
  concurrent structural edits (both deleting a node, say) require reconciliation
  logic that ADR-0019 explicitly defers, and the deferred reconciliation is
  exactly the hard part. An authority removes the problem instead of managing it.
- **Optimistic local apply with server reconciliation.** Rejected for now: it is
  the more responsive design and the likely end state, but it needs the
  reconciliation it would be built to test, and it would hide ordering bugs
  behind a rollback path before the ordering itself is proven. The
  server-ordered path proves the order first.
- **A CRDT per property instead of an order.** Rejected for now, as in
  ADR-0048: it is adopted only against a reproduced conflict, and the
  `PropertyEdit` kind leaves room for it without a wire change.
- **A real socket in the first cut.** Rejected: it adds timing and failure modes
  to a test that should be deterministic. The loopback settles the protocol; the
  socket then implements a settled protocol.
- **Threading the transport.** Rejected: the engine already keeps its device
  single-threaded, and a threaded transport would make every session test racy
  for a concurrency the session does not need.
