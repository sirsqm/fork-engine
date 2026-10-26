# 0088 — Following an asset's own references when packing

Date: 2026-10-25

Status: Accepted

Related: [ADR-0030](0030-asset-pack-format.md),
[ADR-0040](0040-gltf-importer.md), [ADR-0065](0065-pack-reachability-pruning.md),
[ADR-0080](0080-required-and-candidate-references.md)

## Context

[ADR-0065](0065-pack-reachability-pruning.md) makes a pruned pack ship the
startup scene and the assets it names, and names its own gap as a "Not done"
item: transitive references. "A packed asset that itself names another asset (a
glTF that references an external `.bin` or image) is not followed, so pruning can
drop a file such a loader would ask for."

That gap is now a real shipping bug, not a hypothetical. A 3D game that draws a
`.gltf` is exactly the shape the web shipping path targets
([ADR-0076](0076-3d-in-the-browser.md)), and a `.gltf` is a *container*: the
scene names the `.gltf` and nothing else, while the `.gltf` names its vertex
buffer (`.bin`) and its image files. Pruning kept the `.gltf` and dropped the
rest. The mesh loaded, then failed to decode because its buffer was absent, and
its material's maps never resolved — a wrong render or a silently missing asset,
not a failed pack, which is the worst way to ship something broken.

Measured before the fix, on a scene that draws one `.gltf` with an external
buffer and one external texture:

```
fork_pack: pruning to 1 scene-referenced asset(s)
entries: ['assets/meshes/cube.gltf', 'scenes/main.fescene']
```

The `.bin` and the image are gone. The unpruned path (`--no-prune`) is unaffected,
because it packs everything; the bug only bites the shipping-shaped path.

Two constraints decide where the fix lives:

- **`pack` is low-level.** It depends on `core` and `resources` only
  ([ADR-0065](0065-pack-reachability-pruning.md)). It must not learn to parse a
  glTF, for the same reason it must not learn to parse a scene: a glTF is `mesh`'s
  business, and `pack` may not depend on `mesh` (`mesh` sits above `resources`
  and would invert the layering).
- **A format's references are the format's business.** That a glTF's data lives
  in an external `.bin` and its maps in image files is knowledge the loader
  already has; the packer should not re-derive it.

## Decision

**The packer expands a set of paths with the files those assets name internally,
through a reader the caller supplies. The expansion algorithm lives in `pack`;
the format knowledge that answers it lives in `mesh`.**

### The reader seam

`pack::AssetDependencyReader` is a
`std::function<std::vector<std::string>(std::string_view relative_path,
std::string_view bytes)>`: given an asset's path relative to the pack root and its
bytes, it returns the files that asset names, also relative to the pack root, or
an empty list for an asset it does not understand. It is injected by the caller,
so `pack` never names a format. This is the same split
[ADR-0065](0065-pack-reachability-pruning.md) already uses for the scene: the
writer gathers bytes, the caller says what to gather.

### The expansion

`pack::expand_asset_dependencies(paths, pack_root, reader)` walks `paths`, reads
each file under `pack_root`, asks `reader` what it names, and appends whatever is
new, repeatedly until the set stops growing. A worklist rather than a single
pass, because a dependency can itself be a container. A path the packer cannot
read contributes no further dependencies. A dependency the tree does not contain
is still listed, and `PackWriter` — which packs only files that exist under the
root — ignores it, so a missing dependency degrades to today's behaviour rather
than failing the pack.

### The glTF reader

`GltfLoader::external_dependencies(bytes)` returns the external URIs a glTF
document names: its `buffers[].uri` and `images[].uri`, minus data URIs (decoded
from the document) and minus the GLB binary chunk (in the same file), neither of
which is a file to ship. It returns an empty list for bytes that are not a glTF,
so the caller can ask about every asset. `GltfLoader::dependency_reader()`
resolves those URIs against the glTF's own directory — the same resolution the
loader uses at load time — and is the `AssetDependencyReader` the tools hand to
the packer.

### The callers

`fork_pack` and `fork_export` expand their pruned include list with
`GltfLoader::dependency_reader()` before handing it to `PackWriter`. Both call
one shared step, so the two cannot drift, as they once did
([ADR-0080](0080-required-and-candidate-references.md)).

## Consequences

- A pruned pack of a scene that draws a `.gltf` ships the `.gltf`, its `.bin`,
  and its images. Measured after the fix, on the same scene as above:

  ```
  entries: ['assets/meshes/cube.bin', 'assets/meshes/cube.gltf',
            'assets/meshes/textures/checker.png', 'scenes/main.fescene']
  ```

- `pack` stays format-agnostic: it learns a generic algorithm and a callback
  type, and no glTF knowledge. A second container format (an OBJ with a `.mtl`,
  say) is a second reader, not a change to `pack`.
- The closure is only as complete as the readers supplied. A format with no
  reader is treated as self-contained, which is today's behaviour, so the fix is
  strictly additive: it can only add files a loader would ask for.
- Pruning is one step closer to being a safe default. ADR-0065 withheld it
  partly because a container's own references were not followed; that blocker is
  now gone. Flipping the default is still its own change, because it changes what
  an existing build ships, so it is not done here.

## Alternatives considered

- **Make `pack` depend on `mesh` and read glTF directly.** Rejected: it inverts
  the module layering (`mesh` depends on `resources`, and `pack` is below it),
  which is the same reason ADR-0065 refused to let `pack` parse a scene.
- **Put the expansion in `serialization` (where the scene trace lives).**
  Rejected: `serialization` does not depend on `mesh` either, so it would hit the
  same wall; and an asset's internal references are not a scene concern.
- **Hard-code the glTF rules in `pack`** (follow `buffers[].uri` and
  `images[].uri`). Rejected: it puts format knowledge in a format-agnostic
  module, and every new container would edit `pack`.
- **Let the reader read the file itself, given only a path.** Rejected: the
  packer already knows the root and must read the file anyway to pack it, so
  passing bytes keeps one read and one notion of where the root is; a reader that
  read the file would need the root too and could disagree about it.
- **Fail the pack when a dependency is missing.** Rejected: it makes an
  incomplete asset tree a hard error where today it packs, and the scene's own
  required references are already the place a hard error belongs
  ([ADR-0080](0080-required-and-candidate-references.md)).
