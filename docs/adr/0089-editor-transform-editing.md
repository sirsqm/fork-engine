# 0089 — Editing a node's transform in the editor

Date: 2026-10-25

Status: Accepted

## Context

The editor can create, delete, rename, and reparent nodes, and it can add,
remove, and edit components through the reflection registry. It cannot move
anything. There is no Transform section in the inspector, and no `Transform`
type is registered in the reflection registry, so a node's position, rotation,
and scale are not editable at all; the only code that positions a node is
`EditorContext` doing it programmatically. The viewport can pick a node but not
drag one. The practical result is that an author can assemble a scene's parts
and never place them, which is the single largest gap between the editor and one
that can author a game.

The reason a transform was left out is structural, not an oversight. Every
existing edit travels as a `SetProperty` operation whose path is resolved
against a component's `TypeDesc`, and every such value is a `reflect::Value` — a
variant of bool, int, float, string, and `Uuid`. A `Transform` is not a
component, so it has no `TypeDesc` to resolve against, and a transform field is a
`Vec3`, which `reflect::Value` cannot carry. Registering a `Transform` component
would have been the wrong fix: a node owns exactly one transform, the scene
format stores it on the node rather than as a component, and a transform that
could be added twice, removed, or attached to nothing would break both the
hierarchy and the file format.

Two constraints shape the design. First, an edit must stay a serializable
operation: the editor routes every edit through the command stack as an
`Operation`, which is what makes it undoable and what the operation log replays,
so a transform edit has to travel the same way or it would be the one edit that
undo cannot see. Second, the drag must be testable headlessly, and a drag is a
continuous gesture — a naive one-operation-per-frame design would put a hundred
undo steps in the history for one mouse movement.

## Decision

A node's transform is edited through a new operation kind, `SetTransform`, that
addresses the node directly and carries the whole field as a `Vec3`:

* `path` holds one element, the field name: `"position"`, `"rotation"`, or
  `"scale"`.
* `transform_value` holds the new `Vec3`.

The field is addressed as a whole rather than per axis. A drag changes two axes
in a frame, so a per-axis operation would interleave two coalescing keys and
split one gesture into many undo steps; writing the whole field keeps the key —
node id plus field name — stable for the length of the gesture, so the drag
coalesces into exactly one step. `reflect::Value` is left alone; `SetTransform`
gets its own value field rather than widening the variant that every operation of
every kind would then pay for.

The mapping from field name to member is defined once, in
`fe::transform_field` (`engine/include/fe/scene/Transform.h`), and shared by the
operation applier and the inspector. An unknown field name makes `apply` a
no-op and `inverse` return an error, so a malformed or hand-written operation is
rejected rather than silently ignored, and a non-finite value is refused so an
operation cannot write a NaN into a transform.

`inverse` reads the field's previous value from the document, exactly as a
component property's inverse does, so undo restores the state the gesture
started from even after many coalesced frames.

On the toolkit side, the inspector draws a Transform section above the
components — three rows, one per field, each a row of X/Y/Z drags. Rotation is
stored in radians but shown and typed in degrees, which is what a person authors
in. The viewport adds drag-to-move: a left drag moves the selected node in the
view plane, holding the world position the drag started from for the whole
gesture so the node keeps the offset it was grabbed by and the gesture
accumulates no rounding. The screen-to-world mapping that the drag needs
(`drag_to_world_delta`, `dragged_position`) lives in `Viewport`, which is
headless, so it is unit-tested without a window.

## Consequences

A node can now be placed and rotated from the editor, through the inspector and
by dragging in the viewport, and every such edit is a serializable, undoable
operation like any other. This is the first operation kind that is not a
component or script edit, and it establishes the shape for the next one: a
non-component target gets its own kind rather than a synthetic component.

The editor's viewport is still hardcoded 2D (`Camera2D` and `Renderer2D`), so
drag-to-move moves a node in the XY plane and a 3D authoring view is not yet
possible; that is a separate piece of work. The 3D renderer and 3D physics
exist, but the editor cannot show them.

Only the editor's own drag is a single coalesced step. A script or another
client that writes a transform one axis at a time still produces one operation
per write; that is the same behaviour every other property has, and the
coalescing key makes the common case — the editor's own gesture — correct.
