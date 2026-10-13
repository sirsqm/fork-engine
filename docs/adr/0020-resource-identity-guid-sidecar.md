# ADR-0020: Stable resource identity via a per-asset GUID sidecar

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0004](0004-resource-ownership-weak-cache.md), [ADR-0005](0005-reflection-and-serialization.md), [ADR-0006](0006-component-reflection.md)

## Context

A scene component that references an asset (`Sprite2D::texture`,
`MeshRenderer3D::mesh`) stores a **cache key**, which today is derived from the
asset's path: `Texture2DLoader::reference_key("assets/hero.png")` returns
`"tex:assets/hero.png"`, and that string is what is written to the scene file.
The reload path is `ResourceManager::load(key)`, which parses the scheme, finds a
loader, and reads the file at the path embedded in the key.

That makes the **path the identity of the asset**. It is stable across a scene
reload (same path, same key) but it is *not* stable across a file move: rename
`hero.png` to `hero_idle.png`, or move it into a subdirectory, and every scene
reference still points at the old path. `ResourceManager::load` then fails, the
serializer logs "could not load resource", and the component loads with a null
reference. The user sees a scene that silently lost its textures and meshes, and
nothing tells them the asset merely moved.

The requirement is an identity that is (a) unique per asset, (b) persisted and
stable across process runs, and therefore (c) **not derived from the path** —
anything path-derived breaks under a move by construction. The engine already has
a suitable 128-bit id type, `fe::Uuid`, used for node identity (ADR-0002); the
identity for assets must reuse it rather than introduce a second id type.

Three questions have to be answered: where the id is stored, how a reference
resolves from the id back to a file, and how the on-disk scene format changes
without silently misreading the old one.

## Decision

**Every asset file gets a sidecar metadata file that carries a stable `Uuid`.
An `AssetDatabase` owns the sidecars, keeps an in-memory index from `Uuid` to
path, and is the only thing that knows how to turn a `guid:` reference into a
loadable path. Scene files store `guid:` references; the scene format version is
bumped to 2, and version-1 documents are migrated at load time.**

### Where the GUID lives: a sidecar, not the file and not a central manifest

The GUID is stored in a **sidecar** next to the asset:

```
assets/textures/hero.png          the asset (bytes untouched)
assets/textures/hero.png.meta     the metadata sidecar (JSON)
```

The sidecar is a small JSON document:

```json
{
  "format": "fork_asset",
  "format_version": 1,
  "guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
  "source": "hero.png",
  "scheme": "tex"
}
```

- **`guid`** is the stable identity. It is generated once, on first import, and
  never changes — not when the asset moves, not when it is re-imported, not when
  it is renamed.
- **`source`** is the file's own name (informational; the asset's location is
  whatever directory the sidecar is found in).
- **`scheme`** records the loader scheme the asset is imported under ("tex" for a
  GPU texture, "obj" for a mesh, "" for a CPU image). It is stored rather than
  derived because several loaders share an extension — a `.png` is both a CPU
  `Image` and a GPU `Texture2D` — so the scheme is a genuine import setting, and
  it is the seed of M3's "import settings per asset type" work.

Because the sidecar **travels with the file** (a move or rename moves both, or
the next scan regenerates the pair), the identity survives a move even if the
project's index cache is deleted. The index is a *cache* of the sidecars, not the
source of truth: if the index is missing or stale, the database rebuilds it by
scanning the asset directory for `*.meta` files. This is the property that makes
the design robust — a deleted cache is a performance event, never a data-loss
event.

### How a reference resolves

A resource reference in a scene is a string of the form `guid:<uuid>`, for
example `"guid:3f2504e0-4f89-41d3-9a0c-0305e82c3301"`. The **shape** of the
reference is unchanged: it is still a single string the serializer stores and the
`ResourceManager` resolves, and `ResourceLoader::reference_key` still returns a
string. What changes is the *value* of that string.

Resolution is a two-step lookup that the `ResourceManager` performs:

1. The database maps the `Uuid` to the asset's current **path**.
2. The manager reads the asset's import **scheme** from the sidecar (via the
   database) and loads `scheme:path` through the ordinary loader path.

So a scene never names a file; it names an identity, and the database — which
knows where the file currently lives — turns that identity into a path at load
time. Move the file (and its sidecar) and the identity still resolves.

### `ResourceLoader::reference_key` contract

