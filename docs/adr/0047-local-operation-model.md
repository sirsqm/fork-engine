# ADR-0047: The local operation model

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0019](0019-collaborative-editing.md),
  [ADR-0045](0045-component-identity.md),
  [ADR-0015](0015-editor-undo-redo.md),
  [ADR-0005](0005-reflection-and-serialization.md)

## Context

Collaborative editing ([ADR-0019](0019-collaborative-editing.md)) is planned as
three local, headless steps before any network exists: component identity, an
operation model, and a replicated log. The first is done
([ADR-0045](0045-component-identity.md)): every component now carries a `Uuid`
that survives serialization and reimport, and `Scene::find_component` resolves
one anywhere in the tree.

The editor's edits are still expressed as in-memory `Command` objects
([ADR-0015](0015-editor-undo-redo.md)). A `Command` is the right shape for an
undo stack — it owns what it removed, so a delete is undone with identity intact
— but it is not a thing that can be *transmitted* or *replayed*:

- It is a live C++ object with virtual methods and, in one case
  (`SetResourceCommand`), a borrowed `ResourceManager*`. It has no wire form.
- It holds captured state (`old_value_`, `detached_`) that exists only because
  the same object is reused for undo. A peer replaying the edit never has that
  state; it must derive the inverse from the same document state the edit
  started from.
- The component-addressed commands still name a component by *index*. The whole
  point of ADR-0045 was that an index is not an identity a different replica can
  resolve.

ADR-0019's second step is therefore to introduce a **serializable operation**
and route local edits through it, so that the thing an editor builds is the same
thing a log would carry. It is worth doing locally and headlessly first because
it strengthens undo/redo on its own — an operation's inverse is computed from
the document, not stashed in the editor — and because it is the contract the
replicated log is written against.

## Decision

**Model an edit as a `serializable::Operation`: a small tagged struct that names
its objects by stable id, round-trips through JSON, and can be applied to a
`Scene`; the editor's `Command` becomes an adapter that executes an operation and
reverses it with the operation's inverse.**

### The operation set

The set is closed and small, matching the editor's existing edits one for one:

- `CreateNode { node_id, parent_id, index, name }`
- `DeleteNode { node_id }`
- `ReparentNode { node_id, parent_id, index }`
- `RenameNode { node_id, name }`
- `AddComponent { node_id, component_id, type_name, index }`
- `RemoveComponent { node_id, component_id }`
- `SetProperty { node_id, component_id, path, value }`
- `SetResource { node_id, component_id, property_name, reference }`

A `CreateNode` carries the new node's id, so the operation is *deterministic*:
applying it on any replica that shares the log produces the same node id, which
is what lets a later operation address the node. `AddComponent` carries the new
component's id for the same reason. `CreateNode` and `AddComponent` keep an
`index` because position is part of the intent (where the user dropped it) even
though identity is not positional; the index is clamped on apply.

Every object is named by `Uuid`. `SetProperty` and `SetResource` name the
component by id, so they resolve correctly even when a node carries two
components of the same type or a component was inserted since the operation was
built — the failure mode ADR-0045 exists to remove.

### Serialization

An operation serializes to a JSON object with a `"kind"` discriminator and the
fields above; a `Value` (a `reflect::Value`) serializes as a tagged
`{"kind": ..., "value": ...}` object so a bool, an int, a float, a string, and a
`Uuid` are distinguishable on the wire — the reflection `Value` is a `variant`,
and JSON alone cannot tell `1` from `1.0` from `"1"`. Reading is strict: an
unknown kind or a malformed field is an `Error`, not a silently dropped edit.

### Apply and inverse

- `apply(Operation&, Scene&, ResourceManager*)` performs the edit. It uses the
  same registry-driven resolution the commands used, so it names no component
  type. `apply` is total where the document permits it: an operation whose target
  is absent is a no-op, not a crash.
- `inverse(const Operation&, const Scene&)` returns the operation that undoes
  `op` *against the given document state*. It is a pure function of the
  document: `SetProperty`'s inverse reads the current value, `ReparentNode`'s
  inverse reads the current parent and index, `CreateNode`'s inverse is a
  `DeleteNode`. This is the property a replica needs — it never shares the
  editor's captured state, only the document.

### The editor adapter

The editor keeps `CommandStack` and the `Command` interface unchanged; a new
`OperationCommand` wraps an `Operation`. Its `execute` applies the operation and
records the inverse captured from the document before the first apply; its
`undo` applies the recorded inverse. Coalescing still works: a drag builds
successive `SetProperty` operations, and the adapter merges them the same way
`SetPropertyCommand` did, keeping the original inverse. The editor's panels build
operations instead of the bespoke command classes.

The existing command classes are kept: they remain the tested, index-and-id
faithful implementation of each edit, and the new tests assert that an operation
and its command counterpart leave the document in the same state. `OperationCommand`
reuses that behaviour rather than reimplementing it, so there is one place an
edit is actually performed.

## Consequences

- **An edit has a wire form.** `Operation` round-trips through JSON, so the
  replicated log of ADR-0019 can carry it with no further modelling. This is the
  whole point of the step.
- **Undo/redo is now derived, not captured.** `OperationCommand` computes the
  inverse from the document. This is strictly stronger than the old captured
  state: a command whose target was modified between execute and undo now undoes
  correctly, because the inverse was taken against the state the undo runs on.
- **The editor names components by id.** The index-based addressing that
  ADR-0045 flagged as not-an-identity is gone from the edit path. A node with two
  components of one type is unambiguous, and an insert no longer shifts what an
  operation means.
- **One more indirection on the edit path.** A local edit now builds an
  `Operation`, wraps it in an `OperationCommand`, and applies through it. Edits
  happen at human rates, so this is not on a measured hot path; it is recorded
  here so it is a conscious trade, not an accident.
- **Two representations exist for now.** The command classes and the operation
  set both describe an edit. They are kept deliberately in step by a test that
  applies each both ways and compares the document; when the replicated log
  lands, the command classes can be retired in favour of operations alone.
- **`apply` can still no-op.** An operation against a missing node or component
  does nothing. For the local editor that matches the old commands; when a log
  exists, a server validating operations is the place that turns "target absent"
  into a reported error.

## Alternatives considered

- **Serialize the existing `Command` objects directly.** Rejected: they carry
  live captured state and a borrowed manager, and they name components by index.
  Serializing them would freeze the index-addressing the previous ADR removed and
  would put a `ResourceManager` on the wire.
- **A diff/patch of the serialized scene.** Rejected: a JSON diff is not a set of
  named operations, so it cannot express intent (create, reparent, reorder), and
  it does not name objects by id — it names them by position in the document,
  which is exactly what a concurrent log cannot rely on.
- **Compute the inverse only when the log exists.** Rejected: deriving the
  inverse locally is what proves the operation set is complete enough to be
  inverted, which is the property the log depends on. Doing it now, against the
  undo stack that already exercises every edit, is the cheapest possible test.
- **Replace `Command` with `Operation` outright.** Rejected for now: the command
  classes are the tested behaviour of each edit, and the adapter keeps the
  `CommandStack`/coalescing machinery that the UI relies on. Retiring them is a
  mechanical follow-up once the log is the only consumer.
- **Name components by `(node id, type name)` in operations.** Rejected for the
  same reason as in ADR-0045: a node may carry two components of one type, so the
  pair is not unique.
