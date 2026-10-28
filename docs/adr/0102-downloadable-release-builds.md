# 0102 — Downloadable release builds, and an installed SDK a consumer can link

## Status

Accepted.

## Context

The roadmap's Phase 3 has one bar: "a player downloads one file per OS, runs it,
and plays the game." Nothing in the project produced such a file. `cmake
--install` built an installed tree (ADR-0084), and `fork_export` turned a project
into a pack, a runtime, and a manifest (ADR-0064), but the step between them — a
reproducible, versioned archive per platform, produced from a tag — did not
exist. A release was assembled by hand or not at all.

Two problems sat under that.

The first is that a release is the one path a contributor never runs. It fires on
a version tag, so a mistake in it is invisible until a release is cut, and then
the download is broken for everyone. The packaging had to be a script that the
ordinary CI run also exercises, not a workflow-only recipe.

The second is that the installed tree was not linkable, and ADR-0084 had recorded
that as a deferred consequence. The engine library is static and its public
headers include `<nlohmann/json.hpp>`; its link interface names glad `PUBLIC` and
GLFW and miniaudio `PRIVATE`, and a static library's private dependencies still
have to appear on the *consumer's* final link line. None of those were installed.
A developer who downloaded the SDK archive got a tree that looked like an SDK and
failed on the first compile:

```
$ g++ -Iinclude app.cpp -Llib -lfork_engine
include/fe/serialization/Json.h:9:10: fatal error: nlohmann/json.hpp: No such
file or directory
```

A second archive that cannot be built against is not an SDK, so the release work
and the SDK work are one change: the archive is only worth publishing if what is
inside it is usable.

## Decision

**One packaging script, run by CI on every push and by the release workflow on a
tag.** `packaging/package_release.cmake` builds the game archive, the SDK
archive, and `SHA256SUMS`, and unpacks the game archive and plays it. The
release workflow calls the same script; the CI job calls it too, so the path
cannot rot between tags. The script is a CMake script because the project is
already CMake, `cmake -E tar` is portable, and a script can be driven in script
mode by a test (the release guard, below) without a build.

**Two archives per platform, named for their version and platform.**
`fork-engine-<version>-<platform>-<arch>-game` carries the playable game — the
binary, a launcher per OS, and the example's scenes and assets — and
`fork-engine-<version>-<platform>-<arch>-sdk` carries the installed engine. They
are separate because they have different audiences: a player wants the game and
nothing else, a developer wants the SDK. The version comes from the root
`CMakeLists.txt`, so a release cannot be mislabelled (ADR-0094).

**The SDK installs its dependency payload and a CMake package config.**
`cmake/FEInstallDependencies.cmake` installs the third-party headers and archives
the public API and the link line need — nlohmann/json, glad, GLFW, miniaudio, stb
— and writes `ForkEngineConfig.cmake`, so:

```cmake
find_package(ForkEngine REQUIRED)
target_link_libraries(my_game PRIVATE ForkEngine::engine)
```

works against an unpacked SDK archive. The config resolves everything relative to
its own location, so the tree is relocatable, and it names the platform libraries
the dependencies themselves need (dlopen, threads, the Apple frameworks) rather
than leaving them to the consumer, because they follow from the engine's
dependency choices.

**The archives are asserted to be compressed, and the game archive is asserted to
be playable.** `cmake -E tar cf` writes an *uncompressed* tar even when the file
is named `.tar.gz`, and `cmake -E tar xf` reads it back by sniffing the format —
so that mistake passes a round-trip smoke test and fails only for a downloader
whose `tar xzf` rejects it. The script asserts the gzip magic bytes, and unpacks
the game archive and plays it, which is what a download does.

## Consequences

Easier:

- Cutting a release is a tag. The workflow builds each platform, checksums the
  archives, and publishes them; the version in the archive name and the manifest
  comes from one place.
- An installed tree is an SDK. A consumer links against a downloaded archive with
  one `find_package`, and the install smoke test proves it in CI on every push
  rather than at a release.
- A packaging regression fails the ordinary CI run. The CI job runs the packaging
  script, so a change that breaks it is caught on the push that made it.

Harder:

- The SDK archive is larger. It carries the dependency headers and archives
  (glad, GLFW, miniaudio) that a link needs, which is most of its size; a
  header-only tree would be smaller and would not link.
- The packaging script has a per-platform decision — the executable suffix, the
  archive format, the multi-config output directory — so a new platform is an
  edit there. That is recorded in one file rather than spread through the
  workflow.
- The dependency payload is pinned to what the engine was built against, because
  the config finds the archives by path rather than through `find_package`. A
  consumer cannot substitute a system GLFW, which is deliberate: the engine and
  its window backend must agree.

Not done, and recorded rather than half-done:

- There is no `find_package` for a *system* dependency set, so `FE_USE_SYSTEM_DEPS`
  and the installed config are independent paths.
- The game archive is not an AppImage, a `.app` bundle, or a signed installer.
  Those are separate queue items (`linux-appimage`, `macos-app-bundle`,
  `windows-zip`); this change produces the archives they will wrap.
- The Windows and macOS archives are built and started in CI but not played,
  because a headless runner there has no GL 3.3 context. The Linux job plays the
  artifact, so the playable path is exercised on the platform that can.

## Alternatives considered

- **A shell script instead of a CMake script.** Rejected: it would need a
  different implementation per OS (PowerShell on Windows), and the project
  already depends on CMake everywhere. A CMake script in `-P` mode is also
  drivable by a test, which the shell script is not.
- **One archive with the game and the SDK in it.** Simpler to produce, but it
  makes a player download the SDK and a developer download the game. The two
  audiences and the two sizes justify the split.
- **Publish the SDK as headers only and let the consumer supply the libraries.**
  This is the ADR-0084 tree, and it does not link: the consumer would have to
  reconstruct the engine's exact dependency set, including the GL loader, to link
  a static library whose private dependencies are not visible to them.
- **Install the dependencies through their own install rules** (turning on
  `GLFW_INSTALL` and `JSON_Install`). Rejected: it installs whatever those
  projects choose to install, including their CMake configs and examples, and
  couples our layout to theirs. Installing the specific headers and archives we
  need keeps the layout in one place (FEInstall.cmake's variables).
- **Assemble the release in the workflow with `actions/upload-artifact` and shell
  steps only.** Rejected for the reason the CI job exists: the packaging would
  then run only on a tag, and the first person to find a mistake would be a
  downloader.
