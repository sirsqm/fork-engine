# ADR-0066: Editor-driven gameplay-module hot reload

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0041](0041-platform-contract-and-null-backend.md),
  [ADR-0044](0044-scripting-seam.md), [ADR-0046](0046-resource-change-detection-and-reload.md),
  [ADR-0061](0061-cpp-gameplay-modules-hot-reload.md),
  [ADR-0063](0063-reflection-backed-script-properties.md)

## Context

[ADR-0061](0061-cpp-gameplay-modules-hot-reload.md) landed the mechanism for C++
gameplay modules and hot reload: a module is a shared library that registers
behaviours through a versioned entry point, `ScriptLibrary` loads it, and
`reload_module` swaps its code without a restart. It deliberately left one thing
out, and named it:

> **File watching and automatic reload.** The mechanism is here
> (`reload_module`); deciding *when* to call it — watching a module's file for a
> change, debouncing a build — is editor work and lands with editor integration.

[ADR-0063](0063-reflection-backed-script-properties.md) then gave the editor its
script half: `ScriptCatalog` owns a `ScriptHost` and a `ScriptRegistry`, the
inspector attaches and inspects behaviours, and the serializer persists the
bindings. The roadmap's M7 row still reads "Still to do: file-watching hot reload
driven from the editor". That row is this ADR.

The forces at play:

