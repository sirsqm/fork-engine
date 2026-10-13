# ADR-0029: A content-addressed import cache

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0003](0003-error-handling.md),
  [ADR-0004](0004-resource-ownership-weak-cache.md),
  [ADR-0020](0020-resource-identity-guid-sidecar.md),
  [ADR-0027](0027-asset-import-settings.md)

## Context

An asset is imported by decoding it: `ImageLoader` reads the source bytes and
runs them through stb_image; `Texture2DLoader` asks the manager for the decoded
`Image` and uploads it. The `ResourceManager`'s weak cache (ADR-0004) makes a
*second* load in the same process cheap, but it does nothing across processes.
Start the engine again, or build the game, and every image is decoded from
scratch, even though nothing about it changed. A settings change (ADR-0027)
re-imports a texture through the whole decode-and-upload path for a value that
affects only the upload.

Two things make this avoidable. Import is a **pure function** of its inputs: the
same source bytes, the same import settings, and the same importer always
produce the same artifact. And the artifact (a decoded RGBA8 image) is a small,
self-describing value. So the result of an import can be stored under a key
derived from its inputs and reused, turning a re-import into a cache read.

The design questions are what the key is, where the artifact lives, how the
store is invalidated and evicted, and how much of the pipeline to cache in the
first slice. The roadmap item is deliberately the one after import settings
(ADR-0027), because the settings are an input to the key: the cache is only
correct once the settings that change the output are known.

## Decision

**Add an `ImportCache`: a content-addressed store of imported artifacts, keyed
by a hash of `(source bytes, effective import settings, importer identity)`,
living in `resources` beside the `AssetDatabase`. The first artifact it caches
is the decoded `Image`. It is inert when no root is set, so headless tools and
tests are unaffected.**

### The key is a hash of the three inputs that determine the artifact

```
key = fnv1a64( "fork-import-v1" ‖ importer ‖ settings_bytes ‖ source_bytes )
```

- **`source_bytes`** — the file's contents, not its path or timestamp. A
  re-import after an edit misses (new bytes); a re-import of an unchanged file
  hits, even if the file was touched or copied. This is the property that makes
  the cache *content*-addressed rather than a stale metadata cache.
- **`settings_bytes`** — the effective `ImportSettings` serialized to JSON
  (ADR-0027). The settings are already a value that serializes itself, so the
  key reuses that rather than inventing a second encoding. Changing the filter or
  the wrap changes the key and invalidates exactly that entry.
- **`importer`** — the scheme that owns the file (`"tex"`, `""` for the CPU
  image). Two importers can share a file extension (a `.png` is both an `Image`
  and a `Texture2D`), so the importer identity is part of the key: the two never
  collide.
- **A version tag** (`"fork-import-v1"`) is hashed in first. A change to the
  artifact format or the hash itself bumps it, which invalidates every entry at
  once instead of risking a reader misreading an artifact written by an older
  engine. It is the same instinct as the `format_version` guard on the scene and
  sidecar files.

The encoding is **length-prefixed** (each field is written as its byte length
followed by its bytes) and therefore **delimiter-free**, so no combination of
inputs can produce the same byte stream as a different combination. That is what
keeps the key collision-resistant against *structural* collisions (a settings
string that contains a source prefix) rather than only against hash collisions.

### FNV-1a 64-bit, for now

The hash is FNV-1a 64-bit, rendered as 16 lowercase hex characters. The
algorithm is chosen for the job it does:

- The cache key is **not a security boundary**. A collision costs a wrong decode,
  not a compromised system: the artifact is validated on read (a format marker
  and the key), and a mismatch is a miss that falls back to a real import. There
  is no adversary and no network here, so a cryptographic hash buys nothing a
  fast one does not.
- It is a handful of lines, has no dependency, and is deterministic across
  platforms and compilers — the three properties the key actually needs. The
  engine already uses FNV-1a for `std::hash<Uuid>`, so this is not a new
  algorithm in the codebase.
- **64 bits, not a `Uuid`.** A `Uuid` is 128 bits and its canonical form is
  longer than a cache filename needs; 64 bits already makes an accidental
  collision negligible at the scale of a project's asset tree, and the key is an
  **opaque string** to every consumer. If a future consumer needs more (a shared
  or network cache, where a collision is an attack surface), the hash is swapped
  for SHA-256 and the version tag bumped; nothing else changes.

### Layout: one artifact file per key

```
<cache_root>/import/<key>.bin
```

`<key>` is the 16-hex-character hash. The artifact file is a small self-describing
blob: a format marker, the key it was written for, the resource type name, the
dimensions, and the payload bytes. The key is **stored in the file** as well as
being its name, so a reader can detect a hash collision or a corrupted/renamed
file and treat it as a miss rather than returning the wrong artifact. A cache
root that does not exist is not an error: the store simply misses, and the first
`store` creates the directory (through `fs::write_binary_file`, which creates
parents).

### Invalidation is the key; eviction is the root

There is no explicit invalidation step. A change to any input changes the key, so
the old entry is never asked for again; a new entry is written beside it. This is
what makes invalidation *correct by construction* rather than a list of things to
remember to invalidate.

Eviction is deliberately out of scope for this slice. A cache root is
**disposable by construction** — it is derived data, and deleting the directory
is a valid, complete eviction that costs only re-decoding. Bounded growth needs
per-entry statistics (last use, size) and a policy (LRU, a size cap), which is a
larger design than the store itself; until an engine can show the cache growing
unboundedly in practice, the honest eviction policy is "delete the root". A
future ADR adds a GC when there is a number that justifies one.

