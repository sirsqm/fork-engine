# ADR-0113: The Android export template

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0064](0064-export-runtime-and-player.md),
  [ADR-0071](0071-web-first-2d-shipping-path.md),
  [ADR-0105](0105-android-window-backend.md),
  [ADR-0073](0073-emscripten-toolchain.md)

## Context

[ADR-0071](0071-web-first-2d-shipping-path.md) orders the shipping targets web,
then phones, then desktop. Phase 1 (a 2D game in a browser) and most of Phase 2
(the same game on a phone's browser) have shipped. Phase 2's last item is a
*native* Android build, and its first half —
[ADR-0105](0105-android-window-backend.md) — landed the toolchain file and the
`FE_PLATFORM_ANDROID` macro: the seam a backend is written against. The item
after it in `docs/SHIPPING_QUEUE.md` is `android-export-template`: extend
`fork_export` to produce an Android build.

The export step today has two shapes. A desktop build *copies* the native
`fork_player` beside the pack — no compiler in the export, and the same player
serves every game ([ADR-0064](0064-export-runtime-and-player.md)). A web build
*cannot* copy a runtime, because the player is a wasm module a page loads
asynchronously; so the export assembles the wasm pair Emscripten produced and
writes an HTML/JS shell beside it ([ADR-0071](0071-web-first-2d-shipping-path.md)).
Android is a third shape, and neither of the first two fits. There is no single
executable to copy — an Android build is a Gradle project whose app module
compiles the player for the phone's ABI and packages it, the pack, and the
manifest into an APK.

The forces are the same ones the web template met. First, the artifact a phone
installs is *not* the thing an export writes: the export writes a project, and
Gradle builds the APK. Second, the export must not require a toolchain. A web
export does not invoke Emscripten; an Android export must not invoke Gradle or
the NDK, both of which are large, versioned, and absent from most machines —
including the one this change was written on. Third, the contract is a set of
*file names and shapes* that only fail on a device: Gradle resolves the modules,
the manifest names the native library, the JNI CMakeLists builds it. A mismatch
is a build that fails on a phone, the least debuggable place there is.

There is also a fact this change must be honest about. `android-window-backend`
is marked done in the queue, but what landed was the toolchain and the macro; the
native activity, the EGL surface, and the `android_main` entry point are not
written. So `fork_export --target android` can produce a *correct project* whose
native build has no Android entry point to compile yet. That is not a reason to
withhold the export template — it is the same shape ADR-0105 itself took, and the
export template is the seam the backend will be built into — but it must be said
rather than implied.

## Decision

**1. Android is a third export target, `--target android`.** `ExportManifest`
gains `kTargetAndroid` and accepts it on read, beside `kTargetDesktop` and
`kTargetWeb`. A document with no target still reads as desktop, and the desktop
document is still byte-for-byte unchanged, so no existing build is affected.

**2. The template is a value, not a checked-in directory.**
`fe::pack::AndroidExportTemplate` turns an `ExportManifest` and an
`AndroidProjectOptions` into the project's text and writes it. It is exposed the
same way the web shell is
([ADR-0071](0071-web-first-2d-shipping-path.md)): the functions that produce each
file are public so a test can assert the file names and the shapes without
invoking Gradle or touching a device. The names live in `AndroidExportNames`, so
the export and the test cannot disagree about them.

**3. The export writes a Gradle project and stages the pack in it.** The project
is a single `:app` module — `settings.gradle`, a root `build.gradle` pinning the
Android Gradle Plugin, `gradle.properties`, `app/build.gradle`,
`app/src/main/AndroidManifest.xml`, and `app/src/main/jni/CMakeLists.txt`. The
pack and the manifest are written to `app/src/main/assets/`, which is what the
Gradle plugin packages into the APK's assets. This is where Android diverges from
desktop and web: the pack is *not* a sibling of the runtime, because there is no
runtime file at the export root — it is an asset the APK carries.

**4. The runtime is compiled from source, not copied, and the project points at
the engine tree.** The JNI CMakeLists adds the engine's own CMakeLists as a
subdirectory and builds `fork_player` from `tools/player/src/main.cpp` as a
shared library — shared because it is what the native activity loads by name. The
engine source directory is passed on the command line
(`--android-engine-dir`, default the working directory) and made absolute, so
Gradle's working directory cannot change what is built. The export does not
reimplement the engine's build; it points at it, so the same
`FE_PLATFORM_ANDROID` path the toolchain file selects is what builds.

**5. The export invokes no toolchain.** It writes text and copies the pack. It
does not run Gradle, does not need the NDK, and does not need an Android SDK, so
it is testable on the desktop CI exactly as the web template is. Building the APK
is the caller's step, with the toolchain and SDK they already have.

**6. The application id is derived, valid, and lowercase.** When
`--android-application-id` names one it is used verbatim; otherwise it is
`com.forkengine.<name>`, where the project's name is reduced to a valid Java
package: segments of letters and digits, lowercased, no leading digit, no empty
segment. Lowercased because the Play Store requires it — a build that installs
locally but cannot be published is a trap the export should not set.

**7. Modules are refused for the Android target, for now.** A web build refuses
gameplay modules because a browser has no shared-library loader
([ADR-0071](0071-web-first-2d-shipping-path.md)); an Android build loads them
from the app's native library directory, which the export does not stage yet.
Refusing, rather than recording a module the build would never load, keeps a
build from claiming gameplay it cannot run — the same rule the web target
already follows.

## Consequences

- `fork_export --target android --project DIR --out DIR` produces a Gradle
  project whose app module builds the player for `arm64-v8a` and `x86_64`,
  packages the pack and the manifest, and declares the native activity that
  starts it. The pack is staged in the app's assets.
- The template is pure and unit-tested on the desktop CI: the file names, the
  manifest's native-activity declaration, the library name reaching both the
  manifest and the JNI CMakeLists, the derived application id, and the files
  landing on disk are all asserted without Gradle or the NDK.
- The export still needs no toolchain, so it runs on every machine the desktop CI
  runs on, and a change to it cannot fail for a reason unrelated to its own code.
- **The Android runtime entry point is still not written.** A project the export
  produces has a correct Gradle build and a native build with no `android_main`
  to compile, so `./gradlew assembleDebug` does not yet produce a runnable APK.
  That is the `android-window-backend` item's remaining half — native activity,
  EGL surface, suspend/resume, and touch — and it is where the next Android work
  goes. Until then `--target android` is a correct export of a project that does
  not yet run, and the queue item's own bar ("installs and launches on a device
  or emulator") is not met.
- A document written before this change reads unchanged: no target is desktop,
  and the desktop manifest is still written without a target field.

## Alternatives considered

- **Have the export invoke Gradle and produce an APK directly.** Rejected for the
  same reason the web export does not invoke Emscripten: it would put a large,
  versioned, usually-absent toolchain on the export's critical path, so an export
  would fail for reasons that have nothing to do with the game, and could not be
  tested on the desktop CI.
- **Copy a prebuilt `fork_player` `.so` instead of compiling from source.**
  Rejected: it would need one prebuilt per ABI shipped in the checkout, kept in
  step with the engine by hand, and the pack's own asset manager is the only
  thing that must be built for the phone — the engine already builds for Android
  from the toolchain file. Compiling from source also means the export records
  *which* engine tree it built, so a build is reproducible from the checkout it
  names.
- **A checked-in Gradle project the export copies.** Rejected: the project names
  the pack, the scene, and the application id, all of which are per-game values,
  so a static copy would need templating anyway — and the web shell is a string
  for exactly this reason
  ([ADR-0071](0071-web-first-2d-shipping-path.md)). A string keeps the tool that
  writes it and the test that checks it sharing one definition.
- **Stage the pack at the export root, as desktop and web do.** Rejected: an APK
  has no filesystem at the export root; its assets live in a fixed source set,
  and the Gradle plugin packages that. Putting the pack anywhere else would mean
  a copy step in the build that could be forgotten.
- **Wait for the runtime backend before writing the export template.** Rejected:
  the template is the seam the backend is built into, exactly as the toolchain
  and macro were the seam for the backend in
  [ADR-0105](0105-android-window-backend.md). It is pure, testable without an
  Android toolchain, and the backend work that follows it is smaller for having
  it.
