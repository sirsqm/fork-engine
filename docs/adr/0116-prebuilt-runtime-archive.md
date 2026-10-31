# ADR-0116: A prebuilt runtime archive for `fork_export --player`

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0064](0064-export-runtime-and-player.md),
  [ADR-0102](0102-downloadable-release-builds.md),
  [ADR-0084](0084-install-rules.md),
  [ADR-0103](0103-linux-appimage.md)

## Context

`fork_export` copies the `fork_player` built beside it and records it in the
manifest ([ADR-0064](0064-export-runtime-and-player.md)). That is the right
design for the *runtime*: it is generic and driven by the pack, so one player
serves every game and the export step has no compiler in it. The gap is where the
player comes from. It is the one the developer's own build produced, so a
developer without the engine's toolchain — a source checkout and a compiler —
cannot export a game at all, and the release only published a playable *game* and
an SDK, neither of which is a runtime to hand to the export step.

The queue item (`desktop-export-templates`) asks for exactly this: "a documented,
reproducible release build produces a per-OS runtime artifact that `fork_export`
accepts via `--player`, and the smoke test runs it." The build and the packaging
already exist — a tag builds every desktop target and `packaging/package_release.cmake`
turns each build tree into archives ([ADR-0102](0102-downloadable-release-builds.md))
— so the missing piece is one more archive per OS and the acceptance check that
the export path accepts it.

Two constraints decide the shape. First, the artifact must be *relocatable*: a
downloaded player is unpacked somewhere the build never was, so a binary with the
build tree's absolute RPATH breaks the moment it is moved. The installed tree
already solves this — `fe_install_target()` bakes `$ORIGIN/../lib`
([ADR-0084](0084-install-rules.md)) — so the archive must be staged from the
*installed* player, not the build tree's. Second, the claim is a *flow*, not a
file: "a developer without the toolchain can export" is only true if
`fork_export --player <unpacked player>` actually produces a build and the player
runs it, so the smoke must check the flow rather than assert the archive is
non-empty.

## Decision

**1. `packaging/package_release.cmake` writes a third archive,
`fork-engine-<ver>-<platform>-<arch>-runtime.<ext>`.** It carries the installed
`fork_player` (plus any shared libraries the engine produced, in the layout the
RPATH expects: beside the executable on Windows, in `lib/` elsewhere) and a
README that names the two commands a developer runs. It is produced by the same
per-OS release job as the game and SDK archives, so "a documented, reproducible
release build" is the build that already exists rather than a second one.

**2. The runtime is staged from an installed prefix, not the build tree.** The
script runs `cmake --install` into a scratch prefix and copies the player from
there, so the archived binary carries the relative `$ORIGIN/../lib` RPATH and
runs after unpacking anywhere. Copying the build tree's player would ship an
absolute build-path RPATH that works only on the machine that built it — a
failure that is invisible until a downloader runs it.

**3. The smoke checks the export flow.** After unpacking the runtime archive, the
packaging smoke runs `fork_export --player <unpacked player>` against
`examples/demo_project` and asserts the export directory holds the player, the
pack, and the manifest; where a GL context exists (`FE_SMOKE_GAME=ON`, as the
Linux CI job sets it) it then runs that player on that export. The structural
part runs on every platform; the play part is gated the same way the game
smoke's is.

**4. The checksums and the release cover the new archive.** `SHA256SUMS` gains a
line for it, and the release workflow's flatten step already collects every
`*.tar.gz`/`*.zip`, so the runtime travels with the other artifacts. The
`release_workflow` guard test asserts the archive is produced, that the script
invokes `fork_export --player`, and that it is staged from an installed prefix.

## Consequences

- A developer with no toolchain can download the runtime archive for their OS,
  unpack it, and run
  `fork_export --project DIR --out DIR --player /path/to/fork_player` to produce a
  build. The player is generic, so the same one serves every game.
- The runtime archive is roughly the size of the player binary and is built by
  the release job that already runs; there is no second build to keep in sync.
- The "Done when" is satisfied as a tested flow: the release build produces the
  per-OS artifact, `fork_export` accepts it via `--player`, and the smoke runs
  it. The play half needs a GL context and so runs in the Linux job (Xvfb plus
  Mesa), exactly as the game archive's smoke already does; on Windows and macOS
  the export and structural checks run and the play is skipped, which is the
  same limitation the game smoke has and is recorded, not hidden.
- Nothing about the existing game or SDK archives changes: the new archive is a
  third output of the same script, and the two original paths are untouched.

## Alternatives considered

- **Copy the build tree's `fork_player` into the archive.** Rejected: it carries
  an absolute build-path RPATH, so it would work in CI (where the build tree is
  still present) and fail for a downloader. Staging from the installed prefix is
  what makes the artifact relocatable, and it is the same primitive the SDK
  archive already uses.
- **Bundle the runtime inside the SDK archive instead of a separate one.**
  Rejected: the SDK is 70 MB (headers, the static library, the tools) and a
  developer who wants only a runtime to export with should not download all of
  it. A separate small archive is the artifact the queue item names.
- **Ship a prebuilt runtime for the *editor* too.** Out of scope: the item is
  about `fork_export --player`, and the editor's packaging is the AppImage and
  the future `.app`/installer items.
- **Make `fork_export` download a runtime when none is beside it.** Rejected: an
  export step must not depend on the network, and a tool that fetched a binary
  would need a trust decision the project should not make silently. A developer
  downloads the archive once and passes `--player`.
