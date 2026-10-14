# ADR-0046: Resource change detection and reload

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0020](0020-resource-identity-guid-sidecar.md),
  [ADR-0029](0029-import-cache.md), [ADR-0030](0030-asset-pack-format.md),
  [ADR-0019](0019-collaborative-editing.md)

## Context

An editor is only usable when an author can change an asset — repaint a texture,
re-export a mesh — and see the result without restarting. That is "hot reload",
and it is listed in the M6/M8 editor work as `attach, inspect, hot reload`.

The resource system already has the two halves a reload needs. `ResourceManager`
caches one instance per reference and hands out a `shared_ptr`; when the last
reference goes away the instance is released and the next request loads it
afresh. And a resource already knows the path it came from (`Resource::path()`).
What is missing is the *trigger*: nothing records a file's write time at load, so
nothing can tell that the file on disk has changed since.

Two facts about the existing design shape the answer:

- **The cache holds a weak reference.** A resource stays loaded only while a
  caller holds it. That is deliberate — it is what stops the manager pinning
  every asset for the life of the process — but it means an instance loaded and
  stored with no caller to adopt it is released on the spot.
- **A reference may name a path or an identity.** A `guid:` reference resolves
  through the `AssetDatabase` to the asset's *current* path, so a reload must
  re-resolve the identity rather than remember a file name that a move
  invalidates. A bare path reference has no identity and is watched directly.

## Decision

**Record, per cache entry, the file a resource was loaded from and that file's
write time; expose change detection and reload as two separate steps.**

- **`fs::last_write_time_ns`** is added to the filesystem layer — the one place a
  modification time is read — returning an opaque count that callers compare for
  inequality only. A missing file or an unreadable time is `0`.
- **Each `CacheEntry` remembers its watched file and the stamp at load.**
  `load()` records them after a successful load. A `guid:` reference records the
  asset database's *absolute* path for the id; a bare path records its normalized
  path. A resource read from a mounted pack records neither: the pack is replaced
  wholesale, not edited in place, so it has no loose file to watch.
- **`poll_changed()`** returns the references whose file's stamp differs from the
  recorded one, and updates each recorded stamp so a change is reported once. It
  detects; it does not reload.
- **`reload(reference)`** loads the reference afresh through the same resolution
  a first load used, replaces the cache entry, and returns the new instance for
  the caller to hold and to re-wire. It returns an Error when the reference is
  not loaded or the reload fails, and on failure leaves the existing entry
  untouched.

Detection and adoption are two steps on purpose. Because the cache is weak, an
instance swapped in by `poll_changed()` itself would have no owner and be freed
immediately; returning the fresh `shared_ptr` from `reload()` is what gives it
one. The split also keeps the policy decision — *who* re-resolves a reloaded
resource into the component or system that referenced it — in the editor, where
the inspector already holds the manager.

## Consequences

- **A caller can refresh on its own schedule.** `poll_changed()` is a plain poll
  with no OS watch behind it, so the editor's frame loop decides how often to
  ask, and a headless test drives it exactly. There is no watcher thread, no
  platform event API, and no change to the single-threaded model (ADR-0004).
- **A reloaded resource is a new instance; old handles stay valid.** Nothing
  observes a dangling pointer — a system still holding the previous
  `shared_ptr` keeps working with the previous data until it adopts the new one.
  This is the same "replace, do not mutate" rule the cache already followed.
- **A broken file does not tear down a working resource.** A failed reload leaves
  the previous instance in the cache and reports the error, so a half-written
  file on disk degrades to "still the old pixels" rather than a crash or a
  vanished asset.
- **A transient stat failure is not a deletion.** A file whose stamp cannot be
  read (`0`) is left alone rather than reported, so a permission blip does not
  drop a loaded asset.
- **Identity survives the reload.** A `guid:` reference re-resolves through the
  database, so a resource moved since it was loaded reloads from its new path and
  keeps its `AssetId`. This is the property ADR-0020 established, reused.
- **The watched path is absolute.** A path load records the asset database's
  absolute path when the file is a known asset, and the normalized path
  otherwise, so a change check does not depend on the process working directory —
  which a project load can change under it.
- **Polling stats every live entry.** Linear in the number of loaded resources,
  which is small at authoring scale. A platform watcher, or a per-entry opt-out,
  is the measured optimisation if a profile ever shows it hot.

## Alternatives considered

- **An OS filesystem watcher (inotify, FSEvents, ReadDirectoryChangesW).**
  Rejected for now: it is platform-specific, needs a thread and a queue, and
  breaks the "one place decides platform syntax" rule the filesystem layer keeps.
  A poll is portable, testable, and sufficient for an editor frame loop; a
  watcher can sit on top of `poll_changed()` later without changing this seam.
- **Reload inside `poll_changed()`.** Rejected: the weak cache frees an unadopted
  instance at once, so a reload with no caller to receive it is wasted work and
  the caller still has the old pointer. Returning the fresh instance is what
  makes the reload observable.
- **Store a strong reference while a change is pending.** Rejected: it would pin
  a resource the caller may not want, and it hides the adoption step rather than
  naming it.
- **Compare a content hash instead of a write time.** Rejected: it reads and
  hashes the whole file on every poll, which is far more expensive than a stat,
  for a change signal the timestamp already gives. The import cache
  ([ADR-0029](0029-import-cache.md)) hashes content where content identity
  matters; a change *check* does not need it.
- **Reload eagerly for every asset when any file changes.** Rejected: a reload
  costs a decode, and a batch edit would re-decode the world. Reporting the
  changed set lets the caller reload what is actually in use.
- **Watch a pack member.** Rejected: a pack is a built artifact, replaced
  wholesale by the build step, not edited in place; watching inside it would
  report changes a running build cannot honour.
