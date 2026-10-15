# ADR-0058: 3D physics in the editor, and debug drawing

- Status: Accepted
- Date: 2026-10-15
- Deciders: sqm

## Context

M5's last two open rows are "Integration with the scene and the editor" and
"Debug drawing".

Play-in-editor (ADR-0001, `PlaySession`) already runs real gameplay: it copies
the authored scene through a component registry and runs a schedule over the
copy. That schedule registers `SpriteAnimationSystem` and `Physics2DSystem`, and
the editor registers copy factories for `Collider2D` and `RigidBody2D`. The 3D
physics module (ADR-0031, extended with angular dynamics by ADR-0043) has the
same shape as the 2D one - a `Physics3DSystem` over `Collider3D` and
`RigidBody3D` - but nothing schedules it and nothing copies its components. A
3D scene therefore plays without physics, which is the integration gap.

Debug drawing is the visibility gap. There is no way to see where a collider
actually is: a box collider on a node and the mesh drawn for that node can
diverge (a rotated body is collided as its unrotated box, ADR-0031), and the
only way to observe that today is to read numbers in a test. Every engine
eventually needs an overlay that draws the simulation's own shapes.

The RHI is the obstacle. `Device` can draw indexed triangles
(`draw_indexed`), and it has a `position_color_uv` sprite layout and a
`position_normal_uv` mesh layout, but no way to assemble indices as lines and no
vertex format carrying a per-vertex colour. A debug line pass needs both.

## Decision

### 1. The play schedule runs 3D physics too

`PlaySession::register_default_systems` registers `Physics3DSystem` alongside
`Physics2DSystem`. It is registered after the 2D system; the two read disjoint
component sets (a node carries a `Collider2D` or a `Collider3D`, not both), so
the order between them carries no meaning - it is fixed only so a frame's system
list is deterministic.

`EditorContext::register_default_component_factories` registers copy factories
for `Collider3D` and `RigidBody3D`, mirroring the 2D pair, so the play copy
simulates authored 3D bodies and the authored scene is still never touched.

### 2. A `debugdraw` module

A new module, `fe/debugdraw/`, with two halves:

- `DebugDrawList` - a CPU-side list of world-space `Line`s (`a`, `b`, `color`)
  with helpers (`add_box`, `add_circle`, `add_sphere`, `add_cross`). Pure and
  headless: a test asserts the exact geometry a wireframe produces.
- `DebugRenderer` - one shader and one vertex buffer, reused across frames.
  `render` uploads the list and issues a single line-list draw. It is the GPU
  half and runs against the null device headlessly.

`append_collider_wireframes(scene, out)` walks a scene and appends a wireframe
for every 3D collider: a box for a Box shape, a wire sphere for a Sphere. A
collider on a node with a movable `RigidBody3D` is drawn in the dynamic colour
and a static one in the static colour, which is the whole point of the overlay.
`append_collider_wireframes_2d` is the 2D counterpart on the z = 0 plane.

The module depends on `rhi`, `scene`, `physics2d`, and `physics3d`. It cannot
live in `systems`: physics depends on `systems`, so a system that named a
`Collider3D` would create the cycle `systems -> physics3d -> systems`. A separate
module that depends on both is the only placement that respects the dependency
graph.

### 3. The RHI gains a primitive topology and a line vertex format

`rhi::PrimitiveTopology { Triangles, Lines }` and
`Device::set_primitive_topology` set how the next indexed draw assembles its
indices. `Triangles` is the default and every existing pass keeps it, so no
existing draw changes. `VertexLayout::position_color` (vec3 position, vec4
colour) is the line vertex format.

Topology is device state rather than a parameter on `draw_indexed`, so the
existing `draw_indexed(index_count)` signature is unchanged and no call site
outside the debug renderer is touched. A line list has no index buffer of its
own; the debug renderer supplies a trivial `0..n-1` index buffer so the single
indexed draw serves it without a second draw entry point.

## Consequences

- A 3D scene plays with physics in the editor through the same code path a
  shipped game uses, and the authored scene is still isolated from play.
- Collider wireframes can be drawn over a 3D or a 2D scene from one renderer,
  and the list-building half is unit-tested without a GPU.
- The RHI is one small step more complete: a line primitive and a coloured
  vertex format, both inert for existing passes. A later debug pass (contact
  normals, joint anchors, query rays) reuses the same list and renderer.
- The `debugdraw` module is a new node in the dependency graph, depending on both
  physics modules. That is intentional and is the reason it is not in `systems`.

## Alternatives considered

- **Put the wireframe builder in `systems`.** Rejected: `physics3d` depends on
  `systems`, so a system naming `Collider3D` closes a cycle. The module split is
  forced by the existing edges, not chosen for taste.
- **Add a `draw_lines(first, count)` entry point instead of a topology knob.**
  Rejected: it duplicates the bind/attribute state machine the indexed draw
  already owns and adds a second path to keep in step. A topology is smaller and
  composes with the existing draw.
- **Draw debug lines with the 2D renderer.** Rejected: the 2D renderer works in
  screen space with a sprite batch and has no world camera for a 3D scene; debug
  lines are world-space and need the 3D camera's view-projection.
- **Only integrate 3D physics and defer debug drawing.** Rejected: the two rows
  are one story ("see the simulation the editor now runs"), and the RHI addition
  is small and inert for everything else.
