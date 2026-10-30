# 0103 — A single-file Linux download: the AppImage

## Status

Accepted.

## Context

ADR-0102 gave the project per-OS release archives: a game archive, an SDK
archive, and `SHA256SUMS`, produced from a tag by one script that CI also runs.
The game archive is a `.tar.gz` holding the player, a launcher, and the example's
content.

That is a developer's download. A player who is not a developer gets a directory
they have to unpack, a binary they have to find inside it, and — the part that
actually breaks — a binary linked against the C++ runtime of whatever machine
built it. On a distribution a couple of years older than the build host, the
first thing they see is:

```
./fork_breakout: /lib/x86_64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.32'
not found (required by ./fork_breakout)
```

The roadmap's Phase 3 bar is "a player downloads one file per OS, runs it, and
plays the game." A tarball is not one file and it does not run.

Linux has no single distribution format the way macOS has `.app` and Windows has
an installer. The formats that come closest — Flatpak, Snap — each require a
runtime the player has to install first, which is another step, not fewer. The
AppImage is the one that is a single file, needs nothing installed, and runs on
any distribution with a compatible glibc. It is the smallest thing that meets the
bar.

## Decision

**A second packaging script produces one AppImage.**
`packaging/package_appimage.cmake` assembles an AppDir from a build tree and runs
`appimagetool` over it. It is a CMake script, and takes the same kind of inputs
as `package_release.cmake`, for the same reasons: the project is already CMake,
and a script in `-P` mode can be driven by a test without a build. It is run by
the Linux CI job on every push and by the release workflow on a tag, so the path
cannot rot between releases.

**The artifact is named `fork-engine-<version>-<arch>.AppImage`.** The version
comes from the build tree, which takes it from the root `project()` call, so a
download says what it is without being unpacked (ADR-0094).

**The AppDir carries the game, its content, and the C++ runtime — and not
glibc or a driver.** `ldd` names what the binary needs; everything it names is
copied into `usr/lib` *except*:

- **glibc** (`libc`, `ld-linux`, `libm`, ...). The AppImage runtime runs the
  host's loader, and a bundled libc is loaded beside the host's own, so it fails
  on the first versioned symbol. Bundling libc is the classic AppImage mistake
  and it produces an artifact that works on exactly the machine that built it.
- **the graphics stack** (`libGL`, `libEGL`, `libX11`, `libwayland`, ...). The
  game must use the player's driver, not the build host's Mesa; a bundled driver
  is a driver that does not match the GPU.

What is left is the C++ runtime, which is exactly the part an older distribution
does not have. Each bundled library is written by *resolving the symlink*, not by
copying it: on a normal host `libstdc++.so.6` is a symlink to
`libstdc++.so.6.0.33`, and copying the symlink leaves a dangling link, so the
loader silently falls back to the host's copy and the bundle does nothing. That
is the failure the bundle exists to prevent, and it is invisible without a check,
so the script asserts every bundled file is a real, non-empty file and that no
glibc file was bundled.

**`AppRun` cd's to the content root and puts `usr/lib` on the search path.** The
game resolves `examples/breakout/...` relatively, so the AppImage behaves exactly
like running the binary from the repository root; the search path is what makes
the bundled runtime win over the host's.

**The build is reproducible.** `SOURCE_DATE_EPOCH` is pinned, which makes
mksquashfs record a fixed timestamp and the runtime skip the wall-clock fields it
would otherwise embed. Two runs over the same build tree produce byte-identical
artifacts, which is what makes a published checksum meaningful — a checksum that
differs on every build can only be checked against itself.

**The tool is pinned and checksummed, not vendored.**
`packaging/fetch_appimagetool.sh` downloads one release asset of `appimagetool`
and verifies its SHA-256 before use. The tool is not in a distro repository and
is itself an AppImage, so every caller runs it with `APPIMAGE_EXTRACT_AND_RUN=1`
— it unpacks instead of mounting, which works with no FUSE, on a bare host and in
a CI container alike.

**The artifact is proven on a clean machine, not the build machine.**
`tests/appimage/run_appimage_container_smoke.sh` runs the AppImage in a container
that has none of the engine's libraries: first `--help`, which proves the bundle
unpacks and the loader resolves the bundled runtime, then a headless play under a
virtual display and Mesa, which proves the game actually runs. It checks the
loader's own report to confirm the *bundled* libstdc++ was used, because a
fallback to the container's copy would otherwise pass.

## Consequences

Easier:

- A friend can be sent one file. They make it executable and run it; there is no
  unpack step, no library to install, and no place they have to run it from.
- An older distribution works. The C++ runtime travels with the app, so the
  glibc floor is the build host's glibc and nothing else.
- A packaging regression fails the ordinary CI run. The Linux job builds the
  AppImage and the container smoke runs it, so a dropped content directory or a
  broken bundle is caught on the push that made it.

Harder:

- The AppImage is bigger than the tarball, because it carries the C++ runtime and
  a squashfs image; the game archive stays for anyone who wants the smaller file.
- The glibc floor is the build host's. An AppImage is not a container: a player
  on a distribution older than the build host's glibc still cannot run it, and
  closing that would need an older build image. The C++ runtime is bundled
  because that is the common case; libc is not, because it cannot be.
- One more pinned download. `appimagetool` is fetched at packaging time, so the
  packaging step needs the network, and the pin is the security boundary — which
  is why the version and the checksum live in one file and a guard test asserts
  both are checked.

Not done, and recorded rather than half-done:

- There is no AppImage update information embedded, so an installed AppImage does
  not self-update. That is a hosted service, and the project does not carry one.
- There is no `.desktop`-based integration step (`appimaged` or
  `--appimage-integrate`); the artifact runs, and installing it into a menu is
  the player's choice.
- Windows and macOS still get the archives from ADR-0102; the AppImage is the
  Linux answer only.

## Alternatives considered

- **Ship the tarball and document the `libstdc++` fix.** Rejected: it makes the
  player diagnose a link error and install a package before they can play, which
  is the opposite of "download it, try it".
- **A Flatpak or a Snap.** Rejected: both need a runtime installed on the host
  first, so they add a step rather than remove one, and both need a store or a
  remote to publish to.
- **A statically linked binary.** Rejected: it would not remove the graphics
  driver dependency (the game must use the host's GL), and static linking of the
  C++ runtime does not remove the libc floor. It also makes the artifact larger
  than the AppImage for the part it does solve.
- **Build against an older glibc (a container build image) so the AppImage runs
  everywhere.** Not rejected on principle — it is the right next step for
  reach — but it is a separate change: it means a build image, a different
  toolchain in CI, and a decision about which distribution to target. This change
  keeps the build host's floor and records it.
- **Bundle every library `ldd` names, glibc included.** Rejected for the reason
  above: the loader is the host's, so a bundled libc cannot work, and it is the
  mistake that gives AppImages their reputation for working only on the machine
  that made them.
- **`appimagetool` committed into the repository.** Rejected: it is a 15 MB
  binary that changes with its own releases, and vendoring it means the pin and
  the file can drift. A checksummed download keeps the pin in one readable place.
