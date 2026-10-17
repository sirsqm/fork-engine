# ADR-0062: Tilemaps

- Status: Accepted
- Date: 2026-10-17
- Deciders: sqm

## Context

M6 lists "Tilemaps" as an open row, the last unchecked 2D item in the
milestone. The engine can already draw a textured quad (`Sprite2D`, ADR-0008) and
animate one (`SpriteAnimator` + `SpriteAnimationSystem`), and the 2D renderer
consumes a flat array of `render::SpriteInstance` produced by the extraction
seam (ADR-0052). What is missing is the *level*: a 2D game's world is a grid of
tiles, not a tree of sprite nodes.

Building a level out of individual `Sprite2D` nodes works, and it is the obvious
thing to try first, but it is the wrong shape for three reasons:

- **Authoring.** An artist edits a level as a grid — paint a cell, fill a
  rectangle, erase one. A tree of nodes makes that a script. The grid is the
  natural unit of editing, so it should be the natural unit of storage.
- **Scale.** A modest 100x100 level is ten thousand nodes. That is ten thousand
  `Node`s, ten thousand `Component`s, ten thousand world-matrix recomputations,
  and ten thousand entries in the serializer, to say nothing of what the editor
  would show. A grid of `i32` is 40 KB and one component.
- **Batching.** The renderer already sorts by `(layer, texture)` and merges runs
  of equal texture into one draw call (ADR-0052). Ten thousand nodes that all
  reference the same tileset *should* be one draw call, but only if the seam can
  see them as one homogeneous array.

The forces at play:

- **A tile is a textured quad.** The draw path already exists. A tilemap must
  reuse it, not grow a parallel path that then has to be taught every feature
  the sprite path gains.
- **Reflection is a closed, scalar model.** `reflect::Value` (ADR-0005,
  ADR-0006) is a fixed set of scalar leaves plus fixed-size groups. It has no
  "array of ints" kind, and a grid is exactly a variable-length list.
- **The module graph is acyclic and reviewed.** A new module edge is an
  architectural change. A tilemap needs `scene` (components) and `resources`
  (the tileset), both of which are below it, and it is drawn by `renderer2d`,
  which is also below it — so no new edge is required.

## Decision

Add a `Tilemap` component to the `scene` module (`fe::Tilemap`,
`fe::reflect::register_tilemap_types`) and teach the extraction seam to emit
tiles as sprite instances.

**A tile is a `SpriteInstance`.** `render::extract_tiles` walks the scene, and
for every non-empty cell of every `Tilemap` appends one `SpriteInstance` whose
`world` is the map node's world matrix composed with the cell's local offset,
and whose `uv` is the tile's cell in the tileset. The 2D renderer's `extract`
calls `extract_sprites` then `extract_tiles`, filling *one* array, so the layer
sort orders a map and the sprites over it together and the batcher merges them
by texture. Nothing in the draw path knows whether an instance came from a
sprite or a tile.

To make that true, `SpriteInstance` no longer points at a `Sprite2D` for the
values a draw reads. It carries them resolved: `uv`, `color`, `layer`, and
`size`, alongside the existing `texture` and `world`. The `sprite` pointer stays
for provenance (tests and debugging), and a parallel `tilemap` pointer records
the other origin; exactly one of the two is non-null. This is a strict
improvement even without tilemaps: the draw step no longer dereferences a
component from the scene, so the seam is more nearly pure over its array.

**Layout is the convention every tile editor uses.** Cell (0, 0) is the
top-left of the map, +X goes right and +Y goes down. In the node's local space
the map spans x in `[0, width * tile_size.x]` and y in `[-height * tile_size.y,
0]`, so the node's origin is the map's top-left corner and moving, rotating, or
scaling the node transforms the whole map. The tileset texture is a `columns` x
`rows` grid of equal tiles read left-to-right and top-to-bottom, the same
convention `SpriteAnimator` uses for a sprite sheet; a cell's value is a tile
index into it. `Tilemap::kEmpty` (-1) draws nothing, which is what makes a
sparse map cheap to author — and being signed, "empty" is distinct from tile
index 0.

**The grid is a String property holding the component's own JSON.** The
reflection model has no array kind, and rather than widen it for one component,
`Tilemap` exposes a single `grid` property of `ValueKind::String` whose getter
is `to_json()` and whose setter is `from_json()`, exactly as
`AnimationStateMachine` exposes its graph (ADR-0059). The scene serializer
round-trips the string like any other; the component owns the grid's format.
`from_json` rejects a document whose cell count disagrees with its dimensions,
so a hand-edited scene keeps a valid grid rather than loading a half-built one.

**Degenerate input draws nothing, and reads are defined.** A map with no
texture, an empty grid, or a zero tile size contributes no instances. An
out-of-range tile index resolves to the full texture rather than a degenerate
cell, matching `SpriteAnimator::uv`. `at()` out of bounds is `kEmpty` and
`set()` out of bounds is ignored, because a tool walking a rectangle past the
edge is an authoring query, not a bug.

## Consequences

- **A level is one component, and the batcher sees it.** A 100x100 map is one
  `Tilemap` and one draw call per texture per layer, not ten thousand nodes.
- **Both renderers stay 2D-shaped.** Tiles go through the sprite path, so the
  null-device tests, the cull, and the counters (`last_draw_calls`,
  `last_culled`) all cover tiles with no new machinery. The cull is the same
  quad test a sprite uses.
- **The seam got purer, at a small cost.** `SpriteInstance` is larger and its
  fields are duplicated from the component, but the draw step no longer reaches
  into the scene, which is what ADR-0052 wanted. The duplication is resolved
  once, at extraction.
- **The grid's format is the component's business.** The serializer cannot
  validate or diff a grid; it moves an opaque string. The component's own
  `from_json` is the only validator. A dedicated tile editor — a grid view, a
  tileset picker, a paint tool — is a follow-up and is what would motivate a real
  array kind in the reflection model.
- **Tile collisions are not here.** The grid is draw data. Giving it collision
  means generating colliders or a query over the cells, which belongs with the
  physics module and a follow-up cut.
- **Only uniform tile sizes.** Every tile is `tile_size`; mixed tile sizes, tile
  flags (flip, rotate), and multiple layers per map are deliberately out of this
  cut. The `layer` field orders a map against sprites, which is the ordering the
  renderer already understands.

## Alternatives considered

- **One node per tile.** Rejected: it turns authoring, storage, and the node
  graph into the level's data structure. Ten thousand nodes for a level is the
  symptom; the cause is that the tree is not a grid.
- **A separate tile draw path in `renderer2d`.** Rejected: a tile is a quad, and
  a second path would have to be taught every feature the sprite path gains
  (layers, tint, atlas UVs, culling, batching). Emitting sprite instances keeps
  one path.
- **Widening `reflect::Value` with an array kind.** Rejected for this cut: it is
  a change to the reflection model for a single consumer, and the model is
  deliberately closed. The String-of-JSON trade is the established precedent
  (ADR-0059), and a real array kind should be justified by the inspector's needs,
  not one component's.
- **A `Tilemap` in a new module above `scene`.** Rejected: it needs no module the
  scene module does not already depend on, and the renderer that draws it is
  below it. A new module would be an architectural edge for no boundary.
- **Storing the grid as a `vector<ResourceRef>` or one property per cell.**
  Rejected: it explodes the property list, and the reflection model has no
  per-index property. The grid is one value.
