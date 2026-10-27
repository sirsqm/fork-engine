# 0096. A writable user-data storage seam

Date: 2026-10-25

Status: Accepted

## Context

Every engine concern that touches the OS lives behind the `platform` module
(ADR-0041), and the contract there is deliberately stated as *what the engine
depends on*. Reading the contract back, one row is missing that a shipped game
cannot do without: **where a game writes the data it owns.**

The gap is easy to miss because it is not "no file I/O". `core::fs` (ADR-0041's
filesystem row) reads and writes files, and it works. The problem is *where* a
game may write:

- On desktop the only path the engine ever writes to is the import cache, a
  disposable directory a *project* configures (`Engine::Config::cache_root`).
  There is no notion of a per-user, per-game data directory at all, so a game
  that wants to save a best score has nowhere to put it and no way to ask for a
  place.
- On the web there is no filesystem to write to, and `core::fs` does not even
  pretend to help: a browser build has no writable path. Emscripten's `MEMFS` is
  an in-memory filesystem that is *discarded when the tab is reloaded*, so a
  naive "write the score to a file" saves nothing across a reload — the exact
  thing a save is for. Persisting in a browser means a real browser storage API,
  and that API must not leak above the platform layer.

This is not speculative work. The shipping queue's next Phase 4 item,
`slice-persistence` ("Save the best score locally, so progress survives a
reload"), is written against "the storage seam" — and that seam does not exist.
`slice-settings` ("Volume and control settings, persisted") depends on it too.
Two queue items are blocked on one missing platform concern.

The forces:

- **The game owns the data, the engine owns the location.** A game should ask to
  save `"best_score"` and not care whether that lands in
  `~/.local/share/<game>/` on Linux or in browser storage. Choosing the
  location is an OS concern; choosing what to store is the game's.
- **A save must survive a reload, and it must be provable that it did.** The
  web half is the whole point — a desktop-only storage helper is not a seam, it
  is a convenience, and the pillar ("games ship anywhere") is what makes the web
  case load-bearing.
- **The concern must not leak.** No engine header may name `localStorage`,
  `XDG_DATA_HOME`, or `%APPDATA%`. A backend that names its OS API lives in the
  platform module; everything above it names only the seam (ADR-0041).
- **A tool and a test must run it with no OS.** The null backend is the proof a
  seam is real (ADR-0041), and it is also what lets a unit test exercise a
  system that saves without touching the developer's real home directory.

## Decision

**Add `platform::UserData`: a small key/value store for the data a game owns,
with a desktop backend (a real per-user directory), a web backend (the browser's
`localStorage`), and a null backend (in memory) for a host with no persistence.**

### The seam

`UserData` is a *store of named blobs*, not a filesystem. The engine hands out
one handle; a game reads and writes by key.

```cpp
class UserData {
public:
    // The key's bytes, or an Error naming the key when it is absent.
    [[nodiscard]] Result<std::vector<u8>> read(std::string_view key) const;
    // Writes (creating or replacing). The write is durable before it returns on
    // every backend, including the web one, so a caller never sees a save that
    // "worked" but did not survive a reload.
    [[nodiscard]] Error write(std::string_view key, const void* data, usize size);
    [[nodiscard]] Error write(std::string_view key, std::string_view text);
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] Error remove(std::string_view key);
    // The keys present, sorted, so a listing is deterministic for a test.
    [[nodiscard]] std::vector<std::string> keys() const;
    // Where the data lives, for a log line or a "open save folder" button.
    // Empty for a backend with no user-visible location (web, null).
    [[nodiscard]] const std::string& root() const noexcept;
    [[nodiscard]] const char* backend_name() const noexcept;
};
```

The shape is chosen so the two hard cases fall out:

- **A key is a name, not a path.** Keys are restricted to a safe character set
  (`[A-Za-z0-9._-]`, no separators, non-empty) so a key cannot escape the store's
  directory on desktop. The store decides the filename; the game does not.
- **A write is synchronous and durable.** `write()` returning success means the
  bytes will be there after a reload. On desktop that is a normal file write. On
  web it is a synchronous `localStorage.setItem`, which is the reason that store
  is chosen over IndexedDB: an IndexedDB write is asynchronous, so a synchronous
  `write()` built on it would have to block the whole page through
  `-sASYNCIFY` or return before the bytes were stored — the latter a save that
  silently does not survive a reload, which is the one failure this seam must not
  have. This is why the web backend owns its own write path rather than
  deferring to `core::fs`.

### Backends

| Backend | Selected by | Where the data lives | Durability |
| --- | --- | --- | --- |
| Desktop | `FE_PLATFORM_WEB` unset | `<user data dir>/<app-id>/` | A file write |
| Web | `FE_PLATFORM_WEB` | `localStorage`, keyed `<app-id>/<key>` | Synchronous `setItem` (verified by read-back) |
| Null | `UserDataDesc::backend == Null` | In-memory map | None (survives only the process) |

- **Desktop** resolves a per-user directory the way the OS expects: `$XDG_DATA_HOME`
  or `~/.local/share` on Linux, `~/Library/Application Support` on macOS,
  `%APPDATA%` on Windows, each under an application id the engine is given. The
  resolution lives in the backend, so no caller names an environment variable.
- **Web** namespaces each key by application id and stores a base64 encoding of
  the bytes, so `localStorage` (a string store) holds any blob byte-exactly and a
  reload reads it back. A write is confirmed by reading the value back, so a
  write the browser silently dropped (a full quota) is reported as a failure
  rather than discovered by the next run. This is the half that makes the seam
  more than a desktop helper.
- **Null** keeps a map in memory. It is what a test and a tool use, and it is the
  ADR-0041 "prove the contract with no OS" backend for this concern. A run that
  wants *no* persistence gets this rather than a surprise write to a real home
  directory.

### Wiring

- `Engine::Config` gains an application id and an optional explicit data root;
  `Engine::initialize` creates the store, and `Engine::user_data()` returns it.
- `FrameContext` gains a nullable `UserData*`, so a gameplay system can save
  through the seam it is handed rather than reaching for the engine — the same
  rule every other context field follows.
- A headless or test run that sets no application id gets the null backend, so
  the default is "no persistence" and a save is never a surprise.

## Consequences

- `slice-persistence` and `slice-settings` have a seam to build on: a best score
  and a volume level are `write("best_score", ...)` and `read("best_score")`,
  identical code on desktop and web.
- The web target gains real persistence, which is the difference between a demo
  that forgets and a game that remembers.
- A test can exercise a saving system with the null backend and assert the
  round-trip, with no dependency on the machine's home directory — and the
  desktop backend's real path resolution is tested separately against a
  temporary directory.
- The cost is a third storage concept beside `core::fs` and `pack::PackSource`.
  The boundary is stated: `core::fs` is the raw OS file API (untrusted paths, no
  per-user notion), `pack` is a read-only asset source, and `UserData` is the
  writable, keyed, per-user store a game saves to. A caller that wants to read an
  arbitrary path still uses `core::fs`; a caller that wants to save uses this.
- The web backend is only as durable as the browser allows: a user who clears
  site data loses the save, and `localStorage` is a few megabytes and may be
  evicted under storage pressure. That is the platform's behaviour, not a defect,
  and it is recorded here rather than papered over. The size limit is the real
  cost of choosing `localStorage`; a save that outgrows it would need the
  asynchronous IndexedDB path and a change to the "write is synchronous" clause
  above, which is the point at which this decision should be revisited.

## Alternatives considered

- **A per-game directory resolved in `core::fs`.** Rejected: it would put an
  OS-specific location rule (XDG, `%APPDATA%`) into `core`, which is platform-free
  by design, and it would give the web build a path that does not persist. The
  location rule is exactly what the platform layer exists to hide.
- **A "storage" interface with a pluggable filesystem.** Rejected as more seam
  than the problem needs. The engine has one writer (the game) and one datum
  shape (a named blob); a key/value store is the smallest surface that covers it,
  and a virtual filesystem would invite callers to treat user data as assets.
- **`IDBFS` (IndexedDB) with `FS.syncfs` on web.** Rejected: `FS.syncfs` is
  asynchronous, so honouring the "a successful write is durable" clause would
  require `-sASYNCIFY` (which re-enters the whole program and is a large,
  build-wide cost for one call) or returning before the sync completed (a save
  that silently does not survive a reload). `localStorage` is synchronous, which
  makes the clause true without either compromise; the accepted cost is its
  smaller size limit, recorded above.
- **Write to `MEMFS` on web and let the host sync.** Rejected for the same
  reason: it makes durability the host's problem, and a host that forgets
  produces a save that silently does not survive a reload.
- **Reuse the import cache directory as the save location.** Rejected: the cache
  is *disposable* by design (ADR-0029) — a tool may delete it at any time — so
  anything written there is expected to be lost. A save and a cache have opposite
  lifetimes and must not share a directory.
