# ADR-0114: The iOS target's toolchain and platform macro

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0105](0105-android-window-backend.md),
  [ADR-0073](0073-emscripten-toolchain.md),
  [ADR-0113](0113-android-export-template.md),
  [ADR-0032](0032-near-term-platform-scope.md),
  [ADR-0041](0041-platform-contract-and-null-backend.md)

## Context

[ADR-0071](0071-web-first-2d-shipping-path.md) orders the shipping targets web,
then phones, then desktop, and the phone phase has two halves: Android and iOS.
Android got its seam first —
[ADR-0105](0105-android-window-backend.md) landed the toolchain file and the
`FE_PLATFORM_ANDROID` macro — and then its export template
([ADR-0113](0113-android-export-template.md)). iOS is the other half, and it has
neither.

The gap is the same one Android had, and one more. `cmake/FEPlatform.cmake`
already *classifies* `iOS` correctly — it reads `CMAKE_SYSTEM_NAME` first, so an
iOS configure is classified `ios` — but `fe_platform_definitions()` never turned
that into a macro: there was no `FE_PLATFORM_IOS`, and no toolchain file that
produces an iOS configure. The *extra* gap is the trap this ADR exists to name:
iOS is `APPLE`, so the host-variable fallback in `fe_detect_platform()` would
classify an iOS configure as `macos` if `CMAKE_SYSTEM_NAME` were not read first,
and `fe_platform_definitions()` would then hand it `FE_PLATFORM_DESKTOP` and
`FE_PLATFORM_MACOS`. The build would select the GLFW backend, which cannot open a
window on iOS, and the mistake would compile on a macOS host and fail on a device
— the least debuggable place there is.

The forces are the same as ADR-0073 and ADR-0105, plus one that is unique to iOS.
First, the target platform must be read from `CMAKE_SYSTEM_NAME`, because the
host variables are wrong for a cross build — an iOS configure runs on macOS and
targets `arm64-apple-ios`. Second, the decision must be *observable*: a desktop
suite that only ever compiles GLFW cannot notice that iOS was handed the desktop
macros, because it never takes that branch.

The unique force is a hard *host* constraint. An Android build needs the NDK,
which is a large but freely downloadable artifact; the toolchain file can find it
or fail with instructions. An iOS build needs Xcode, which exists only on macOS
and cannot be fetched on another host at all. So the honest thing this seam can
do on a non-macOS machine is refuse, at configure time, with the reason — which is
exactly what the queue item says to do ("record the constraint rather than
stubbing it").

## Decision

**1. iOS is its own platform macro, not a flavour of macOS or web.**
`fe_platform_definitions()` defines `FE_PLATFORM_IOS=1` when the target
classifies as `ios`, and returns before the desktop branch, so an iOS build is
never also given `FE_PLATFORM_DESKTOP`, `FE_PLATFORM_MACOS`, or
`FE_PLATFORM_WEB`. The macro contract is now *exactly one of*
`FE_PLATFORM_DESKTOP` / `FE_PLATFORM_WEB` / `FE_PLATFORM_ANDROID` /
`FE_PLATFORM_IOS`.

**2. Both mobile platforms also define `FE_PLATFORM_MOBILE`.** A source that cares
only "is this a phone?" — a touch path, a fullscreen lifecycle — asks once and
cannot forget one of the two. It is defined *in addition to* the platform macro,
not instead of it, so a source that needs the iOS backend specifically still asks
for `FE_PLATFORM_IOS`. The macro contract is therefore "exactly one platform
macro, plus `FE_PLATFORM_MOBILE` on the two mobile ones".

**3. Predicates to select the backend on: `fe_platform_is_ios()` and
`fe_platform_is_mobile()`**, counterparts to `fe_platform_is_web()` and
`fe_platform_is_android()`. The build chooses which window backend to compile on
the predicate, the way `engine/CMakeLists.txt` already does for web and Android,
rather than on the desktop macro.

**4. A checked-in toolchain file, `cmake/toolchains/ios.cmake`.** It refuses to
run on a host that is not macOS, with a message that says the toolchain and SDKs
are part of Xcode and are macOS-only; sets `CMAKE_SYSTEM_NAME` to `iOS` before
`project()`, which is what makes the classification correct and avoids the
`APPLE`-means-macOS trap; pins a deployment target and a default architecture so
a configure does not silently depend on the host's SDK or CPU; and adds the same
build-option defaults the web and Android toolchains do (`FE_BUILD_EDITOR`,
`FE_BUILD_TESTS`, `FE_BUILD_BENCHMARKS` all `OFF`, each overridable). It does not
and cannot download Xcode.

**5. The build fails loudly on a mobile target, for now.** `engine/CMakeLists.txt`
selects the window backend on `fe_platform_is_web()` and `fe_platform_is_mobile()`
and, for a mobile target, stops with a message naming the missing backend. The
alternative — falling through to `GlfwWindow.cpp` — would compile GLFW into a
build that cannot use it and fail with an error a caller has to decode. A mobile
configure is not yet a buildable target; this makes that fact explicit at the
first step rather than at the last.

## Consequences

- `cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake`
  classifies the target as `ios` on a macOS host, and fails with the host
  constraint on any other host. Either way it does so with a message that names
  the reason.
- `FE_PLATFORM_IOS` and `FE_PLATFORM_MOBILE` are asserted by
  `cmake/tests/test_fe_platform_definitions.cmake` and
  `cmake/tests/test_fe_platform.cmake`, on the desktop CI, without an Apple
  toolchain: the classification and the macro set are pure functions, so the
  branch a desktop build never takes is still covered. In particular the tests
  assert that iOS is *not* macOS and macOS is *not* iOS, which is the trap.
- **The iOS runtime is not written.** A `--target ios` export does not exist, and
  the iOS window backend — a Metal or GLES surface, the lifecycle, and touch — is
  the item's remaining half. The seam is the coherent, tested first step, exactly
  as ADR-0105 was for Android, and the export template (its own queue item)
  follows the same shape as ADR-0113.
- A mobile configure now fails with a clear message instead of a GLFW compile
  error, which is a small change to what a `-DCMAKE_TOOLCHAIN_FILE=…android.cmake`
  run does today: it previously reached the compiler and failed there.
- Nothing about a desktop, web, or Android build changes: the new macro is
  defined only for an iOS target, and `FE_PLATFORM_MOBILE` only for the two
  mobile targets.

## Alternatives considered

- **Treat iOS as macOS (`FE_PLATFORM_MACOS`), since both are Apple.** Rejected:
  the window backends are different (GLFW versus a UIKit surface), the packaging
  is different (an `.app` versus an `.ipa`), and the SDK is different. Collapsing
  them would force every iOS-specific source to re-derive that it is iOS, and it
  is the specific mistake the `APPLE` fallback would make.
- **Collapse both mobile platforms into one `FE_PLATFORM_MOBILE` macro and no
  per-platform macro.** Rejected: Android and iOS differ where it matters — a
  native activity versus a UIKit view controller, the NDK versus Xcode — the same
  reason Android was not collapsed into web in ADR-0105. `FE_PLATFORM_MOBILE` is
  additive, for the parts that genuinely are shared.
- **Give the toolchain a stub that configures on Linux.** Rejected: it would
  produce a build tree that cannot compile, and a configure that succeeds and a
  build that fails is worse than a configure that fails with the reason. The
  queue says to record the constraint, not to stub it.
- **Wait for the iOS runtime before adding the toolchain and macro.** Rejected:
  the seam is what the runtime is written against, exactly as ADR-0105's seam was
  for Android. It is pure and testable on the desktop CI, so it can land before
  there is any way to build for the target.
