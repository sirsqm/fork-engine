# ADR-0004: Resource ownership with a weak cache

- **Status:** Accepted
- **Date:** 2026-01
- **Related:** [ADR-0002](0002-hybrid-scene-ecs-architecture.md)

## Context

Assets — textures, meshes, audio, scenes, scripts — are shared: many objects
reference the same texture, and the same texture may be requested from several
places. The engine needs a policy that answers three questions reliably:

1. If two things ask for the same asset, do they get the same instance?
2. When is an asset released?
3. How does a reference survive a scene reload, a project change, or an asset
   reimport?

The naive options are both wrong. **A manager that owns every asset forever**
leaks: memory grows without bound and reimports cannot take effect. **Every
caller loading its own copy** duplicates work and memory, and breaks identity
(two sprites with "the same" texture would hold different GPU handles).

## Decision

`ResourceManager` holds a **weak** reference in its cache; callers hold
`std::shared_ptr<Resource>`.

- `load(path)` returns the cached instance if a live one exists, otherwise
  loads, caches a `weak_ptr`, and returns a `shared_ptr`.
- An asset lives exactly as long as something references it. When the last
  `shared_ptr` goes away, the resource is destroyed and the stale cache entry is
  replaced on the next load.
- `collect_garbage()` drops expired entries; it is typically called between
  scene loads.
- A **scheme** prefix selects a loader and is part of the cache key, so
  `assets/hero.png` (a CPU `Image`) and `tex:assets/hero.png` (a GPU `Texture2D`)
  are distinct cache entries that share the decode step.
- A component that references an asset stores the **cache key** (with its
  scheme), not the resource's own file path. This is what makes a reference
  survive serialization and reload: the reloaded reference resolves to the same
  GPU resource.

The GPU lifetime rule follows from the platform: RHI resources are released when
their `shared_ptr` count reaches zero, and their destructors call into the
graphics API, so they must be released before the device is shut down. The
runtime enforces this ordering by releasing the scene before shutting the engine
down.

## Consequences

**Easier:**

- No manual release calls; release is automatic and correct.
- Identity is preserved: everyone asking for the same key gets the same object.
- Reimport and reload "just work": drop the references, and the next load reads
  the new file.
- Memory is bounded by what is actually in use.

**Harder:**

- A caller must hold a reference for as long as it needs the data; forgetting to
  is a silent reload, not a crash, which can be a performance surprise rather
  than a correctness bug. The ownership rule is documented on `ResourceManager`
  and `Sprite2D`.
- Cache keys must be stable and canonical (hence `FileSystem::normalize`, which
  collapses repeated separators and normalises separators).

## Alternatives considered

- **Strong cache owned by the manager for the process lifetime.** Simple, but
  leaks and blocks reimports. Rejected.
- **Manual reference counting with explicit `acquire`/`release`.** Correct but
  error-prone; `shared_ptr` already provides the counting. Rejected.
- **A handle/ID indirection layer (resources referenced by integer handle).**
  Good for cache locality and for surviving moves, but adds an indirection and a
  registry to maintain. Deferred: GUID-based references are on the roadmap
  (M3), layered on top of this contract rather than replacing it.
