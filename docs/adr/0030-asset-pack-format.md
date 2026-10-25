# ADR-0030: Asset packs — one container the runtime mounts like a directory

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0003](0003-error-handling.md), [ADR-0004](0004-resource-ownership-weak-cache.md),
  [ADR-0005](0005-reflection-and-serialization.md), [ADR-0018](0018-multi-platform-shipping.md),
  [ADR-0020](0020-resource-identity-guid-sidecar.md), [ADR-0025](0025-project-file.md),
  [ADR-0027](0027-asset-import-settings.md)

## Context

[ADR-0018](0018-multi-platform-shipping.md) decided that a game built here must
reach every platform its players use, and that assets therefore ship in a **pack**
rather than as a folder of loose files: a browser and a mobile app do not hand a
game an ordinary filesystem, and a distributed build that reads loose files
invites missing-file bugs. It named three pieces — a pack format, a runtime that
mounts it through the same resource paths it uses on disk, and per-target export
templates — and left the format and the mount for M8.

The pieces the pack has to fit are already in place:

- A **project** is a directory with `fork.project.json` naming an asset root and a
  startup scene ([ADR-0025](0025-project-file.md)).
- An **asset** has a stable GUID in a per-asset sidecar and typed import settings
  ([ADR-0020](0020-resource-identity-guid-sidecar.md),
  [ADR-0027](0027-asset-import-settings.md)); `AssetDatabase` is the source of
  truth and rebuilds its index by rescanning.
- A **reference** is either `guid:<uuid>` or `<scheme>:<path>`, and
  `ResourceManager` resolves both to a `Resource` through a loader
  ([ADR-0004](0004-resource-ownership-weak-cache.md)).
- A **scene** is versioned UTF-8 JSON read and written by
  `serialization::scene` ([ADR-0005](0005-reflection-and-serialization.md)).
- Failures are **values**, never exceptions ([ADR-0003](0003-error-handling.md)).

What is missing is a container format, a way to *produce* one from a project, and
a way for the runtime to *mount* one so the loaders read from it without knowing
it exists. This record covers that first slice only; export templates and the
per-OS runtimes are a later stage and are called out as not done.

## Decision

**Add an `assets`-side `pack` module holding (a) a pure, versioned pack container
value type, (b) a writer that turns a project's startup scene and indexed assets
into one pack file, and (c) a `PackSource` the `ResourceManager` mounts so the
loaders and the scene reader transparently read from the pack instead of disk.**

### Purpose and responsibilities

The module does one thing: move a project's bytes into one file and let the
engine read them back through the paths it already uses.

- **`PackFile`** is the container as a value. It knows the on-disk layout (a
  versioned header, a table of contents, a payload), serialises itself, and
  parses itself, with no filesystem and no graphics device. It is the pack
  analogue of `ImportSettings` ([ADR-0027](0027-asset-import-settings.md)) and of
  the scene format ([ADR-0005](0005-reflection-and-serialization.md)): a pure
  value type that is exhaustively testable on its own.
- **`PackWriter`** gathers the bytes. Given a pack root, a resolved startup
  scene, and the key the scene should live under, it packs every file under the
  pack root — carrying each indexed asset's scheme and stable id, and packing an
  unindexed file under the default scheme with no id — plus the startup scene. It
  reads files; it does not parse the project file, and it does not know what a
  project is. The `fork_pack` tool passes the **project directory** as the pack
  root, because a scene's asset references are project-relative
  (`assets/textures/checker.png`); keying the pack the same way is what makes
  them resolve in a shipped run. The explicit form passes the asset root instead.
- **`PackSource`** is the runtime mount. It parses a pack once and answers byte
  reads by path, and a GUID lookup by returning the asset's path and scheme. It
  is a `ResourceSource` — the byte-source seam on `ResourceManager` — so mounting
  one changes where the loaders read from and nothing else.

### Dependencies and public interfaces

`pack` depends on `core` and `resources` and sits beside `physics2d` and `audio`
in the graph; it adds no edge to a renderer and nothing above it changes shape.

```
core, resources  <-  pack
```

- `fe::pack::PackFile` — `add_file`, `write`, `parse`, `find_path`, `find_guid`,
  `entries`. A pure value.
- `fe::pack::PackWriter` — `build(assets, startup_scene_path, scene_key)` and
  `write(pack_path, assets, startup_scene_path, scene_key)`, both returning
  `Result`/`Error`.
- `fe::resources::ResourceSource` — the seam: `read_binary`, `read_text`,
  `exists`, and an optional `resolve_asset(guid)` for sources that own identity.
  `ResourceManager` implements it (delegating to the filesystem) and gains
  `mount(std::shared_ptr<ResourceSource>)`.
- `fe::pack::PackSource` — a `ResourceSource` over a parsed pack; `open(path)`.

The three byte-reading loaders (`ImageLoader`, `ObjLoader`, `WavLoader`) read
through the source their manager gives them instead of calling `fs::` directly.
`Texture2DLoader` already reads through the manager. `serialization::scene::load`
reads through the manager's source when a manager is supplied, so a mounted pack
serves the startup scene through the same call the loose-file path uses.

### The format

The pack is a little-endian binary container, deliberately small:

