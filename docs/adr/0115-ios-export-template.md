# ADR-0115: The iOS export template

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0114](0114-ios-window-backend.md),
  [ADR-0113](0113-android-export-template.md),
  [ADR-0064](0064-export-runtime-and-player.md),
  [ADR-0071](0071-web-first-2d-shipping-path.md),
  [ADR-0030](0030-asset-pack-format.md)

## Context

An export has to turn a project into something a player can be handed, and each
target has a different shape. Desktop is a copied runtime and a pack
([ADR-0064](0064-export-runtime-and-player.md)); web is a wasm pair and an HTML
shell ([ADR-0071](0071-web-first-2d-shipping-path.md)); Android is a Gradle
project that compiles the player and packages an APK
([ADR-0113](0113-android-export-template.md)). iOS is a fifth shape: there is no
executable to copy and no Gradle, but a project CMake can turn into an app bundle
when it is configured with the iOS toolchain
([ADR-0114](0114-ios-window-backend.md)).

The seam is in place — `cmake/toolchains/ios.cmake`, `FE_PLATFORM_IOS`, and the
mobile predicates — but `fork_export --target ios` did not exist, so a project
could not be exported for it at all. Android had the same gap between its seam
and its template, and the template is what makes the target reachable from a
build.

Two forces shape the decision, and they are the ones ADR-0113 named. First, the
template must be a *string*, not a checked-in directory: the tool that writes the
project and the test that checks it share one definition, so they cannot disagree
about a file name — and the names are the contract, because CMake resolves the
subdirectory, the app target is a bundle, the plist names it, and the pack
travels in its resources. Second, the template must be *pure*: it turns values
into text and writes files, and it never invokes CMake or Xcode, so it is
testable on a Linux or Windows CI. An export step that needed Xcode to run would
be unusable on the machines most developers have.

## Decision

**1. `fork_export --target ios` writes a CMake/Xcode project.** A new
`fe::pack::IosExportTemplate` turns the `ExportManifest` and an
`IosProjectOptions` into project text: a top-level `CMakeLists.txt` and an
`app/Info.plist`. The export tool gains the `ios` target, the `--ios-bundle-id`
and `--ios-engine-dir` options, and writes the pack and the manifest into
`app/assets`, which the project copies into the app bundle's Resources. This
mirrors ADR-0113 exactly, because the two targets are the same shape.

**2. The project builds the runtime as an app bundle, from source.** The
`CMakeLists.txt` adds the engine as a subdirectory and declares
`add_executable(fork_player MACOSX_BUNDLE …)`. `MACOSX_BUNDLE` is what makes
CMake produce a `.app` rather than a bare executable; under the iOS toolchain it
is an iOS `.app`. Like the Android project, it is built from the checkout the
export was pointed at, so there is no runtime file for the export to copy and no
toolchain for the export to require — the same rule ADR-0064 set for desktop.

**3. The names are the contract, and they are exposed as strings.** `fork_export`
and the tests both call the template's `ios_project_cmake_lists()` and
`ios_info_plist()` rather than reading a checked-in tree, so a name cannot drift
between the project the tool writes and the project the test checks. The
`Info.plist` is generated whole, with the values substituted rather than left as
`@VARS@`, because the file is generated and one less indirection is one less
thing to get wrong. The project points `MACOSX_BUNDLE_INFO_PLIST` at exactly the
plist the export writes.

**4. The plist carries the keys iOS requires to launch, and its values are
escaped.** `CFBundleExecutable` names the bundle's executable (CMake names it
after the target); `CFBundleIdentifier` is the signing identity; `MinimumOSVersion`
comes from the options so the plist and the toolchain agree; the launch and
orientation keys make it a fullscreen landscape game rather than a windowed app
iOS rejects. A game named `Tom & Jerry <Deluxe>` must not produce a plist that is
not XML, so text that reaches the plist is XML-escaped, the same reason the
Android template quotes its Gradle values.

**5. A module is refused, as it is for Android.** The export does not stage a
native library directory for iOS, so recording a module the build would never
load would claim gameplay the build cannot run. It is refused with a sentence
rather than recorded and ignored.

## Consequences

- `fork_export --project DIR --out DIR --target ios` writes a project that,
  configured on a macOS host with the iOS toolchain, builds the player and the
  pack into an app bundle. On any other host the toolchain refuses with the host
  constraint ([ADR-0114](0114-ios-window-backend.md)), which is the documented
  behaviour.
- The template is asserted by `tests/src/test_export.cpp` on the desktop CI: the
  bundle is built as a `MACOSX_BUNDLE`, the plist the project points at is the
  one the export writes, the pack travels in the Resources, the plist carries the
  keys iOS requires, the bundle id is derived and valid, and a name with XML
  characters is escaped. None of it invokes CMake or Xcode.
- **The iOS runtime is not written.** The app bundle this project builds is
  configured with the iOS toolchain, and that toolchain selects the mobile
  backend, which `engine/CMakeLists.txt` refuses with a clear message until the
  backend exists ([ADR-0114](0114-ios-window-backend.md)). So the project is
  correct and the export is complete, but the app does not run yet. This is the
  same position Android was in after ADR-0113: the template is the seam the
  backend is built into, and the "installs and launches" half waits on the
  backend.
- Nothing about a desktop, web, or Android export changes: the new target is
  reached only by `--target ios`, and the shared code paths are guarded on the
  new `project_target` flag exactly as Android's were.

## Alternatives considered

- **Copy a prebuilt iOS runtime instead of writing a project.** Rejected: an iOS
  app is a signed bundle built for a specific SDK and architecture, not a file
  that can be copied between machines. Building from source is the only shape
  that works, and it is the shape Android already uses.
- **A checked-in Xcode project (`project.pbxproj`) rather than a CMake project.**
  Rejected: a pbxproj is a large, opaque, merge-hostile file, and the engine is
  CMake-based. Generating a small CMake project keeps one build description and
  lets the same file be read by a human.
- **Invoke `xcodebuild` from the export step.** Rejected: it would make an export
  require Xcode, so a developer on Linux could not export for iOS at all, and the
  template would no longer be pure or testable on CI. The export writes the
  project; building it is the developer's step, exactly as with Android.
- **Emit `@VARS@` in the plist for CMake to substitute.** Rejected: the file is
  generated by the tool, which already knows every value, so a second
  substitution pass is an extra indirection that can silently not run. The tool
  substitutes the values itself.
