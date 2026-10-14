# ADR-0043: Stable component identity

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0019](0019-collaborative-editing.md),
  [ADR-0020](0020-resource-identity-guid-sidecar.md),
  [ADR-0015](0015-editor-undo-redo.md),
  [ADR-0002](0002-hybrid-scene-ecs-architecture.md)

## Context

Collaborative editing ([ADR-0019](0019-collaborative-editing.md)) rests on one
precondition: every edit must name the exact object it changes, and that name
must survive serialization, reimport, and reordering. Nodes have had that since
the first cut — a `Uuid` assigned at creation, written to the document, and read
back by the loader — and resource references have had it since
[ADR-0020](0020-resource-identity-guid-sidecar.md). Components have not.

Today a component is addressed by *index*: `Node::detach_component(usize)` and
`Node::insert_component(usize, ...)`, and the editor's `SetPropertyCommand`,
`SetResourceCommand`, `AddComponentCommand`, and `RemoveComponentCommand` all
carry a `component_index`. That is enough for a single-user undo stack, where the
stack and the scene move together, but it is not an identity:

- An index is positional. Inserting a component shifts every later index, so a
  command captured before the insert now names a different component.
- A node may carry several components of the same type (the composition model in
  [ADR-0002](0002-hybrid-scene-ecs-architecture.md) allows it), so the type name
  is not an identity either — two `Sprite2D`s on one node are not
  interchangeable.
- An operation log is replayed by a *different* replica, which never saw the
  same pointer or the same insertion order. "Component 1 of node N" is not
  something a peer can resolve; "component with id X of node N" is.

ADR-0019 lists "component identity" as the first, local, headless step for
exactly this reason: it strengthens undo/redo on its own, before any networking
exists, and it is the foundation the operation model is written on top of.

## Decision

**Give every `Component` a `Uuid`, assigned at creation, written into the scene
document, and read back by the loader — the same contract `Node` already has.**

- `Component` gains `id()` and `set_id()`, and a private `Uuid id_` initialized
  to `Uuid::generate()`. A component therefore has an identity the moment it
  exists, with no separate "assign ids" pass to forget.
- The scene format goes to **version 3**. Each serialized component gains an
  `"id"` field beside `"type"`, exactly as a node has carried `"id"` since
  version 1. The reader restores the saved id; a document without one (version 2
  and older, or hand-written) keeps the id the component was created with. No
  migration code is needed, because the fallback is the correct behaviour.
- `Scene::find_component(const Uuid&)` resolves a component id anywhere in the
  tree, the counterpart of `Scene::find_node`. It walks the tree rather than
  keeping a second index: a component is created and destroyed far more often
  than a node, and an index would have to be maintained from every attach and
  detach. The walk is the correct first cut; an index is a measured
  optimisation if a profile ever shows it hot.

This ADR does **not** convert the editor's commands from index to id. That is the
next step of ADR-0019's plan — "operation model (local only)" — and it is a
deliberate, separate change so that this one stays small and provably correct.
The identity is the prerequisite; re-plumbing the commands onto it is a follow-up
that can then be a mechanical substitution.

## Consequences

- **A component can be named across a save/load cycle.** An operation that
  targets "the collider on this node" survives reordering and insertion, because
  it names an id rather than a position. This is the property the operation log
  needs and the reason it comes first.
- **A node with two components of one type is unambiguous.** Identity is per
  component, not per type, so the two are distinguishable by name.
- **Identity is preserved across a detach/reattach.** A component taken out of a
  node and put back keeps its id, so the editor's remove/undo pair does not
  renumber anything, and an operation that named the component still resolves.
- **The format bump is backward-compatible in the only direction that matters.**
  A version-2 document loads and gets fresh ids; a version-3 document is rejected
  by an older reader, which is the reader's existing policy for a newer version
  and is correct — the old reader cannot honour an id it does not read.
- **`find_component` is linear in the number of components.** That is fine for a
  scene of authoring scale. It is the same trade the node lookup made before its
  index existed, and it is recorded here so a future index has a reason to exist.
- **Ids cost 16 bytes per component and one generation each.** Negligible, and
  the alternative — a scheme that assigns ids lazily — is one more thing that can
  be forgotten before an edit is captured.

## Alternatives considered

- **Address components by (node id, type name).** Rejected: a node may carry two
  components of the same type, and the composition model treats that as normal.
  The pair is not unique, so it is not an identity.
- **Address components by (node id, index).** Rejected: positional, and it breaks
  the moment a component is inserted or removed — precisely the structural edits
  a collaboration log has to express.
- **Keep index-based commands and give them ids only at the log boundary.**
  Rejected: the log would have to map a positional reference to a stable one at
  the moment of capture, which needs the id to exist anyway. Assigning it at
  creation is simpler and removes the mapping.
- **Convert the editor commands to ids in the same change.** Rejected for now:
  it widens the diff across the editor's command set and its tests for no extra
  proof. Identity first, then the commands, keeps each change small enough to
  review and to reason about.
- **A scene-level index over component ids.** Rejected for now: it must be
  maintained from `Node::attach_component`/`detach_component`, which are hot in
  the editor, and nothing has measured the lookup as a cost. The walk is
  correct and cheap at authoring scale.