```
header   magic 'FEPK', format_version u32, entry_count u32,
         index_offset u64, payload_offset u64
index    per entry: key length + key (UTF-8 path),
         scheme length + scheme, guid (16 raw bytes, nil when absent),
         payload offset u64, payload length u64
payload  raw file bytes, referenced by the index
```

Every offset and length is checked against the file size on parse. A wrong magic,
a version newer than the reader, a truncated index, or an out-of-range span is an
**error**, not a best-effort read: the parser never produces a pack whose entries
point outside the buffer. That is the same strictness as the scene and sidecar
readers ([ADR-0005](0005-reflection-and-serialization.md),
[ADR-0020](0020-resource-identity-guid-sidecar.md)).

A pack is keyed by the **path** a loader is already given
(`assets/hero.png`), because that is the string `ResourceManager` hands a loader
today; the GUID and scheme ride alongside on the entry so a pack is
self-describing and a `guid:` reference resolves without the loose sidecars. The
startup scene is an ordinary entry under its project-relative path.

### How it fits the architecture

- **The engine still does not know what a project is.** The writer takes resolved
  paths; only the `fork_pack` tool reads `fork.project.json`, exactly as only the
  editor does today ([ADR-0025](0025-project-file.md)). This keeps the promise
  that the engine is reusable and project-agnostic.
- **The loaders still do not know what a pack is.** They read bytes from a
  `ResourceSource`; the filesystem is one implementation and the pack is another.
  This is the "second storage backend behind one interface" ADR-0018 asked for.
- **Identity is unchanged.** A GUID is still the stable reference; the pack just
  carries the mapping so it survives without the sidecars.

### How it is tested

The pure format is tested with no filesystem: a built pack parses back to the
same entries and payloads; a wrong magic, a future version, a truncated buffer,
and an out-of-range offset each return an `Error`. The mount is tested
end-to-end without mocks: build a real project tree, pack it, **delete the loose
files**, mount the pack on a `ResourceManager`, and load the startup scene and a
real texture through the ordinary `serialization` and loader paths, asserting the
pixels came from the pack. The writer is tested by packing a temp tree and
checking the entry count and the scene bytes. The `fork_pack` tool is exercised
by the sandbox's headless run.

## Consequences

**Easier:**

- A project can be assembled into one file and run from it, on a machine with no
  loose assets, through the same resource paths as before.
- The runtime no longer assumes a real filesystem for assets *or* scenes, which
  is the precondition for the web and mobile targets.
- The format is a pure value, so it is cheap to test and cheap to extend (a
  compression flag or a second index level is a version bump, not a rewrite).

**Harder / accepted costs:**

- **A second versioned format to maintain.** It is small and shares the scene
  format's shape, but it is real, and a pack is a build artifact that must be
  regenerated when its inputs change.
- **The byte-source seam touches the resource system.** `ResourceManager` gains
  a source, the base `ResourceLoader` gains a byte-read helper, and three loaders
  switch from `fs::` to that helper. The alternative — threading a pack pointer
  into every loader — is larger and leaks the pack upward; a source on the
  manager is the smaller, contained change.
- **The writer over-includes.** It packs every file under the asset root, not the
  transitive closure of the startup scene's references. Precise reachability is
  deferred; an over-full pack is correct and simple, and the asset set of a game
  is small enough that the wasted bytes do not matter yet. Sidecars (`.meta`) are
  the one exclusion: their scheme and id already ride on the entry.
- **Unindexed files are packed but not identity-resolvable.** A file that was
  never imported has no sidecar and no id, so it goes in under the default scheme
  with no GUID: a `guid:` reference to it cannot resolve, though a path reference
  to it works from the pack. Importing the asset is already what gives it a
  stable reference ([ADR-0020](0020-resource-identity-guid-sidecar.md)).
- **No compression, no streaming, no export templates.** The payload is raw and
  the whole pack is read into memory. Those are later stages.

## Alternatives considered

- **Ship loose files with a manifest.** Rejected in ADR-0018 and again here: it
  is not viable where there is no filesystem and it invites missing-file bugs.
- **A zip or tar container.** Rejected for now: a general archive brings a
  compression dependency, a larger parser, and a format whose failure modes are
  not shaped to our needs. A small, explicit container is easier to validate
  strictly and to extend deliberately; a compressed pack can be added later as a
  version.
- **Make the pack a `ResourceLoader` (a "pack:" scheme).** Rejected: a pack is
  not one asset, it is a *source of many*. Modelling it as a loader would mean
  the manager still resolves each inner path through the filesystem first, and
  the scene reader would still read loose. A `ResourceSource` mounted on the
  manager is the seam that makes "read from the pack" true for everything the
  manager loads.
- **Materialise the pack to a temp directory and read normally.** Rejected: it
  needs a writable filesystem, which is exactly what a browser does not have, and
  it doubles the I/O.
- **Put the pack concept in `core` or in the editor.** Rejected: the format
  belongs with the resource system it serves, and the editor must not be required
  to produce a shipping artifact. A `pack` module keeps the edge pointing down.
- **Promote `ProjectSettings` into the engine and have the writer read the
  project file.** Deferred, not rejected: it would remove the small project
  reader the `fork_pack` tool carries today, but it widens the engine's job
  before there is a second consumer. The tool reading the project file is the
  smaller first step and keeps ADR-0025's boundary intact.