`reference_key` keeps its shape and its meaning: given a file path it returns a
string the serializer can store and the `ResourceManager` can resolve. Today it
returns a path-derived key (`"tex:assets/hero.png"`); that remains the **fallback**
when no asset database is in play.

The GUID is attached one layer up, in the **serializer**, which is the only
consumer that has both the reference key and the `ResourceManager`. When a scene
is written with a manager whose asset database knows the referenced asset, the
serializer rewrites the path key to a `guid:` key through
`ResourceManager::resolve_reference`. When there is no database (a headless test,
a tool, a project with no asset root), the path key is written unchanged.

This is deliberate. It keeps `reference_key` a pure function of a path — no
resource or manager parameter, no call-site churn in the component
registrations — and it keeps Sprite2D and MeshRenderer3D unchanged: they store a
`shared_ptr<Resource>` exactly as before, and the persistence layer decides
whether that resolves to a path or an identity. The cost is that a GUID is only
produced on a save that has a manager; a save without one is a path-based save,
which is the pre-M3 behaviour and still loads correctly.

The manager's `load()` grows one case (resolve `guid:` through the database);
`resolve_reference()` is new. Both are noted in the commit message.

### Format version 2 and migration

`fe::serialization::scene::kFormatVersion` is bumped from 1 to 2. The reader
handles three cases explicitly:

- **version > 2** — rejected with an error ("newer than supported"), as before.
- **version == 1** — the document stores path-derived keys (`tex:assets/hero.png`).
  The reader **migrates** them: each reference is looked up in the asset database
  by path and rewritten to the asset's `guid:` key before being resolved. A
  version-1 reference whose file has no asset entry (no sidecar) is reported as
  an error rather than loaded with a silently broken reference, because a path
  that resolves to nothing is exactly the silent failure this ADR removes.
  Migration is logged, so a user can see which references changed.

  Note that migration happens in `from_json`, which never touches the filesystem.
  Adopting a loose asset into the database (writing its sidecar) is an explicit
  import step, `AssetDatabase::import`, not a side effect of loading a scene.
- **version == 2** — references are `guid:` keys and are resolved directly.

A v1 document with **no** database available cannot be migrated (there is nothing
to resolve a path against), and the reader reports that clearly rather than
loading a scene with silently broken references.

## Consequences

**Easier:**

- References survive a file move or rename: the identity is the GUID, and the
  GUID travels in the sidecar.
- The sidecar is a natural home for the next milestone's per-asset import
  settings, so M3's "import settings per asset type" builds on this rather than
  adding a second mechanism.
- Deleting the index cache is harmless; it is rebuilt by scanning.
- Path-based references still work when no database exists, so tests and tools
  are unaffected.

**Harder:**

- An asset is now two files, and they must be kept together. A move that takes
  the asset but not its sidecar orphans the GUID; the next scan notices the
  missing sidecar and re-adopts the file with a **new** GUID, which breaks
  references to it. This is the real trade-off of a sidecar over a central
  manifest, and it is why the database reports re-adoptions.
- The database must scan the asset tree to rebuild, so a very large project pays
  a directory walk on first use. The index cache bounds this to once.
- The scene reader grows a migration branch that must be tested and eventually
  retired.

## Alternatives considered

- **Store the GUID in the file itself** (a header on the PNG/OBJ). Rejected: it
  rewrites user asset bytes, which breaks asset hashing, re-export, and
  round-tripping through external tools, and it cannot represent assets whose
  formats have no safe place to hide a header (or are not files at all).
- **A single central manifest** (`assets/.assets.json` mapping path → GUID).
  Rejected as the source of truth: it makes one file a merge-conflict magnet in
  version control, and a manifest that is lost or edited takes every identity
  with it. A per-asset sidecar fails small and locally. (The index cache *is* a
  central file, but it is derived and disposable, which is the important
  difference.)
- **Content hashing** (the GUID is a hash of the bytes). Rejected: the identity
  then changes on every edit, so re-importing an asset would break every
  reference to it — the opposite of what is wanted. Content addressing is useful
  for a deduplicating cache, which is a different problem.
- **A path-independent integer handle assigned at load time.** Rejected: a
  runtime-assigned handle is not stable across sessions, so it cannot be what a
  saved scene stores.
- **Keep path identity and add move-detection heuristics.** Rejected: heuristics
  (match by filename, by content, by size) are guesses that fail exactly when the
  user has two similar assets, and they leave the file format unable to express
  "this is the same asset, now at a new path".
