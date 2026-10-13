# ADR-0015: Editor undo/redo — a command stack with coalescing

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0001](0001-engine-architecture-overview.md),
  [ADR-0006](0006-component-reflection.md),
  [ADR-0007](0007-editor-ui-toolkit.md)

## Context

By the end of M2 the editor could edit a scene - add and remove nodes, rename
them, change a reflected property - but every edit was immediate and
irreversible. That is not a shippable authoring tool: a mis-drag on a colour
picker or an accidental delete is unrecoverable, and users stop experimenting
once an edit can destroy work.

Undo/redo needs a model before any UI is wired to it, because the model decides
what a "unit of undo" is. Three forces shape the choice:

- **The scene has no change log.** `Scene`/`Node` are deliberately pure data
  (ADR-0002); they emit no events and keep no history. Anything that observes
  edits has to be told about them by the editor.
- **Property edits are generic.** The inspector names no component type or field
  (ADR-0006): it walks a `TypeDesc` and calls getters/setters. An undo model that
  needed one record type per component field would reintroduce exactly the
  per-type knowledge reflection removed.
- **Continuous edits are the common case.** Dragging a float produces a change
  every frame. A naive model would record hundreds of undo steps for one
  gesture, which is worse than useless.

## Decision

Put a **command stack** in `fork_editor_core` (headless, no ImGui), and express
every edit as a `Command` with `execute()` and `undo()`.

- A `Command` operates on a `Scene&`. `execute()` performs the edit; `undo()`
  reverses it. Both must be exact: after `undo()` the scene is byte-for-byte the
  state it was in before `execute()`.
- `CommandStack` owns the applied commands, supports `execute()`, `undo()`,
  `redo()`, and clears the redo history when a new command is executed.
  `undo()` past the beginning and `redo()` past the end are no-ops, not errors.
- Commands address scene objects by **`Uuid`, never by raw pointer**, and hold
  the detached object themselves while the command is undone. This is what makes
  a delete undoable: the command takes ownership of the node's subtree on
  `execute()` and hands it back on `undo()`, so identity (and every `Uuid` in
  the subtree) survives. The same pattern keeps a removed component alive and
  restores it at its original index.
- `SetPropertyCommand` is **generic through `reflect::Registry`**: it records a
  node id, a component index, a property *path* (a list of names, for groups),
  and the old and new `reflect::Value`. It resolves the setter by walking the
  `TypeDesc` at apply time. One class covers every reflected field of every
  component type.
- **Coalescing** is a rule, not a special case. A command may report a
  non-empty `coalesce_key()`; when a new command's key equals the top of the
  stack's, the stack calls `top.merge_with(new)` and, if it succeeds, discards
  the new command. A `SetPropertyCommand`'s key is the node id plus component
  index plus property path, and its merge adopts the newer value while keeping
  the original old value, so one undo reverts the whole drag. The editor calls
  `CommandStack::break_coalescing()` when ImGui reports the edit gesture ended
  (`IsItemDeactivatedAfterEdit`), which starts a fresh entry.
- The stack lives on `EditorContext`, next to the document it edits. Panels
  build commands and hand them to the context; they never mutate the scene
  directly. The scene is mutated only inside `Command::execute/undo`.

Editing is disabled while play-in-editor is running: play shows a copy of the
scene (ADR-0008), so an edit or an undo against the authored scene while it is
hidden would be invisible and confusing. `EditorContext` exposes this as one
predicate and the panels honour it.

## Consequences

**Easier:**

- The whole stack is testable headlessly: build a scene, execute commands, undo,
  and assert the scene returned to its prior state - no window, no ImGui.
- Adding an undoable operation means adding one `Command`, not touching the
  stack, the panels, or the scene.
- A new reflected property is undoable the moment it is reflected: the generic
  `SetPropertyCommand` needs no per-field code.
- Identity is preserved through delete/undo, so selection, the hierarchy, and
  any future node reference stay valid.

**Harder:**

- Every editing path must go through a command. A panel that mutates the scene
  directly would silently break undo, so this is a convention to hold to (and a
  reason the panels are already thin projections of the model).
- A command must be able to reconstruct exact prior state, which is why the
  engine gained a handful of precise, symmetric APIs (positional
  `add_child`/`add_root`, and `detach_component`/`insert_component`). A plain
  append or a "first of type" remove cannot express undo exactly.

## Alternatives considered

- **Snapshot / memento of the whole scene.** Simple to implement - serialize the
  scene before each edit - but memory grows with scene size per step, and it
  discards object identity on restore (a restored scene is a new tree of nodes).
  It also makes coalescing awkward (what is "the same edit"?). Rejected for the
  memory and identity costs; the command model pays only for what changed.
- **Per-property change records with no command object.** Lighter, but it
  cannot express structural edits (create/delete/reparent), which are exactly
  the operations that need undo most. A command interface subsumes the simple
  case.
- **A scene change log / observer inside `scene`.** Would put editor concerns
  (and undo policy) into the data model, violating ADR-0002's "scene is pure
  data" and adding a per-mutation cost to the runtime. Rejected.
- **Undo by replaying the inverse from a serialized diff.** Reintroduces
  serialization on the edit hot path and the identity problem above. Rejected.
- **No coalescing (one entry per change).** Trivial to implement and unusable:
  a single drag would fill the undo history. Rejected; coalescing is the
  feature, not a refinement.
