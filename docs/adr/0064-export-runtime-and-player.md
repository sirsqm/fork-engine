# ADR-0064: Export — a runtime that plays a pack

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0003](0003-error-handling.md), [ADR-0018](0018-multi-platform-shipping.md),
  [ADR-0025](0025-project-file.md), [ADR-0030](0030-asset-pack-format.md),
  [ADR-0032](0032-near-term-platform-scope.md), [ADR-0041](0041-platform-contract-and-null-backend.md),
  [ADR-0044](0044-scripting-seam.md), [ADR-0061](0061-cpp-gameplay-modules-hot-reload.md),
  [ADR-0063](0063-reflection-backed-script-properties.md)

## Context

[ADR-0030](0030-asset-pack-format.md) shipped the first half of M8's export path:
a versioned pack container, a writer that bakes a project's assets and startup
scene into one file, and a `PackSource` the `ResourceManager` mounts so every
loader reads from a pack exactly as it read from disk. It said explicitly that
"export templates and the per-OS runtimes are a later stage and are called out as
not done".

That later stage is this one. A pack nobody can run is not a shipped game. What
is missing is the *runtime* — the thing that mounts a pack and plays it — and the
*assembler* that turns a project into a directory a player can be handed.

Three pieces already exist and constrain the shape of the answer:

- **A pack is self-describing.** It carries its entries keyed by the same
  resource path a loader is given, each with its scheme and stable id, and the
  startup scene is one of the entries. So a runtime needs the pack path and the
  scene key, and nothing else.
- **Scripts are code, not data.** A behaviour lives in a C++ module that
  registers it by name ([ADR-0061](0061-cpp-gameplay-modules-hot-reload.md)); a
  scene records the behaviour *name* and its property values
  ([ADR-0063](0063-reflection-backed-script-properties.md)). A runtime that
  ignores modules loads such a scene with its nodes unscripted and logs the
  behaviour it could not find.
- **Desktop is the scoped target**
  ([ADR-0032](0032-near-term-platform-scope.md)). The web, mobile, and console
  templates are sequenced behind this, not delivered by it.

## Decision

**Add a `fork_player` executable that mounts a pack and runs the game loop, and a
`fork_export` tool that assembles a runnable build from a project by copying the
player beside a freshly written pack.**

### The player

`fork_player` is a `fe::Application` that:

1. mounts the pack on the engine's `ResourceManager` *before* any content load,
2. registers the gameplay modules it was given — the ones a manifest records, or
   ones named on the command line — into the registry the scene's bindings resolve
   through,
3. loads the startup scene from the pack through the ordinary scene reader, which
   attaches the behaviours the scene binds,
4. runs the normal frame loop.

It is built against the engine's public headers only and links `FE::engine` and
nothing else — no editor, no ImGui. That is the same constraint `fork_pack` and
the examples already carry, and it is what keeps the shipping runtime small.

It is deliberately a *generic* runtime, not generated code. A build produced here
is the player binary, a pack, and a manifest; the game's identity is the data,
not a per-game compiled entry point. This is the smallest thing that makes a pack
runnable, and it means the export step has no compiler in it — it cannot fail
because a developer's toolchain differs from the one that built the engine.

The player is headless-capable (`--headless N`) so CI can run a shipped build
with no display, exactly as the `fork_headless` example is run today.

### The assembler

`fork_export` reads the project file, writes the pack with the existing
`PackWriter`, copies the player and any gameplay modules beside it, and writes a
manifest naming the pack, the startup scene key, and the modules. Output is a
directory:

```
build/export/<name>/
  fork_player[.exe]     the runtime
  game.fepack           the assets and the startup scene
  gameplay[.so/.dll]    gameplay modules named on the command line, if any
  fork.export.json      the manifest: pack file, scene key, modules, version
```

The manifest exists so a launcher, a packager, or a later per-target template has
one file naming what to run, rather than a convention it has to infer. Modules
are listed in it and resolved relative to the manifest's directory, so the player
loads them from the export directory in the order the manifest records. Modules
are named to `fork_export` on the command line rather than discovered: which
libraries a game needs is a property of the game, and a build should record
exactly what it was told to ship.

`fork_export` is built with the engine, like `fork_pack`, because exporting is
part of the shipping path rather than an editor feature
([ADR-0030](0030-asset-pack-format.md)).

### What this does not do

- **No per-target templates.** The output runs on the machine that built it. A
  prebuilt runtime per OS, cross-compilation, and platform app packaging
  (`.app`, `.apk`, Emscripten) remain M8 work.
- **No reachability pruning.** `PackWriter` packs the whole asset tree by
  default; packing only what the startup scene references is
  [ADR-0065](0065-pack-reachability-pruning.md), which adds an opt-in `--prune`
  to `fork_pack` and `fork_export`. It is not the default, and it does not follow
  a packed asset's own references (see that record's "not done").
- **No module discovery.** A module is copied because it was named, not because
  a scene references a behaviour it provides. Inferring the module set from a
  scene's bindings is a possible later refinement, not a correctness need: a
  build that records too many modules still runs.

## Consequences

- A project can be turned into something runnable and tested end to end, which is
  what makes "assets ship in a pack" a claim with an executable proof rather than
  a unit test on a container format.
- CI gains a real end-to-end shipping check: pack a project, export it, run the
  exported player headless, and grep its frame report. The export path fails loudly
  if the pack, the scene key, or the player's mount regress.
- The player is one more artifact to keep building on every platform, and its
  `Application` loop is now a second consumer of the runtime after the examples.
  It is intentionally thin so this stays cheap.
- Because the runtime is generic and driven by the pack, per-target templates
  later become "the same player, built for another OS" instead of a new design.
- A game whose gameplay lives in C++ modules is fully described by the export
  directory: the manifest records the modules, the player loads them before it
  reads the scene, and the behaviours a scene binds resolve against them. What was
  *not* described was a game that ships assets referenced only by runtime code
  rather than by the scene; [ADR-0065](0065-pack-reachability-pruning.md) adds the
  opt-in pruning that closes the common case of it.

## Alternatives considered

- **Generate a per-game `main.cpp` and compile it during export.** Rejected: it
  puts a compiler in the export path, makes export fail for toolchain reasons,
  and produces a build that is not reproducible from the pack alone.
- **Fold the player into the editor binary behind a flag.** Rejected: it would
  drag ImGui, the editor's core library, and its dependencies into every shipped
  build, which is exactly the size and dependency cost the shipping runtime
  exists to avoid.
- **Have the player read the project file.** Rejected: a project file is an
  authoring document naming an asset root and a scene *on disk*
  ([ADR-0025](0025-project-file.md)). A shipped build has a pack, not a project
  tree, and giving the runtime a second, editor-shaped way to find content would
  mean two content paths to keep working. The pack plus a scene key is the whole
  contract.
- **Skip the manifest; pass the pack path on the command line.** Rejected as the
  *only* mechanism: it works for a developer and not for a launcher, a store
  upload, or a later template that needs to know what to run without guessing a
  file name.
