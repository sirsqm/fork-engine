# ADR-0105: The Android target's toolchain and platform macro

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0073](0073-emscripten-toolchain.md), [ADR-0071](0071-web-first-2d-shipping-path.md),
  [ADR-0032](0032-near-term-platform-scope.md), [ADR-0041](0041-platform-contract-and-null-backend.md)

## Context

[ADR-0071](0071-web-first-2d-shipping-path.md) orders the shipping targets web,
then phones, then desktop. Web landed first, and
[ADR-0073](0073-emscripten-toolchain.md) gave it the two things every cross
target needs before any of its own code can be written: a toolchain file and a
platform macro, both tested on a desktop-only CI. Phones are next, and Android is
the first of them.

The gap is the same one web had. `cmake/FEPlatform.cmake` already *classifies*
`Android` correctly — it reads `CMAKE_SYSTEM_NAME` first, so an NDK configure is
classified `android` and not desktop Linux — but `fe_apply_platform_definitions()`
only ever turned that into `FE_PLATFORM_DESKTOP` plus a per-OS macro, or into
`FE_PLATFORM_WEB`. There was no `FE_PLATFORM_ANDROID`, and no toolchain file that
produces an Android configure in the first place. A source file that needs the
Android backend had no macro to ask for, and the build had no way to be told it
was targeting Android.

The same two forces as ADR-0073 apply, and point the same way. First, the target
platform must be read from `CMAKE_SYSTEM_NAME`, because the host variables are
wrong for a cross build: an NDK configure running on Linux sets `UNIX` for the
host while targeting `aarch64-linux-android`. Second, the decision must be
*observable*: a desktop suite that only ever compiles the GLFW backend cannot
notice that Android was handed the desktop macros, because it never takes that
branch.

Android is also not web. Both are cross builds that skip GLFW, but they differ
where it matters: web runs on Emscripten's emulated platform, Android is a real
POSIX platform with an NDK sysroot. Collapsing them into one "not desktop" macro
would force every cross-target source to re-derive which one it is.

## Decision

**1. Android is its own platform macro, not a flavour of web or desktop.**
`fe_apply_platform_definitions()` defines `FE_PLATFORM_ANDROID=1` when the target
classifies as `android`, and returns before the desktop branch, so an Android
build is never also given `FE_PLATFORM_DESKTOP` or `FE_PLATFORM_WEB`. The macro
contract is now *exactly one of* `FE_PLATFORM_DESKTOP` / `FE_PLATFORM_WEB` /
`FE_PLATFORM_ANDROID`.

**2. A predicate to select the backend on, `fe_platform_is_android()`,** the
counterpart to `fe_platform_is_web()`. The build chooses which window backend to
compile on the predicate, the way `engine/CMakeLists.txt` already does for web,
rather than on the desktop macro.

**3. A checked-in toolchain file, `cmake/toolchains/android.cmake`.** It sets
`CMAKE_SYSTEM_NAME` to `Android` before `project()` — the thing that makes the
classification correct — then delegates the ABI, sysroot, compiler, and API level
to the NDK's own `build/cmake/android.toolchain.cmake`, which is versioned with
the NDK and is the only thing that knows them. It adds the same build-option
defaults the web toolchain does (`FE_BUILD_EDITOR`, `FE_BUILD_TESTS`,
`FE_BUILD_BENCHMARKS` all `OFF`, each overridable), and it fails loudly, at
configure time, with install instructions when the NDK is absent.

**4. The toolchain does not install the NDK.** The NDK is a multi-gigabyte
download; an automation run must not spend its budget on one. A configure without
it fails at the explicit check, with a message that says how to point at one.

**5. The definition mapping is tested, not just the classification.**
`fe_apply_platform_definitions()` is split: the new `fe_platform_definitions()`
returns the macro list and applies nothing, and the applying half is a one-line
caller of it. `add_compile_definitions()` does not exist in `cmake -P` script
mode, so without the split the mapping is unreachable from a test — and the
mapping is exactly the mistake that compiles on desktop and only misbehaves on a
phone. `cmake/tests/test_fe_platform_definitions.cmake` drives the pure half for
each target and asserts the resulting macro set, including that `Android` yields
`FE_PLATFORM_ANDROID` alone; it is registered as the `platform_definitions`
ctest. The existing `test_fe_platform.cmake` gains the matching
`fe_platform_is_android()` assertion for every platform it already classifies.

## Consequences

- A source file that needs the Android backend can ask for `FE_PLATFORM_ANDROID`,
  and one that needs the browser backend for `FE_PLATFORM_WEB`, and no build
  defines both. The desktop build is unchanged: it still defines
  `FE_PLATFORM_DESKTOP` and its per-OS macro and nothing else.
- The Android configure is a documented, reproducible command, and its failure
  mode without an NDK is a clear message rather than a confusing compile error
  from a host compiler.
- A wrong macro mapping now fails a ctest on the desktop CI instead of surviving
  to a phone.
- The window backend itself is *not* in this ADR. The toolchain and the macro are
  the seam it is written against; the native activity, EGL surface, lifecycle,
  and touch come next, and the APK packaging after that. Until then an Android
  configure is a build-system path, not a runnable target.
- The cross-platform build of this change is a CMake-only change plus comments;
  it adds no source that a macOS or Windows compiler sees.

## Alternatives considered

- **Reuse `FE_PLATFORM_WEB` for Android, since both are "not desktop".** Rejected:
  they are different platforms with different runtimes, and a shared macro would
  force every cross-target source to re-derive which one it is — the `#ifdef`
  scatter ADR-0073 set out to avoid.
- **Detect Android from the compiler (`__ANDROID__`) in the sources.** Rejected for
  the same reason ADR-0073 rejected `__EMSCRIPTEN__`: the build knows the target,
  and a backend that cannot compile for a target must be absent from it, not
  merely unused.
- **Reimplement the NDK toolchain's ABI/sysroot/compiler setup here.** Rejected: it
  is a large, versioned surface that the NDK already maintains; duplicating it
  would be a second source of truth that drifts.
- **Fetch and install the NDK from the toolchain file.** Rejected: a multi-gigabyte
  download inside a configure is a surprise, and it is the one thing that would
  make an automation run fail on the clock rather than on the code.
