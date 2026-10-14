# ADR-0040: A glTF 2.0 importer for 3D content

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0004](0004-resource-ownership-weak-cache.md),
  [ADR-0010](0010-3d-rendering-foundation.md),
  [ADR-0027](0027-asset-import-settings.md),
  [ADR-0029](0029-import-cache.md),
  [ADR-0030](0030-asset-pack-format.md)

## Context

The 3D path can render, light, shadow, and cull a mesh, but the only way to get
real geometry into the engine is the Wavefront OBJ loader from the M1
foundation. OBJ carries positions, UVs, and normals and nothing else: no
hierarchy, no materials, no animation, no binary container. It was enough to
prove the renderer and is not enough for real content.

glTF 2.0 is the interchange format the rest of the industry settled on. It is a
JSON scene description (`.gltf`) with either external or embedded buffers, or a
single binary container (`.glb`) that packs the JSON and the buffers together.
It is versioned, openly specified, and what every DCC tool and asset store
exports. Adopting it is how the engine stops being a demo renderer.

The constraints are the ones the rest of the engine already holds to: the loader
reads its bytes through `ResourceSource` so a mounted pack works unchanged
(ADR-0030); it produces a `mesh::Mesh` and knows nothing about the GPU, exactly
as `ObjLoader` and `ImageLoader` do; it reports failure as an `Error` value and
never aborts (ADR-0003); and it adds no heavy dependency.

The question is therefore not *whether* glTF, but *how much* of it to implement
in the first cut, and with which JSON facility, so the loader is honest about
what it supports rather than silently dropping data.

## Decision

Add a `GltfLoader` to the existing `mesh` module. No new module, no change to
`Mesh`, `Vertex3D`, `Scene`, or any shared header.

### One mesh per primitive, and what is supported

glTF describes a scene of nodes, each pointing at a mesh; a mesh holds one or
more primitives; a primitive is a set of vertex attributes plus an index list.
The engine's `Mesh` is a single indexed triangle list with a fixed
position/normal/uv `Vertex3D` (ADR-0010). The loader therefore maps **one glTF
primitive to one `Mesh`** and exposes the primitives it found through a small
result type, rather than inventing a multi-primitive container the renderer
cannot draw.

The first cut supports:

- `.gltf` (JSON) and `.glb` (binary container).
- Buffers as external URIs, as data URIs (`data:application/octet-stream;base64,`),
  and as the GLB binary chunk.
- The `POSITION`, `NORMAL`, and `TEXCOORD_0` vertex attributes, interleaved or
  not, any of the component types the spec allows for these accessors.
- Indexed geometry (`u16`/`u32` indices) and non-indexed geometry (a sequential
  index list is synthesised).
- Triangle primitives (`mode` 4 and the default).

It explicitly does **not** support, in this cut, and says so in its errors and
in this record rather than dropping data quietly:

- Multiple primitives rendered as one object. Each primitive is a separate
  `Mesh`; a caller that wants them together composes them in the scene.
- Materials, textures, and the metallic-roughness model. The renderer's material
  is authored in the scene, not imported (the glTF material is ignored, as OBJ's
  `.mtl` already is).
- Skinning, morph targets, and animation. These belong with the animation
  milestone (M6); the loader ignores the `skins`, `animations`, and `targets`
  keys rather than failing, so an animated asset still imports its static pose.
- Sparse accessors and non-triangle primitive modes. A primitive that uses them
  is skipped, and the loader reports how many it skipped.

Ignoring an unsupported *key* is deliberate: an animated, materialed asset is
still a valid static mesh, and refusing it would make the loader useless against
real exports. Ignoring an unsupported *attribute a primitive needs* is not: if a
primitive has no `POSITION`, that primitive is a hard error.

### The JSON facility is the one already vendored

`nlohmann/json` is already a dependency of the engine (`FE_JSON_TARGET`,
pinned at v3.11.3) for scene serialization (ADR-0005). The glTF `.gltf` form is
JSON, so the loader parses it with `nlohmann::json` and does not add a second
JSON library. This is not a new dependency; it is the reuse the project's
"prefer proven libraries" rule asks for, and it keeps the license surface
unchanged (nlohmann/json is MIT).

The `.glb` form is read as bytes: the 12-byte header, then the JSON chunk and
the binary chunk, parsed and sliced by hand (the container is a four-field
header and two length-prefixed chunks).

### Accessor decoding is the only non-trivial part

glTF stores vertex data in buffers at a byte offset, described by an accessor
(type, component type, count) and a buffer view (buffer, byte offset, byte
stride). The loader decodes positions, normals, and UVs from those descriptions
with the same flat-normal and zero-UV fallbacks the OBJ loader already uses, so
an asset that omits normals or UVs still shades and still renders. Bounds come
from `Mesh` itself, which recomputes them on construction.

## Consequences

- Real 3D content (anything a DCC tool or an asset store exports) imports.
- A `.glb` and its external-buffer `.gltf` sibling produce the same `Mesh`, so a
  project can ship whichever form its pipeline produces.
- The loader works from disk and from a pack with no change, because it reads
  through `ResourceSource` like every other loader.
- The unsupported list is a contract, not a wish: materials, skinning, and
  animation are separate, later work, and the loader does not pretend otherwise.
- `Vertex3D` is unchanged, so the OBJ path, the 3D renderer, and the mesh tests
  are untouched. Adding skin weights later is a second vertex layout and a shader
  that consumes it, as ADR-0010 already anticipates.

## Alternatives considered

- **Vendor tinygltf or cgltf.** A complete, proven glTF parser, but it is a
  second JSON path beside nlohmann/json and a large surface to review. Rejected
  for the first cut; if the supported subset grows to need the full spec
  (skinning, sparse accessors, every extension), revisiting this is reasonable
  and would be a superseding ADR.
- **One `Mesh` for the whole glTF file.** Simpler for a single-primitive asset,
  but it forces the engine's single-index-list `Mesh` to represent something it
  cannot, and hides the node/primitive structure a scene needs. Rejected in
  favour of one mesh per primitive.
- **Extend `Vertex3D` now** with tangents or skin weights. Speculative: nothing
  consumes them yet, and the renderer's shader would ignore them. Deferred until
  a feature needs them, per the project's "no speculative frameworks" rule.
- **A separate `gltf` module.** The loader is geometry decoding and belongs with
  `Mesh` and `ObjLoader`; a module for one file format is over-structuring.
