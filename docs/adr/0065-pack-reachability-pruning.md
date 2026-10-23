# ADR-0065: Pack reachability pruning

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0020](0020-resource-identity-guid-sidecar.md),
  [ADR-0025](0025-project-file.md), [ADR-0030](0030-asset-pack-format.md),
  [ADR-0044](0044-scripting-seam.md), [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0063](0063-reflection-backed-script-properties.md),
  [ADR-0064](0064-export-runtime-and-player.md)

## Context

[ADR-0030](0030-asset-pack-format.md) shipped a `PackWriter` that walks the whole
asset root and packs every file it finds. [ADR-0064](0064-export-runtime-and-player.md)
then made a pack runnable, and named reachability pruning as the deliberate gap it
left: "packing only what the startup scene references is a separate roadmap item".

Packing everything is correct but not shipping-shaped. A project with a hundred
levels and a shared art tree exports one pack containing all of it, so the build a
player downloads carries every level they will not open and every texture no
reachable scene draws. The pack format already keys entries by resource path and
carries each asset's stable id ([ADR-0030](0030-asset-pack-format.md)); what is
missing is a decision about *which* entries to write.

Two forces constrain the answer:

- **The `pack` module is low-level.** It depends on `core` and `resources` only
  (see `engine/CMakeLists.txt`). It must not learn to parse a scene, because a
  scene is `serialization`'s business and pulling that in would invert the module
  layering the whole engine is built on.
- **A reference is not one shape.** A component's resource reference is a
  reflected `ResourceRef` property ([ADR-0052](0052-render-extraction-seam.md) is
  the same "read the description, not the type" idea); a scene writes it as a
  stable `guid:<uuid>` or a loader key (`tex:assets/hero.png`). A script's
  property values ([ADR-0063](0063-reflection-backed-script-properties.md)) are
  written from whatever the behaviour exposes, and the behaviour's property kinds
  are known only once its module is loaded — which a packer does not do.

## Decision

**Trace the startup scene's resource references through reflection, resolve them
against the asset database, and hand the resulting paths to `PackWriter`, which
packs only those plus the scene. The trace lives in `serialization`, the filter
lives in `pack`, and the two meet at a plain list of paths.**

### The tracer

A new `serialization::scene::collect_resource_references(scene_json)` walks the
scene document and returns the references it names, as written:

1. For each component, it looks the type up in the reflection registry and, for
   every `ResourceRef` property (descending into groups), reads the reference
   string the document holds. A new resource property is traced the moment it is
   registered — the same property that makes it serializable makes it traceable,
   so the two cannot drift apart.
2. For each script's `"properties"` object, it collects every string. A
   behaviour's property kinds are unknown until its module is loaded, so a script
   string is a *candidate* rather than a certainty. The caller resolves candidates
   against its asset database and keeps the ones that name a real file, so a
   false candidate (a plain label like `"hero"`) costs nothing and a script
   resource reference is never missed.

It reads no files and needs no `ResourceManager`: it returns reference strings, and
the caller resolves them. That is what keeps it a pure function of the document,
and what lets a test drive it with a JSON literal.

### The resolver

`serialization::scene::reference_to_asset_path(reference, assets)` maps one
reference to a root-relative asset path: a `guid:<uuid>` is looked up for its
current path, and a `<scheme>:<path>` or bare path has its scheme stripped. The
scheme is informational here because the pack keys an asset by its path, which is
what a loader asks for once mounted ([ADR-0030](0030-asset-pack-format.md)).

### The filter

`PackWriter` gains a `PackOptions { bool prune; std::vector<std::string> include; }`.
The writer's own default (`prune == false`) packs the whole tree, exactly as
before, so every existing caller is unaffected; the tools choose pruning. When pruning, only the listed paths are packed;
the startup scene is always packed because it is the entry point. The writer still
does not parse a scene: it receives a list of paths.

### The caller

The tools (`fork_pack`, `fork_export`) prune by default and take `--no-prune` to
pack the whole tree. Each reads the startup scene as text, traces it, resolves
each reference, and passes the paths to the writer. A reference that resolves to
neither an indexed asset nor a file present under the pack root is a hard error:
shipping a build whose scene names an asset that is not in it produces a game
that runs until it draws, which is worse than a failed pack. An unimported asset (a file with no sidecar) still resolves by path,
because the unpruned writer packs it too.

## Consequences

- A build carries the startup scene and what it draws, not the whole project. The
  demo project's pack drops from four entries to two.
- The trace needs no per-component code: adding a resource property to any
  registered type extends pruning automatically, for the same reason it extends
  the serializer.
- A script that references an asset only through a property whose module is not
  loaded at pack time is covered by the string-candidate rule, at the cost of
  resolving every script string against the database. That is cheap (a hash
  lookup each) and safe (a miss is discarded).
- Pruning is the tools' default; `--no-prune` keeps the old, whole-tree
  behaviour. A game that loads an asset only from runtime code the packer cannot
  see opts out at its own risk, or names the extra path.
- **Not done:** transitive references. A packed asset that itself names another
  asset (a glTF that references an external `.bin` or image) is not followed, so
  pruning can drop a file such a loader would ask for. The default (no pruning)
  is unaffected, and following references through a loader is a later refinement.

## Alternatives considered

- **Teach `PackWriter` to parse scenes.** Rejected: it would make `pack` depend on
  `serialization` and invert the module layering. The writer gathering bytes and
  the caller naming what to gather is the same split `fork_pack` already makes for
  the project file ([ADR-0025](0025-project-file.md)).
- **Prune by convention — pack only files under the scene's directory.** Rejected:
  a project's art is rarely laid out one-directory-per-scene, and a shared texture
  referenced by two scenes would be missed by both.
- **Require every asset to be imported (have a sidecar) before it can be pruned.**
  Rejected: the unpruned writer packs loose files by path, and pruning should not
  silently narrow what a pack can contain. A reference resolves by index *or* by a
  file present under the root.
- **Make pruning the default.** Rejected for now: it changes what an existing
  build ships, and the transitive-reference gap above means it is not yet a safe
  default. It becomes one once a loader's own references are followed.