- **The engine already has a change-detection pattern, and it is a poll.** Asset
  hot reload ([ADR-0046](0046-resource-change-detection-and-reload.md)) is
  `ResourceManager::poll_changed()` plus `reload()`, with no OS watch: a caller
  (the editor's frame loop, a test) decides how often to ask, and detection is
  separated from adoption. A second, different mechanism for modules would be a
  second thing to keep correct for no gain.
- **The editor's registry is the library's registry.** After ADR-0063 the
  inspector lists whatever is in the catalog's `ScriptRegistry`. A module loaded
  through a *separate* library would register its behaviours somewhere the
  inspector never looks, so "load a module" and "offer its behaviour" would not be
  the same act.
- **A reload must rebind the nodes a *scene load* scripted, not only the ones
  attached through the library.** A scene opened in the editor attaches its
  scripts through the serializer's ordinary reader, which goes straight to the
  `ScriptHost` and never touches `ScriptLibrary`. The library's rebind list would
  be empty for those nodes, so a reload would silently drop their scripts.
- **A reload is a between-frames act.** Scripts must not be attached or detached
  from inside a script's `update()` (the `ScriptHost` contract), so the poll and
  the reload belong where the editor already makes structural changes: in the
  frame loop, between frames.

## Decision

**The editor watches its loaded gameplay modules by polling, and hot-reloads a
changed one in place, rebinding every node it scripted — including the nodes a
scene load scripted. The mechanism stays in `ScriptLibrary`; the editor supplies
the *when* and the ownership.**

### 1. `ScriptLibrary` gains a poll, mirroring the resource manager

`ScriptLibrary` records each module's image write time at load, and:

- `poll_changed()` returns the module ids whose image changed on disk since it was
  loaded, and updates each one's recorded time so a change is reported once. A
  module whose file cannot be stat-ed is left alone rather than treated as
  deleted, so a transient read error does not tear down a running game.
- `reload_module(host, id, new_image_path)` is ADR-0061's reload, unchanged: it
  destroys the live scripts, unloads the old image, loads the new one, and
  rebinds. It records the new image's write time so a second rebuild is detected
  too.

This is `ResourceManager::poll_changed()`/`reload()` applied to modules, down to
the "detect, then adopt" split: the poll reports, the caller reloads. There is no
OS watch and no thread; the loop decides how often to ask.

### 2. The library learns bindings it did not create

A scene load attaches scripts through the serializer, not through the library, so
the library would not know to rebind them. Two additions close that gap:

- `track_binding(node_id, behaviour)` records a binding without attaching a
  script.
- `adopt_bindings(host)` rebuilds the whole binding record from the scripts
  currently in a host, so a caller that has just attached a set (a load) can hand
  them over in one call.
- `clear_bindings()` forgets them all, so a binding to a node that is gone is not
  rebound after the scene is replaced.

`EditorContext::load_scene` calls `adopt_bindings()` after a successful load;
`ScriptCatalog::clear()` (called before a different scene is loaded) calls
`clear_bindings()`. The binding record is now correct for scripts that arrived by
either route.

### 3. The editor's catalog owns the library

`ScriptCatalog` owns a `ScriptLibrary` instead of a bare `ScriptRegistry`, and
exposes `load_module`, `reload_module`, `poll_changed_modules`, and
`adopt_bindings`. Its `registry()` accessor now returns the library's registry, so
a module's behaviours appear in the inspector's attach control the moment its
module loads — loading and offering are the same act.

The declaration order matters and is documented at the member: the library (which
owns the images) is declared before the host, so the host — holding scripts whose
code lives in the images — is destroyed first. Destroying a script after its image
is unmapped would call into freed memory.

### 4. The host loads modules at startup and polls each frame

`EditorHost::Config` gains `modules` (shared-library paths). `initialize()` loads
them *before* any scene opens, so a scene that records a behaviour by name attaches
it during deserialization. A module that will not load is logged, not fatal: the
editor still opens and the inspector simply does not offer its behaviours.

`EditorHost::step()` calls `EditorContext::poll_script_modules()` after consuming a
play request and before the frame runs. That method polls, reloads each changed
module, and returns the ids reloaded (logged, so a developer sees the swap). A
reload that fails is logged and the module dropped, not thrown.

The shell gains a repeatable `--module PATH`, and `fork_editor_shell --module X`
watches `X` for a rebuild.

## Consequences

**Easier:**

- A developer edits a behaviour, rebuilds the module, and the editor picks it up on
  the next frame: no restart, no reload of the scene. This is the M7 row's promise,
  kept.
- The change-detection pattern is now uniform. A module and an asset are both
  "remember the file and its time, poll for a change, adopt it deliberately", so
  there is one thing to understand, not two.
- The poll is headless and deterministic, so the whole path is tested without a
  window: a test loads two real images, replaces one on disk, polls, reloads, and
  watches the node run the new code through the ordinary schedule.
- Because the catalog owns the library, a module's behaviours are in the
  inspector's list by construction, not by a second registration step.

**Harder:**

- **The reload rebuilds the script; it does not preserve its state.** This is
  ADR-0061's limitation, carried forward: a reloaded script starts fresh. A
  designer who had a live, mid-play value loses it on a rebuild. Carrying state
  across a reload means serializing it, which is a separate cut.
- **In-place rebuild is a POSIX-only convenience.** On Linux and macOS a build
  system replaces a loaded image's file (a new inode renamed over the old) and the
  loader's mapping stays valid; on Windows a loaded image's file cannot be
  replaced, so a rebuild-in-place is not expressible and a reload must load a
  distinct file. The tests split accordingly: the in-place detection test is
  POSIX-only, and the swap test (an explicit replacement path) runs everywhere.
- **The poll is a stat per module per frame.** It is cheap for the handful of
  modules a game has, and it runs once a frame, not per node. A project with
  hundreds of modules would want a cheaper trigger; that is not this cut.
- **A failed reload drops the module and leaves its nodes unscripted** (ADR-0061's
  policy). The editor logs it and keeps running, so a broken rebuild is a visible
  empty binding, not a crash — but it is not a rollback to the previous code.

**Deliberately not in this cut (named so they are planned, not forgotten):**

- **Debouncing a build.** A build that writes an image in several steps could be
  observed mid-write. The poll reports the change once per observed write time, and
  a partial image fails to load and is dropped; a real debounce (wait for the file
  to settle) is a refinement, not a correctness gap, and is not here.
- **Debugging support.** A debugger attached to a reloaded module is the remaining
  M7 row.
- **State preservation across a reload.** Noted above; it is the reflection-backed
  serialization of a live script, a later cut.

## Alternatives considered

- **An OS watch (inotify/FSEvents/ReadDirectoryChangesW).** Rejected for this cut.
  It would add a third platform-specific row to the platform contract and a thread
  or a poll around the watcher, to learn something a stat per frame already tells
  us at this scale. The resource manager already chose the poll (ADR-0046); using
  the same shape keeps one pattern, and an OS watch can replace the poll later
  behind the same `poll_changed()` signature without touching a caller.
- **Load modules through a separate library in the editor, leaving the catalog's
  registry for in-process behaviours.** Rejected: the inspector lists the catalog's
  registry, so a module's behaviours would be invisible to the attach control. The
  catalog owning the library is what makes "loaded" and "offerable" one thing.
- **Rebind only the nodes the library attached.** Rejected: a scene opened in the
  editor attaches its scripts through the serializer, so the library's list would
  be empty and a reload would silently drop them. `adopt_bindings()` makes the
  binding record independent of how the script arrived.
- **Reload on the frame a change is seen, inside `step()`, without a separate
  poll method.** Rejected as the seam. Keeping `poll_changed_modules()` and
  `reload_module()` separate matches the resource path, keeps the detection
  testable on its own, and lets a caller choose to reload a subset or log first —
  which the editor does.
- **Reload every module every frame.** Rejected: unloading and reloading an
  unchanged image would destroy and rebuild every live script for no reason, and
  throw away any state a script had accumulated. Detection is what makes the
  reload rare and therefore cheap.