### The seam is in the loader; the first artifact is the decoded image

The cache is checked **in the loader**, not wrapped around it by the manager.
`ImageLoader` gains an `ImportCache*` (null by default, so its existing
construction sites are unchanged) and its `load` becomes:

```
load(path):
  source = read_binary_file(path)
  key = cache ? cache->key_for(source, current_settings(), "image") : ""
  if cache and cache->contains(key):
      if artifact = decode_artifact(cache->fetch(key)):   // validated on read
          return Image(artifact)
  image = stb_decode(source)                              // the real import
  if cache: cache->store(key, encode_artifact(image))
  return image
```

The manager already scopes the asset's settings for the loader call
(`current_import_settings()`, ADR-0027), so the loader has the settings the key
needs without any new plumbing. Keeping the check in the loader means **no new
module edge**: `resources` already depends on `core` and on its own loaders, and
a future mesh or audio importer adds a cache check to its own loader the same
way. Wrapping the loaders in the manager was rejected because it would make the
manager coordinate with each loader about artifact formats, which is knowledge
the loader already owns.

**The GPU `Texture2D` is not cached in this slice.** `Texture2DLoader` decodes
the CPU `Image` through the manager, so once the image decode is cached the
texture load reuses the warm decoded artifact and pays only the upload — which is
the expensive half of the win, since the decode is what the cache removes. Caching
the uploaded texture itself would need a serialized `rhi::TextureDesc` and a
device at read time, and would only save the upload, not the decode. The seam is
defined (any loader may use the cache); the artifact format for a GPU texture is
a follow-up. This is a deliberate small slice that genuinely works, not a broad
one that does not.

### Inert by default, exactly like the asset database

With no root set, `ImportCache::is_enabled()` is false, every `contains` is
false, and every `store` is a no-op. A `ResourceManager` with no cache root, a
loader constructed with a null cache, and a headless test all behave exactly as
before this ADR — the same guarantee ADR-0020 gives for the `AssetDatabase`. The
sandbox opts in with `--cache DIR`; `Engine::Config` carries `cache_root`.

## Consequences

**Easier:**

- A re-import and a rebuild are cheap: an unchanged asset is a cache read
  instead of a decode. The first load after a change pays the decode once.
- Invalidation is correct by construction. Nothing has to remember to invalidate
  an entry; the inputs are the key, so a changed input is a different entry.
- The mechanism is general. A mesh, sprite-sheet, or audio importer adds its
  artifact format and a cache check to its own loader; it does not add a second
  cache or touch the manager.
- The cache is exhaustively unit-testable headlessly: the key is a pure function,
  the store round-trips, and a hit produces an `Image` identical to a cold decode.

**Harder / accepted costs:**

- **Derived data on disk that must be disposable.** The cache is only ever a
  speedup, so a corrupted or stale entry must be a miss, never a wrong result.
  That is why the key is written inside the artifact and validated on read: the
  cost is a slightly larger artifact file, and the benefit is that a bad cache
  cannot produce a wrong image.
- **A second on-disk location beside the sidecars.** Assets now have identity
  sidecars (`*.meta`, source of truth) and cache artifacts (derived). They are
  different by design and live in different trees (the sidecar travels with the
  asset; the cache lives in the cache root and can be deleted), but a reader has
  to know which is which. The naming (`import/<key>.bin` under an explicit root)
  keeps them apart.
- **No eviction policy yet.** An unbounded cache root grows. This is accepted
  for the slice because the root is disposable and the alternative (a policy) is
  a larger design; the ADR records the gap rather than hiding it.
- **FNV-1a is not cryptographic.** Accepted for a local, non-adversarial cache;
  the key is opaque and the hash is swappable behind a version bump.

## Alternatives considered

- **A metadata cache keyed by path + mtime.** Cheaper to key, but wrong: an
  `mtime` changes on a copy or a checkout without the content changing (a
  spurious miss), and can be preserved across an edit (a spurious hit). Content
  addressing is the property the roadmap asks for ("content-addressed store") and
  the only one that is correct under both.
- **SHA-256 (or another cryptographic hash).** Correct and future-proof, but
  larger to implement and slower, for a cache with no adversary. Rejected for
  this slice, with the swap recorded: the key is opaque, so upgrading is a hash
  change plus a version bump, not an interface change.
- **Cache the GPU `Texture2D` artifact directly.** Saves the upload as well as
  the decode, but needs a serialized `rhi::TextureDesc`, a device at read time,
  and a decision about driver/format compatibility across machines (a GPU
  artifact is not portable the way a decoded image is). Deferred: the decode
  cache already removes the dominant cost, and the seam is defined.
- **A central cache manifest** (`import/index.json` mapping keys to artifacts).
  Rejected for the same reason ADR-0020 rejected a central asset manifest: it is
  a merge-conflict magnet and a single point of corruption, for a store whose
  entries are independent and whose names already *are* the keys. The file tree
  is the index.
- **An LRU with a size cap now.** A real eviction policy, but it needs per-entry
  stats and a budget the engine cannot yet justify with a measurement
  ([ADR-0017](0017-performance-strategy.md): nothing is cached or optimised until
  a number shows it matters). Rejected as premature; "delete the disposable root"
  is the honest policy until a GC earns its place.
- **Wrap the loaders in the manager instead of checking inside them.** Keeps the
  loaders free of cache code, but makes the manager coordinate artifact formats
  it does not own, and gives every loader's cache check a different home. The
  loader is where the artifact format already lives, so the check belongs there.
