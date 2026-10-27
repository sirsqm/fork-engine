# 0094 — Stamping the engine version into an export

Date: 2026-10-27

Status: Accepted

Related: [ADR-0064](0064-export-runtime-and-player.md),
[ADR-0071](0071-web-first-2d-shipping-path.md)

## Context

A downloadable build has to say which version it is. The engine version has one
source of truth — the `project(... VERSION ...)` call in the root
`CMakeLists.txt`, from which `cmake/FEVersion.cmake` generates
`fe/version/Version.h` — but an exported game carried no record of it. An export
directory held a pack, a scene and a runtime and nothing that said what produced
them, so a bug report from a shipped build could not be tied to a version and a
player could not tell a stale download from a fresh one.

The version was already reachable in code: `fe::version::kEngineVersionString`
is compiled into every binary, and the engine logs it at startup
(`fork engine v0.1.0 starting`). What was missing was a record *in the export*,
which is the artifact that leaves the developer's machine.

## Decision

The export manifest gains an `engine_version` field, stamped by `fork_export`
from `fe::version::kEngineVersionString`. The tool never takes a version on the
command line and never has a default: the value comes from the same generated
header the rest of the engine reads, so it cannot be typed in wrong and cannot
drift from the CMake package version.

The field is optional in the document. A manifest written before the field
existed has none, which reads as an unrecorded version — the honest reading of a
build that predates version stamping — rather than being rejected or defaulted
to the current version, which would claim a build was made by an engine it
predates. `to_json` omits the field when it is empty, so a manifest written by a
caller that does not know the version (a unit test, an older tool) keeps the
shape it had. A value of the wrong type is rejected rather than coerced, the
same rule the rest of the manifest follows.

The version is surfaced in two places a person actually looks:

- the page's `<title>`, as `Name 0.1.0`, so a tab, a bookmark and a screenshot
  carry it. The title is set by the runtime from the window title, so on the web
  the shell also passes the manifest's name and version as `--name` and
  `--engine-version`: the shell is handed the pack and the scene rather than the
  manifest, and the runtime is handed argv, so the two facts travel with it. A
  manifest without a version leaves the title exactly as it was;
- the player's log, one line when the manifest recorded a version. A run pointed
  at a pack and a scene directly, rather than at an export directory, has no
  manifest and so logs nothing.

The format version is not bumped. `format_version` guards the *shape* of the
document — the fields a reader must understand to use it — and an added optional
field that a reader may ignore does not change the shape. An old reader loads a
new manifest and drops the field; a new reader loads an old manifest and reads
no version. Bumping would make every new build unreadable by an older runtime
for no gain.

## Consequences

- A shipped build can be tied to the engine that made it, from the artifact
  alone, without a separate release note.
- The single-source-of-truth rule holds end to end: `CMakeLists.txt` to
  `Version.h` to the manifest, with no second place to type a version.
- A stale runtime is still not detected. The manifest says what made the build,
  not what is running it; comparing the two and refusing a mismatch is a
  separate decision, and refusing would break a runtime that is merely older
  than the tool that wrote the manifest.
- The web title now varies with the version. The headless check reads the
  player's result out of the title only after the runtime starts, so the initial
  title's shape does not affect it.

## Alternatives considered

- **A `--version` flag on `fork_export`.** Rejected: it makes the version an
  input, which is exactly the drift the single-source-of-truth rule exists to
  prevent. The tool knows the version; it should not be told it.
- **A separate `version.txt` beside the manifest.** Rejected: one more file to
  copy, one more file to forget, and the manifest already names everything else
  the build needs.
- **Bump `format_version`.** Rejected: it would make a new export unreadable by
  any runtime built before the change, for a field neither needs to function.
- **Compare the manifest's version against the running engine and warn on a
  mismatch.** Deferred: useful, but a *warning* needs a place to appear (a
  console a player never sees, a status element a game may not have), and
  *refusing* on a mismatch is wrong for a runtime that is older than the tool
  rather than incompatible with the pack.
